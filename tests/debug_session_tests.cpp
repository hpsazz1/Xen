#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#ifdef ERROR
#undef ERROR
#endif
#include "debug_session/debug_session.h"
#include "runtime/runtime.h"
#include "recoil/recoil_debug_run.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
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
    std::atomic_bool hold_background_owner{false};
    const std::thread::id owner_thread=std::this_thread::get_id();
    bool output_owner_exclusive() const noexcept override {
        while(hold_background_owner.load()&&std::this_thread::get_id()!=owner_thread)std::this_thread::yield();
        return true;
    }
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
void test_idle_hud_show() {
    Session session; Context context; Request request;
    const auto before = session.snapshot();
    require(session.dispatch(Action::SHOW_HUD,request,context),"空闲显示HUD请求未接收");
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    HWND window = nullptr;
    while (std::chrono::steady_clock::now() < deadline) {
        session.poll(); window = FindWindowW(L"XenCounterpulseReadOnlyHud",nullptr);
        if (window && IsWindowVisible(window)) break;
        std::this_thread::sleep_for(5ms);
    }
    require(window && IsWindowVisible(window),"空闲SHOW_HUD只改了快照，没有创建真实可见HUD");
    require(!session.busy() && session.snapshot()->generation == before->generation &&
        session.snapshot()->state == before->state && session.snapshot()->prepared_id.empty(),
        "空闲显示HUD不得创建调试任务或准备身份");
    const auto wait_visible = [&](bool visible, const char* phase) {
        const auto until = std::chrono::steady_clock::now() + 2s;
        do { session.poll();
            if (session.snapshot()->hud_visible == visible && (IsWindowVisible(window) != FALSE) == visible) return;
            std::this_thread::sleep_for(5ms);
        } while (std::chrono::steady_clock::now() < until);
        DWORD owner = 0; GetWindowThreadProcessId(window, &owner);
        throw std::runtime_error(std::string("HUD实际可见状态未同步到Session: ") + phase +
            " expected=" + std::to_string(visible) + " snapshot=" + std::to_string(session.snapshot()->hud_visible) +
            " actual=" + std::to_string(IsWindowVisible(window)) + " owner=" + std::to_string(owner) +
            " current=" + std::to_string(GetCurrentProcessId()));
    };
    session.dispatch(Action::HIDE_HUD,request,context); wait_visible(false,"hide");
    session.dispatch(Action::SHOW_HUD,request,context); wait_visible(true,"show/reopen");
    require(FindWindowW(L"XenCounterpulseReadOnlyHud",nullptr) == window,"空闲隐藏/显示不应创建第二窗口");
    PostMessageW(window,WM_CLOSE,0,0); wait_visible(false,"close");
    require(!session.snapshot()->hud_requested,"用户关闭HUD必须取消显示开关");
    session.dispatch(Action::SHOW_HUD,request,context); wait_visible(true,"show/reopen");
    require(!session.busy() && session.snapshot()->generation == before->generation,
        "关闭复开HUD不得启动任务或改变准备身份");
    session.set_theme(UiTheme::DARK);
    session.request_shutdown(); wait_idle(session);
    require(!IsWindow(window),"Session关闭必须销毁其持久窗口");
}

