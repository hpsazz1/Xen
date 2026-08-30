#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <Windows.h>
#include <dxgi.h>

#ifdef ERROR
#undef ERROR
#endif

#include "overlay/overlay_internal.h"

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "[失败] " << message << '\n';
}

bool nearly_equal(float lhs, float rhs) {
    return std::abs(lhs - rhs) < 0.0001f;
}

struct PresentCapture {
    std::array<HRESULT, 4> results{};
    std::array<UINT, 4> sync_intervals{};
    std::array<UINT, 4> flags{};
    std::size_t result_count = 0;
    std::size_t call_count = 0;
};

HRESULT present_from_capture(
        void* context, UINT sync_interval, UINT flags) noexcept {
    auto& capture = *static_cast<PresentCapture*>(context);
    const std::size_t index = capture.call_count++;
    capture.sync_intervals[index] = sync_interval;
    capture.flags[index] = flags;
    return index < capture.result_count ? capture.results[index] : E_FAIL;
}

overlay::detail::PresentAdapter adapter_for(
        PresentCapture& capture) noexcept {
    return {&capture, present_from_capture};
}

void test_present_success_statuses_preserve_submission() {
    PresentCapture capture;
    capture.results[0] = S_OK;
    capture.results[1] = DXGI_STATUS_OCCLUDED;
    capture.result_count = 2;
    overlay::detail::PresentBoundary boundary(adapter_for(capture));

    expect(boundary.present(1, 0) && boundary.present(0, 0) &&
               !boundary.failed() && boundary.last_error().empty() &&
               capture.call_count == 2 &&
               capture.sync_intervals[0] == 1 &&
               capture.sync_intervals[1] == 0 &&
               capture.flags[0] == 0 && capture.flags[1] == 0,
           "Present 必须用 FAILED 语义接受 S_OK/DXGI_STATUS_OCCLUDED，"
           "并原样传递 vsync 与 flags");
}

void test_present_device_removed_failure_is_owned_and_latched() {
    PresentCapture capture;
    capture.results[0] = DXGI_ERROR_DEVICE_REMOVED;
    capture.results[1] = S_OK;
    capture.result_count = 2;
    overlay::detail::PresentBoundary boundary(adapter_for(capture));

    const bool first = boundary.present(1, 0);
    const std::string owned_error = boundary.last_error();
    const bool repeated = boundary.present(1, 0);
    expect(!first && !repeated && boundary.failed() &&
               capture.call_count == 1 &&
               owned_error ==
                   "IDXGISwapChain::Present 失败，HRESULT=0x887A0005 "
                   "(DXGI_ERROR_DEVICE_REMOVED)。" &&
               boundary.last_error() == owned_error,
           "DEVICE_REMOVED 必须形成含原始 HRESULT 的 owned error，"
           "并锁存失败以禁止下一轮 Present");
}

void test_present_device_reset_and_generic_failures_are_rejected() {
    PresentCapture reset_capture;
    reset_capture.results[0] = DXGI_ERROR_DEVICE_RESET;
    reset_capture.result_count = 1;
    overlay::detail::PresentBoundary reset_boundary(
        adapter_for(reset_capture));
    expect(!reset_boundary.present(0, 0) &&
               reset_boundary.last_error() ==
                   "IDXGISwapChain::Present 失败，HRESULT=0x887A0007 "
                   "(DXGI_ERROR_DEVICE_RESET)。",
           "DEVICE_RESET 必须保留具体 HRESULT 与符号名");

    PresentCapture generic_capture;
    generic_capture.results[0] = E_FAIL;
    generic_capture.result_count = 1;
    overlay::detail::PresentBoundary generic_boundary(
        adapter_for(generic_capture));
    expect(!generic_boundary.present(0, 0) &&
               generic_boundary.last_error() ==
                   "IDXGISwapChain::Present 失败，HRESULT=0x80004005。",
           "Present seam 必须拒绝所有 FAILED HRESULT，而非只枚举设备错误");
}

void test_programmatic_shutdown_preserves_terminal_error_visibility() {
    expect(
        !overlay::detail::should_post_quit_on_main_window_destroy(true) &&
            overlay::detail::should_post_quit_on_main_window_destroy(false),
        "程序化 shutdown 销毁主窗口时不得投递 WM_QUIT 终止后续错误框，"
        "非程序化销毁仍必须通知 App 退出");
}

void test_extended_key_name_lparam() {
    constexpr std::uint32_t kEndAndNum1ScanCode = 0x4fU;
    const auto end_data = overlay::detail::make_key_name_lparam(
        kEndAndNum1ScanCode, true);
    const auto num1_data = overlay::detail::make_key_name_lparam(
        kEndAndNum1ScanCode, false);
    expect(end_data == 0x014f0000U && num1_data == 0x004f0000U,
           "End 必须通过 bit 24 与相同扫描码的小键盘 Num 1 区分");
}

