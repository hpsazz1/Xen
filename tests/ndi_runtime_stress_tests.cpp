#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <Windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#ifdef ERROR
#undef ERROR
#endif

#include "ndi_capture_test_system.h"
#include "capture/network_internal.h"
#include "runtime/runtime_internal.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "[失败] " << message << '\n';
    }
    return condition;
}

// 只记外部SDK资源借还；帧解析、颜色转换、池和Runtime交接均走生产实现。
class CountedSystem final : public capture::detail::INdiCaptureSystem {
public:
    ndi_test::CaptureSystem input;
    std::atomic<std::uint64_t> videos{0}, freed_videos{0};
    std::atomic<std::uint64_t> finders{0}, freed_finders{0};
    std::atomic<std::uint64_t> receivers{0}, freed_receivers{0};
    std::atomic<std::uint64_t> opened{0}, closed{0};
    std::atomic<int> receiving{0}, borrowed{0};
    std::atomic<bool> lifecycle_error{false};

    CaptureStatus open(const clock_sync::ClientConfig& config,
                       std::string& error) noexcept override {
        const auto result = input.open(config, error);
        if (result == CaptureStatus::READY) {
            if (active_.exchange(true)) lifecycle_error = true;
            ++opened;
        }
        return result;
    }
    void close() noexcept override {
        if (active_.exchange(false)) {
            if (receiving.load() || borrowed.load()) lifecycle_error = true;
            ++closed;
        }
        input.close();
    }
    NDIlib_find_instance_t find_create(const NDIlib_find_create_t* settings) noexcept override {
        const auto value = input.find_create(settings);
        if (value) ++finders;
        return value;
    }
    void find_destroy(NDIlib_find_instance_t value) noexcept override {
        input.find_destroy(value);
        ++freed_finders;
    }
    bool find_wait(NDIlib_find_instance_t value, std::uint32_t timeout) noexcept override {
        return input.find_wait(value, timeout);
    }
    const NDIlib_source_t* find_sources(NDIlib_find_instance_t value,
                                      std::uint32_t* count) noexcept override {
        return input.find_sources(value, count);
    }
    NDIlib_recv_instance_t recv_create(const NDIlib_recv_create_v3_t* settings) noexcept override {
        const auto value = input.recv_create(settings);
        if (value) ++receivers;
        return value;
    }
    void recv_destroy(NDIlib_recv_instance_t value) noexcept override {
        if (receiving.load() || borrowed.load()) lifecycle_error = true;
        input.recv_destroy(value);
        ++freed_receivers;
    }
    int recv_connections(NDIlib_recv_instance_t value) noexcept override {
        return input.recv_connections(value);
    }
    NDIlib_frame_type_e recv_capture(NDIlib_recv_instance_t value,
                                    NDIlib_video_frame_v2_t* video,
                                    NDIlib_metadata_frame_t* metadata,
                                    std::uint32_t timeout) noexcept override {
        ++receiving;
        const auto type = input.recv_capture(value, video, metadata, timeout);
        if (type == NDIlib_frame_type_video) {
            ++videos;
            if (borrowed.fetch_add(1) != 0) lifecycle_error = true;
        }
        --receiving;
        return type;
    }
    void recv_free_video(NDIlib_recv_instance_t value,
                         NDIlib_video_frame_v2_t* video) noexcept override {
        input.recv_free_video(value, video);
        if (borrowed.fetch_sub(1) != 1) lifecycle_error = true;
        ++freed_videos;
    }
    void recv_free_metadata(NDIlib_recv_instance_t value,
                            NDIlib_metadata_frame_t* metadata) noexcept override {
        input.recv_free_metadata(value, metadata);
    }
    void recv_performance(NDIlib_recv_instance_t value,
                          NDIlib_recv_performance_t* total,
                          NDIlib_recv_performance_t* dropped) noexcept override {
        input.recv_performance(value, total, dropped);
    }
    void recv_queue(NDIlib_recv_instance_t value, NDIlib_recv_queue_t* queue) noexcept override {
        input.recv_queue(value, queue);
    }
    clock_sync::MappingResult map_source_timestamp(
            std::int64_t timestamp, Clock::time_point now) const noexcept override {
        return input.map_source_timestamp(timestamp, now);
    }
    bool balanced() const noexcept {
        return !lifecycle_error && !receiving.load() && !borrowed.load() &&
            videos.load() == freed_videos.load() &&
            receivers.load() == freed_receivers.load() &&
            finders.load() == freed_finders.load() && opened.load() == closed.load();
    }

private:
    std::atomic<bool> active_{false};
};