void test_save_weapon_timing(const std::filesystem::path& root) {
    Session session;
    auto device = std::make_shared<FakeDevice>();
    auto context = context_for(device);
    const auto path = root / "save-weapon-timing.json";
    context.config.weapon_timing_file = utf8(path);
    auto catalog = weapon::default_timing_catalog();
    std::string error;
    require(weapon::save_timing_catalog(path,catalog,error),"保存测试初始目录失败");
    Request request; request.mode = Mode::FIRE_TEST; request.show_hud = false;
    request.load_path = utf8(path);
    require(session.dispatch(Action::LOAD_WEAPON_TIMING,request,context),"保存测试载入请求未接收");
    wait_idle(session);
    // 模拟UI载入后其他编辑器保存；本次必须保留磁盘最新的其他武器。
    catalog.revision = 17; catalog.profiles[0].shot_hold_ms = 91;
    require(weapon::save_timing_catalog(path,catalog,error),"保存测试外部更新失败");
    const auto before = read_json(path);
    const auto draft = session.snapshot()->draft_plan;
    const auto draft_revision = session.snapshot()->draft_plan_revision;
    request.weapon_id = "ak47"; request.shot_hold_ms = 85; request.fire_interval_ms = 420;
    request.load_path = utf8(root / "must-not-be-written.json");
    require(session.dispatch(Action::SAVE_WEAPON_TIMING,request,context),"武器参数保存请求未接收");
    wait_idle(session);
    require(session.snapshot()->state == State::COMPLETED,"有效武器参数保存失败");
    const auto after = read_json(path);
    auto expected = before; expected["revision"] = 18;
    for (auto& row : expected["profiles"]) if (row["canonical_id"] == "ak47") {
        row["shot_hold_ms"] = 85; row["fire_interval_ms"] = 420;
    }
    require(after == expected,"保存覆盖了最新其他武器或没有仅更新所选两字段和版本");
    require(!std::filesystem::exists(std::filesystem::u8path(request.load_path)),"保存不得使用导入路径作为目的地");
    weapon::TimingCatalog loaded;
    require(weapon::load_timing_catalog(path,loaded,error) && loaded.revision == 18 &&
        weapon::find_timing(loaded,"ak47")->shot_hold_ms == 85,"保存结果无法通过正式读取器回读");
    require(session.snapshot()->timing_catalog_valid && session.snapshot()->timing_catalog.revision == 18 &&
        session.snapshot()->draft_plan == draft && session.snapshot()->draft_plan_revision == draft_revision,
        "保存应刷新资料快照但不能覆盖射击草稿");
    const auto bytes = [&] { std::ifstream file(path,std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file),{}); };
    const auto reject_unchanged = [&] {
        const auto original = bytes();
        require(session.dispatch(Action::SAVE_WEAPON_TIMING,request,context),"无效保存应提交后台报告原因");
        wait_idle(session);
        require(session.snapshot()->state == State::FAILED && bytes() == original,
            "无效参数、目录或保存失败不能修改原文件");
        require(session.snapshot()->timing_catalog.revision == 18,"失败不能发布未保存目录");
    };
    request.weapon_id = "unknown"; reject_unchanged();
    request.weapon_id = "revolver"; reject_unchanged();
    request.weapon_id = "ak47";
    request.shot_hold_ms = 0; reject_unchanged();
    request.shot_hold_ms = 501; reject_unchanged();
    request.shot_hold_ms = 85; request.fire_interval_ms = 85; reject_unchanged();
    request.fire_interval_ms = 2001; reject_unchanged();
    request.fire_interval_ms = 420;
    require(SetFileAttributesW(path.c_str(),FILE_ATTRIBUTE_READONLY) != FALSE,"只读目标测试设置失败");
    struct RestoreAttributes { std::filesystem::path path;
        ~RestoreAttributes() { SetFileAttributesW(path.c_str(),FILE_ATTRIBUTE_NORMAL); } } restore{path};
    reject_unchanged();
    require(SetFileAttributesW(path.c_str(),FILE_ATTRIBUTE_NORMAL) != FALSE,"只读目标测试恢复失败");
    loaded.revision = (std::numeric_limits<std::uint64_t>::max)();
    require(weapon::save_timing_catalog(path,loaded,error),"版本上限测试资料写入失败");
    reject_unchanged();
    { std::ofstream file(path); file << "broken"; }
    reject_unchanged();
    std::filesystem::remove(path);
    reject_unchanged();
    require(!std::filesystem::exists(path),"缺失自定义资料不能静默用默认资料创建");
    require(device->outputs == 0 && device->opens == 0 && device->closes == 0 && device->subscriptions == 0,
        "保存武器资料不能触碰设备");
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
    require(session.snapshot()->draft_plan_revision == 1 && session.snapshot()->draft_plan_mode == Mode::COUNTERPULSE &&
        session.snapshot()->draft_plan.at("baseline") == "stationary", "显式LOAD_PLAN必须保留原地计划并标记急停草稿来源");
    const auto imported = session.snapshot()->draft_plan;
    request.load_path.clear();
    require(session.dispatch(Action::LOAD_FIRE_SETTINGS,request,context), "空文件请求应由后台报告失败");
    wait_idle(session);
    require(session.snapshot()->state == State::FAILED && session.snapshot()->draft_plan_revision == 1 &&
        session.snapshot()->draft_plan == imported,"文件载入失败不能发布新草稿或复用旧计划覆盖编辑器");
    const auto catalog_path = root / "isolated-weapon-timing.json";
    std::string catalog_error;
    require(weapon::save_timing_catalog(catalog_path,weapon::default_timing_catalog(),catalog_error),"合成武器资料写入失败");
    request.load_path = utf8(catalog_path);
    require(session.dispatch(Action::LOAD_WEAPON_TIMING,request,context),"共享资料请求未接收");
    wait_idle(session);
    require(session.snapshot()->draft_plan_revision == 1 && session.snapshot()->draft_plan == imported,
        "刷新共享目录不能重发历史计划到草稿");
    const auto fire_path = root / "isolated-fire-settings.json";
    { std::ofstream file(fire_path); file << R"({"shot_hold_ms":70,"fire_interval_ms":410})"; }
    request.load_path = utf8(fire_path);
    require(session.dispatch(Action::LOAD_FIRE_SETTINGS,request,context),"射击文件载入请求未接收");
    wait_idle(session);
    require(session.snapshot()->draft_plan_revision == 2 && session.snapshot()->draft_plan_mode == Mode::FIRE_TEST &&
        session.snapshot()->draft_plan.at("fire_interval_ms") == 410,"成功文件导入必须发布对应射击页的显式草稿");

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
void test_fire_start_block_is_visible(const std::filesystem::path& root) {
    Session session;
    Request request; request.mode = Mode::FIRE_TEST; request.show_hud = false;
    request.output_root = utf8(root / "start-blocked");
    auto device = std::make_shared<FakeDevice>();
    auto context = context_for(device);
    context.runtime_idle = false;
    require(session.dispatch(Action::PREPARE,request,context), "准备未提交");
    wait_idle(session);
    require(session.snapshot()->state == State::PREPARED, "离线准备应保留可审阅计划");
    require(session.snapshot()->message.find("Runtime") != std::string::npos,
        "生产Runtime运行期间准备未显示实际职责冲突");
    const auto id = session.snapshot()->prepared_id;
    require(!session.dispatch(Action::START,request,context,id,true,physical_confirmation()), "生产职责冲突未拒绝");
    require(device->outputs == 0 && device->opens == 0 && device->subscriptions == 0, "被拒绝启动触碰设备");
    context.runtime_idle = true;
    require(!session.dispatch(Action::START,request,context,"stale",true,physical_confirmation()), "旧身份未拒绝");
    require(session.snapshot()->message.find("重新准备") != std::string::npos, "过期身份拒绝没有具体说明");
    session.request_shutdown(); wait_idle(session);
}
void test_edit_during_prepare_cannot_restore_repeat(const std::filesystem::path& root) {
    Session session;
    Request request; request.mode = Mode::FIRE_TEST; request.show_hud = false;
    request.output_root = utf8(root / "edit-prepare");
    const auto context = context_for(std::make_shared<FakeDevice>());
    for (int i = 0; i < 32; ++i) {
        require(session.dispatch(Action::PREPARE,request,context), "竞争准备未提交");
        if (i % 2) std::this_thread::sleep_for(1ms);
        session.invalidate_repeat();
        wait_idle(session);
        require(!session.snapshot()->repeat_ready, "Prepare后台完成复活已编辑失效的快捷键模板");
        auto enabled = context; enabled.config.keyboard.debug_test_enabled = true;
        require(!session.repeat(enabled), "已编辑失效的模板仍接受快捷键");
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
    const auto completed_generation=session.snapshot()->generation;
    request.load_path=utf8(root/"missing-timing-after-result.json");
    require(session.dispatch(Action::LOAD_WEAPON_TIMING,request,context),"完成后读取新文档请求未接收");
    require(session.snapshot()->generation>completed_generation&&!session.snapshot()->result,
        "新代次启动时不得携带旧结果让UI重复消费");
    wait_idle(session);
    require(session.snapshot()->state==State::FAILED&&!session.snapshot()->result,
        "新请求失败也不能复活上一组已完成结果");
    session.request_shutdown(); wait_idle(session);
}
void test_repeat_admission_and_invalidation(const std::filesystem::path& root) {
    Session session;
    auto device = std::make_shared<FakeDevice>();
    auto context = context_for(device);
    require(!context.config.keyboard.debug_test_enabled, "调试快捷键必须默认关闭");
    context.config.keyboard.debug_test_enabled = true;
    require(!session.repeat(context), "未准备物理模板仍接受快捷键");
    Request request; request.mode = Mode::DERIVE_DEFAULTS; request.show_hud = false;
    request.output_root = utf8(root / "repeat-admission");
    require(session.dispatch(Action::PREPARE, request, context), "离线模板准备未提交");
    wait_idle(session);
    require(!session.snapshot()->repeat_ready && !session.repeat(context), "离线模板被当成物理重复模板");

    request.mode = Mode::FIRE_TEST;
    context.cleanup_known = false;
    require(session.dispatch(Action::PREPARE, request, context), "清理未知模板准备未提交");
    wait_idle(session);
    require(!session.snapshot()->repeat_ready && !session.repeat(context), "清理未知形成了有效重复模板");
    context.cleanup_known = true;
    require(session.dispatch(Action::PREPARE, request, context), "有效模板准备未提交");
    wait_idle(session);
    require(session.snapshot()->repeat_ready, "有效物理准备没有发布重复模板");
    context.config.keyboard.debug_test_enabled = false;
    require(!session.repeat(context), "关闭开关仍接受重复任务");
    context.config.keyboard.debug_test_enabled = true;
    context.runtime_idle = false;
    require(!session.repeat(context), "重复启动没有复核当前生产职责");
    require(session.snapshot()->message.find("Runtime") != std::string::npos, "职责冲突拒绝缺少具体说明");
    context.runtime_idle = true;
    auto changed = context; changed.device = std::make_shared<FakeDevice>();
    require(!session.repeat(changed), "设备身份变化仍可使用原重复模板");
    changed = context; changed.config.mouse.allow_send_input = false;
    require(!session.repeat(changed), "物理输出关闭仍接受重复任务");
    request.shot_hold_ms = 120;
    session.invalidate_repeat(); // 草稿修改通知使用生产失效接口。
    require(!session.snapshot()->repeat_ready && !session.repeat(context), "草稿失效后仍可重复旧模板");
    require(session.dispatch(Action::PREPARE, request, context), "更新草稿准备未提交");
    wait_idle(session);
    require(session.snapshot()->repeat_ready && session.snapshot()->plan.at("shot_hold_ms") == 120,
        "重新准备没有冻结新草稿");
    session.cancel();
    require(!session.snapshot()->repeat_ready && !session.repeat(context), "取消未清除重复模板");
    require(device->opens == 0 && device->closes == 0 && device->outputs == 0 && device->subscriptions == 0,
        "重复准入拒绝或模板操作触碰设备");
    session.request_shutdown(); wait_idle(session);
}
void test_repeat_creates_independent_run(const std::filesystem::path& root) {
    std::filesystem::path previous_run;
    // 原生缺少源焦点会早失败并锁定清理状态；用独立会话覆盖两次目录分配，不能伪造成功再重用会话。
    for (int iteration = 0; iteration < 2; ++iteration) {
        Session session;
        auto device = std::make_shared<FakeDevice>();
        auto context = context_for(device);
        context.config.keyboard.debug_test_enabled = true;
        context.config.source_context.host.clear(); context.config.source_context.port = 0;
        Request request; request.mode = Mode::FIRE_TEST; request.show_hud = false;
        request.output_root = utf8(root / "repeat-runs");
        require(session.dispatch(Action::PREPARE, request, context), "重复目录测试准备未提交");
        wait_idle(session);
        const auto prepared = session.snapshot();
        const auto original = std::filesystem::u8path(prepared->report_directory);
        const auto original_document = read_json(original / "prepare.json");
        request.shot_hold_ms = 120; // 独立变量不会改变冻结副本，实际UI编辑另行调用invalidate_repeat。
        require(session.repeat(context), "有效物理模板未接收单组快捷键请求");
        session.cancel();
        wait_idle(session);
        const auto finished = session.snapshot();
        const auto run = std::filesystem::u8path(finished->report_directory).parent_path();
        require(run != original && run != previous_run, "重复任务复用了准备或已有Run目录");
        const auto repeated = read_json(run / "prepare.json");
        require(repeated.at("repeat_of") == prepared->prepared_id && repeated.at("trigger") == "USER_HOTKEY",
            "独立Run丢失模板来源或前台按键来源");
        require(repeated.at("prepared_id") != prepared->prepared_id && repeated.at("plan").at("shot_hold_ms") == 80,
            "重复任务未分配新身份或冻结计划被草稿污染");
        require(read_json(original / "prepare.json") == original_document, "重复任务修改了原准备报告");
        require(!finished->repeat_ready && !session.repeat(context), "取消或原生失败后模板仍可重复");
        require(finished->state != State::COMPLETED, "取消或缺少源焦点的Run冒充成功");
        require(device->opens == 0 && device->closes == 0 && device->outputs == 0 && device->subscriptions == 0,
            "取消或源焦点缺失路径触碰了设备");
        previous_run = run;
        session.request_shutdown(); wait_idle(session);
    }
}
void test_recording_ownership_and_async(const std::filesystem::path& root) {
    Runtime runtime;
    Session session;
    auto device = std::make_shared<FakeDevice>();
    struct ReleaseReader { std::shared_ptr<FakeDevice> device; ~ReleaseReader() { device->release_reader = true; } } release{device};
    device->release_reader = false;
    auto context = context_for(device);
    context.config.keyboard.debug_test_enabled = true;
    Request physical; physical.mode = Mode::FIRE_TEST; physical.show_hud = false;
    physical.output_root = utf8(root / "busy-template");
    require(session.dispatch(Action::PREPARE, physical, context), "繁忙测试模板准备未提交");
    wait_idle(session);
    require(session.snapshot()->repeat_ready, "繁忙测试需要有效物理模板");
    require(session.record_inputs(runtime, utf8(root / "record"), context), "原始记录请求未接收");
    const auto deadline = std::chrono::steady_clock::now() + 4s;
    while (!device->reader_entered && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    require(device->reader_entered, "假设备Reader未进入");
    require(!session.record_inputs(runtime, utf8(root / "duplicate"), context), "第二记录任务未被拒绝");
    const auto recording_generation = session.snapshot()->generation;
    require(!session.repeat(context), "记录期间快捷键请求被接受或排队");
    require(session.snapshot()->message.find("不排队") != std::string::npos, "繁忙拒绝未说明不排队");
    Request request; request.show_hud = false;
    require(!session.dispatch(Action::VALIDATE, request, context), "记录期间接受了第二后台任务");
    // Reader仍受控阻塞，此处UI接口须返回；若退回同步实现，CTest超时会明确失败。
    for (int i = 0; i < 100; ++i) { session.poll(); require(session.snapshot()->busy, "记录未结束却提前释放busy"); }
    session.cancel();
    require(session.busy(), "取消请求不应冒充Reader已退出");
    device->release_reader = true;
    wait_idle(session);
    require(session.snapshot()->generation == recording_generation && !session.snapshot()->repeat_ready,
        "取消记录后执行了此前被拒绝的快捷键或恢复了模板");
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

void test_recoil_freeze_and_admission(const std::filesystem::path& root) {
    auto device=std::make_shared<FakeDevice>();auto context=context_for(device);
    context.config.keyboard.debug_test_enabled=true;
    context.config.keyboard.debug_test_virtual_keys={5};context.config.recoil.sensitivity=1;
    RecoilProfile profile;profile.id="debug-candidate";profile.weapon_id="ak47";
    profile.state=RecoilProfileState::SCHEMA_VALID;profile.points={{0,0,0},{100,2,4}};
    const auto path=root/"recoil-candidate.json";
    {std::ofstream file(path);file<<serialize_recoil_profile(profile);}
    auto calibration=prepare_wall_debug_plan(true,"ak47",1500,{},context.config);
    calibration["geometry"]={{"input_size",{320,320}},{"processed_size",{320,320}}};
    calibration["samples"]={{{"counts",{4,0}},{"pixel_delta",{2,0}},{"acknowledged",true},{"evidence_id","x"}},
        {{"counts",{0,4}},{"pixel_delta",{0,2}},{"acknowledged",true},{"evidence_id","y"}}};
    const auto calibration_path=root/"recoil-calibration.json";
    {std::ofstream file(calibration_path);file<<calibration.dump();}
    for(int failure=0;failure<4;++failure){
        auto bad=calibration;
        if(failure==0)bad["samples"][1]["counts"]={8,0};
        if(failure==1)bad["samples"][1]["acknowledged"]=false;
        if(failure==2)bad["samples"][1]["evidence_id"]="x";
        if(failure==3){
            bad["samples"].push_back({{"counts",{-4,0}},{"pixel_delta",{-0.2,0}},{"acknowledged",true},{"evidence_id","reverse-x"}});
            bad["samples"].push_back({{"counts",{0,-4}},{"pixel_delta",{0,-2}},{"acknowledged",true},{"evidence_id","reverse-y"}});
        }
        {std::ofstream file(calibration_path);file<<bad.dump();}
        bool rejected=false;try{prepare_wall_debug_plan(false,"ak47",1500,calibration_path,context.config);}catch(...){rejected=true;}
        require(rejected,"坏标定必须在准备阶段拒绝，不能先射击再分析失败");
    }
    {std::ofstream file(calibration_path);file<<calibration.dump();}
    Request request;request.mode=Mode::RECOIL_TEST;request.recoil_profile_path=utf8(path);
    request.recoil_calibration_path=utf8(calibration_path);request.output_root=utf8(root/"recoil-debug");
    Session session;require(session.dispatch(Action::PREPARE,request,context),"弹道准备接收");wait_idle(session);
    require(session.snapshot()->repeat_ready,"弹道测试完成准备但未生成快捷键模板");
    const auto frozen=session.snapshot()->plan;
    require(frozen.at("limits").at("firing_ms").get<int>()>=request.recoil_duration_ms,
        "前段短曲线测试更长阶段必须延長会话时间预算");
    require(frozen.at("limits").at("total_counts").get<int>()==10,
        "延长阶段不得扩展counts预算或外推旧曲线尾部");
    require(recoil_debug_geometry_matches(frozen,calibration.at("geometry")),"相同标定画面应通过几何绑定");
    auto changed_geometry=calibration.at("geometry");changed_geometry["input_size"]={640,640};
    require(!recoil_debug_geometry_matches(frozen,changed_geometry),"同配置但实际画面尺寸变化必须拒绝");
    require(!recoil_debug_geometry_matches(Json::object(),calibration.at("geometry")),"缺标定不得通过几何绑定");
    {std::ofstream file(path);file<<"changed after prepare";}
    require(session.snapshot()->plan==frozen&&frozen.at("profile").at("points").size()==2,"准备曲线未被冻结");
    require(device->opens==0&&device->outputs==0&&device->closes==0,"准备不得输出或重建设备");
    context.runtime_idle=false;require(!session.repeat(context),"Runtime运行时不得弹道测试");context.runtime_idle=true;
    context.config.keyboard.debug_test_virtual_keys={6};require(!session.repeat(context),"变更键绑定须重新准备");
    session.cancel();require(!session.snapshot()->repeat_ready,"取消必须撤销弹道模板");
    request.mode=Mode::RECOIL_CALIBRATE;request.weapon_id="ak47";request.recoil_target_shots=0;
    require(session.dispatch(Action::PREPARE,request,context),"无效分段准备应返回后台错误");wait_idle(session);
    require(session.snapshot()->state==State::FAILED&&!session.snapshot()->repeat_ready,"阶段0发不得生成测试模板");
    request.recoil_target_shots=5;request.mode=Mode::RECOIL_CAPTURE;
    request.recoil_calibration_path=utf8(root/"missing.json");
    require(session.dispatch(Action::PREPARE,request,context),"缺标定请求接收");wait_idle(session);
    require(session.snapshot()->state==State::FAILED,"无画面标定不能准备counts采集");
    // 冻结首组许可在启动时消费，不依赖后续UI渲染或执行成功才撤销。
    for(const auto mode:{Mode::RECOIL_CALIBRATE,Mode::RECOIL_CAPTURE}){
        Session one_shot;request.mode=mode;request.recoil_calibration_path=utf8(calibration_path);
        context.config.source_context.host.clear();context.config.source_context.port=0;
        require(one_shot.dispatch(Action::PREPARE,request,context),"单次采集准备接收");wait_idle(one_shot);
        require(one_shot.snapshot()->repeat_ready,"单次采集也须先提供一次前台许可");
        device->hold_background_owner=true;
        require(one_shot.repeat(context),"首次标定或采集按键应被接收");
        const bool consumed=!one_shot.snapshot()->repeat_ready;
        one_shot.cancel();device->hold_background_owner=false;wait_idle(one_shot);
        require(consumed,"初始标定和采集模板必须在首次启动时消费，不能重复采集同一前段");
    }
    require(device->outputs==0&&device->closes==0,"拒绝和取消不得操作借用设备");
}
void test_recoil_existing_curve_without_measurement(const std::filesystem::path& root) {
    auto device=std::make_shared<FakeDevice>();auto context=context_for(device);
    context.config.keyboard.debug_test_enabled=true;context.config.keyboard.debug_test_virtual_keys={5};
    context.config.recoil.sensitivity=1;
    RecoilProfile imported;imported.id="imported-existing";imported.weapon_id="ak47";
    imported.state=RecoilProfileState::IMPORTED;imported.points={{0,0,0},{100,2,4}};
    const auto path=root/"existing-imported.json";const auto original=serialize_recoil_profile(imported);
    {std::ofstream file(path);file<<original;}
    Session session;Request request;request.mode=Mode::RECOIL_TEST;
    request.recoil_profile_path=utf8(path);request.output_root=utf8(root/"existing-no-measurement");
    require(session.dispatch(Action::PREPARE,request,context),"已有导入曲线验证准备请求接收");wait_idle(session);
    const auto plan=session.snapshot()->plan;
    require(session.snapshot()->repeat_ready,"结构有效IMPORTED曲线应可不经画面标定准备验证");
    require(!plan.value("measurement_enabled",true)&&!plan.contains("calibration"),"无标定验证不得宣称启用画面测量");
    require(plan.at("source_profile_state")=="IMPORTED"&&plan.at("profile").at("state")=="SCHEMA_VALID",
        "仅本次冻结副本获得结构验证状态，原始来源身份须保留");
    require(read_json(path).at("state")=="IMPORTED"&&read_json(path)==Json::parse(original),"验证准备不得改写源文件校准状态");
    require(recoil_debug_geometry_matches(plan,{{"input_size",{320,320}}}),"仅人工观察无需先有画面标定");
    context.runtime_idle=false;require(!session.repeat(context),"直接验证仍须Runtime停止独占设备");
    context.runtime_idle=true;session.cancel();require(!session.snapshot()->repeat_ready,"取消仍撤销直接验证许可");
    request.recoil_calibration_path=utf8(root/"not-found.json");
    require(session.dispatch(Action::PREPARE,request,context),"显式坏标定请求接收");wait_idle(session);
    require(session.snapshot()->state==State::FAILED&&!session.snapshot()->repeat_ready,"显式坏标定不得静默降级为人工观察");
    request.recoil_calibration_path.clear();request.recoil_duration_ms=0;
    require(session.dispatch(Action::PREPARE,request,context),"无标定零时长请求接收");wait_idle(session);
    require(session.snapshot()->state==State::FAILED,"无标定仍须验证有界时长");
    request.recoil_duration_ms=1500;
    auto invalid=Json::parse(original);invalid["points"][1][0]=0;
    {std::ofstream file(path);file<<invalid.dump();}
    require(session.dispatch(Action::PREPARE,request,context),"无效导入曲线请求接收");wait_idle(session);
    require(session.snapshot()->state==State::FAILED&&!session.snapshot()->repeat_ready,"IMPORTED仍须真实节点结构校验");
    require(device->outputs==0&&device->opens==0&&device->closes==0,"纯准备与拒绝不得设备输出或重建设备");
}
}
int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("xen-debug-session-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        {
            Session session;
            auto device = std::make_shared<FakeDevice>();
            auto context = context_for(device);
            context.config.mouse.kmbox_command_timeout_ms = 300;
            context.config.mouse.kmbox_connect_timeout_ms = 5000;
            context.config.keyboard.debug_test_enabled = true;
            context.config.source_context.host.clear(); context.config.source_context.port = 0;
            Request request; request.mode = Mode::FIRE_TEST; request.show_hud = false;
            request.sampling_text = "invalid-model-draft";
            request.output_root = utf8(root / "existing-connection-timeout");
            require(session.dispatch(Action::PREPARE,request,context), "现有连接准备请求未接收");
            wait_idle(session);
            require(session.snapshot()->repeat_ready,
                "已连接设备被ACK最大等待或历史连接超时配置拒绝，未测实际时序就禁止测试");
            require(session.repeat(context), "有效已有设备快捷键启动仍被配置超时拒绝");
            wait_idle(session);
            require(session.snapshot()->state == State::FAILED && !session.snapshot()->cleanup_unknown,
                "尚未输出的源配置失败不应锁死为设备清理未知");
            require(session.snapshot()->message.find("源") != std::string::npos,
                "尚未输出的失败必须显示具体源配置原因");
            require(device->outputs == 0 && device->opens == 0 && device->closes == 0,
                "无源配置且取消的测试不得真实输出或重建设备");
        }
        test_recoil_freeze_and_admission(root);
        test_recoil_existing_curve_without_measurement(root);
        test_idle_hud_show();
        test_save_weapon_timing(root);
        test_documents_and_frozen_prepare(root);
        test_cancel_prepare(root);
        test_fire_start_block_is_visible(root);
        test_edit_during_prepare_cannot_restore_repeat(root);
        test_offline_start_uses_frozen_mode(root);
        test_repeat_admission_and_invalidation(root);
        test_repeat_creates_independent_run(root);
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
