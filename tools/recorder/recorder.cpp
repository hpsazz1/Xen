// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <obs.h>
#include <obs-encoder.h>
#include <graphics/vec2.h>
#include "recorder.h"
#include "marker.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

namespace xen::recorder {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr std::size_t kPacketCapacity = 2048, kHistoryCapacity = 1024, kPendingCapacity = 128;
std::atomic<bool> g_instance_claimed{false};
std::atomic<std::uint64_t> g_file_sequence{0};
std::int64_t steady_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}
std::string utf8(const std::filesystem::path& path) {
    const auto value = path.u8string(); return std::string(value.begin(), value.end());
}
std::string utf8(const wchar_t* value) {
    const int length = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), length, nullptr, nullptr);
    result.pop_back(); return result;
}
struct Data {
    obs_data_t* value = nullptr;
    explicit Data(obs_data_t* data = nullptr) : value(data ? data : obs_data_create()) {}
    ~Data() { if (value) obs_data_release(value); }
    Data(const Data&) = delete;
    Data& operator=(const Data&) = delete;
};
struct PacketStamp {
    std::uint64_t cts = 0;
    std::int64_t pts = 0;
    std::int32_t timebase_num = 0, timebase_den = 0;
};
// 编码回调只复制固定小对象；满载或读端竞争明确计缺口，不等待磁盘、网络或消费者。
struct PacketQueue {
    std::mutex mutex;
    std::array<PacketStamp, kPacketCapacity> values{};
    std::size_t begin = 0, count = 0;
    std::atomic<std::uint64_t> dropped{0};
    void push(const PacketStamp& packet) noexcept {
        std::unique_lock lock(mutex, std::try_to_lock);
        if (!lock) { dropped.fetch_add(1, std::memory_order_relaxed); return; }
        if (count == values.size()) { begin = (begin + 1) % values.size(); --count; ++dropped; }
        values[(begin + count++) % values.size()] = packet;
    }
    std::vector<PacketStamp> drain() {
        std::vector<PacketStamp> result; result.reserve(values.size());
        std::lock_guard lock(mutex);
        for (std::size_t i = 0; i < count; ++i) result.push_back(values[(begin + i) % values.size()]);
        begin = count = 0; return result;
    }
    void reset() {
        std::lock_guard lock(mutex); begin = count = 0; dropped.store(0);
    }
};
struct DisplayInfo {
    std::string id;
    std::uint32_t width = 0, height = 0;
};
DisplayInfo primary_display() {
    const POINT origin{0, 0};
    const auto monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW info{}; info.cbSize = sizeof(info);
    if (!monitor || !GetMonitorInfoW(monitor, reinterpret_cast<MONITORINFO*>(&info))) throw std::runtime_error("无法取得主显示器");
    DEVMODEW mode{}; mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsExW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode, 0) || !mode.dmPelsWidth || !mode.dmPelsHeight)
        throw std::runtime_error("无法取得主显示器原生像素尺寸");
    return {utf8(info.szDevice), mode.dmPelsWidth, mode.dmPelsHeight};
}
bool encoder_available(const char* wanted) {
    const char* id = nullptr;
    for (std::size_t i = 0; obs_enum_encoder_types(i, &id); ++i) if (id && std::string_view(id) == wanted) return true;
    return false;
}
std::filesystem::path executable_directory() {
    std::wstring path(32768, L'\0');
    const auto size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!size || size >= path.size()) throw std::runtime_error("无法取得录制程序目录");
    path.resize(size); return std::filesystem::path(path).parent_path();
}
}

struct Recorder::Impl {
    mutable std::mutex lifecycle;
    Config config;
    Snapshot state;
    bool owns_instance = false, obs_started = false;
    std::string data_directory;
    std::uint32_t render_total_baseline = 0, render_lagged_baseline = 0;
    std::uint32_t encoder_total_baseline = 0, encoder_skipped_baseline = 0;
    obs_scene_t* scene = nullptr;
    obs_source_t* picture = nullptr;
    obs_source_t* loopback = nullptr;
    obs_encoder_t* video_encoder = nullptr;
    obs_encoder_t* audio_encoder = nullptr;
    obs_output_t* output = nullptr;
    MarkerSource marker;
    PacketQueue packets;
    std::atomic<std::uint64_t> encoded_packets{0}, pending_count{0};
    std::atomic<bool> output_stopped{false};
    std::atomic<int> stop_code{std::numeric_limits<int>::min()};
    std::mutex stop_mutex;
    std::condition_variable stop_changed;
    struct Pending {
        std::string id;
        std::int64_t requested_ns = 0;
        std::uint64_t render_gaps = 0, packet_gaps = 0;
    };
    std::vector<Pending> pending;
    std::vector<EncodedMarker> completed;
    std::deque<PacketStamp> packet_history;
    std::deque<RenderedMarker> render_history;
    std::deque<std::string> recent_ids;