CaptureConfig stress_config() {
    auto config = ndi_test::config();
    config.roi_width = config.roi_height = 320;
    config.ndi_receive_timeout_ms = 2;
    config.acquire_timeout_ms = 10;
    config.ndi_disconnect_timeout_ms = 60000;
    config.enable_performance_probes = true;
    return config;
}

void send(CountedSystem& system, std::uint8_t color) {
    ndi_test::VideoInput video;
    video.width = video.height = 320;
    video.blue = color;
    system.input.send_video(std::move(video));
}

bool has_color(const CapturedFrame& frame, std::uint8_t color) {
    return !frame.bgr.empty() && frame.width == 320 && frame.height == 320 &&
        frame.bgr.type() == CV_8UC3 && frame.bgr_storage &&
        frame.bgr.at<cv::Vec3b>(0, 0)[0] == color &&
        frame.bgr.at<cv::Vec3b>(319, 319)[2] == color;
}

bool wait_freed(CountedSystem& system, std::uint64_t count) {
    const auto deadline = Clock::now() + 2s;
    while (system.freed_videos.load() < count && Clock::now() < deadline)
        std::this_thread::yield();
    return system.freed_videos.load() >= count;
}

void test_capture_leases_survive_pool_exhaustion_and_close() {
    auto system = std::make_unique<CountedSystem>();
    auto* input = system.get();
    auto capture = capture::detail::create_ndi_capture(stress_config(), std::move(system));
    if (!expect(capture && capture->open(), "帧租约测试必须打开生产NdiCapture")) return;

    constexpr auto count = capture::detail::NetworkLatestFramePool::kSlotCount;
    std::array<CapturedFrame, count> held;
    std::array<std::weak_ptr<const cv::Mat>, count> weak;
    std::set<const cv::Mat*> slots;
    for (std::size_t i = 0; i < count; ++i) {
        send(*input, static_cast<std::uint8_t>(i + 1));
        if (!expect(ndi_test::receive(*capture, held[i]), "必须逐一借到生产帧池全部槽")) return;
        weak[i] = held[i].bgr_storage;
        slots.insert(held[i].bgr_storage.get());
    }
    expect(slots.size() == count, "仍被借用的帧不得被后续输入复用");

    send(*input, 99);
    if (!expect(wait_freed(*input, count + 1), "池满拒绝帧仍须归还SDK视频buffer")) return;
    CapturedFrame rejected;
    expect(capture->grab(rejected) == CaptureStatus::NO_FRAME,
           "所有槽均被持有时必须有界拒绝，不能另分配无界帧槽");
    for (std::size_t i = 0; i < count; ++i)
        expect(has_color(held[i], static_cast<std::uint8_t>(i + 1)),
               "池满及拒绝不能覆写仍被消费的旧帧像素");

    ndi_test::release(held[0]);
    send(*input, 77);
    if (!expect(ndi_test::receive(*capture, rejected) && has_color(rejected, 77),
                "归还一个租约后生产采集必须恢复且使用本次像素")) return;
    expect(rejected.timing.source_dropped_frames >= 1,
           "生产帧池拒绝必须进入既有源帧丢弃账");
    expect(slots.contains(rejected.bgr_storage.get()), "释放后必须复用已有槽而非扩大帧池");
    ndi_test::release(rejected);
    capture->close();
    expect(input->balanced(), "close返回前必须收回SDK借帧、receiver、finder与接收线程调用");
    capture.reset();

    for (std::size_t i = 1; i < count; ++i) {
        expect(has_color(held[i], static_cast<std::uint8_t>(i + 1)),
               "Capture析构后在途共享像素仍须有效，不能提前释放");
        ndi_test::release(held[i]);
    }
    for (const auto& reference : weak)
        expect(reference.expired(), "最后租约归还后帧槽必须释放，不能存在残留强引用");
}

