// SPDX-License-Identifier: GPL-2.0-or-later
#include "model.h"
#include <iostream>

int main() {
    using xen_obs::Json;
    xen_obs::Model model;
    int failures = 0;
    const auto check = [&](bool value, const char* reason) {
        if (!value) { std::cerr << reason << '\n'; ++failures; }
    };
    Json selection{{"state", "selected"}, {"generation", "g1"}, {"request_id", "1"},
        {"plan_id", "t1.json"}, {"actual_plan_id", "t1.json"},
        {"guidance", {{"state", "selected"}, {"plan_id", "t1.json"}, {"message", "已选择"}}}};
    Json reference{{"ready", true}, {"plan_id", "t1.json"}, {"revision", "r1"}};
    Json catalog = Json::array({{{"id", "t1.json"}, {"name", "T1 VIP烟"}}});
    auto view = model.update(selection, reference, catalog, "http://host:5015");
    check(!view.visible, "启动已有selected只建立隐藏基线");
    selection["request_id"] = "2";
    view = model.update(selection, reference, catalog, "http://host:5015");
    check(view.visible && view.payload["title"] == "T1 VIP烟", "实际方案有图时显示正确标题");
    check(view.payload["aim_url"] == "http://host:5015/reference/aim.png?plan_id=t1.json&revision=r1",
          "图片绑定实际方案及资源版本");
    selection["guidance"]["state"] = "aligned";
    check(!model.update(selection, reference, catalog, "http://host:5015").visible, "对齐成功立即隐藏");
    selection["guidance"]["state"] = "selected";
    check(!model.update(selection, reference, catalog, "http://host:5015").visible, "投掷完成selected不重弹旧提示");
    selection["request_id"] = "3";
    check(model.update(selection, reference, catalog, "http://host:5015").visible, "重新点选同方案可重新显示");
    selection["guidance"]["state"] = "hidden";
    check(!model.update(selection, reference, catalog, "http://host:5015").visible, "开始投掷或取消立即隐藏");
    selection["guidance"]["state"] = "selected";
    check(!model.update(selection, reference, catalog, "http://host:5015").visible, "隐藏后旧selected快照不能重弹");
    selection["request_id"] = "3b";
    check(model.update(selection, reference, catalog, "http://host:5015").visible, "隐藏后重新人工点选可显示");
    selection["guidance"]["state"] = "aligned";
    model.update(selection, reference, catalog, "http://host:5015");
    selection["guidance"]["state"] = "selected"; reference["revision"] = "r2";
    check(!model.update(selection, reference, catalog, "http://host:5015").visible, "F10新资源不能复活已隐藏提示");
    selection["guidance"]["state"] = "aligning";
    check(!model.update(selection, reference, catalog, "http://host:5015").visible, "重新找点不能替代新的人工选择");
    selection["request_id"] = "4"; selection["guidance"]["state"] = "selected";
    check(model.update(selection, reference, catalog, "http://host:5015").visible, "新人工选择重新显示");
    reference["revision"] = "r3";
    view = model.update(selection, reference, catalog, "http://host:5015");
    check(view.visible && view.payload["aim_url"].get<std::string>().ends_with("revision=r3"), "F10更新当前可见卡片");
    model.reset();
    check(!model.update(selection, reference, catalog, "http://host:5015").visible, "重连不重播现存selected");
    selection["request_id"] = "5";
    view = model.update(selection, reference, catalog, "http://host:5015");
    check(view.visible, "重连后下一人工选择可以显示");
    reference["plan_id"] = "other.json";
    view = model.update(selection, reference, catalog, "http://host:5015");
    check(view.visible && !view.payload["ready"] && !view.payload.contains("aim_url"), "旧资源不能显示成当前图片");
    selection["state"] = "pending"; selection["request_id"] = "6";
    check(!model.update(selection, reference, catalog, "http://host:5015").visible, "切换未提交时不显示旧方案");
    selection["state"] = "selected"; reference["plan_id"] = "t1.json";
    selection["actual_plan_id"] = "previous.json";
    check(!model.update(selection, reference, catalog, "http://host:5015").visible, "新回执先到时等待实际方案发布");
    selection["actual_plan_id"] = "t1.json";
    check(model.update(selection, reference, catalog, "http://host:5015").visible, "新pending请求完成后显示");
    selection["state"] = "selected"; selection["guidance"]["plan_id"] = "other.json";
    check(!model.update(selection, reference, catalog, "http://host:5015").visible, "不一致引导身份隐藏");
    selection["actual_plan_id"] = "";
    check(!model.update(selection, reference, catalog, "http://host:5015").visible, "无有效实际方案隐藏");
    model.reset();
    Json idle{{"state", "idle"}, {"generation", "g2"}, {"request_id", ""}, {"plan_id", ""}, {"actual_plan_id", ""}};
    check(!model.update(idle, reference, catalog, "http://host:5015").visible, "初始idle保持隐藏");
    selection["generation"] = "g2"; selection["actual_plan_id"] = "t1.json"; selection["guidance"]["plan_id"] = "t1.json";
    check(model.update(selection, reference, catalog, "http://host:5015").visible, "idle之后首个人工选择显示");
    check(xen_obs::percent_encode("a&b\" c") == "a%26b%22%20c", "浏览器查询参数保持数据边界");
    check(!xen_obs::valid_plan_id("../a.json") && !xen_obs::valid_plan_id("C:\\a.json"), "拒绝路径充当方案ID");
    return failures ? 1 : 0;
}
