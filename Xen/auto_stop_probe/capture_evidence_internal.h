#ifndef AUTO_STOP_CAPTURE_EVIDENCE_INTERNAL_H
#define AUTO_STOP_CAPTURE_EVIDENCE_INTERNAL_H
#include "auto_stop_probe/preroll_internal.h"
#include "auto_stop_probe/probe_internal.h"
#include "capture/capture.h"
#include <opencv2/imgcodecs.hpp>
#include <atomic>
#include <fstream>
#include <mutex>
#include <thread>
namespace auto_stop_probe_detail {
struct Evidence {
    static constexpr std::size_t frame_limit = 1000;
    static constexpr std::size_t byte_limit = 384ULL * 1024 * 1024;
    struct Frame { cv::Mat pixels; FrameTiming timing; std::int64_t received_ns; };
    std::unique_ptr<ICapture> capture;
    std::vector<Frame> frames;
    std::jthread thread;
    std::atomic<bool> failed{false};
    std::atomic<int> count{0};
    std::atomic<std::int64_t> latest_ns{0};
    std::atomic<std::int64_t> first_ns{0};
    std::atomic<int> last_status{static_cast<int>(CaptureStatus::CLOSED)};
    std::atomic<int> failure_code{0};
    std::atomic<int> capture_phase{0}; // 0等待，1抓帧，2复制/发布
    std::atomic<std::int64_t> grab_started_ns{0}, grab_returned_ns{0};
    std::atomic<int> no_frame_count{0};
    std::int64_t opened_ns = 0;
    mutable std::mutex observation_mutex;
    CaptureBackend backend;
    explicit Evidence(CaptureConfig config) : backend(config.backend) {
        config.enable_d3d11_cuda_interop = false;
        config.enable_d3d11_directml_interop = false;
        capture = create_capture(config);
        if (!capture || !capture->open()) throw std::runtime_error("采集源不可用");
        opened_ns = ns(Clock::now());
        frames.reserve(frame_limit);
        thread = std::jthread([this](std::stop_token stop) {
            std::size_t bytes = 0;
            try {
                while (!stop.stop_requested()) {
                    const auto began = Clock::now();
                    CapturedFrame frame;
                    capture_phase.store(1);
                    grab_started_ns.store(ns(Clock::now()));
                    const auto status = capture->grab(frame);
                    grab_returned_ns.store(ns(Clock::now()));
                    last_status.store(static_cast<int>(status));
                    if (status == CaptureStatus::FRAME) {
                        capture_phase.store(2);
                        const auto size = frame.bgr.total() * frame.bgr.elemSize();
                        // 20发650ms连同前后观察窗留在约16秒容量内；首帧检查整组像素内存预算。
                        if (frame.bgr.empty() || size > byte_limit / frame_limit ||
                            frames.size() >= frame_limit || size > byte_limit - bytes) {
                            failure_code.store(1); failed.store(true); break;
                        }
                        frames.push_back({frame.bgr.clone(), frame.timing, ns(Clock::now())});
                        bytes += size;
                        {
                            std::lock_guard lock(observation_mutex);
                            latest_ns.store(ns(Clock::now()));
                            if (first_ns.load() == 0) first_ns.store(latest_ns.load());
                            count.store(static_cast<int>(frames.size()));
                        }
                    } else if (status != CaptureStatus::NO_FRAME) { failure_code.store(2); failed.store(true); break; }
                    else no_frame_count.fetch_add(1);
                    capture_phase.store(0);
                    std::this_thread::sleep_until(began + std::chrono::milliseconds(16));
                }
            } catch (...) { failure_code.store(3); failed.store(true); }
        });
    }
    void finish() {
        if (thread.joinable()) { thread.request_stop(); thread.join(); }
        if (capture) capture->close();
    }
    ~Evidence() { finish(); }
    Json snapshot() const {
        std::lock_guard lock(observation_mutex);
        const auto first = first_ns.load();
        const auto latest = latest_ns.load();
        return {{"frames", count.load()}, {"failed", failed.load()}, {"failure_code", failure_code.load()},
            {"observed_at_ns", ns(Clock::now())}, {"last_frame_received_ns", latest},
            {"capture_phase", capture_phase.load()}, {"grab_started_ns", grab_started_ns.load()},
            {"grab_returned_ns", grab_returned_ns.load()}, {"no_frame_count", no_frame_count.load()},
            {"last_status", CaptureStatusName(static_cast<CaptureStatus>(last_status.load()))},
            {"first_frame_delay_ns", first ? Json(first - opened_ns) : Json(nullptr)},
            {"last_frame_age_ns", latest ? Json(ns(Clock::now()) - latest) : Json(nullptr)}};
    }
    PrerollDecision evaluate(CounterpulsePrerollGate& gate, bool cancelled) const {
        std::lock_guard lock(observation_mutex);
        return gate.evaluate(ns(Clock::now()), first_ns.load(), latest_ns.load(), count.load(), failed.load(), cancelled);
    }
    void save(const std::filesystem::path& directory) {
        finish();
        std::filesystem::create_directory(directory);
        std::ofstream csv(directory / "frames.csv");
        csv.exceptions(std::ios::badbit | std::ios::failbit);
        csv << "status,error,png,captured_at_ns,receive_ns,source_time_at_ns,source_time_valid,source_clock_uncertainty_ms,time_basis\n";
        for (std::size_t i = 0; i < frames.size(); ++i) {
            const auto& frame = frames[i];
            const auto file = "frame_" + std::to_string(i) + ".png";
            if (!cv::imwrite((directory / file).string(), frame.pixels)) throw std::runtime_error("帧保存失败");
            // captured_at保持Capture原语义，映射源时刻另列，不能暗中冒充曝光时间。
            csv << "FRAME,," << file << ',' << ns(frame.timing.captured_at) << ',' << frame.received_ns << ','
                << ns(frame.timing.source_time_at) << ',' << frame.timing.source_time_timing_valid << ','
                << frame.timing.source_clock_uncertainty_ms << ',' << CaptureBackendName(backend) << '\n';
        }
    }
};
}
#endif