std::shared_ptr<const CapturedFrame> publish_one(
        ICapture& capture, CountedSystem& input,
        runtime::detail::LatestFrameQueue& queue,
        runtime::detail::CaptureFramePublisher& publisher,
        std::uint64_t previous, std::uint8_t color) {
    auto destination = queue.acquire_write();
    if (!expect(destination != nullptr, "有空闲槽时Runtime必须取得可写帧")) return {};
    send(input, color);
    const auto deadline = Clock::now() + 2s;
    bool published = false;
    while (Clock::now() < deadline) {
        const auto outcome = publisher.capture_and_publish(capture, destination, true);
        if (outcome.frame_publish_result == runtime::detail::FramePublishResult::PUBLISHED) {
            published = true;
            break;
        }
        if (outcome.capture_status != CaptureStatus::NO_FRAME &&
            outcome.capture_status != CaptureStatus::READY) break;
    }
    if (!expect(published, "SDK输入须经真实NdiCapture和Runtime publisher发布")) return {};
    const std::atomic<bool> stop{false};
    auto result = queue.wait_latest(previous, stop);
    if (!expect(result && result->timing.sequence == previous + 1 && has_color(*result, color),
                "Runtime必须逐帧取得当前序号和像素")) return {};
    return result;
}

void test_runtime_borrowers_remain_bounded() {
    auto system = std::make_unique<CountedSystem>();
    auto* input = system.get();
    auto capture = capture::detail::create_ndi_capture(stress_config(), std::move(system));
    if (!expect(capture && capture->open(), "旁路借用测试必须打开生产NdiCapture")) return;
    runtime::detail::LatestFrameQueue queue;
    runtime::detail::CaptureFramePublisher publisher(queue);
    constexpr auto count = std::tuple_size_v<decltype(queue.initialization_slots())>;
    std::array<std::shared_ptr<const CapturedFrame>, count> held;
    for (std::size_t i = 0; i < count; ++i) {
        held[i] = publish_one(*capture, *input, queue, publisher, i,
                              static_cast<std::uint8_t>(11 + i));
        if (!held[i]) return;
    }
    expect(!queue.acquire_write(), "Runtime全部槽被消费者借用时必须有界退让");
    auto preview = queue.peek_latest();
    expect(preview && preview == held.back(), "道具旁路必须借用现有最新帧");
    const cv::Mat pixels = preview->bgr.clone();
    held[0].reset();
    auto resumed = publish_one(*capture, *input, queue, publisher, count, 88);
    expect(resumed != nullptr, "释放一个Runtime槽后应继续消费");
    expect(has_color(*held[1], 12) && pixels.at<cv::Vec3b>(0, 0)[0] == 13,
           "在途帧与旁路克隆不能被新输入覆写");
    expect(queue.overwritten_frames() == 0, "peek旁路不能破坏真实消费者的消费水位");
    preview.reset(); resumed.reset();
    for (auto& frame : held) frame.reset();
    queue.reset();
    capture->close();
    expect(input->balanced(), "旁路完成后所有SDK资源必须成对归还");
}

struct ResourceSample {
    std::uint64_t private_bytes = 0, working_set_bytes = 0;
    DWORD handles = 0, threads = 0;
};