    static void packet_callback(obs_output_t*, encoder_packet* packet, encoder_packet_time* timing, void* data) noexcept {
        auto* self = static_cast<Impl*>(data);
        if (!packet || packet->type != OBS_ENCODER_VIDEO || packet->track_idx != 0) return;
        self->encoded_packets.fetch_add(1, std::memory_order_relaxed);
        if (!self->pending_count.load(std::memory_order_acquire)) return;
        if (!timing || !timing->cts || packet->timebase_num <= 0 || packet->timebase_den <= 0) {
            self->packets.dropped.fetch_add(1, std::memory_order_relaxed); return;
        }
        self->packets.push({timing->cts, packet->pts, packet->timebase_num, packet->timebase_den});
    }
    static void stop_callback(void* data, calldata_t* parameters) noexcept {
        auto* self = static_cast<Impl*>(data);
        self->stop_code.store(static_cast<int>(calldata_int(parameters, "code")), std::memory_order_release);
        self->output_stopped.store(true, std::memory_order_release);
        self->stop_changed.notify_all();
    }
    void load_module(const char* name, bool required = true) {
        const auto binary = config.obs_root / "obs-plugins" / "64bit" / (std::string(name) + ".dll");
        const auto data = config.obs_root / "data" / "obs-plugins" / name;
        obs_module_t* module = nullptr;
        const bool okay = std::filesystem::is_regular_file(binary) &&
            obs_open_module(&module, utf8(binary).c_str(), utf8(data).c_str()) == MODULE_SUCCESS && obs_init_module(module);
        if (!okay && required) throw std::runtime_error("加载录制模块失败：" + std::string(name));
    }
    std::string output_error(const char* fallback) const {
        const char* message = output ? obs_output_get_last_error(output) : nullptr;
        if (message && *message) return message;
        message = video_encoder ? obs_encoder_get_last_error(video_encoder) : nullptr;
        return message && *message ? message : fallback;
    }
    void release_output() noexcept {
        if (!output) return;
        obs_output_remove_packet_callback(output, packet_callback, this);
        signal_handler_disconnect(obs_output_get_signal_handler(output), "stop", stop_callback, this);
        obs_output_release(output); output = nullptr;
    }
    void release_graph() noexcept {
        release_output();
        if (video_encoder) { obs_encoder_release(video_encoder); video_encoder = nullptr; }
        if (audio_encoder) { obs_encoder_release(audio_encoder); audio_encoder = nullptr; }
        // 编码断开会排空在途帧；在shutdown清空统计前补取最终重复帧数。
        if (obs_started) update_statistics();
        if (obs_started) { obs_set_output_source(0, nullptr); obs_set_output_source(1, nullptr); }
        if (scene) { obs_scene_release(scene); scene = nullptr; }
        marker.reset();
        if (picture) { obs_source_release(picture); picture = nullptr; }
        if (loopback) { obs_source_release(loopback); loopback = nullptr; }
        if (obs_started) { obs_shutdown(); obs_started = false; }
        if (!data_directory.empty()) { obs_remove_data_path(data_directory.c_str()); data_directory.clear(); }
        if (owns_instance) { g_instance_claimed.store(false, std::memory_order_release); owns_instance = false; }
    }
    void update_statistics() {
        state.encoded_packets = encoded_packets.load(std::memory_order_relaxed);
        state.markers_pending = pending.size();
        state.packet_queue_gaps = packets.dropped.load(std::memory_order_relaxed);
        if (output) {
            state.total_frames = static_cast<std::uint64_t>(std::max(0, obs_output_get_total_frames(output)));
            state.bytes_written = obs_output_get_total_bytes(output);
            state.output_dropped_frames = static_cast<std::uint64_t>(std::max(0, obs_output_get_frames_dropped(output)));
            if (state.running && !state.stopping && output_stopped.load(std::memory_order_acquire)) {
                state.running = false; state.stop_completed = false;
                state.last_error = output_error("录制输出意外停止，请检查录制日志");
            }
        }
        if (obs_started) {
            const auto delta = [](std::uint32_t value, std::uint32_t baseline) -> std::uint64_t {
                return value >= baseline ? value - baseline : 0;
            };
            state.render_total_frames = delta(obs_get_total_frames(), render_total_baseline);
            state.render_lagged_frames = delta(obs_get_lagged_frames(), render_lagged_baseline);
            state.active_render_fps = obs_get_active_fps(); state.average_render_ns = obs_get_average_frame_time_ns();
            if (const auto* video = obs_get_video()) {
                state.encoder_total_frames = delta(video_output_get_total_frames(video), encoder_total_baseline);
                state.encoder_skipped_frames = delta(video_output_get_skipped_frames(video), encoder_skipped_baseline);
            }
        }
        state.packet_timing_valid = state.render_lagged_frames==0 && state.encoder_skipped_frames==0 &&
            state.output_dropped_frames==0 && state.render_queue_gaps==0 && state.packet_queue_gaps==0;
    }
    void finish(std::size_t index, EncodedMarker result) {
        if (result.matched) ++state.markers_matched; else ++state.markers_dropped;
        completed.push_back(std::move(result)); pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(index));
        pending_count.store(pending.size(), std::memory_order_release);
    }
    void collect(bool final) {
        update_statistics();
        const auto rendered = marker.drain();
        state.render_queue_gaps = rendered.dropped;
        for (const auto& frame : rendered.markers) {
            if (std::none_of(pending.begin(), pending.end(), [&](const auto& value) { return value.id == frame.id; })) continue;
            render_history.push_back(frame);
            if (render_history.size() > kHistoryCapacity) render_history.pop_front();
        }
        for (const auto& packet : packets.drain()) {
            packet_history.push_back(packet);
            if (packet_history.size() > kHistoryCapacity) packet_history.pop_front();
        }
        state.packet_queue_gaps = packets.dropped.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < pending.size();) {
            const auto& item = pending[i];
            EncodedMarker result; result.id = item.id;
            if (!state.packet_timing_valid) {
                result.error = "本段存在渲染/编码重复或诊断缺口，无法仅凭CTS确认可见内容";
            } else if (state.render_queue_gaps != item.render_gaps || state.packet_queue_gaps != item.packet_gaps) {
                result.error = "标记期间诊断队列存在缺口，无法确认首个编码可见帧";
            } else {
                for (const auto& packet : packet_history) {
                    const auto frame = std::find_if(render_history.begin(), render_history.end(), [&](const auto& value) {
                        return value.id == item.id && value.encoder_timing_valid && value.encoder_frame_time_ns != 0 && value.encoder_frame_time_ns == packet.cts;
                    });
                    if (frame == render_history.end()) continue;
                    result.render_begin_steady_ns = frame->render_begin_steady_ns;
                    result.render_end_steady_ns = frame->render_end_steady_ns;
                    result.render_begin_system_unix_ns = frame->render_begin_system_unix_ns;
                    result.render_end_system_unix_ns = frame->render_end_system_unix_ns;
                    result.render_frame_time_ns = frame->render_frame_time_ns;
                    result.encoder_frame_time_ns = frame->encoder_frame_time_ns;
                    result.timing_mode = state.gpu_texture_input ? "gpu_previous_completed" : "raw_current";
                    result.video_pts = packet.pts; result.timebase_num = packet.timebase_num; result.timebase_den = packet.timebase_den;
                    result.matched = true; break;
                }
                if (!result.matched) {
                    const auto failure = std::find_if(render_history.begin(), render_history.end(), [&](const auto& value) {
                        return value.id == item.id && !value.error.empty();
                    });
                    if (failure != render_history.end()) result.error = failure->error;
                    else if (final) result.error = "录制已结束，未取得该标记对应的编码帧PTS";
                    else if (steady_ns() - item.requested_ns > 8'000'000'000LL)
                        result.error = "标记编码对齐超过8秒，未取得可核对的PTS";
                }
            }
            if (result.matched || !result.error.empty()) finish(i, std::move(result)); else ++i;
        }
        update_statistics();
    }
    bool begin_output(const char* encoder_id, bool software) {
        Data settings(obs_encoder_defaults(encoder_id));
        if (software) {
            obs_data_set_string(settings.value, "rate_control", "CRF"); obs_data_set_int(settings.value, "crf", 23);
            obs_data_set_string(settings.value, "preset", "veryfast"); obs_data_set_string(settings.value, "tune", "zerolatency");
        } else {
            obs_data_set_string(settings.value, "rate_control", "CBR");
            obs_data_set_int(settings.value, "bitrate", config.bitrate_kbps);
            obs_data_set_string(settings.value, "preset", config.quality_encoder ? "p5" : "p1");
            obs_data_set_string(settings.value, "multipass",config.quality_encoder ? "qres" : "disabled");
            obs_data_set_int(settings.value, "bf", 0);
            obs_data_set_bool(settings.value, "lookahead", false);
        }
        obs_data_set_int(settings.value, "keyint_sec", 2);
        video_encoder = obs_video_encoder_create(encoder_id, "xen-recorder-video", settings.value, nullptr);
        if (!video_encoder) return false;
        obs_encoder_set_video(video_encoder, obs_get_video());
        Data output_settings;
        const auto filename = utf8(state.video_path);
        obs_data_set_string(output_settings.value, "path", filename.c_str());
        output = obs_output_create("ffmpeg_muxer", "xen-recorder-file", output_settings.value, nullptr);
        if (!output) return false;
        obs_output_set_video_encoder(output, video_encoder); obs_output_set_audio_encoder(output, audio_encoder, 0);
        signal_handler_connect(obs_output_get_signal_handler(output), "stop", stop_callback, this);
        obs_output_add_packet_callback(output, packet_callback, this);
        output_stopped.store(false); stop_code.store(std::numeric_limits<int>::min());
        // 只统计本次输出开始后的窗口，排除图形初始化和模块探测预热。
        render_total_baseline = obs_get_total_frames(); render_lagged_baseline = obs_get_lagged_frames();
        encoder_total_baseline = video_output_get_total_frames(obs_get_video());
        encoder_skipped_baseline = video_output_get_skipped_frames(obs_get_video());
        if (!obs_output_start(output)) return false;
        state.encoder_quiescent = false;
        // OBS的公开id保持orig_info，不能据此识别模块内部reroute。独占video encoder时，
        // video_output_active只表示实际raw订阅；在成功start后读取真实输入路径。
        state.gpu_texture_input = !video_output_active(obs_encoder_video(video_encoder));
        marker.set_gpu_texture_timing(state.gpu_texture_input);
        state.encoder_id = obs_encoder_get_id(video_encoder); state.software_fallback = software;
        return true;
    }
};

