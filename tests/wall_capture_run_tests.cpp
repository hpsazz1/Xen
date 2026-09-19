#include "recoil_tuner/wall_capture_run.h"
#include <atomic>
#include <opencv2/imgcodecs.hpp>
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
class RecordedCapture final : public ICapture {
public:
    RecordedCapture(cv::Mat before,cv::Mat after,std::function<bool()> fired)
        : before_(std::move(before)),after_(std::move(after)),fired_(std::move(fired)) {}
    bool open() noexcept override{return !before_.empty()&&!after_.empty();}
    CaptureStatus grab(CapturedFrame& frame) noexcept override {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        frame.bgr=fired_()?after_:before_;frame.width=frame.bgr.cols;frame.height=frame.bgr.rows;
        frame.timing.sequence=++sequence_;frame.timing.captured_at=std::chrono::steady_clock::now();
        return CaptureStatus::FRAME;
    }
    void close() noexcept override{}
    CaptureStatus status() const noexcept override{return CaptureStatus::READY;}
    std::string last_error() const override{return {};}
private:
    cv::Mat before_,after_;std::function<bool()> fired_;std::uint64_t sequence_=0;
};
class FakeDevice final : public IMouseController {
public:
    bool open() noexcept override { return true; }
    bool output_owner_exclusive() const noexcept override { return true; }
    bool supports_left_button() const noexcept override { return true; }
    bool left_button_cleanup_required() const noexcept override { return down; }
    MouseMoveReceipt move(const MouseMoveCommand&) noexcept override {
        ++moves;
        MouseMoveReceipt receipt;
        receipt.succeeded = acknowledge_moves; receipt.protocol_ack_received = acknowledge_moves;
        receipt.backend_completed_at = std::chrono::steady_clock::now();
        return receipt;
    }
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
    bool down = false, unknown_down = false, acknowledge_moves = false;
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
    check(result.report.value("search_policy",std::string{})=="real_template_midpoint_support_v1" &&
        result.report.at("search_region").is_array() && result.report.at("explicit_search_limit")==nlohmann::json({0,0}),
        "成功测量也必须保留搜索策略与真实范围，不能只按算法族冒充新策略证据");

    const auto fixtures=std::filesystem::path(__FILE__).parent_path()/"fixtures/recoil_registration";
    for(const auto* name:{"shot-a.png","shot-b.png","shot-before-a.png"}){
        device=std::make_shared<FakeDevice>();request.output_directory=root/name;
        const auto before=cv::imread((fixtures/(std::string(name)=="shot-b.png"?"reference.png":"reference-a.png")).string());
        const auto after=cv::imread((fixtures/name).string());
        request.capture_factory=[device,before,after](const CaptureConfig&){
            return std::make_unique<RecordedCapture>(before,after,[device]{return device->ever_down.load();});
        };
        const auto replay=recoil_tuner::run_wall_capture(request,device,canceled);
        check(replay.completed&&device->downs==1&&device->ups==1&&!device->down,
            "真实开枪失败图回放必须完成有界采集并释放假设备");
    }
    for(int i=0;i<3;++i){
        device=std::make_shared<FakeDevice>();request.output_directory=root/("range-"+std::to_string(i));
        const auto before=cv::imread((fixtures/("reference-range-"+std::to_string(i)+".png")).string());
        const auto after=cv::imread((fixtures/("shot-range-"+std::to_string(i)+".png")).string());
        request.capture_factory=[device,before,after](const CaptureConfig&){
            return std::make_unique<RecordedCapture>(before,after,[device]{return device->ever_down.load();});
        };
        const auto replay=recoil_tuner::run_wall_capture(request,device,canceled);
        check(replay.completed&&device->downs==1&&device->ups==1&&!device->down,
            "三发及五发真实范围失败图在线回放必须通过真实支撑搜索并完成假设备释放");
    }
    request.capture_factory=[](const CaptureConfig&){return std::make_unique<FakeCapture>();};

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
    check(result.report.value("recovery_action", std::string{}) == "retarget" &&
        result.message.find("减少") == std::string::npos && result.message.find("重新标定") == std::string::npos,
        "几何未变的匹配失效只要求重新对准，不得丢弃仍有效的counts映射或归因于弹数过多");
    check(result.report.contains("registration_failure"), "失配报告必须保存实际判据以区分纹理、相关性和范围");
    check(std::filesystem::is_regular_file(request.output_directory / "registration-failure.png") &&
        result.report.at("registration_failure").value("image_file",std::string{}) == "registration-failure.png" &&
        result.report.at("registration_failure").value("reference_file",std::string{}) == "frames/frame-0.png",
        "首枪失配即使早于保存间隔，也必须保存真正失败帧及参考帧引用");
    device = std::make_shared<FakeDevice>();
    request.output_directory = root / "texture-insufficient";
    request.capture_factory = [](const CaptureConfig&) { return std::make_unique<FakeCapture>([] { return true; }); };
    result = recoil_tuner::run_wall_capture(request, device, canceled);
    check(!result.completed && result.report.value("recovery_action", std::string{}) == "retarget" &&
        result.message.find("纹理不足") != std::string::npos && result.message.find("减少") == std::string::npos &&
        result.message.find("重新标定") == std::string::npos,
        "静止无纹理背景要求重新对准，不代表设备counts映射失效");
    check(result.report.at("registration_failure").at("raw_midpoint_residual").is_null(),
        "尚未计算的原始残差必须是null，不能伪装成零差");
    request.capture_factory = [](const CaptureConfig&) { return std::make_unique<FakeCapture>(); };

