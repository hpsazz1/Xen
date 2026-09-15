#include "auto_stop_probe/debug_runner.h"
#include "auto_stop_probe/counterpulse_internal.h"
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
        require(baseline.at("settings_source") == "REFERENCE_INITIAL_ASSUMPTIONS" &&
            std::filesystem::is_regular_file(root / "plan.json"), "默认计划必须同核生成且不需要设备");
        rejects([&] { run_debug(request); });
        std::filesystem::remove_all(root);
        std::cout << "原生调试计划、边界和离线入口通过\n";
        return 0;
    } catch (const std::exception& error) {
        if (std::filesystem::exists(root)) std::filesystem::remove_all(root);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