Recorder::Recorder() : impl_(std::make_unique<Impl>()) {}
Recorder::~Recorder() { stop(); }

bool Recorder::start(const Config& config, std::string& error) noexcept {
    std::lock_guard lock(impl_->lifecycle);
    if (impl_->obs_started || impl_->output) { error = "录制已启动，请先停止当前录制"; return false; }
    if (!impl_->pending.empty() || !impl_->completed.empty()) { error = "请先回收上一段录制的标记结果"; return false; }
    try {
        if (config.fps == 0 || config.fps > 240 || config.bitrate_kbps < 500 || config.bitrate_kbps > 150000 ||
            (config.width == 0) != (config.height == 0)) throw std::runtime_error("分辨率、帧率或码率参数无效");
        impl_->config = config; impl_->state = {};
        impl_->config.obs_root = std::filesystem::absolute(config.obs_root);
        impl_->config.directory = std::filesystem::absolute(config.directory);
        if (config.obs_root.empty() || config.directory.empty() || !std::filesystem::is_directory(impl_->config.obs_root))
            throw std::runtime_error("OBS安装根目录或录制目录无效");
        std::filesystem::create_directories(impl_->config.directory);
        const auto utc_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const auto filename = "recording-" + std::to_string(utc_ms) + "-" + std::to_string(GetCurrentProcessId()) + "-" +
            std::to_string(g_file_sequence.fetch_add(1, std::memory_order_relaxed) + 1) + ".mkv";
        impl_->state.video_path = impl_->config.directory / filename;
        if (std::filesystem::exists(impl_->state.video_path)) throw std::runtime_error("唯一录制文件已存在，拒绝覆盖");
        const auto exe_dir = executable_directory();
        if (!std::filesystem::is_regular_file(exe_dir / "obs-ffmpeg-mux.exe"))
            throw std::runtime_error("录制程序旁缺少同版本obs-ffmpeg-mux.exe");
        if (!config.allow_software_encoder && !std::filesystem::is_regular_file(exe_dir / "obs-nvenc-test.exe"))
            throw std::runtime_error("录制程序旁缺少同版本obs-nvenc-test.exe");
        bool expected = false;
        if (!g_instance_claimed.compare_exchange_strong(expected, true)) throw std::runtime_error("本进程已有libobs录制实例");
        impl_->owns_instance = true;
        if (obs_initialized()) throw std::runtime_error("本进程已初始化外部libobs，拒绝共享其场景或配置");
        const auto private_config = utf8(impl_->config.directory / "libobs-config");
        if (!obs_startup("zh-CN", private_config.c_str(), nullptr)) throw std::runtime_error("libobs独立初始化失败");
        impl_->obs_started = true;
        const char* version = obs_get_version_string();
        if (!version || !std::string_view(version).starts_with("32.1.")) throw std::runtime_error("录制工具需要现有OBS32.1运行库");
        // 官方check_path直接拼接目录和文件名，必须带尾分隔符；该路径跨shutdown保留，收尾显式移除。
        impl_->data_directory = utf8(impl_->config.obs_root / "data" / "libobs") + "/";
        obs_add_data_path(impl_->data_directory.c_str());
        DisplayInfo display;
        if (!config.synthetic) display = primary_display();
        const auto width = config.width ? config.width : (config.synthetic ? 1280U : display.width);
        const auto height = config.height ? config.height : (config.synthetic ? 720U : display.height);
        if (width < 128 || height < 96 || width > 8192 || height > 8192 || (width % 2) || (height % 2))
            throw std::runtime_error("NV12录制要求偶数像素，宽128至8192、高96至8192");
        if (!config.synthetic && (width != display.width || height != display.height))
            throw std::runtime_error("屏幕录制保持主显示器原生分辨率，请勿传入缩放尺寸");
        const auto graphics = utf8(impl_->config.obs_root / "bin" / "64bit" / "libobs-d3d11.dll");
        obs_video_info video{}; video.graphics_module = graphics.c_str(); video.fps_num = config.fps; video.fps_den = 1;
        video.base_width = video.output_width = width; video.base_height = video.output_height = height;
        video.output_format = VIDEO_FORMAT_NV12; video.gpu_conversion = true;
        video.colorspace = VIDEO_CS_709; video.range = VIDEO_RANGE_PARTIAL; video.scale_type = OBS_SCALE_BILINEAR;
        if (obs_reset_video(&video) != OBS_VIDEO_SUCCESS) throw std::runtime_error("D3D11原生分辨率录制初始化失败");
        obs_audio_info audio{48000, SPEAKERS_STEREO};
        if (!obs_reset_audio(&audio)) throw std::runtime_error("48kHz立体声音频时钟初始化失败");
        // win-capture按当前图形后端注册；必须在D3D11视频初始化后加载。
        if (!config.synthetic) impl_->load_module("win-capture");
        if (config.audio && !config.synthetic) impl_->load_module("win-wasapi");
        impl_->load_module("obs-nvenc", !config.allow_software_encoder);
        impl_->load_module("obs-ffmpeg"); impl_->load_module("obs-outputs");
        if (config.allow_software_encoder) impl_->load_module("obs-x264");
        obs_post_load_modules();
        impl_->scene = obs_scene_create_private("xen-recorder-scene");
        if (!impl_->scene) throw std::runtime_error("独立录制场景创建失败");
        if (config.synthetic) impl_->picture = create_synthetic_background(width, height,config.synthetic_motion);
        else {
            Data settings(obs_get_source_defaults("monitor_capture"));
            obs_data_set_int(settings.value, "method", 1); // 官方DXGI主显示器源，不创建game_capture。
            obs_data_set_string(settings.value, "monitor_id", display.id.c_str());
            obs_data_set_bool(settings.value, "capture_cursor", true);
            impl_->picture = obs_source_create_private("monitor_capture", "xen-primary-monitor", settings.value);
        }
        if (!impl_->picture || !obs_scene_add(impl_->scene, impl_->picture)) throw std::runtime_error("录制图像源创建失败");
        if (!impl_->marker.create(std::min<std::uint32_t>(width - 16, 560), 72)) throw std::runtime_error("编码前同步标记源创建失败");
        const auto marker_item = obs_scene_add(impl_->scene, impl_->marker.source());
        if (!marker_item) throw std::runtime_error("编码前同步标记未加入场景");
        vec2 position{}; position.x = position.y = 8.0f; obs_sceneitem_set_pos(marker_item, &position);
        obs_set_output_source(0, obs_scene_get_source(impl_->scene));
        if (!config.synthetic) {
            const auto limit = Clock::now() + 5s;
            while ((!obs_source_get_width(impl_->picture) || !obs_source_get_height(impl_->picture)) && Clock::now() < limit)
                std::this_thread::sleep_for(20ms);
            if (obs_source_get_width(impl_->picture) != width || obs_source_get_height(impl_->picture) != height)
                throw std::runtime_error("主显示器DXGI画面未就绪或尺寸不一致，未开始编码");
        }
        if (config.audio && !config.synthetic) {
            Data settings(obs_get_source_defaults("wasapi_output_capture"));
            obs_data_set_string(settings.value, "device_id", "default"); obs_data_set_bool(settings.value, "use_device_timing", true);
            impl_->loopback = obs_source_create_private("wasapi_output_capture", "xen-system-loopback", settings.value);
            if (!impl_->loopback) throw std::runtime_error("系统声音回环源创建失败");
            obs_source_set_audio_mixers(impl_->loopback, 1); obs_set_output_source(1, impl_->loopback);
        }
        // OBS32.1 ffmpeg_muxer固定要求AV；关闭系统声/合成模式保留静音AAC，不打开音频设备。
        Data audio_settings(obs_encoder_defaults("ffmpeg_aac")); obs_data_set_int(audio_settings.value, "bitrate", 192);
        impl_->audio_encoder = obs_audio_encoder_create("ffmpeg_aac", "xen-recorder-audio", audio_settings.value, 0, nullptr);
        if (!impl_->audio_encoder) throw std::runtime_error("AAC音频编码器创建失败");
        obs_encoder_set_audio(impl_->audio_encoder, obs_get_audio());
        impl_->packets.reset(); impl_->encoded_packets.store(0); impl_->pending_count.store(0);
        impl_->packet_history.clear(); impl_->render_history.clear(); impl_->recent_ids.clear();
        impl_->state.width = width; impl_->state.height = height; impl_->state.fps = config.fps;
        // texture走GPU共享纹理；其内容CTS由marker采真实前轮主合成完成值配对。
        // raw仅用于有界对照，不让游戏高负载时全帧GPU→CPU→GPU回传成为默认路径。
        const char* nvenc = config.raw_frames ? "obs_nvenc_h264_soft" : "obs_nvenc_h264_tex";
        bool started = encoder_available(nvenc) && impl_->begin_output(nvenc, false);
        if (!started) {
            const auto hardware_error = impl_->output_error("NVENC H.264不可用");
            impl_->release_output();
            if (impl_->video_encoder) { obs_encoder_release(impl_->video_encoder); impl_->video_encoder = nullptr; }
            if (!config.allow_software_encoder) throw std::runtime_error(hardware_error + "；未允许软件回退");
            if (!encoder_available("obs_x264") || !impl_->begin_output("obs_x264", true))
                throw std::runtime_error(hardware_error + "；显式x264回退也未启动：" + impl_->output_error("输出启动失败"));
        }
        impl_->state.running = true; impl_->state.stop_completed = false; error.clear(); return true;
    } catch (const std::exception& failure) {
        error = failure.what(); impl_->state.last_error = error;
    } catch (...) { error = "独立录制初始化发生未知异常"; impl_->state.last_error = error; }
    impl_->state.running = false; impl_->state.stop_completed = false;
    impl_->release_graph(); return false;
}

