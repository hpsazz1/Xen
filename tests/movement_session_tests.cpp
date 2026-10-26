#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef ERROR
#undef ERROR
#endif
#include "debug_session/debug_session.h"
#include "movement/timeline.h"
#include "source_context/source_context_internal.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
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
    require(!session.busy(), "身法会话未在限时内结束");
    session.poll();
}
Json read_json(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    require(static_cast<bool>(file), "预期方案文件不存在");
    return Json::parse(file);
}
// 只报告能力与所有权；本文件所有路径都必须在调用输出前失败或结束。
class FakeDevice : public IMouseController {
public:
    std::atomic_int touches{0};
    bool open() noexcept override { ++touches; return true; }
    void close() noexcept override { ++touches; }
    MouseMoveReceipt move(const MouseMoveCommand&) noexcept override { ++touches; return {}; }
    ButtonReceipt set_left_button(bool) noexcept override { ++touches; return {}; }
    KeyboardReceipt set_wasd_keyboard(std::uint8_t) noexcept override { ++touches; return {}; }
    KeyboardReceipt set_wasd_mask(std::uint8_t, bool) noexcept override { ++touches; return {}; }
    KeyboardReceipt set_space_key(bool) noexcept override { ++touches; return {}; }
    KeyboardReceipt set_left_ctrl_key(bool) noexcept override { ++touches; return {}; }
    KeyboardReceipt cleanup_wasd_keyboard() noexcept override { ++touches; return {}; }
    bool poll_input(InputSnapshot&) noexcept override { ++touches; return false; }
    bool set_input_report_subscription(bool) noexcept override { ++touches; return false; }
    bool set_movement_report_subscription(bool) noexcept override { ++touches; return false; }
    bool supports_wasd_keyboard() const noexcept override { return true; }
    bool supports_left_ctrl_key() const noexcept override { return true; }
    bool output_owner_exclusive() const noexcept override { return true; }
    MouseStatus status() const noexcept override { return MouseStatus::READY; }
    std::string last_error() const override { return {}; }
};
// 合成源协议端仅绑定回环；不查询真实窗口，不创建生产SourceContextServer。
class FocusFixture final {
public:
    source_context::SourceContextConfig config;
    std::atomic_bool foreground{true}, failed{false};
    FocusFixture() {
        config.enabled = true; config.host = "127.0.0.1";
        config.process_name = "fixture.exe"; config.token = std::string(32, 't'); config.ttl_ms = 500;
        socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        require(socket_ != INVALID_SOCKET, "回环fixture建socket失败");
        sockaddr_in address{}; address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        const bool bound = bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
        int size = sizeof(address);
        if (!bound || getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
            closesocket(socket_); throw std::runtime_error("回环fixture绑定失败");
        }
        config.port = ntohs(address.sin_port);
        worker_ = std::thread([this] { serve(); });
    }
    ~FocusFixture() {
        stop_ = true;
        if (worker_.joinable()) worker_.join();
        closesocket(socket_);
    }
private:
    void serve() noexcept {
        try {
            std::uint64_t sequence = 0;
            while (!stop_) {
                fd_set readers; FD_ZERO(&readers); FD_SET(socket_, &readers);
                timeval timeout{0, 10000};
                if (select(0, &readers, nullptr, nullptr, &timeout) <= 0) continue;
                source_context::detail::Packet packet{};
                sockaddr_in peer{}; int length = sizeof(peer);
                const int size = recvfrom(socket_, reinterpret_cast<char*>(packet.data()), static_cast<int>(packet.size()),
                    0, reinterpret_cast<sockaddr*>(&peer), &length);
                source_context::detail::Message message;
                require(size == static_cast<int>(packet.size()) &&
                    source_context::detail::decode(packet, config.token, message), "回环请求鉴权失败");
                message.response = true; message.session = 11; message.sequence = ++sequence;
                message.focus = foreground ? source_context::detail::Focus::Foreground : source_context::detail::Focus::Background;
                require(source_context::detail::encode(message, config.token, packet), "回环响应编码失败");
                require(sendto(socket_, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()),
                    0, reinterpret_cast<sockaddr*>(&peer), length) == static_cast<int>(packet.size()), "回环响应发送失败");
            }
        } catch (...) { failed = true; }
    }
    SOCKET socket_ = INVALID_SOCKET;
    std::atomic_bool stop_{false};
    std::thread worker_;
};
class RecordingDevice final : public FakeDevice {
public:
    FocusFixture* revoke_on_down = nullptr;
    std::atomic_int downs{0}, cleanups{0}, last_down_mask{0};
    std::atomic_int single_press{0};
    bool poll_input(InputSnapshot& input) noexcept override {
        input = {}; input.status = InputMonitorStatus::READY; input.state_valid = true;
        if (const auto key = single_press.exchange(0); key > 0 && key < 256)
            input.virtual_keys[static_cast<std::size_t>(key)] = true;
        return true;
    }
    KeyboardReceipt set_wasd_keyboard(std::uint8_t mask) noexcept override {
        if (mask) { last_down_mask = mask; ++downs; if (revoke_on_down) revoke_on_down->foreground = false; }
        KeyboardReceipt receipt; receipt.disposition = KeyboardDisposition::ACKNOWLEDGED; return receipt;
    }
    KeyboardReceipt cleanup_wasd_keyboard() noexcept override {
        ++cleanups; return set_wasd_keyboard(0);
    }
};
// 只提供录制事实；所有物理输出方法仍继承计数fake，任何调用都将使断言失败。
class TraceDevice final : public FakeDevice {
public:
    bool incomplete = false;
    std::atomic_int begins{0}, ends{0}, delivered{0};
    bool begin_movement_trace() noexcept override { ++begins; delivered = 0; return true; }
    void end_movement_trace() noexcept override { ++ends; }
    bool read_movement_trace(MovementTraceBatch& batch) noexcept override {
        batch = {};
        const int step = delivered.load();
        if (step >= 7) return true;
        MovementTraceEvent event;
        event.at_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        event.completed_ns = event.at_ns;
        event.acknowledged = true;
        if (step == 0 || step == 1 || step == 6) {
            event.kind = MovementTraceEvent::Kind::INPUT;
            event.input.state_valid = true;
            event.input.received_at_steady_ns = event.at_ns;
            event.baseline = step == 0;
            event.input.wasd_mask = step == 1 ? 1 : 0;
        } else if (step == 2 || step == 5) {
            event.kind = MovementTraceEvent::Kind::MASK;
            event.wasd = 1; event.masked = step == 2;
        } else {
            event.kind = MovementTraceEvent::Kind::KEYBOARD;
            event.wasd = step == 3 ? 2 : 0;
        }
        try { batch.events.push_back(event); } catch (...) { return false; }
        batch.gap = incomplete && step == 2;
        ++delivered;
        return true;
    }
};
Context context_for(const std::shared_ptr<FakeDevice>& device = {}) {
    Context context;
    context.device = device;
    context.runtime_idle = context.input_recording_idle = context.cleanup_known = true;
    context.config.mouse.backend = MouseBackend::KMBOX_NET;
    context.config.mouse.allow_send_input = true;
    context.config.source_context.enabled = false;
    context.config.keyboard.debug_test_enabled = true;
    context.config.keyboard.debug_test_virtual_keys = {0x76};
    return context;
}
Json example_plan() {
    movement::timeline::Plan plan;
    plan.name = "并行身法测试";
    plan.map = "本地训练";
    plan.location = "测试起点";
    plan.notes = "仅验证会话，不发送设备输入";
    plan.actions = {
        {movement::timeline::ActionKind::KEY, movement::timeline::Key::W, 0, 200},
        {movement::timeline::ActionKind::KEY, movement::timeline::Key::SPACE, 100, 30},
        {movement::timeline::ActionKind::KEY, movement::timeline::Key::A, 100, 150},
        {movement::timeline::ActionKind::TURN, movement::timeline::Key::W, 100, 150, -25.0}
    };
    return movement::timeline::plan_json(plan);
}
Request request_for(const std::filesystem::path& root) {
    Request request;
    request.mode = Mode::MOVEMENT_TEST;
    request.show_hud = false;
    request.output_root = utf8(root);
    request.movement_library_root = utf8(root / "movement-library");
    request.plan_text = example_plan().dump();
    // 其他页的损坏草稿不应影响身法校验或准备。
    request.sampling_text = "不是采样JSON";
    return request;
}
void submit(Session& session, Action action, const Request& request, const Context& context, State expected) {
    require(session.dispatch(action, request, context), "会话未接受后台请求");
    wait_idle(session);
    require(session.snapshot()->state == expected, "会话结果状态不符合预期");
}
std::size_t file_count(const std::filesystem::path& root) {
    if (!std::filesystem::exists(root)) return 0;
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
        if (entry.is_regular_file()) ++count;
    return count;
}
void test_documents(const std::filesystem::path& root) {
    Session session;
    auto request = request_for(root / "documents");
    const auto context = context_for();
    const auto files_before_validate = file_count(std::filesystem::u8path(request.output_root));
    submit(session, Action::VALIDATE, request, context, State::COMPLETED);
    require(file_count(std::filesystem::u8path(request.output_root)) == files_before_validate, "校验创建了持久化文件");
    require(session.snapshot()->plan == example_plan() && session.snapshot()->sampling.empty(),
        "校验丢失并行动作或混入采样参数");
    submit(session, Action::SAVE_PLAN, request, context, State::COMPLETED);
    const auto saved = session.snapshot()->movement_selected_path;
    require(!saved.empty() && read_json(std::filesystem::u8path(saved)) == example_plan(), "独立文件没有保存完整方案");
    require(!session.snapshot()->movement_test_enabled && !session.snapshot()->repeat_ready, "保存产生输出许可");
    const auto catalog = session.snapshot();
    require(std::any_of(catalog->movement_saved_plans.begin(), catalog->movement_saved_plans.end(),
        [&](const auto& item) { return item.path == saved; }), "保存后未刷新方案目录");
    request.load_path = saved;
    const auto saved_count = file_count(std::filesystem::u8path(request.movement_library_root));
    auto edited = example_plan(); edited["name"] = "同一方案改参";
    request.plan_text = edited.dump();
    submit(session, Action::SAVE_PLAN, request, context, State::COMPLETED);
    require(session.snapshot()->movement_selected_path == saved && read_json(std::filesystem::u8path(saved)) == edited &&
        file_count(std::filesystem::u8path(request.movement_library_root)) == saved_count, "保存当前方案新建了文件");
    request.movement_save_as = true;
    submit(session, Action::SAVE_PLAN, request, context, State::COMPLETED);
    require(session.snapshot()->movement_selected_path != saved &&
        file_count(std::filesystem::u8path(request.movement_library_root)) == saved_count + 1, "另存未生成独立方案");
    request.movement_save_as = false;
    request.plan_text = example_plan().dump();
    submit(session, Action::SAVE_PLAN, request, context, State::COMPLETED);
    submit(session, Action::LOAD_PLAN, request, context, State::COMPLETED);
    const auto imported = session.snapshot();
    require(imported->draft_plan == example_plan() && imported->draft_plan_mode == Mode::MOVEMENT_TEST &&
        imported->draft_plan_revision == 1, "导入没有标记独立身法草稿");
    request.mode = Mode::COUNTERPULSE;
    submit(session, Action::LOAD_PLAN, request, context, State::FAILED);
    require(session.snapshot()->draft_plan == imported->draft_plan &&
        session.snapshot()->draft_plan_revision == imported->draft_plan_revision,
        "急停页错误导入身法方案覆盖了草稿");
    request.plan_text.clear(); request.sampling_text.clear();
    submit(session, Action::SAVE_PLAN, request, context, State::COMPLETED);
    const auto counter_directory = std::filesystem::u8path(session.snapshot()->report_directory);
    require(session.snapshot()->plan.at("baseline") == "counter", "原急停默认计划被身法修改");
    request.mode = Mode::MOVEMENT_TEST;
    request.load_path = utf8(counter_directory / "plan.json");
    submit(session, Action::LOAD_PLAN, request, context, State::FAILED);
    require(session.snapshot()->draft_plan == imported->draft_plan, "身法页接受了急停文档");
    request = request_for(root / "documents");
    submit(session, Action::PREPARE, request, context, State::PREPARED);
    require(!session.snapshot()->repeat_ready && !session.snapshot()->prepared_id.empty(),
        "无设备准备应保留审阅身份但不开放快捷键");
    const auto prepared_directory = std::filesystem::u8path(session.snapshot()->report_directory);
    require(std::filesystem::exists(prepared_directory / "movement-run-plan.json") &&
        !std::filesystem::exists(prepared_directory / "plan.json"), "准备报告被保存为可载入方案");
    session.cancel();
    require(session.snapshot()->state == State::CANCELED && session.snapshot()->prepared_id.empty(),
        "取消未撤销准备身份");
}
void test_invalid_plans(const std::filesystem::path& root) {
    Session session;
    auto request = request_for(root / "invalid");
    const auto check = [&](Json plan) {
        request.plan_text = plan.dump();
        submit(session, Action::VALIDATE, request, context_for(), State::FAILED);
        require(session.snapshot()->prepared_id.empty() && !session.snapshot()->repeat_ready,
            "无效方案留下准备身份");
    };
    auto plan = example_plan(); plan["actions"][0]["duration_ms"] = 0; check(plan);
    plan = example_plan(); plan["actions"][0]["start_ms"] = -1; check(plan);
    plan = example_plan(); plan["actions"][0]["start_ms"] = 59999;
    plan["actions"][0]["duration_ms"] = 2; check(plan);
    plan = example_plan(); plan["actions"].push_back(plan["actions"][3]); check(plan);
    plan = example_plan(); plan["mask"]["wasd"] = 16; check(plan);
}
void test_frozen_and_missing_source(const std::filesystem::path& root) {
    Session session;
    auto device = std::make_shared<FakeDevice>();
    auto context = context_for(device);
    auto request = request_for(root / "frozen");
    submit(session, Action::PREPARE, request, context, State::PREPARED);
    const auto prepared = session.snapshot();
    require(prepared->repeat_ready, "合成就绪设备未生成快捷键模板");
    auto changed_keys = context;
    changed_keys.config.keyboard.emergency_virtual_keys = {0x24};
    require(!session.repeat(changed_keys) && device->touches == 0,
        "准备后改变急停键仍允许快捷键执行或触碰设备");
    request.plan_text = "修改中的无效草稿";
    require(session.snapshot()->plan == example_plan(), "草稿污染冻结参数");
    require(!session.dispatch(Action::START, request, context, prepared->prepared_id), "缺少许可仍启动");
    require(!session.dispatch(Action::START, request, context, "旧身份", true, physical_confirmation()),
        "旧身份仍启动");
    require(session.dispatch(Action::START, request, context, prepared->prepared_id, true, physical_confirmation()),
        "有效准备启动未接受");
    wait_idle(session);
    require(session.snapshot()->state == State::FAILED && !session.snapshot()->cleanup_unknown &&
        session.snapshot()->message.find("源焦点") != std::string::npos,
        "缺少源配置应在输出前失败并保留清理已知");
    require(!session.snapshot()->repeat_ready && !session.repeat(context), "失败后快捷键许可未撤销");
    require(!session.dispatch(Action::START, request, context, prepared->prepared_id, true, physical_confirmation()),
        "已消费准备身份被重复使用");

    request = request_for(root / "frozen");
    submit(session, Action::PREPARE, request, context, State::PREPARED);
    const auto repeat_parent = session.snapshot()->prepared_id;
    request.plan_text = "快捷键必须忽略未提交草稿";
    require(session.repeat(context), "冻结模板快捷键未接受");
    wait_idle(session);
    const auto repeated = session.snapshot();
    const auto directory = std::filesystem::u8path(repeated->report_directory);
    require(read_json(directory / "movement-run-plan.json") == example_plan() &&
        read_json(directory / "prepare.json").at("repeat_of") == repeat_parent,
        "快捷键没有复制冻结方案与来源身份");
    require(repeated->state == State::FAILED && !repeated->cleanup_unknown,
        "快捷键缺源失败误报清理未知");
    require(device->touches == 0, "准备或缺源路径触碰设备输入输出");
}
void test_cancel_prepare(const std::filesystem::path& root) {
    Session session;
    const auto request = request_for(root / "cancel");
    for (int i = 0; i < 4; ++i) {
        require(session.dispatch(Action::PREPARE, request, context_for()), "准备取消测试未接受");
        if (i % 2) std::this_thread::sleep_for(1ms);
        session.cancel();
        wait_idle(session);
        require(session.snapshot()->state != State::PREPARED && session.snapshot()->prepared_id.empty() &&
            !session.snapshot()->repeat_ready, "取消后后台复活了准备或快捷键许可");
    }
}
void test_source_lifecycle(const std::filesystem::path& root, bool revoke) {
    FocusFixture focus;
    auto device = std::make_shared<RecordingDevice>();
    if (revoke) device->revoke_on_down = &focus;
    auto context = context_for(device);
    context.config.source_context = focus.config;
    auto request = request_for(root / (revoke ? "focus-revoked" : "focus-success"));
    movement::timeline::Plan plan;
    plan.name = "源焦点生命周期合成测试"; plan.mask.enabled = false;
    plan.actions = {{movement::timeline::ActionKind::KEY, movement::timeline::Key::W, 0, revoke ? 2000 : 300}};
    request.plan_text = movement::timeline::plan_json(plan).dump();
    Session session;
    require(session.dispatch(Action::ENABLE_MOVEMENT_TEST, request, context) && session.snapshot()->movement_test_enabled,
        "源焦点生命周期启用失败");
    require(session.repeat(context), "源焦点生命周期快捷键启动失败");
    wait_idle(session);
    const auto result = session.snapshot();
    require(!focus.failed && result->state == (revoke ? State::CANCELED : State::COMPLETED) &&
        !result->cleanup_unknown && result->result && result->result->at("cleanup_known").get<bool>(),
        "源焦点完成或撤销结果不正确");
    require(result->result->at("success").get<bool>() == !revoke &&
        result->result->at("canceled").get<bool>() == revoke &&
        !result->result->at("physical_validation_passed").get<bool>(), "合成结果被写成真实验收");
    require(device->downs == 1 && device->cleanups == 1 && device->touches == 0,
        "身法生命周期没有恰好按下与清理一次，或误触其他设备方法");
    if (revoke) {
        require(!result->movement_test_enabled && !result->repeat_ready && !session.repeat(context) &&
            !session.update_movement_draft(request, context), "失焦取消仍保留会话或快捷键重复许可");
        require(device->downs == 1, "失焦后重复快捷键产生额外按下");
    } else require(result->movement_test_enabled && result->repeat_ready, "正常完成错误撤销身法会话");
}
void test_edit_while_running(const std::filesystem::path& root) {
    FocusFixture focus;
    auto device = std::make_shared<RecordingDevice>();
    auto context = context_for(device);
    context.config.source_context = focus.config;
    auto request = request_for(root / "edit-while-running");
    movement::timeline::Plan plan;
    plan.name = "运行中编辑"; plan.mask.enabled = false;
    plan.actions = {{movement::timeline::ActionKind::KEY, movement::timeline::Key::W, 0, 1500}};
    const auto original = movement::timeline::plan_json(plan);
    request.plan_text = original.dump();
    Session session;
    require(session.dispatch(Action::ENABLE_MOVEMENT_TEST, request, context) &&
        session.dispatch(Action::RUN_MOVEMENT_TEST, request, context), "运行中编辑测试未启动");
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (device->downs.load() == 0 && session.busy() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    require(device->downs == 1 && session.busy(), "未观察到长动作开始");
    plan.actions[0].key = movement::timeline::Key::A;
    plan.actions[0].duration_ms = 50;
    request.plan_text = movement::timeline::plan_json(plan).dump();
    require(session.update_movement_draft(request, context), "运行中无法更新下一次草稿");
    require(!session.repeat(context) && !session.dispatch(Action::RUN_MOVEMENT_TEST, request, context),
        "运行中接受了额外触发");
    wait_idle(session);
    auto result = session.snapshot();
    require(result->state == State::COMPLETED && result->movement_test_enabled &&
        read_json(std::filesystem::u8path(result->report_directory) / "movement-run-plan.json") == original &&
        device->downs == 1 && device->cleanups == 1 && device->last_down_mask == 1 && !session.busy(),
        "当前执行被新草稿改变或繁忙触发进入队列");
    require(session.repeat(context), "运行期间更新的草稿未供下一次使用");
    wait_idle(session);
    result = session.snapshot();
    require(result->state == State::COMPLETED &&
        read_json(std::filesystem::u8path(result->report_directory) / "movement-run-plan.json") == movement::timeline::plan_json(plan) &&
        device->downs == 2 && device->cleanups == 2 && device->last_down_mask == 2,
        "下一次执行未采用新草稿");
    require(!focus.failed && device->touches == 0, "运行中编辑测试误触设备操作或源fixture失败");
}
void test_enabled_edit_and_cancel(const std::filesystem::path& root) {
    FocusFixture focus;
    auto device = std::make_shared<RecordingDevice>();
    auto context = context_for(device);
    context.config.source_context = focus.config;
    auto request = request_for(root / "enabled-edit");
    movement::timeline::Plan plan;
    plan.name = "连续编辑测试"; plan.mask.enabled = false;
    plan.actions = {{movement::timeline::ActionKind::KEY, movement::timeline::Key::W, 0, 40}};
    request.plan_text = movement::timeline::plan_json(plan).dump();
    Session session;
    require(!session.dispatch(Action::RUN_MOVEMENT_TEST, request, context) && device->downs == 0,
        "未启用时直接测试仍产生按下");
    require(session.dispatch(Action::ENABLE_MOVEMENT_TEST, request, context) &&
        session.snapshot()->movement_test_enabled && session.snapshot()->repeat_ready,
        "一次启用没有建立身法测试会话");
    submit(session, Action::SAVE_PLAN, request, context, State::COMPLETED);
    require(session.snapshot()->movement_test_enabled && session.snapshot()->repeat_ready,
        "保存方案错误撤销已启用的会话");
    request.load_path = session.snapshot()->movement_selected_path;
    const auto saved_path = request.load_path;
    const auto saved_plan = read_json(std::filesystem::u8path(saved_path));
    const auto library_count = file_count(std::filesystem::u8path(request.movement_library_root));
    require(session.dispatch(Action::RUN_MOVEMENT_TEST, request, context), "启用后按钮直测失败");
    wait_idle(session);
    require(session.snapshot()->state == State::COMPLETED && session.snapshot()->movement_test_enabled &&
        device->downs == 1 && device->cleanups == 1, "完成后没有保留会话或清理次数错误");

    plan.actions[0].key = movement::timeline::Key::A;
    plan.actions[0].duration_ms = 75;
    request.plan_text = movement::timeline::plan_json(plan).dump();
    require(session.update_movement_draft(request, context) && session.repeat(context),
        "改参后必须重新启用才能热键测试");
    wait_idle(session);
    const auto second = session.snapshot();
    require(second->state == State::COMPLETED && second->movement_test_enabled &&
        read_json(std::filesystem::u8path(second->report_directory) / "movement-run-plan.json") == movement::timeline::plan_json(plan) &&
        device->downs == 2 && device->cleanups == 2, "第二次测试未采用当前草稿或未清理");

    require(file_count(std::filesystem::u8path(request.movement_library_root)) == library_count &&
        read_json(std::filesystem::u8path(saved_path)) == saved_plan &&
        session.snapshot()->movement_selected_path == saved_path &&
        !std::filesystem::exists(std::filesystem::u8path(second->report_directory) / "plan.json"),
        "编辑或测试持久化了草稿或改变了当前方案");
    request.plan_text = "无效草稿";
    require(!session.update_movement_draft(request, context) && session.snapshot()->movement_test_enabled &&
        !session.snapshot()->repeat_ready && !session.repeat(context) &&
        !session.dispatch(Action::RUN_MOVEMENT_TEST, request, context) && device->downs == 2,
        "无效草稿仍可执行上一版有效方案");
    plan.actions[0].duration_ms = 55;
    request.plan_text = movement::timeline::plan_json(plan).dump();
    require(session.dispatch(Action::RUN_MOVEMENT_TEST, request, context), "修正草稿后要求重复启用");
    wait_idle(session);
    require(session.snapshot()->state == State::COMPLETED && device->downs == 3 && device->cleanups == 3,
        "修正后的直接测试失败");
    session.cancel();
    require(!session.snapshot()->movement_test_enabled && !session.snapshot()->repeat_ready &&
        !session.update_movement_draft(request, context) && !session.repeat(context) &&
        !session.dispatch(Action::RUN_MOVEMENT_TEST, request, context) && device->downs == 3,
        "取消后草稿更新或启动复活了输出许可");

    require(session.dispatch(Action::ENABLE_MOVEMENT_TEST, request, context), "明确重新启用失败");
    auto changed_context = context;
    changed_context.config.keyboard.emergency_virtual_keys = {0x24};
    require(!session.update_movement_draft(request, changed_context) && !session.snapshot()->movement_test_enabled &&
        !session.repeat(context) && device->downs == 3, "急停键变化未撤销身法会话");
    require(!focus.failed && device->touches == 0, "会话测试误触非方案设备操作或源fixture失败");
}
bool finish_voice(Session& session, const Context& context, bool current = true) {
    const auto deadline = std::chrono::steady_clock::now() + 8s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (const auto result = session.finish_voice(context, current)) return *result;
        std::this_thread::sleep_for(1ms);
    }
    throw std::runtime_error("语音载入未在限时内结束");
}
void test_voice_selection(const std::filesystem::path& root) {
    auto device = std::make_shared<FakeDevice>();
    auto context = context_for(device);
    context.map.valid = true; context.map.map_name = "de_dust2"; context.map.context_epoch = 11;
    auto request = request_for(root / ("voice-selection-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())));
    auto first = example_plan(); first["map"] = context.map.map_name; first["voice_command"] = "跳箱";
    request.plan_text = first.dump();
    Session session;
    submit(session, Action::SAVE_PLAN, request, context, State::COMPLETED);
    const auto first_path = session.snapshot()->movement_selected_path;
    require(session.snapshot()->movement_current == first && session.snapshot()->movement_baseline == first,
        "手动保存未建立内容基线");
    auto second = first; second["name"] = "第二方案"; second["voice_command"] = "转身";
    second["actions"][0]["duration_ms"] = 220;
    request.plan_text = second.dump(); request.movement_save_as = true;
    submit(session, Action::SAVE_PLAN, request, context, State::COMPLETED);
    const auto second_path = session.snapshot()->movement_selected_path;
    request.movement_save_as = false; request.load_path = first_path;
    require(!session.select_voice(request, context), "未启用测试不得接受语音选择");
    require(session.dispatch(Action::ENABLE_MOVEMENT_TEST, request, context), "语音测试启用失败");
    session.update_map(context.map);
    require(session.select_voice(request, context), "干净已保存草稿应接受语音选择");
    require(!session.select_voice(request, context) && !session.repeat(context), "载入期间不得排队第二选择或执行");
    require(finish_voice(session, context), "页面关闭时语音选择未成功");
    auto snapshot = session.snapshot();
    require(snapshot->repeat_ready && snapshot->movement_test_enabled && snapshot->movement_voice_selected &&
        snapshot->draft_plan == first && snapshot->movement_current == first && snapshot->movement_baseline == first &&
        snapshot->movement_selected_path == first_path, "语音成功未原子发布草稿、基线与热键模板");
    request.load_path = second_path;
    require(session.select_voice(request, context) && finish_voice(session, context), "语音载入后错误保持脏状态");
    const auto selected = session.snapshot()->draft_plan;
    request.load_path = first_path;
    require(session.select_voice(request, context), "换图拒绝fixture未开始");
    ++context.map.context_epoch; context.map.map_name = "de_mirage";
    session.update_map(context.map);
    require(!finish_voice(session, context) && session.snapshot()->draft_plan == selected &&
        session.snapshot()->movement_test_enabled && !session.snapshot()->repeat_ready && !session.repeat(context),
        "换图迟到载入覆盖草稿、关闭测试或旧语音模板仍可执行");
    auto manual = second; manual["notes"] = "手动接管"; request.plan_text = manual.dump();
    require(session.update_movement_draft(request, context) && !session.snapshot()->movement_voice_selected &&
        session.snapshot()->repeat_ready, "实际编辑不能手动接管并恢复模板");
    require(!session.select_voice(request, context), "未保存编辑不得被语音覆盖");
    request.load_path = second_path;
    submit(session, Action::SAVE_PLAN, request, context, State::COMPLETED);
    require(session.snapshot()->movement_baseline == manual, "手动保存未清除脏状态");
    context.map.map_name = "de_dust2"; ++context.map.context_epoch; session.update_map(context.map);
    request.load_path = first_path;
    require(session.select_voice(request, context), "编辑竞态fixture未开始");
    auto edited = manual; edited["notes"] = "载入期间新编辑"; request.plan_text = edited.dump();
    require(session.update_movement_draft(request, context), "载入期间手动编辑未接管");
    require(!finish_voice(session, context) && session.snapshot()->movement_current == edited,
        "迟到语音覆盖新编辑");
    request.plan_text = manual.dump(); session.update_movement_draft(request, context);
    require(session.select_voice(request, context), "急停竞态fixture未开始");
    session.cancel("语音回归急停");
    require(!finish_voice(session, context) && !session.snapshot()->movement_test_enabled &&
        !session.snapshot()->repeat_ready && session.snapshot()->movement_current == manual,
        "急停后迟到语音回写或恢复许可");
    require(session.dispatch(Action::ENABLE_MOVEMENT_TEST, request, context), "重启测试失败");
    require(session.select_voice(request, context) && !finish_voice(session, context, false),
        "已取消目录上下文仍提交语音载入");
    auto denied = context; denied.runtime_idle = false;
    require(!session.select_voice(request, denied), "Runtime繁忙仍接受语音选择");
    denied = context; denied.input_recording_idle = false;
    require(!session.select_voice(request, denied), "输入录制中仍接受语音选择");
    const auto before_device_change = session.snapshot();
    require(session.select_voice(request, context), "设备变化竞态fixture未开始");
    auto changed_device = context; changed_device.device = std::make_shared<FakeDevice>();
    require(!finish_voice(session, changed_device) &&
        session.snapshot()->movement_current == before_device_change->movement_current &&
        session.snapshot()->movement_baseline == before_device_change->movement_baseline &&
        session.snapshot()->draft_plan == before_device_change->draft_plan,
        "设备变化导致失败语音部分写入草稿或基线");
    require(device->touches == 0, "语音选择专项触碰了设备操作");
}
void test_grenade_domain_isolation(const std::filesystem::path& root) {
    FocusFixture focus;
    auto device = std::make_shared<RecordingDevice>();
    auto context = context_for(device); context.config.source_context = focus.config;
    context.map.valid = true; context.map.map_name = "de_mirage"; context.map.context_epoch = 21;
    const auto domain_root = root / ("domain-isolation-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    auto move = request_for(domain_root);
    auto grenade = move; grenade.mode = Mode::GRENADE_TEST;
    grenade.movement_library_root = utf8(domain_root / "grenade-library");
    movement::timeline::Plan plan; plan.name = "身法独立方案"; plan.map = "de_mirage";
    plan.mask.enabled = false;
    plan.actions = {{movement::timeline::ActionKind::KEY,movement::timeline::Key::W,0,40}};
    move.plan_text = movement::timeline::plan_json(plan).dump();
    plan.name = "Mirage道具独立方案"; plan.actions[0].key = movement::timeline::Key::A;
    grenade.plan_text = movement::timeline::plan_json(plan).dump();
    Session session;
    submit(session,Action::SAVE_PLAN,move,context,State::COMPLETED);
    move.load_path = session.snapshot()->movement_selected_path;
    const auto movement_catalog = session.snapshot()->movement_catalog_revision;
    submit(session,Action::SAVE_PLAN,grenade,context,State::COMPLETED);
    grenade.load_path = session.snapshot()->grenade_selected_path;
    auto snap = session.snapshot();
    require(snap->movement_selected_path == move.load_path && snap->movement_catalog_revision == movement_catalog &&
        !grenade.load_path.empty() && snap->grenade_saved_plans.size() == 1 && snap->movement_saved_plans.size() >= 1 &&
        snap->grenade_current == Json::parse(grenade.plan_text) && snap->movement_current == Json::parse(move.plan_text),
        "道具保存污染身法目录、选中项或草稿");
    submit(session,Action::LOAD_PLAN,grenade,context,State::COMPLETED);
    require(session.snapshot()->draft_plan_mode == Mode::GRENADE_TEST &&
        session.snapshot()->draft_plan == Json::parse(grenade.plan_text), "道具载入未标记独立草稿域");
    // 两页依次完成载入、UI尚未消费时，两份发布内容都必须留存。
    const auto grenade_revision = session.snapshot()->grenade_loaded_revision;
    submit(session,Action::LOAD_PLAN,move,context,State::COMPLETED);
    require(session.snapshot()->movement_loaded_draft == Json::parse(move.plan_text) &&
        session.snapshot()->movement_loaded_revision == 1 &&
        session.snapshot()->grenade_loaded_draft == Json::parse(grenade.plan_text) &&
        session.snapshot()->grenade_loaded_revision == grenade_revision,
        "另一页载入吞掉未消费的独立草稿发布");
    require(session.dispatch(Action::ENABLE_MOVEMENT_TEST,move,context), "身法owner启用失败");
    submit(session,Action::LOAD_PLAN,grenade,context,State::COMPLETED);
    require(session.snapshot()->movement_test_mode == Mode::MOVEMENT_TEST && session.snapshot()->repeat_ready &&
        !session.dispatch(Action::RUN_MOVEMENT_TEST,grenade,context), "异域载入或运行抢占身法F11");
    require(session.repeat(context), "道具载入后身法F11失效"); wait_idle(session);
    snap = session.snapshot();
    require(snap->state == State::COMPLETED && snap->result->at("mode") == "movement" &&
        read_json(std::filesystem::u8path(snap->report_directory)/"movement-run-plan.json") == Json::parse(move.plan_text),
        "身法F11误执行道具草稿");
    require(session.dispatch(Action::ENABLE_MOVEMENT_TEST,grenade,context) &&
        session.snapshot()->movement_test_mode == Mode::GRENADE_TEST && !session.snapshot()->movement_voice_selected,
        "明确启用道具未切换owner或清除语音来源");
    require(!session.select_voice(move,context), "道具测试仍接受身法语音选择");
    auto invalid_move = move; invalid_move.plan_text = "损坏的另一页草稿";
    require(!session.update_movement_draft(invalid_move,context) && session.snapshot()->repeat_ready &&
        session.snapshot()->movement_test_mode == Mode::GRENADE_TEST, "另一页坏草稿撤销道具F11");
    require(!session.update_movement_draft(move,context), "另一页编辑错误获得道具输出许可");
    require(session.snapshot()->grenade_loaded_draft == Json::parse(grenade.plan_text) &&
        session.snapshot()->movement_loaded_draft == Json::parse(move.plan_text),
        "普通草稿编辑改写显式载入发布槽");
    submit(session,Action::LOAD_PLAN,move,context,State::COMPLETED);
    require(session.snapshot()->grenade_selected_path == grenade.load_path &&
        session.snapshot()->grenade_baseline == Json::parse(grenade.plan_text) && session.snapshot()->repeat_ready &&
        !session.dispatch(Action::RUN_MOVEMENT_TEST,move,context), "身法载入污染道具基线或抢占owner");
    require(session.repeat(context), "道具F11执行失败"); wait_idle(session);
    snap = session.snapshot();
    require(snap->state == State::COMPLETED && snap->result->at("mode") == "grenade" &&
        read_json(std::filesystem::u8path(snap->report_directory)/"grenade-run-plan.json") == Json::parse(grenade.plan_text) &&
        !std::filesystem::exists(std::filesystem::u8path(snap->report_directory)/"movement-run-plan.json"),
        "道具F11误执行身法或报告归属错误");
    session.cancel();
    require(!session.snapshot()->movement_test_enabled && !session.repeat(context) && device->cleanups == 2,
        "急停未撤销共享许可或未完成两次清理");
    require(!focus.failed && device->touches == 0, "域隔离专项误触设备或源fixture失败");
}
void test_grenade_guidance(const std::filesystem::path& root) {
    auto device = std::make_shared<FakeDevice>();
    auto context = context_for(device);
    context.map.valid = true; context.map.map_name = "de_mirage"; context.map.context_epoch = 30;
    context.config.keyboard.grenade_assist_enabled = true;
    auto request = request_for(root / ("grenade-guidance-"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())));
    request.mode = Mode::GRENADE_TEST;
    auto plan = example_plan(); plan["map"] = "de_mirage"; plan["voice_command"] = "出生点一窗口烟";
    request.plan_text = plan.dump();
    Session session;
    session.update_map(context.map);
    submit(session,Action::SAVE_PLAN,request,context,State::COMPLETED);
    request.load_path = session.snapshot()->grenade_selected_path;
    require(session.dispatch(Action::ENABLE_MOVEMENT_TEST,request,context),"道具辅助启用fixture失败");
    require(session.select_voice(request,context) && finish_voice(session,context),"道具语音未提交当前域");
    const auto selected = session.snapshot();
    require(selected->grenade_guidance_state=="selected" && !selected->grenade_guidance_plan_id.empty() &&
        !selected->grenade_guidance_reference_id.empty() && selected->movement_current.is_null(),
        "道具语音未发布独立双图身份或污染身法");
    require(!session.throw_grenade(context),"下键未对齐仍执行");
    require(session.align_grenade(context),"无资源路径应在后台报告缺失");
    wait_idle(session);
    require(session.snapshot()->state==State::FAILED && session.snapshot()->grenade_guidance_state=="failed" && session.snapshot()->movement_test_enabled &&
        session.snapshot()->repeat_ready && !session.snapshot()->cleanup_unknown && device->touches==0,
        "缺参考图触碰设备、误报清理未知或撤销原人工投掷方案");
    session.cancel_grenade();
    require(session.snapshot()->grenade_guidance_state=="hidden" && session.snapshot()->movement_test_enabled &&
        session.snapshot()->repeat_ready && !session.throw_grenade(context),"左键取消未保留当前许可或复活已对齐");
    require(session.select_voice(request,context),"局部取消后不能重新语音选择");
    session.cancel_grenade();
    require(!finish_voice(session,context) && session.snapshot()->grenade_guidance_state=="hidden",
        "左键取消后迟到语音复活缩略图");
    require(session.select_voice(request,context) && finish_voice(session,context),"重新选择失败");
    session.maintain_grenade(context,false);
    require(session.snapshot()->grenade_guidance_state=="hidden" && !session.throw_grenade(context),
        "输入失信保留旧对齐状态");
    require(session.select_voice(request,context) && finish_voice(session,context),"输入恢复后显式选择失败");
    ++context.map.context_epoch; session.update_map(context.map);
    require(session.snapshot()->grenade_guidance_state=="hidden" && !session.snapshot()->repeat_ready &&
        session.snapshot()->movement_test_enabled,"地图变化未撤销道具语音模板或误关测试");
    session.cancel();
    require(!session.snapshot()->movement_test_enabled && device->touches==0,"End未保留全局撤销语义");
}
void test_grenade_worker_cancel(const std::filesystem::path& root) {
    FocusFixture focus;
    for (const bool global : {false,true}) {
        auto device = std::make_shared<RecordingDevice>();
        auto context = context_for(device);
        context.config.source_context = focus.config;
        context.config.keyboard.grenade_assist_enabled = true;
        auto request = request_for(root / (global ? "grenade-global-cancel" : "grenade-local-cancel"));
        request.mode = Mode::GRENADE_TEST;
        Session session;
        require(session.dispatch(Action::ENABLE_MOVEMENT_TEST,request,context),"worker取消fixture启用失败");
        // 仅worker会看到一次按下，模拟短按在App下一帧之前已释放。
        device->single_press = global ? context.config.keyboard.emergency_virtual_keys.front()
                                     : context.config.keyboard.grenade_cancel_virtual_keys.front();
        require(session.repeat(context),"worker取消fixture未启动");
        wait_idle(session);
        const auto result = session.snapshot();
        require(result->state == State::CANCELED && !result->cleanup_unknown &&
            device->single_press == 0 && device->downs == 0 && device->touches == 0,
            "worker短按取消未停止或触碰输出");
        require(result->movement_test_enabled == !global && result->repeat_ready == !global,
            "worker捕获的End未撤销模板，或左键误撤销全局许可");
        if (global) require(!session.repeat(context),"急停后仍能复用旧模板");
    }
    require(!focus.failed,"worker取消源fixture失败");
}
void test_passive_recording(const std::filesystem::path& root, bool incomplete) {
    auto device = std::make_shared<TraceDevice>();
    device->incomplete = incomplete;
    auto context = context_for(device);
    auto request = request_for(root / (incomplete ? "record-incomplete" : "record-complete"));
    request.movement_record_motion_mode = 1;
    Session session;
    submit(session, Action::SAVE_PLAN, request, context, State::COMPLETED);
    const auto selected_path = session.snapshot()->movement_selected_path;
    const auto original_plan = read_json(std::filesystem::u8path(selected_path));
    const auto saved_count = file_count(std::filesystem::u8path(request.movement_library_root));
    const auto catalog_revision = session.snapshot()->movement_catalog_revision;
    require(session.dispatch(Action::ENABLE_MOVEMENT_TEST, request, context) &&
        session.snapshot()->movement_test_enabled, "录制前启用fixture失败");
    // 正在运行原身法时亦可被动录制；不需要启用软件物理输出或有效编辑草稿。
    context.runtime_idle = false;
    context.config.mouse.allow_send_input = false;
    request.plan_text = "录制不应解析已有草稿";
    require(session.dispatch(Action::START_MOVEMENT_RECORDING, request, context),
        "Runtime运行中拒绝了被动录制");
    require(!session.snapshot()->movement_test_enabled && !session.snapshot()->repeat_ready,
        "开始录制未撤销回放授权");
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (session.busy() && device->delivered < 7 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    if (!incomplete) {
        require(device->delivered == 7 && session.snapshot()->movement_recording && session.busy(),
            "未收到完整录制输入或录制意外结束");
        require(!session.dispatch(Action::START_MOVEMENT_RECORDING, request, context),
            "录制期间接受了第二个录制任务");
        require(session.dispatch(Action::STOP_MOVEMENT_RECORDING, request, context),
            "忙碌时不能停止被动录制");
    }
    wait_idle(session);
    const auto result = session.snapshot();
    require(!result->movement_recording && !result->busy && !result->movement_test_enabled &&
        !result->repeat_ready && !result->cleanup_unknown && device->begins == 1 && device->ends == 1,
        "停止后录制生命周期或输出授权未清理");
    require(device->touches == 0, "被动录制调用了输入订阅或设备输出清理");
    require(!result->movement_recording_path.empty(), "录制未公布原始记录文件");
    const auto trace = read_json(std::filesystem::u8path(result->movement_recording_path));
    require(trace.at("gap").get<bool>() == incomplete && !trace.at("game_validation_passed").get<bool>() &&
        !trace.at("events").empty(), "原始轨迹缺失或误报真实验收");
    require(result->movement_selected_path == selected_path && result->movement_catalog_revision == catalog_revision &&
        file_count(std::filesystem::u8path(request.movement_library_root)) == saved_count &&
        read_json(std::filesystem::u8path(selected_path)) == original_plan &&
        !std::filesystem::exists(std::filesystem::u8path(result->report_directory) / "plan.json"),
        "录制自动保存方案、改变选中路径或覆盖原方案");
    if (incomplete) {
        require(result->draft_plan_revision == 0, "不完整轨迹发布了可执行方案");
    } else {
        require(result->draft_plan_revision == 1 && result->draft_plan_mode == Mode::MOVEMENT_TEST &&
            !result->draft_plan.empty(), "停止录制未发布可修改草稿");
        movement::timeline::Plan plan; std::string error;
        require(movement::timeline::parse_plan(result->draft_plan, plan, error), "录制方案不可解析");
        bool physical_w = false, software_a = false, mask_w = false;
        for (const auto& action : plan.actions) {
            physical_w |= action.kind == movement::timeline::ActionKind::KEY && action.physical && action.key == movement::timeline::Key::W;
            software_a |= action.kind == movement::timeline::ActionKind::KEY && !action.physical && action.key == movement::timeline::Key::A;
            mask_w |= action.kind == movement::timeline::ActionKind::MASK && action.key == movement::timeline::Key::W;
        }
        require(physical_w && software_a && mask_w, "录制丢失物理、软件或屏蔽轨道");
    }
    require(!session.dispatch(Action::STOP_MOVEMENT_RECORDING, request, context),
        "完成后仍接受重复停止录制");
}
}
int main(int argc, char** argv) {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 2;
    struct CleanupWinsock { ~CleanupWinsock() { WSACleanup(); } } cleanup_winsock;
    // 仅当前测试进程的固定向量，不读取部署凭据。
    if (_putenv_s("XEN_SOURCE_CONTEXT_TOKEN", std::string(32, 't').c_str()) != 0) return 2;
    try {
        require(argc == 2, "需要显式提供构建目录内的fixture目录");
        const auto root = std::filesystem::absolute(std::filesystem::u8path(argv[1]));
        test_documents(root);
        test_invalid_plans(root);
        test_frozen_and_missing_source(root);
        test_cancel_prepare(root);
        test_source_lifecycle(root, false);
        test_source_lifecycle(root, true);
        test_enabled_edit_and_cancel(root);
        test_edit_while_running(root);
        test_voice_selection(root);
        test_grenade_domain_isolation(root);
        test_grenade_guidance(root);
        test_grenade_worker_cancel(root);
        test_passive_recording(root, false);
        test_passive_recording(root, true);
        std::cout << "身法会话测试通过（合成设备，未执行真实输出）\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