std::optional<ResourceSample> resources() {
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    ResourceSample result;
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)) ||
        !GetProcessHandleCount(GetCurrentProcess(), &result.handles)) return {};
    result.private_bytes = memory.PrivateUsage;
    result.working_set_bytes = memory.WorkingSetSize;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return {};
    THREADENTRY32 thread{};
    thread.dwSize = sizeof(thread);
    if (!Thread32First(snapshot, &thread)) { CloseHandle(snapshot); return {}; }
    do {
        if (thread.th32OwnerProcessID == GetCurrentProcessId()) ++result.threads;
        thread.dwSize = sizeof(thread);
    } while (Thread32Next(snapshot, &thread));
    const DWORD error = GetLastError();
    CloseHandle(snapshot);
    return error == ERROR_NO_MORE_FILES && result.threads
        ? std::optional<ResourceSample>(result) : std::nullopt;
}

void print_resources(int batch, const ResourceSample& value) {
    std::cout << "[资源] 批次=" << batch << " 私有字节=" << value.private_bytes
              << " 工作集字节=" << value.working_set_bytes << " 句柄=" << value.handles
              << " 线程=" << value.threads << '\n';
}

// 各分支必须分进程运行，防止前一分支暖机掩盖后一分支的资源来源。
// 这些分支仅输出归因证据；无参主测试的资源断言保持独立。
bool diagnose_handles(std::string_view mode) {
    std::cout << "[句柄诊断] 模式=" << mode << '\n';
    const auto record = [](int index) {
        const auto sample = resources();
        if (!expect(sample.has_value(), "句柄控制分支必须完整读到资源")) return false;
        print_resources(index, *sample);
        return true;
    };
    if (mode == "sampling") {
        for (int i = 0; i <= 64; ++i) {
            const auto sample = resources();
            if (!expect(sample.has_value(), "重复资源采样必须成功")) return false;
            if (i % 8 == 0) print_resources(i, *sample);
        }
        return true;
    }
    if (mode == "thread") {
        if (!record(0)) return false;
        for (int i = 1; i <= 64; ++i) {
            std::thread([] {}).join();
            if (i % 8 == 0 && !record(i)) return false;
        }
        return true;
    }
    const bool single_thread = mode == "stream-single-thread";
    if (mode != "receiver" && mode != "stream" && !single_thread) {
        expect(false, "句柄诊断模式仅支持sampling/thread/receiver/stream/stream-single-thread");
        return false;
    }
    if (single_thread) cv::setNumThreads(1);
    if (mode == "stream" || single_thread) {
        const auto& information = cv::getBuildInformation();
        const auto start = information.find("Parallel framework:");
        const auto end = information.find('\n', start);
        std::cout << "[OpenCV] " << (start == std::string::npos ? std::string("并行后端未知") :
            information.substr(start, end == std::string::npos ? end : end - start))
                  << " 请求线程=" << cv::getNumThreads() << '\n';
    }
    auto system = std::make_unique<CountedSystem>();
    auto* input = system.get();
    auto capture = capture::detail::create_ndi_capture(stress_config(), std::move(system));
    if (!expect(capture != nullptr, "句柄控制分支必须创建生产Capture")) return false;
    if (mode == "receiver") {
        if (!record(0)) return false;
        for (int i = 1; i <= 64; ++i) {
            const auto target = input->receivers.load() + 1;
            if (!expect(capture->open(), "无视频接收生命周期必须可打开")) return false;
            const auto deadline = Clock::now() + 2s;
            while (input->receivers.load() < target && Clock::now() < deadline)
                std::this_thread::yield();
            if (!expect(input->receivers.load() == target,
                        "无视频控制仍须实际建立接收器后再关闭")) return false;
            capture->close();
            if (!expect(input->balanced(), "无视频生命周期仍须精确归还全部外部资源")) return false;
            if (i % 8 == 0 && !record(i)) return false;
        }
    } else {
        runtime::detail::LatestFrameQueue queue;
        runtime::detail::CaptureFramePublisher publisher(queue);
        if (!expect(capture->open(), "单会话供帧控制必须可打开")) return false;
        const auto started = Clock::now();
        std::uint64_t sequence = 0;
        const auto block = [&] {
            std::shared_ptr<const CapturedFrame> held;
            for (int i = 0; i < 512; ++i) {
                if (!expect(Clock::now() - started < 45s, "单会话控制必须有界结束")) return false;
                const auto color = static_cast<std::uint8_t>(sequence % 251 + 1);
                auto frame = publish_one(*capture, *input, queue, publisher, sequence, color);
                if (!frame) return false;
                ++sequence;
                if (i == 64) held = frame;
                if (i == 192) held.reset();
                if (i % 8 == 0) {
                    const auto borrowed = queue.peek_latest();
                    if (!expect(borrowed != nullptr, "单会话旁路须取得真实最新帧")) return false;
                    const auto copy = borrowed->bgr.clone();
                    if (!expect(copy.at<cv::Vec3b>(0, 0)[0] == color,
                                "单会话旁路必须保持当前像素")) return false;
                }
            }
            return true;
        };
        if (!block() || !record(0)) return false;
        for (int i = 1; i <= 32; ++i) {
            if (!block()) return false;
            if (i % 8 == 0 && !record(i)) return false;
        }
        queue.reset();
        capture->close();
        if (!expect(input->balanced(), "单会话结束必须归还全部视频/接收器/发现器")) return false;
        if (!record(33)) return false;
    }
    std::cout << "[句柄诊断] 会话=" << input->opened.load()
              << " SDK视频=" << input->videos.load() << '/' << input->freed_videos.load()
              << " 接收器=" << input->receivers.load() << '/' << input->freed_receivers.load()
              << " 发现器=" << input->finders.load() << '/' << input->freed_finders.load() << '\n';
    return true;
}