void Recorder::stop() noexcept {
    if (!impl_) return;
    std::lock_guard lock(impl_->lifecycle);
    if (!impl_->obs_started) return;
    try {
        impl_->state.stopping = true;
        if (impl_->output) {
            if (obs_output_active(impl_->output)) obs_output_stop(impl_->output);
            std::unique_lock stopped(impl_->stop_mutex);
            if (!impl_->stop_changed.wait_for(stopped, 10s, [&] { return impl_->output_stopped.load(std::memory_order_acquire); })) {
                obs_output_force_stop(impl_->output);
                impl_->state.last_error = "正常封尾超过10秒，已请求强制停止；成片完整性未确认";
                impl_->stop_changed.wait_for(stopped, 3s, [&] { return impl_->output_stopped.load(std::memory_order_acquire); });
            }
            stopped.unlock();
            // stop信号先于OBS后台断开编码器；必须等真实inactive才能冻结最终重复帧计数。
            const auto inactive_deadline = Clock::now()+5s;
            while ((obs_output_active(impl_->output) || obs_encoder_active(impl_->video_encoder)) &&
                   Clock::now()<inactive_deadline) std::this_thread::sleep_for(2ms);
            const bool inactive = !obs_output_active(impl_->output) && !obs_encoder_active(impl_->video_encoder);
            impl_->state.encoder_quiescent = inactive;
            if (!inactive) {
                impl_->state.last_error = "输出已请求停止，但编码器未在期限内断开；封尾与时间完整性未确认";
                obs_output_force_stop(impl_->output);
            }
            impl_->update_statistics();
            const bool signal_ok = impl_->output_stopped.load(std::memory_order_acquire) && impl_->stop_code.load() == 0;
            const bool nonempty = std::filesystem::is_regular_file(impl_->state.video_path) && std::filesystem::file_size(impl_->state.video_path) > 0;
            impl_->state.stop_completed = signal_ok && inactive && nonempty && impl_->state.encoded_packets != 0 && impl_->state.last_error.empty();
            if (!impl_->state.stop_completed && impl_->state.last_error.empty())
                impl_->state.last_error = impl_->output_error("录制未成功封尾，未取得成功停止信号或有效成片");
        }
        impl_->collect(true);
    } catch (const std::exception& failure) { impl_->state.last_error = failure.what(); impl_->state.stop_completed = false; }
      catch (...) { impl_->state.last_error = "录制封尾发生未知异常"; impl_->state.stop_completed = false; }
    impl_->state.running = false; impl_->state.stopping = false;
    impl_->release_graph();
}

