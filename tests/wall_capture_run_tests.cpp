#include "recoil_tuner/wall_capture_run.h"
#include <atomic>
#include <filesystem>
#include <iostream>
#include <thread>

namespace {
int failed = 0;
void check(bool ok, const char* message) { if (!ok) { ++failed; std::cerr << message << '\n'; } }
class FakeCapture final : public ICapture {
public:
    explicit FakeCapture(std::function<bool()> lose = {}) : lose_(std::move(lose)) {
        image_ = cv::Mat(128, 128, CV_8UC3); cv::RNG generator(1234); generator.fill(image_, cv::RNG::UNIFORM, 0, 255);
    }
    bool open() noexcept override { return true; }
    CaptureStatus grab(CapturedFrame& frame) noexcept override {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        frame.bgr = image_;
        if (lose_ && lose_()) frame.bgr = cv::Mat(128, 128, CV_8UC3, cv::Scalar(100, 100, 100));
        frame.width = frame.height = 128;
        frame.timing.sequence = ++sequence_;
        frame.timing.captured_at = std::chrono::steady_clock::now();
        return CaptureStatus::FRAME;
    }
    void close() noexcept override {}
    CaptureStatus status() const noexcept override { return CaptureStatus::READY; }
    std::string last_error() const override { return {}; }
private: std::uint64_t sequence_ = 0; cv::Mat image_; std::function<bool()> lose_;
};
class FakeDevice final : public IMouseController {
public:
    bool open() noexcept override { return true; }
    bool output_owner_exclusive() const noexcept override { return true; }
    bool supports_left_button() const noexcept override { return true; }
    bool left_button_cleanup_required() const noexcept override { return down; }
    MouseMoveReceipt move(const MouseMoveCommand&) noexcept override { ++moves; return {}; }
    bool poll_input(InputSnapshot& input) noexcept override {
        input.state_valid = true; input.status = InputMonitorStatus::READY; input.sequence = ++sequence;
        return true;
    }
    ButtonReceipt set_left_button(bool value) noexcept override {
        if (value) { ++downs; ever_down = true; } else ++ups;
        down = value;
        ButtonReceipt receipt;
        receipt.disposition = value && unknown_down ? ButtonDisposition::APPLICATION_UNKNOWN : ButtonDisposition::ACKNOWLEDGED;
        receipt.backend_completed_at = std::chrono::steady_clock::now(); receipt.cleanup_required = value;
        return receipt;
    }
    void close() noexcept override {}
    MouseStatus status() const noexcept override { return MouseStatus::READY; }
    std::string last_error() const override { return {}; }
    bool down = false, unknown_down = false;
    std::atomic<bool> ever_down{false};
    int downs = 0, ups = 0, moves = 0;
    std::uint64_t sequence = 0;
};
}
int main() {
    const auto root = std::filesystem::temp_directory_path() / ("xen-wall-run-test-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    recoil_tuner::WallRunRequest request;
    request.weapon_id = "ak47"; request.sensitivity = 1; request.duration_ms = 100;
    request.trigger_virtual_key = 5; request.cancel_virtual_key = 35;
    request.capture_factory = [](const CaptureConfig&) { return std::make_unique<FakeCapture>(); };
    request.context_valid = [] { return true; };
    std::atomic<bool> canceled{false};
    auto device = std::make_shared<FakeDevice>();
    request.mode = recoil_tuner::WallRunMode::TEST;
    request.measurement_required = false;
    int capture_requests = 0;
    request.capture_factory = [&](const CaptureConfig&) -> std::unique_ptr<ICapture> { ++capture_requests; return {}; };
    request.geometry_valid = [](const auto&) { return false; };
    request.output_directory = root / "direct-verification-without-video";
    auto direct = recoil_tuner::run_wall_capture(request, device, canceled);
    check(direct.completed && device->downs == 1 && device->ups == 1 && !device->down,
        "无测量已有曲线验证不得因图像不可用阻止单次测试与UP");
    check(capture_requests == 0 && direct.frames.empty() && !direct.report.value("training_eligible",true),
        "无测量验证不得创建Capture或提供图像训练资格");
    device=std::make_shared<FakeDevice>();device->unknown_down=true;
    request.output_directory=root/"direct-unknown-down";
    direct=recoil_tuner::run_wall_capture(request,device,canceled);
    check(!direct.completed&&device->downs==1&&device->ups==1&&!device->down,
        "无测量验证仍须UNKNOWN后的UP清理");
    device=std::make_shared<FakeDevice>();request.context_valid=[device]{return !device->ever_down;};
    request.context_block_reason=[]{return std::string("源焦点测试已丢失");};
    request.output_directory=root/"direct-focus-loss";
    direct=recoil_tuner::run_wall_capture(request,device,canceled);
    check(!direct.completed&&device->ups==1&&direct.message=="源焦点测试已丢失",
        "无测量仍保留源上下文撤销且输出具体原因");
    device=std::make_shared<FakeDevice>();request.context_valid=[]{return false;};
    request.context_block_reason=[]{return std::string("等待GSI测试状态");};
    request.output_directory=root/"direct-block-reason";
    bool specific_progress=false;
    direct=recoil_tuner::run_wall_capture(request,device,canceled,[&](const std::string& message){
        if(message=="等待GSI测试状态"){specific_progress=true;canceled=true;}
    });
    check(specific_progress&&!direct.completed&&device->downs==0&&
        direct.report.value("readiness_blocker",std::string{})=="等待GSI测试状态",
        "等待具体原因须进入progress与结果，取消不得输出");
    canceled=false;request.context_valid=[]{return true;};request.context_block_reason={};
    request.mode = recoil_tuner::WallRunMode::CAPTURE; request.measurement_required = true;
    request.capture_factory = [](const CaptureConfig&) { return std::make_unique<FakeCapture>(); };
    request.geometry_valid = {};
    device = std::make_shared<FakeDevice>();
    request.output_directory = root / "complete";
    auto result = recoil_tuner::run_wall_capture(request, device, canceled);
    check(result.completed && !result.cleanup_unknown, "正常ACK的DOWN清理债务不得当作失败");
    check(device->downs == 1 && device->ups == 1 && !device->down, "完成必须单次DOWN和UP");
    check(!result.frames.empty() && std::filesystem::is_regular_file(request.output_directory / "run.json"), "图像与报告必须落盘");

    device = std::make_shared<FakeDevice>();
    request.output_directory = root / "focus-lost";
    request.context_valid = [device] { return !device->ever_down; };
    result = recoil_tuner::run_wall_capture(request, device, canceled);
    check(!result.completed && device->downs == 1 && device->ups == 1 && !device->down, "DOWN后丢焦点必须UP并拒绝完整成功");

    device = std::make_shared<FakeDevice>(); device->unknown_down = true;
    request.output_directory = root / "unknown-down"; request.context_valid = [] { return true; };
    int accepted_signals = 0;
    request.on_firing_started = [&](auto, const auto&) { ++accepted_signals; return true; };
    result = recoil_tuner::run_wall_capture(request, device, canceled);
    check(!result.completed && device->ups == 1 && accepted_signals == 0, "未知DOWN不得提供射击信号且必须UP");

    device = std::make_shared<FakeDevice>();
    request.output_directory = root / "registration-lost";
    request.capture_factory = [device](const CaptureConfig&) { return std::make_unique<FakeCapture>([device] { return device->ever_down.load(); }); };
    result = recoil_tuner::run_wall_capture(request, device, canceled);
    check(!result.completed && device->ups == 1 && !result.report.value("training_eligible", true), "在线观测失配必须停止并排除训练");
    check(result.report.value("recovery_action", std::string{}) == "recalibrate" &&
        result.message.find("减少") == std::string::npos,
        "匹配失效必须要求重新标定，不能无依据归因于弹数过多");
    check(result.report.contains("registration_failure"), "失配报告必须保存实际判据以区分纹理、相关性和范围");
    device = std::make_shared<FakeDevice>();
    request.output_directory = root / "texture-insufficient";
    request.capture_factory = [](const CaptureConfig&) { return std::make_unique<FakeCapture>([] { return true; }); };
    result = recoil_tuner::run_wall_capture(request, device, canceled);
    check(!result.completed && result.report.value("recovery_action", std::string{}) == "recalibrate" &&
        result.message.find("纹理不足") != std::string::npos && result.message.find("减少") == std::string::npos,
        "静止无纹理背景也会失败，必须提示换靶面重新标定而不是减少弹数");
    request.capture_factory = [](const CaptureConfig&) { return std::make_unique<FakeCapture>(); };

    device = std::make_shared<FakeDevice>();
    request.output_directory = root / "overshoot";
    request.target_shots = 5; request.observed_ammo_delta = [] { return std::optional<int>{6}; };
    result = recoil_tuner::run_wall_capture(request, device, canceled);
    check(result.completed && result.report.value("overshoot", 0) == 1 && !result.report.value("training_eligible", true), "超阶段弹数必须排除训练");

    device = std::make_shared<FakeDevice>();
    request.output_directory = root / "geometry-mismatch";
    request.geometry_valid = [](const auto&) { return false; };
    result = recoil_tuner::run_wall_capture(request, device, canceled);
    check(!result.completed && device->downs == 0 && device->moves == 0, "实际几何不匹配必须在任何输出前拒绝");
    check(result.report.value("recovery_action", std::string{}) == "recalibrate", "旧标定几何不匹配必须要求重标定");
    request.geometry_valid = {};

    device = std::make_shared<FakeDevice>(); canceled.store(true);
    request.output_directory = root / "canceled";
    result = recoil_tuner::run_wall_capture(request, device, canceled);
    check(!result.completed && device->downs == 0 && device->moves == 0, "取消不得产生设备输出");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::cout << (failed ? "FAILED" : "PASS") << " wall_capture_run\n";
    return failed ? 1 : 0;
}