void test_repeated_stream_sessions_and_resources() {
    auto system = std::make_unique<CountedSystem>();
    auto* input = system.get();
    auto capture = capture::detail::create_ndi_capture(stress_config(), std::move(system));
    if (!expect(capture != nullptr, "长期压力必须创建生产NdiCapture")) return;
    runtime::detail::LatestFrameQueue queue;
    std::set<const CapturedFrame*> runtime_slots;
    std::map<const cv::Mat*, std::weak_ptr<const cv::Mat>> decoded_slots;
    std::uint64_t frames = 0;
    const auto started = Clock::now();

    const auto session = [&](int session_index) {
        if (!expect(capture->open(), "同一生产Capture必须支持重复打开")) return false;
        runtime::detail::CaptureFramePublisher publisher(queue);
        std::shared_ptr<const CapturedFrame> held;
        for (std::uint64_t i = 0; i < 512; ++i) {
            if (Clock::now() - started > 45s) {
                expect(false, "有界压力测试超过45秒，拒绝无限等待");
                return false;
            }
            const auto color = static_cast<std::uint8_t>((i * 17 + session_index) % 251 + 1);
            auto frame = publish_one(*capture, *input, queue, publisher, i, color);
            if (!frame) return false;
            runtime_slots.insert(frame.get());
            decoded_slots.emplace(frame->bgr_storage.get(), frame->bgr_storage);
            if (!expect(frame->timing.receiver_generation == 1,
                        "显式重开不能继承上一会话的receiver代次")) return false;
            if (i == 64) held = frame;
            if (i == 192) held.reset();
            if (i % 8 == 0) {
                const auto borrowed = queue.peek_latest();
                if (!expect(borrowed && borrowed->timing.sequence == i + 1,
                            "旁路必须取得与本帧一致的最新序号")) return false;
                const auto pixels = borrowed->bgr.clone();
                if (!expect(pixels.at<cv::Vec3b>(0, 0)[0] == color,
                            "反复借用/克隆必须保持当前真实像素")) return false;
            }
            ++frames;
        }
        held.reset();
        expect(publisher.published_frames() == 512 && queue.overwritten_frames() == 0,
               "逐帧消费和旁路混合压力不能漏记或制造覆盖");
        queue.reset();
        capture->close();
        return expect(input->balanced(), "每轮结束必须收回全部SDK资源与生产接收线程调用");
    };

    // 默认Concurrency后端的同shape单会话对照约8192帧后才进入资源平台；
    // 完整暖机16384帧，避免把延迟建立的并行调度资源误判为采集泄漏。
    // SDK借还和固定帧池约束仍逐会话检查，后续资源门槛不因暖机而放宽。
    constexpr int warmup_sessions = 32;
    ResourceSample warmup_envelope;
    const auto observe_warmup = [&](const ResourceSample& sample) {
        warmup_envelope.private_bytes = std::max(warmup_envelope.private_bytes, sample.private_bytes);
        warmup_envelope.working_set_bytes = std::max(warmup_envelope.working_set_bytes, sample.working_set_bytes);
        warmup_envelope.handles = std::max(warmup_envelope.handles, sample.handles);
        warmup_envelope.threads = std::max(warmup_envelope.threads, sample.threads);
    };
    for (int i = 0; i < warmup_sessions; ++i) {
        if (!session(i)) return;
        if ((i + 1) % 8 == 0) {
            const auto sample = resources();
            if (!expect(sample.has_value(), "暖机阶段必须保留资源建立过程")) return;
            observe_warmup(*sample);
            std::cout << "[暖机] 已完成会话=" << i + 1 << '\n';
            print_resources(0, *sample);
        }
    }
    auto baseline = resources();
    if (!expect(baseline.has_value(), "暖机后必须可读私有提交量、句柄和线程数")) return;
    observe_warmup(*baseline);
    // Concurrency后台资源可在相邻空闲快照间回落；用同路径暖机的高水位
    // 包络作比较，不能把某一次低谷当作工作态基线。容差仍保持原值。
    *baseline = warmup_envelope;
    std::cout << "[基线] 同路径暖机资源高水位包络\n";
    print_resources(0, *baseline);
    for (int batch = 1; batch <= 4; ++batch) {
        for (int i = 0; i < 8; ++i) if (!session(batch * 8 + i)) return;
        const auto current = resources();
        if (!expect(current.has_value(), "每批结束必须完整读取资源趋势")) return;
        print_resources(batch, *current);
        // 硬约束来自有界槽与SDK资源配对；以下仅拦截粗粒度泄漏，容纳allocator留存。
        expect(current->private_bytes <= baseline->private_bytes + 32ULL * 1024 * 1024,
               "同负载暖机后额外私有提交量不得持续放大到32MiB以上");
        expect(current->handles <= baseline->handles + 4,
               "重复打开/关闭后句柄不得按会话累积");
        expect(current->threads <= baseline->threads + 2,
               "重复关闭后接收线程不得按会话累积");
        expect(decoded_slots.size() <= capture::detail::NetworkLatestFramePool::kSlotCount &&
                   runtime_slots.size() <= std::tuple_size_v<decltype(queue.initialization_slots())>,
               "跨会话和旁路负载下生产帧槽基数必须保持既定上界");
    }
    const auto videos = input->videos.load();
    const auto freed_videos = input->freed_videos.load();
    const auto sessions = input->opened.load();
    capture.reset();
    for (const auto& [slot, reference] : decoded_slots)
        expect(reference.expired(), "全部会话结束并析构后不得遗留生产解码帧强引用");
    std::cout << "[压力] 帧=" << frames << " SDK接收/归还=" << videos << '/' << freed_videos
              << " 会话=" << sessions << " 解码槽=" << decoded_slots.size()
              << " Runtime槽=" << runtime_slots.size() << " 总毫秒="
              << std::chrono::duration<double, std::milli>(Clock::now() - started).count() << '\n';
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--diagnose-handles")
        return diagnose_handles(argv[2]) && !failures ? 0 : 1;
    if (argc != 1) {
        std::cerr << "用法：ndi_runtime_stress_tests [--diagnose-handles sampling|thread|receiver|stream|stream-single-thread]\n";
        return 2;
    }
    test_capture_leases_survive_pool_exhaustion_and_close();
    test_runtime_borrowers_remain_bounded();
    test_repeated_stream_sessions_and_resources();
    if (failures) {
        std::cerr << "NDI/Runtime资源压力失败数: " << failures << '\n';
        return 1;
    }
    std::cout << "NDI/Runtime生产接口资源与生命周期压力测试通过；未运行真实NDI/设备\n";
    return 0;
}
