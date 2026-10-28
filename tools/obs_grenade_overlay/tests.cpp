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
    check(!model.update(selection, reference, catalog, "http://host:5015").visible, "PgDn新资源不能复活已隐藏提示");
    selection["guidance"]["state"] = "aligning";
    check(!model.update(selection, reference, catalog, "http://host:5015").visible, "重新找点不能替代新的人工选择");
    selection["request_id"] = "4"; selection["guidance"]["state"] = "selected";
    check(model.update(selection, reference, catalog, "http://host:5015").visible, "新人工选择重新显示");
    reference["revision"] = "r3";
    view = model.update(selection, reference, catalog, "http://host:5015");
    check(view.visible && view.payload["aim_url"].get<std::string>().ends_with("revision=r3"), "PgDn更新当前可见卡片");
    auto single = reference;
    single["ready"] = false; single["aim_ready"] = false; single["standing_ready"] = true;
    view = model.update(selection,single,catalog,"http://host:5015");
    check(view.visible && view.payload.contains("standing_url") && !view.payload.contains("aim_url"),
        "PgUp独立站位图可显示，瞄点保持待采集");
    single["ready"] = true; single["aim_ready"] = true; single["standing_ready"] = false;
    view = model.update(selection,single,catalog,"http://host:5015");
    check(view.visible && !view.payload.contains("standing_url") && view.payload.contains("aim_url"),
        "PgDn独立瞄点图可显示，不要求先采站位");
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
    const auto relative_page = std::filesystem::relative(std::filesystem::path(__FILE__));
    const auto page_url = xen_obs::file_url(relative_page);
    check(page_url.starts_with("file:///") && page_url.size() > 10 && page_url[9] == ':' &&
          page_url.find("/../") == std::string::npos && page_url.ends_with("/tools/obs_grenade_overlay/tests.cpp"),
          "OBS相对资源路径必须转换为带盘符的绝对文件URL");

    xen_obs::CaptureJobState capture;
    const Json sent{{"terminal", true}, {"state", "sent"}, {"message", "采集完成，已同步到辅机"}};
    check(capture.start(1000) && capture.running(), "空闲时允许一次采集");
    check(!capture.start(2000), "采集中再次按PgDn不排队、不重置超时");
    check(!capture.poll(30999, std::nullopt, sent) && capture.running(), "子进程未结束时不凭文件提前宣称完成");
    auto message = capture.poll(31000, std::nullopt, Json::object());
    check(message && message->find("超时") != std::string::npos && !capture.running(), "30秒超时结束等待");
    check(!capture.poll(32000, 0, sent), "超时后的晚到退出不重复发布成功");
    check(capture.start(33000), "上次结束后下一次PgDn可重新采集");
    message = capture.poll(34000, 0, sent);
    check(message && *message == sent["message"].get<std::string>() && !capture.running(), "退出0且sent终态才发布成功");
    capture.start(35000);
    message = capture.poll(36000, 4, sent);
    check(message && message->find("异常退出") != std::string::npos && *message != sent["message"].get<std::string>(),
          "sent文件不能覆盖子进程异常退出");
    capture.start(37000);
    message = capture.poll(38000, 0, Json{{"state", "sent"}, {"message", "成功"}});
    check(message && message->find("没有完成回执") != std::string::npos, "缺少terminal不能宣称成功");
    capture.start(39000);
    message = capture.poll(40000, 0, Json::object());
    check(message && message->find("没有完成回执") != std::string::npos, "缺少状态文件不能宣称成功");
    capture.start(41000);
    Json pending{{"terminal", true}, {"state", "pending"}, {"message", "截图已保存，辅机断线，等待重发"},
        {"package", "capture/t1.zip"}};
    const auto original_pending = pending;
    message = capture.poll(42000, 2, pending);
    check(message && *message == pending["message"].get<std::string>() && pending == original_pending && !capture.running(),
          "待发送原说明及资源数据保持，不把pending当作成功");
    capture.start(43000);
    const Json failed{{"terminal", true}, {"state", "failed"}, {"message", "未取得游戏画面"}};
    message = capture.poll(44000, 1, failed);
    check(message && *message == failed["message"].get<std::string>(), "失败保留采集工具原说明");

    xen_obs::MenuSelectionState menu;
    Json choice{{"state", "selected"}, {"generation", "menu-g1"}, {"request_id", "1"},
        {"plan_id", "t1.json"}, {"actual_plan_id", "t1.json"}, {"guidance", {{"plan_id", "t1.json"}}}};
    check(!menu.should_close(choice), "菜单未打开时不处理选择关闭");
    menu.open(choice);
    check(!menu.should_close(choice), "打开菜单不被已有选择立即关闭");
    choice["request_id"] = "2"; choice["state"] = "pending";
    check(!menu.should_close(choice), "新选择尚在载入时保持菜单");
    choice["state"] = "failed";
    check(!menu.should_close(choice), "选择失败保留菜单供重试");
    choice["state"] = "selected"; choice["actual_plan_id"] = "old.json";
    check(!menu.should_close(choice), "回执尚未匹配实际方案不关闭菜单");
    choice["actual_plan_id"] = "t1.json"; choice["guidance"]["plan_id"] = "old.json";
    check(!menu.should_close(choice), "引导仍属于旧方案不关闭菜单");
    choice["guidance"]["plan_id"] = "t1.json";
    check(menu.should_close(choice), "同方案新请求实际成功后关闭菜单");
    menu.reset();
    check(!menu.should_close(choice), "菜单重置后旧完成回执不产生关闭动作");
    menu.open(choice); choice["request_id"] = "";
    check(!menu.should_close(choice), "缺少请求身份不当作人工选择");
    menu.open(Json()); choice["request_id"] = "3";
    check(!menu.should_close(choice), "无publication打开时首个旧selected只建立基线");
    check(!menu.should_close(choice), "建立空快照基线后旧回执持续保持菜单");
    choice["request_id"] = "4";
    check(menu.should_close(choice), "空快照建立基线后新选择可关闭");
    menu.open(Json()); choice["request_id"] = "5"; choice["state"] = "pending";
    check(!menu.should_close(choice), "无publication打开后首先看到pending保持菜单");
    choice["state"] = "selected";
    check(menu.should_close(choice), "打开后首个pending以相同请求完成时可关闭");
    choice["state"] = "pending"; menu.open(choice); choice["state"] = "selected";
    check(!menu.should_close(choice), "打开之前已经存在的pending完成不能误关闭");
    return failures ? 1 : 0;
}