void test_metric_history_order_and_overwrite() {
    overlay::detail::MetricHistory<3> history;
    expect(history.empty() && history.size() == 0,
           "新历史环必须为空");

    history.push(1.0f);
    history.push(2.0f);
    history.push(3.0f);
    expect(history.size() == 3 &&
               nearly_equal(history.at(0), 1.0f) &&
               nearly_equal(history.at(2), 3.0f),
           "未满容量时必须按时间顺序读取样本");

    history.push(4.0f);
    expect(history.size() == 3 &&
               nearly_equal(history.at(0), 2.0f) &&
               nearly_equal(history.at(1), 3.0f) &&
               nearly_equal(history.at(2), 4.0f),
           "满载后必须覆盖最旧样本并保持时间顺序");
    expect(nearly_equal(history.latest(), 4.0f) &&
               nearly_equal(history.maximum(0.5f), 4.0f),
           "最新值和动态上界必须反映保留样本");
}

void test_metric_history_clear_and_floor() {
    overlay::detail::MetricHistory<2> history;
    history.push(0.25f);
    expect(nearly_equal(history.maximum(1.0f), 1.0f),
           "动态上界不得低于调用方提供的显示下限");
    history.clear();
    expect(history.empty() && history.size() == 0 &&
               nearly_equal(history.latest(), 0.0f) &&
               nearly_equal(history.at(0), 0.0f),
           "清空后不得暴露上一轮运行的历史值");
}

void test_preview_geometry_mapping() {
    const auto fitted = overlay::detail::fit_preview_size(
        512, 256, 400.0f, 300.0f);
    expect(nearly_equal(fitted.width, 400.0f) &&
               nearly_equal(fitted.height, 200.0f),
           "预览显示区域必须等比缩小且不得超过可用空间");
    const auto original = overlay::detail::fit_preview_size(
        320, 320, 500.0f, 500.0f);
    expect(nearly_equal(original.width, 320.0f) &&
               nearly_equal(original.height, 320.0f),
           "预览纹理不得为填满面板而放大低分辨率图像");

    const auto point = overlay::detail::map_preview_point(
        100.0f, 50.0f, 0.5f, 0.25f);
    expect(nearly_equal(point.x, 50.0f) &&
               nearly_equal(point.y, 12.5f),
           "ROI 点必须按横纵独立比例映射到显示区域");
    const auto rectangle = overlay::detail::map_preview_rect(
        120.0f, -20.0f, -10.0f, 200.0f,
        0.5f, 0.5f, 50.0f, 60.0f);
    expect(nearly_equal(rectangle.x1, 0.0f) &&
               nearly_equal(rectangle.y1, 0.0f) &&
               nearly_equal(rectangle.x2, 50.0f) &&
               nearly_equal(rectangle.y2, 60.0f),
           "预览框必须规范化反向坐标并裁剪到图像边界");
}

void test_detached_preview_geometry() {
    const auto enlarged = overlay::detail::fit_detached_preview_size(
        320, 320, 640.0f, 480.0f);
    expect(nearly_equal(enlarged.width, 480.0f) &&
               nearly_equal(enlarged.height, 480.0f),
           "独立预览应保持宽高比并允许放大到客户区");

    const auto fitted = overlay::detail::fit_detached_preview_size(
        1280, 720, 640.0f, 480.0f);
    expect(nearly_equal(fitted.width, 640.0f) &&
               nearly_equal(fitted.height, 360.0f),
           "独立预览缩小时必须保持宽高比");

    const auto invalid = overlay::detail::fit_detached_preview_size(
        0, 320, 640.0f, 480.0f);
    expect(nearly_equal(invalid.width, 0.0f) &&
               nearly_equal(invalid.height, 0.0f),
           "独立预览必须拒绝非法图像尺寸");
}

void test_preview_subscription_state() {
    using overlay::detail::preview_subscription_required;
    expect(!preview_subscription_required(false, false, false),
           "没有可见预览面时不得订阅 Runtime 画面");
    expect(!preview_subscription_required(false, true, false),
           "仅请求内嵌预览时，离开检测页必须取消订阅");
    expect(preview_subscription_required(true, true, false),
           "检测页内嵌预览开启时必须订阅 Runtime 画面");
    expect(preview_subscription_required(false, false, true) &&
               preview_subscription_required(false, true, true),
           "独立窗口开启后切换标签页必须保持 Runtime 预览订阅");
}

void test_detached_preview_refresh_state() {
    using overlay::detail::detached_preview_content_changed;
    expect(!detached_preview_content_changed(false, 0, false, 0),
           "持续等待画面时不得按主 UI 帧率重复重绘");
    expect(detached_preview_content_changed(false, 0, true, 1),
           "首个预览帧到达时必须刷新独立窗口");
    expect(!detached_preview_content_changed(true, 7, true, 7),
           "同一预览序号不得重复重绘");
    expect(detached_preview_content_changed(true, 7, true, 8),
           "新预览序号到达时必须刷新独立窗口");
    expect(detached_preview_content_changed(true, 8, false, 0),
           "Runtime 停止后必须清除独立窗口中的旧画面");
}

