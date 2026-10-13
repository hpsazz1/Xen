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
    int downs = 0, cleanups = 0;
    bool poll_input(InputSnapshot& input) noexcept override {
        input = {}; input.status = InputMonitorStatus::READY; input.state_valid = true; return true;
    }
    KeyboardReceipt set_wasd_keyboard(std::uint8_t mask) noexcept override {
        if (mask) { ++downs; if (revoke_on_down) revoke_on_down->foreground = false; }
        KeyboardReceipt receipt; receipt.disposition = KeyboardDisposition::ACKNOWLEDGED; return receipt;
    }
    KeyboardReceipt cleanup_wasd_keyboard() noexcept override {
        ++cleanups; return set_wasd_keyboard(0);
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
void test_documents(const std::filesystem::path& root) {
    Session session;
    auto request = request_for(root / "documents");
    const auto context = context_for();
    submit(session, Action::VALIDATE, request, context, State::COMPLETED);
    require(session.snapshot()->plan == example_plan() && session.snapshot()->sampling.empty(),
        "校验丢失并行动作或混入采样参数");
    submit(session, Action::SAVE_PLAN, request, context, State::COMPLETED);
    const auto directory = std::filesystem::u8path(session.snapshot()->report_directory);
    require(read_json(directory / "plan.json") == example_plan(), "独立文件没有保存完整方案");
    require(!read_json(directory / "prepare.json").at("physical_output").get<bool>(), "保存产生输出许可");
    request.load_path = utf8(directory / "plan.json");
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
    require(read_json(directory / "plan.json") == example_plan() &&
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
    submit(session, Action::PREPARE, request, context, State::PREPARED);
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
        require(!result->repeat_ready && !session.repeat(context), "失焦取消仍保留快捷键重复许可");
        require(device->downs == 1, "失焦后重复快捷键产生额外按下");
    } else require(result->repeat_ready, "正常完成错误撤销冻结快捷键模板");
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
        std::cout << "身法会话测试通过（合成设备，未执行真实输出）\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
