#include "auto_stop_probe/debug_runner.h"
#include "auto_stop_probe/counterpulse_internal.h"
#include "auto_stop_probe/debug_resources_internal.h"
#include <atomic>
#include <thread>
#include <chrono>
#include <fstream>
#include <iostream>

namespace {
using namespace auto_stop_probe_detail;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F&& f) {
    bool rejected = false;
    try { f(); } catch (const std::exception&) { rejected = true; }
    require(rejected, "应拒绝无效请求");
}
class UntouchedDevice final : public IMouseController {
public:
    int opens = 0, closes = 0, moves = 0, polls = 0;
    bool open() noexcept override { ++opens; return true; }
    void close() noexcept override { ++closes; }
    MouseMoveReceipt move(const MouseMoveCommand&) noexcept override { ++moves; return {}; }
    bool poll_input(InputSnapshot&) noexcept override { ++polls; return false; }
    MouseStatus status() const noexcept override { return MouseStatus::CLOSED; }
    std::string last_error() const override { return {}; }
};
class LifecycleDevice final : public IMouseController {
public:
    Clock::time_point now{std::chrono::seconds(10)};
    bool wasd_subscribed = true;
    std::uint64_t epoch = 1, sequence = 0;
    std::vector<WasdEvent> history;
    std::atomic_bool input_subscribed{false}, reader_entered{false}, release_reader{false}, reader_exited{false}, source_released{false};
    int opens = 0, closes = 0, downs = 0;
    bool open() noexcept override { ++opens; return true; }
    void close() noexcept override { ++closes; }
    MouseMoveReceipt move(const MouseMoveCommand&) noexcept override { return {}; }
    bool poll_input(InputSnapshot& value) noexcept override {
        value = {}; value.status = InputMonitorStatus::READY; value.state_valid = true; value.sequence = 1; return true;
    }
    bool output_owner_exclusive() const noexcept override { return true; }
    bool supports_wasd_keyboard() const noexcept override { return true; }
    bool supports_left_button() const noexcept override { return true; }
    MouseStatus status() const noexcept override { return MouseStatus::READY; }
    std::string last_error() const override { return {}; }
    ButtonReceipt set_left_button(bool value) noexcept override {
        if (value) ++downs; now += std::chrono::microseconds(1);
        ButtonReceipt receipt; receipt.disposition = ButtonDisposition::ACKNOWLEDGED;
        receipt.protocol_ack_received_at = receipt.backend_completed_at = now; return receipt;
    }
    KeyboardReceipt set_wasd_keyboard(std::uint8_t) noexcept override {
        now += std::chrono::microseconds(1); KeyboardReceipt receipt; receipt.disposition = KeyboardDisposition::ACKNOWLEDGED;
        receipt.protocol_ack_received_at = receipt.backend_completed_at = now; return receipt;
    }
    KeyboardReceipt cleanup_wasd_keyboard() noexcept override { return set_wasd_keyboard(0); }
    bool set_wasd_event_subscription(bool enabled) noexcept override {
        // 与KMBOX生产实现相同：相同订阅值不换epoch，新订阅才清历史。
        if (wasd_subscribed == enabled) return true;
        wasd_subscribed = enabled; ++epoch; sequence = 0; history.clear(); return true;
    }
    void physical_wasd(std::uint8_t mask) {
        if (!wasd_subscribed) return;
        WasdEvent event; event.sequence = ++sequence; event.held_mask = mask; event.state_valid = true;
        history.push_back(event);
    }
    bool read_wasd_events(WasdEventCursor& cursor, WasdEventBatch& batch) noexcept override {
        batch = {}; batch.subscribed = wasd_subscribed;
        if (!wasd_subscribed) return true;
        if (cursor.epoch != epoch) { batch.gap = cursor.epoch != 0; cursor = {epoch,0}; }
        for (const auto& event : history) if (event.sequence > cursor.sequence && batch.count < batch.events.size()) {
            batch.events[batch.count++] = event; cursor.sequence = event.sequence;
        }
        return true;
    }
    bool set_input_report_subscription(bool enabled) noexcept override {
        input_subscribed = enabled;
        if (!enabled) source_released = true; // 仅Reader闭包析构走此入口，freeze保持独立。
        return true;
    }
    bool freeze_input_reports() noexcept override { input_subscribed = false; return true; }
    bool read_input_reports(InputReportCursor&, InputReportBatch& batch) noexcept override {
        reader_entered = true;
        while (!release_reader) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        batch = {}; batch.status = InputMonitorStatus::READY; batch.epoch = 1;
        batch.frozen = !input_subscribed; batch.subscribed = input_subscribed; reader_exited = true; return true;
    }
};
void test_repeat_subscription(const Json& fire) {
    LifecycleDevice device;
    device.physical_wasd(2); device.physical_wasd(0);
    const auto plan = parse_counterpulse_plan(fire);
    std::uint64_t previous_epoch = device.epoch;
    for (int group = 0; group < 2; ++group) {
        {
            DebugWasdSubscription subscription(device);
            require(subscription.start(), "每组必须建立独立订阅");
            require(device.epoch != previous_epoch, "下一组不能复用旧epoch");
            previous_epoch = device.epoch;
            const auto result = execute_counterpulse(device,plan,{},
                {[&] { return device.now; },[&](auto deadline) { device.now = std::max(device.now,deadline); }},true);
            require(result.value("success",false), "已全松的新组不能被旧WASD历史取消");
        }
        require(!device.wasd_subscribed, "每组结束必须关闭自有WASD订阅");
        device.physical_wasd(8); device.physical_wasd(0);
    }
    require(device.downs == 30 && device.opens == 0 && device.closes == 0, "两组使用同一设备且各15次按住");
}
void test_monitor_timeout(const std::filesystem::path& root) {
    auto device = std::make_shared<LifecycleDevice>();
    struct ReleaseReader { std::shared_ptr<LifecycleDevice> device; ~ReleaseReader() { device->release_reader = true; } };
    input_training::Limits limits; limits.stop_timeout_ms = 5;
    MonitorTraining training(device,root / "timeout-recording",limits);
    ReleaseReader release{device};
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    while (!device->reader_entered && Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(device->reader_entered, "生产Session Reader必须实际进入");
    bool unknown = false;
    try { finish_monitor_training(training); }
    catch (const DebugRunFailure& failure) {
        unknown = !failure.output_not_started && std::string(failure.what()).find("MONITOR_TRAINING_STOP_TIMEOUT") != std::string::npos;
    }
    require(unknown && !device->reader_exited && !device->input_subscribed,
        "Reader仍存活的停止超时必须传播unknown并保留冻结水位");
    device->release_reader = true;
    const auto completion_deadline = Clock::now() + std::chrono::seconds(2);
    while (!device->source_released && Clock::now() < completion_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    // 生产Session在run_live全部归档写入之后才清空Reader闭包，不能只等read返回。
    require(device->reader_exited && device->source_released,
        "生产Session必须结束归档并释放Reader闭包后才能清理测试目录");
}
}
int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("xen-debug-runner-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        const auto fire = make_fire_test_plan({{"shot_hold_ms",80},{"fire_interval_ms",800}});
        const auto plan = parse_counterpulse_plan(fire);
        require(plan.baseline == "stationary" && !plan.capture_enabled && plan.shots == 15 &&
            plan.fire_delay_ms == 1 && plan.shot_hold_ms == 80 && plan.fire_interval_ms == 800,
            "原地射击必须保留15次按住及UP ACK等待");
        rejects([] { make_fire_test_plan({{"shot_hold_ms",80.0},{"fire_interval_ms",800}}); });
        rejects([] { make_fire_test_plan({{"shot_hold_ms",80},{"fire_interval_ms",80}}); });
        rejects([] { make_fire_test_plan({{"shot_hold_ms",80},{"fire_interval_ms",800},{"extra",0}}); });
        (void)make_fire_test_plan({{"shot_hold_ms",2000},{"fire_interval_ms",2100}});
        rejects([] { make_fire_test_plan({{"shot_hold_ms",2000},{"fire_interval_ms",5000}}); });
        DebugRunRequest request;
        request.plan = fire; request.output = root;
        const auto device = std::make_shared<UntouchedDevice>();
        request.device = device;
        rejects([&] { run_debug(request); });
        require(device->opens == 0 && device->closes == 0 && device->moves == 0 && device->polls == 0,
            "无授权请求不能触碰注入设备生命周期或输入");
        require(!std::filesystem::exists(root), "缺少物理授权不得创建执行目录");
        request.mode = DebugRunMode::DeriveDefaults;
        rejects([&] { run_debug(request,{[] { return true; },{}}); });
        require(!std::filesystem::exists(root), "开始前取消不得生成产物");
        const auto baseline = run_debug(request);
        test_repeat_subscription(fire);
        test_monitor_timeout(root);
        require(baseline.at("settings_source") == "REFERENCE_INITIAL_ASSUMPTIONS" &&
            std::filesystem::is_regular_file(root / "plan.json"), "默认计划必须同核生成且不需要设备");
        rejects([&] { run_debug(request); });
        DebugRunRequest source_request;
        source_request.plan = fire; source_request.output = root / "missing-source";
        source_request.device = device; source_request.allow_physical_output = true;
        source_request.confirmation = "AUTO_STOP_COUNTERPULSE";
        source_request.config.mouse.backend = MouseBackend::KMBOX_NET;
        source_request.config.mouse.kmbox_command_timeout_ms = 300;
        source_request.config.mouse.kmbox_connect_timeout_ms = 5000;
        source_request.config.source_context.host.clear();
        bool missing_source = false;
        try { (void)run_debug(source_request); }
        catch (const DebugRunFailure& error) {
            missing_source = error.output_not_started && std::string(error.what()) ==
                "源焦点配置缺失或不可用：SOURCE_CONFIGURATION_MISSING [SOURCE_CONFIGURATION]";
        }
        require(missing_source, "已连接300/5000ms配置不能被旧超时门槛拒绝，缺源配置须明确标记尚未开始");
        require(device->opens == 0 && device->closes == 0 && device->moves == 0 && device->polls == 0,
            "源配置失败之前不能调用注入设备");
        { std::ifstream file(source_request.output / "failure.json"); const auto failure = Json::parse(file);
          require(failure["reason"] == "SOURCE_CONFIGURATION_MISSING" && failure["stage"] == "SOURCE_CONFIGURATION" &&
              failure["output_not_started"] == true && failure["execution_entered"] == false,
              "未开始失败必须保存具体阶段和原因"); }
        source_request.output = root / "bad-plan"; source_request.plan = {{"unknown",1}};
        rejects([&] { (void)run_debug(source_request); });
        require(!std::filesystem::exists(source_request.output), "坏计划仍不得创建目录");
        source_request.mode = DebugRunMode::ManualRecording; source_request.allow_physical_output = false;
        source_request.confirmation.clear(); source_request.output = root / "bad-manual";
        bool manual_unknown = false;
        try { (void)run_debug(source_request); } catch (const DebugRunFailure& error) { manual_unknown = !error.output_not_started; }
        require(manual_unknown, "人工记录失败不能借用物理未开始标记解除设备锁存");
        std::filesystem::remove_all(root);
        std::cout << "原生调试计划、边界和离线入口通过\n";
        return 0;
    } catch (const std::exception& error) {
        if (std::filesystem::exists(root)) std::filesystem::remove_all(root);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