bool Recorder::mark(const std::string& id) noexcept {
    if (!impl_) return false;
    std::lock_guard lock(impl_->lifecycle);
    try {
        if (!impl_->state.running || impl_->state.stopping || id.empty() || id.size() > 128 ||
            impl_->pending.size() + impl_->completed.size() >= kPendingCapacity ||
            std::find(impl_->recent_ids.begin(), impl_->recent_ids.end(), id) != impl_->recent_ids.end()) return false;
        impl_->pending.push_back({id, steady_ns(), impl_->state.render_queue_gaps, impl_->packets.dropped.load()});
        impl_->pending_count.store(impl_->pending.size(), std::memory_order_release);
        if (!impl_->marker.mark(id)) {
            impl_->pending.pop_back(); impl_->pending_count.store(impl_->pending.size(), std::memory_order_release); return false;
        }
        impl_->recent_ids.push_back(id); if (impl_->recent_ids.size() > kPendingCapacity) impl_->recent_ids.pop_front();
        ++impl_->state.markers_requested; impl_->update_statistics(); return true;
    } catch (...) { impl_->state.last_error = "同步标记入队失败"; return false; }
}

Snapshot Recorder::snapshot() const {
    std::lock_guard lock(impl_->lifecycle); impl_->update_statistics(); return impl_->state;
}
std::vector<EncodedMarker> Recorder::drain_markers() {
    std::lock_guard lock(impl_->lifecycle);
    if (impl_->obs_started) impl_->collect(false);
    std::vector<EncodedMarker> result; result.swap(impl_->completed); return result;
}
} // namespace xen::recorder