    device = std::make_shared<FakeDevice>(); device->acknowledge_moves = true;
    request.mode = recoil_tuner::WallRunMode::CALIBRATE;
    request.output_directory = root / "calibration-ack-without-image-response";
    result = recoil_tuner::run_wall_capture(request, device, canceled);
    check(!result.completed && device->moves == 1 && device->downs == 0 &&
        result.calibration.size() == 1 && result.report.at("calibration").size() == 1,
        "ACK后首步整图未变必须保留本步证据并立即停止，不得继续反向或自动补发");
    check(result.message.find("X正向") != std::string::npos &&
        result.message.find("ACK") != std::string::npos && result.message.find("前后画面完全相同") != std::string::npos &&
        result.report.value("recovery_action",std::string{}) == "recalibrate" &&
        !result.report.value("training_eligible",true),
        "标定首步未观测必须明确方向及ACK证据边界，不能宣称标定完成或有效训练");
    request.mode = recoil_tuner::WallRunMode::CAPTURE;

    device = std::make_shared<FakeDevice>();
    request.output_directory = root / "overshoot";
    request.target_shots = 5; request.observed_ammo_delta = [] { return std::optional<int>{6}; };
    result = recoil_tuner::run_wall_capture(request, device, canceled);
    check(!result.completed && result.report.value("overshoot", 0) == 1 && !result.report.value("training_eligible", true), "超阶段弹数必须报告失败并排除训练");

    // 真实Run出现3→4及3→0；不能只用曾到目标加最终overshoot==0判合格。
    for (const int final_count : {0, 2, 3, 4}) {
        device = std::make_shared<FakeDevice>();
        request.output_directory = root / ("three-shot-final-" + std::to_string(final_count));
        request.target_shots = 3;
        request.observed_ammo_delta = [device, final_count] { return std::optional<int>{device->down ? 3 : final_count}; };
        result = recoil_tuner::run_wall_capture(request, device, canceled);
        check(result.completed == (final_count == 3) && result.report.value("training_eligible", false) == (final_count == 3),
            "目标3发必须以停后GSI恰好3为准，0/2/4均失败且不可训练");
        check(device->downs == 1 && device->ups == 1, "计数失配不得自动补发或重新开火");
    }
    device = std::make_shared<FakeDevice>();
    request.output_directory = root / "three-shot-regressed-then-restored";
    int settled_reads = 0;
    request.observed_ammo_delta = [device, &settled_reads] { return std::optional<int>{device->down || ++settled_reads > 1 ? 3 : 0}; };
    result = recoil_tuner::run_wall_capture(request, device, canceled);
    check(!result.completed && !result.report.value("training_eligible", true), "停后GSI计数回退再恢复仍须拒绝，不能掩盖换弹或状态乱序");

    device = std::make_shared<FakeDevice>();
    request.output_directory = root / "three-shot-delayed-until-after-up";
    request.observed_ammo_delta = [device] { return std::optional<int>{device->down ? 2 : 3}; };
    result = recoil_tuner::run_wall_capture(request, device, canceled);
    check(result.completed && result.report.value("gsi_count_matched",false) && result.report.value("training_eligible",false),
        "最长时长先UP、停后GSI才确认3发时可按最终计数核对，不能要求按住时已到目标");
    check(result.report.value("stop_reason",std::string{}) == "duration_limit", "记录最长时长停枪而非伪称按GSI目标停枪");

    device = std::make_shared<FakeDevice>();
    request.output_directory = root / "three-shot-gsi-unavailable";
    request.observed_ammo_delta = [device]() -> std::optional<int> { return device->down ? std::optional<int>{3} : std::nullopt; };
    result = recoil_tuner::run_wall_capture(request, device, canceled);
    check(!result.completed && !result.report.value("training_eligible",true), "停后GSI缺失不能沿用按住时计数宣称合格");

    device = std::make_shared<FakeDevice>();
    request.output_directory = root / "slow-start-budget";
    request.on_firing_started = [](auto, const auto&) { std::this_thread::sleep_for(std::chrono::milliseconds(120)); return true; };
    int down_count_reads = 0;
    request.observed_ammo_delta = [device, &down_count_reads] { if(device->down)++down_count_reads; return std::optional<int>{3}; };
    result = recoil_tuner::run_wall_capture(request, device, canceled);
    check(result.completed && down_count_reads == 0 && device->downs == 1 && device->ups == 1,
        "DOWN启动回调已耗尽100ms最长时长时必须直接UP，不能重新计时");
    request.on_firing_started = {};

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
