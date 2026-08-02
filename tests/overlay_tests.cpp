#include "overlay/overlay_internal.h"

#include <cmath>
#include <iostream>
#include <string>

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

} // namespace

int main() {
    test_metric_history_order_and_overwrite();
    test_metric_history_clear_and_floor();
    test_preview_geometry_mapping();
    test_detached_preview_geometry();
    test_preview_subscription_state();
    test_detached_preview_refresh_state();
    if (failures != 0) {
        std::cerr << "Overlay 测试失败数: " << failures << '\n';
        return 1;
    }
    std::cout << "Overlay 测试全部通过。\n";
    return 0;
}