void test_detection_role_mapping() {
    using overlay::detail::DetectionRole;
    const std::array<int, 2> persons{0, 2};
    const std::array<int, 2> heads{1, 2};
    expect(overlay::detail::classify_detection_role(0, persons, heads) ==
               DetectionRole::PERSON,
           "配置中的身体类别必须映射为 PERSON");
    expect(overlay::detail::classify_detection_role(1, persons, heads) ==
               DetectionRole::HEAD &&
               overlay::detail::classify_detection_role(2, persons, heads) ==
                   DetectionRole::HEAD,
           "头部类别必须映射为 HEAD，且与身体列表重叠时头部优先");
    expect(overlay::detail::classify_detection_role(3, persons, heads) ==
               DetectionRole::OTHER,
           "未配置类别必须映射为 OTHER");
    const std::span<const int> empty_persons;
    expect(overlay::detail::classify_detection_role(
               3, empty_persons, heads) == DetectionRole::PERSON,
           "身体类别列表为空时，非头部类别必须按身体处理");
}

void test_hotkey_capture_state_machine() {
    using overlay::detail::HotkeyCaptureResultType;
    overlay::detail::HotkeyCaptureState state;
    std::array<bool, 256> keys{};
    keys[0x01] = true;
    overlay::detail::begin_hotkey_capture(state, keys);
    auto result = overlay::detail::update_hotkey_capture(state, keys);
    expect(result.type == HotkeyCaptureResultType::NONE && state.active,
           "进入捕获时不得把点击按钮的鼠标左键误记为绑定");

    keys[0x01] = false;
    overlay::detail::update_hotkey_capture(state, keys);
    keys[0x05] = true;
    result = overlay::detail::update_hotkey_capture(state, keys);
    expect(result.type == HotkeyCaptureResultType::ASSIGNED &&
               result.virtual_key == 0x05 && !state.active,
           "鼠标侧键上升沿必须可作为快捷键绑定");

    keys = {};
    overlay::detail::begin_hotkey_capture(state, keys);
    keys[0x1B] = true;
    result = overlay::detail::update_hotkey_capture(state, keys);
    expect(result.type == HotkeyCaptureResultType::CLEARED && !state.active,
           "捕获期间按 Esc 必须清空绑定并退出捕获");
}

void test_output_arm_requires_input_health() {
    using overlay::detail::output_arm_available;
    expect(output_arm_available(true, false, true, true, false),
           "所有安全门满足时必须允许请求武装");
    expect(!output_arm_available(true, false, true, false, false),
           "输入健康未验证时必须禁用武装");
    expect(!output_arm_available(false, false, true, true, false) &&
               !output_arm_available(true, true, true, true, false) &&
               !output_arm_available(true, false, false, true, false) &&
               !output_arm_available(true, false, true, true, true),
           "运行、急停、配置和重载任一门关闭时都不得武装");
}

void test_delay_compensation_tooltip_states_new_app_default() {
    const auto source_path = std::filesystem::path(__FILE__)
                                 .parent_path()
                                 .parent_path() /
                             "Xen/overlay/overlay.cpp";
    std::ifstream input(source_path, std::ios::binary);
    const bool source_opened = input.is_open();
    const std::string source{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    constexpr std::string_view kLabel = "\"启用延迟补偿\"";
    constexpr std::string_view kToggle =
        "\"##enable_delay_compensation\"";
    constexpr std::string_view kExpectedDefault =
        "新生成的 App 配置默认开启。";

    const std::size_t label = source.find(kLabel);
    const std::size_t toggle = label == std::string::npos
        ? std::string::npos
        : source.find(kToggle, label);
    const std::string_view tooltip =
        label != std::string::npos && toggle != std::string::npos
        ? std::string_view(source).substr(label, toggle - label)
        : std::string_view{};
    expect(source_opened && label != std::string::npos &&
               source.find(kLabel, label + kLabel.size()) ==
                   std::string::npos &&
               toggle != std::string::npos &&
               tooltip.find(kExpectedDefault) != std::string_view::npos &&
               tooltip.find("默认关闭") == std::string_view::npos,
           "延迟补偿 tooltip 必须说明新生成的 App 配置默认开启，"
           "且不得继续声称默认关闭");
}

} // namespace

int main() {
    test_present_success_statuses_preserve_submission();
    test_present_device_removed_failure_is_owned_and_latched();
    test_present_device_reset_and_generic_failures_are_rejected();
    test_programmatic_shutdown_preserves_terminal_error_visibility();
    test_extended_key_name_lparam();
    test_metric_history_order_and_overwrite();
    test_metric_history_clear_and_floor();
    test_preview_geometry_mapping();
    test_detached_preview_geometry();
    test_preview_subscription_state();
    test_detached_preview_refresh_state();
    test_detection_role_mapping();
    test_hotkey_capture_state_machine();
    test_output_arm_requires_input_health();
    test_delay_compensation_tooltip_states_new_app_default();
    if (failures != 0) {
        std::cerr << "Overlay 测试失败数: " << failures << '\n';
        return 1;
    }
    std::cout << "Overlay 测试全部通过。\n";
    return 0;
}
