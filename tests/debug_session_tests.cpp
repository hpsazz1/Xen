#include "debug_session/debug_session.h"
#include "runtime/runtime.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace debug_session;
using namespace std::chrono_literals;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
std::string utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
void wait_idle(Session& session) {
    const auto deadline = std::chrono::steady_clock::now() + 8s;
    while (session.busy() && std::chrono::steady_clock::now() < deadline) {
        session.poll();
        std::this_thread::sleep_for(1ms);
    }
    require(!session.busy(), "调试会话未在限时内结束");
    session.poll();
}
Json read_json(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "预期报告文件不存在");
    return Json::parse(input);
}
class FakeDevice final : public IMouseController {
public:
    std::atomic_int opens{0}, closes{0}, outputs{0}, subscriptions{0}, reads{0};
    std::atomic_bool subscribed{false}, reader_entered{false}, release_reader{true};
    bool open() noexcept override { ++opens; return true; }
    void close() noexcept override { ++closes; }
    MouseMoveReceipt move(const MouseMoveCommand&) noexcept override { ++outputs; return {}; }
    ButtonReceipt set_left_button(bool) noexcept override { ++outputs; return {}; }
    KeyboardReceipt set_wasd_keyboard(std::uint8_t) noexcept override { ++outputs; return {}; }
    KeyboardReceipt set_wasd_mask(std::uint8_t, bool) noexcept override { ++outputs; return {}; }
    KeyboardReceipt cleanup_wasd_keyboard() noexcept override { ++outputs; return {}; }
    bool poll_input(InputSnapshot& snapshot) noexcept override {
        snapshot = {}; snapshot.status = InputMonitorStatus::READY; snapshot.state_valid = true;
        return true;
    }
    bool output_owner_exclusive() const noexcept override { return true; }
    MouseStatus status() const noexcept override { return MouseStatus::READY; }
    std::string last_error() const override { return {}; }
    bool set_input_report_subscription(bool value) noexcept override {
        if (value) ++subscriptions;
        subscribed = value;
        return true;
    }
    bool freeze_input_reports() noexcept override { subscribed = false; return true; }
    bool read_input_reports(InputReportCursor& cursor, InputReportBatch& batch) noexcept override {
        reader_entered = true;
        while (!release_reader) std::this_thread::yield();
        ++reads;
        batch = {}; batch.status = InputMonitorStatus::READY; batch.epoch = 1;
        batch.subscribed = subscribed; batch.frozen = !subscribed; batch.final_sequence = 2;
        if (cursor.sequence < 2) {
            auto& event = batch.events[0];
            event.epoch = 1; event.sequence = ++cursor.sequence; cursor.epoch = 1;
            event.received_at_steady_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            event.state_valid = true; event.wasd_mask = event.sequence == 1 ? 1 : 0;
            event.datagram_size = event.raw_report.size();
            batch.count = 1;
        }
        return true;
    }
};
Context context_for(const std::shared_ptr<FakeDevice>& device = {}) {
    Context context;
    context.device = device;
    context.runtime_idle = context.input_recording_idle = context.cleanup_known = true;
    context.config.mouse.backend = MouseBackend::KMBOX_NET;
    context.config.mouse.allow_send_input = true;
    context.config.mouse.kmbox_command_timeout_ms = 50;
    return context;
}
void test_documents_and_frozen_prepare(const std::filesystem::path& root) {
    Session session;
    Request request;
    request.mode = Mode::FIRE_TEST; request.show_hud = false;
    request.output_root = utf8(root / "documents");
    auto device = std::make_shared<FakeDevice>();
    const auto context = context_for(device);
    require(session.dispatch(Action::VALIDATE, request, context), "校验请求未接收");
    wait_idle(session);
    require(session.snapshot()->state == State::COMPLETED, "有效点射参数未通过校验");
    require(!std::filesystem::exists(root / "documents"), "纯校验不应创建报告目录");

    require(session.dispatch(Action::SAVE_PLAN, request, context), "保存请求未接收");
    wait_idle(session);
    const auto saved = std::filesystem::u8path(session.snapshot()->report_directory);
    require(read_json(saved / "plan.json").at("shot_hold_ms") == 80, "保存未使用提交参数");
    require(!read_json(saved / "prepare.json").at("physical_output").get<bool>(), "保存不能授予物理输出");
    request.load_path = utf8(saved / "plan.json");
    require(session.dispatch(Action::LOAD_PLAN, request, context), "计划读取请求未接收");
    wait_idle(session);
    require(session.snapshot()->plan.at("shots") == 15, "读取计划丢失15次按住契约");

    require(session.dispatch(Action::PREPARE, request, context), "准备请求未接收");
    wait_idle(session);
    const auto first = session.snapshot();
    require(first->state == State::PREPARED && !first->prepared_id.empty(), "准备结果未发布");
    request.shot_hold_ms = 120;
    require(session.snapshot()->plan.at("shot_hold_ms") == 80, "修改草稿污染已冻结参数");
    require(!session.dispatch(Action::START, request, context, first->prepared_id, false, {}), "缺少授权仍启动了测试");
    require(!session.dispatch(Action::START, request, context_for(), first->prepared_id, true, physical_confirmation()), "没有设备仍启动了测试");
    require(!session.dispatch(Action::START, request, context, "old-id", true, physical_confirmation()), "无效准备身份被接受");
    session.cancel();
    require(!session.dispatch(Action::START, request, context, first->prepared_id, true, physical_confirmation()), "已取消准备身份仍可启动");
    require(session.dispatch(Action::PREPARE, request, context), "再次准备请求未接收");
    wait_idle(session);
    require(session.snapshot()->prepared_id != first->prepared_id && session.snapshot()->plan.at("shot_hold_ms") == 120,
        "新准备未使用独立身份和新参数");
    require(!session.dispatch(Action::START, request, context, first->prepared_id, true, physical_confirmation()), "旧准备身份复用了新准备");
    require(device->opens == 0 && device->closes == 0 && device->outputs == 0 && device->subscriptions == 0,
        "离线操作或拒绝启动触碰了设备");
    session.request_shutdown(); wait_idle(session);
}
void test_cancel_prepare(const std::filesystem::path& root) {
    Session session;
    Request request;
    request.mode = Mode::FIRE_TEST; request.show_hud = false;
    request.output_root = utf8(root / "cancel");
    const auto context = context_for();
    // 覆盖提交后立即取消与后台已获得调度的取消竞争；不存在物理设备。
    for (int i = 0; i < 32; ++i) {
        require(session.dispatch(Action::PREPARE, request, context), "竞争测试准备请求未接收");
        if (i % 2) std::this_thread::sleep_for(1ms);
        session.cancel();
        wait_idle(session);
        require(session.snapshot()->state != State::PREPARED && session.snapshot()->prepared_id.empty(),
            "取消后准备结果复活");
    }
    session.request_shutdown(); wait_idle(session);
}
void test_offline_start_uses_frozen_mode(const std::filesystem::path& root) {
    Session session;
    Request request;
    request.mode = Mode::DERIVE_DEFAULTS; request.show_hud = false;
    request.output_root = utf8(root / "offline-start");
    const auto context = context_for();
    require(session.dispatch(Action::PREPARE, request, context), "离线准备未接收");
    wait_idle(session);
    const auto id = session.snapshot()->prepared_id;
    request.mode = Mode::FIRE_TEST;
    require(session.dispatch(Action::START, request, context, id), "离线启动错误使用了后来修改的物理模式草稿");
    wait_idle(session);
    require(session.snapshot()->state == State::COMPLETED && session.snapshot()->result,
        "无设备的离线执行未完成");
    const auto directory = std::filesystem::u8path(session.snapshot()->report_directory);
    require(std::filesystem::is_regular_file(directory / "default-baseline.json"), "冻结的离线模式未实际执行");
    require(!session.dispatch(Action::START, request, context, id), "已消费的准备身份重复执行");
    session.request_shutdown(); wait_idle(session);
}
void test_recording_ownership_and_async(const std::filesystem::path& root) {
    Runtime runtime;
    Session session;
    auto device = std::make_shared<FakeDevice>();
    struct ReleaseReader { std::shared_ptr<FakeDevice> device; ~ReleaseReader() { device->release_reader = true; } } release{device};
    device->release_reader = false;
    const auto context = context_for(device);
    require(session.record_inputs(runtime, utf8(root / "record"), context), "原始记录请求未接收");
    const auto deadline = std::chrono::steady_clock::now() + 4s;
    while (!device->reader_entered && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    require(device->reader_entered, "假设备Reader未进入");
    require(!session.record_inputs(runtime, utf8(root / "duplicate"), context), "第二记录任务未被拒绝");
    Request request; request.show_hud = false;
    require(!session.dispatch(Action::VALIDATE, request, context), "记录期间接受了第二后台任务");
    // Reader仍受控阻塞，此处UI接口须返回；若退回同步实现，CTest超时会明确失败。
    for (int i = 0; i < 100; ++i) { session.poll(); require(session.snapshot()->busy, "记录未结束却提前释放busy"); }
    session.cancel();
    require(session.busy(), "取消请求不应冒充Reader已退出");
    device->release_reader = true;
    wait_idle(session);
    const auto training = runtime.snapshot().training;
    require(training && training->status == input_training::Status::STOPPED, "原始记录没有完成冻结和归档");
    require(training->received_events == 2, "冻结尾水位未完整保存两条假事件");
    require(device->subscriptions == 1 && device->opens == 0 && device->closes == 0 && device->outputs == 0,
        "记录重开设备、重复订阅或发送了输出");
    const auto archive = training->directory;
    require(session.load_inputs(runtime, archive), "离线回看请求未接收");
    wait_idle(session);
    require(runtime.snapshot().training && runtime.snapshot().training->replay_source, "未通过Runtime进入离线回看");
    require(device->subscriptions == 1 && device->outputs == 0, "离线回看触碰了设备");
    session.request_shutdown(); wait_idle(session);
}
void test_recording_and_replay_failure_status(const std::filesystem::path& root) {
    Runtime runtime;
    Session session;
    auto device = std::make_shared<FakeDevice>();
    struct ReleaseReader { std::shared_ptr<FakeDevice> device; ~ReleaseReader() { device->release_reader = true; } } release{device};
    device->release_reader = false;
    require(session.record_inputs(runtime,utf8(root / "failed-record"),context_for(device)), "失败记录请求未接收");
    const auto deadline = std::chrono::steady_clock::now() + 4s;
    while ((!device->reader_entered || !runtime.snapshot().training) && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    require(device->reader_entered, "失败记录Reader未进入");
    const auto recording = runtime.snapshot().training;
    require(recording != nullptr, "失败记录缺少状态");
    // 用本测试拥有的目录占据最终清单路径，真实归档发布必须失败。
    std::filesystem::create_directory(std::filesystem::u8path(recording->directory) / "manifest.txt");
    session.cancel();
    device->release_reader = true;
    wait_idle(session);
    require(runtime.snapshot().training->status == input_training::Status::FAILED && session.snapshot()->state == State::FAILED,
        "归档失败不能因停止按钮或后台退出显示已完成");
    require(device->opens == 0 && device->closes == 0 && device->outputs == 0, "归档失败不能触发设备输出或重开");

    const auto broken = root / "broken-replay";
    std::filesystem::create_directory(broken);
    { std::ofstream file(broken / "manifest.txt"); file << "INVALID_MANIFEST\n"; }
    require(session.load_inputs(runtime,utf8(broken)), "损坏档案回看请求未接收");
    wait_idle(session);
    require(runtime.snapshot().training->status == input_training::Status::FAILED && session.snapshot()->state == State::FAILED,
        "异步清单读取失败不能显示回看已完成");

    const auto limited = root / "limited-replay";
    std::filesystem::create_directory(limited);
    // 正式归档为二进制LF；Windows文本模式会加CR并被严格表头检查拒绝。
    { std::ofstream file(limited / "manifest.txt",std::ios::binary); file << "XEN_INPUT_TRAINING_V1\n0 0 0 0 " <<
        static_cast<int>(input_training::Status::LIMIT) << " 0 262144\n"; }
    require(session.load_inputs(runtime,utf8(limited)), "有限档案回看请求未接收");
    wait_idle(session);
    require(runtime.snapshot().training->status == input_training::Status::LIMIT && session.snapshot()->state == State::FAILED,
        "预算截断档案不能显示完整回看成功");
    session.request_shutdown(); wait_idle(session);
}

}
int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("xen-debug-session-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        test_documents_and_frozen_prepare(root);
        test_cancel_prepare(root);
        test_offline_start_uses_frozen_mode(root);
        test_recording_ownership_and_async(root);
        test_recording_and_replay_failure_status(root);
        std::filesystem::remove_all(root);
        std::cout << "调试会话冻结、拒绝、取消、异步记录及设备owner合同通过\n";
        return 0;
    } catch (const std::exception& error) {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
