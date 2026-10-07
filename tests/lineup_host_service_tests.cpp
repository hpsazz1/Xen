#include "lineup/service.h"
#include "lineup/host_capture_internal.h"
#include <fstream>
#include <iostream>
#include <opencv2/imgcodecs.hpp>
#include <thread>

using namespace lineup;
namespace {
void check(bool value, const char *reason) {
    if (!value) throw std::runtime_error(reason);
}
CommandResult command(Service &service, Json request) {
    const auto state = service.state();
    static std::uint64_t sequence = 0;
    request["epoch"] = state.at("epoch");
    request["revision"] = state.at("revision");
    request["request_id"] = "host-service-" + std::to_string(++sequence);
    return service.command(request);
}
Json send(Service &service, Json request) {
    const auto response = command(service, std::move(request));
    check(response.status == 200, "生产命令被意外拒绝");
    return response.json;
}
Json profile() {
    return {{"schema", 1}, {"source_id", "fixture-ndi"}, {"source_width", 640}, {"source_height", 480},
        {"map", "de_dust2"}, {"team", "CT"}, {"grenade", "smoke"}, {"throw_instructions", "左键投掷"},
        {"throw_action", {{"schema", 1}, {"type", "phases"}, {"phases", Json::array({
            {{"buttons", Json::array({"left"})}, {"movement", Json::array()}, {"jump", false}, {"duration_ms", nullptr}},
            {{"buttons", Json::array()}, {"movement", Json::array()}, {"jump", false}, {"duration_ms", nullptr}}})}}}};
}
Json profile_command() {
    auto result = profile();
    for (const auto key : {"schema", "source_id", "source_width", "source_height"}) result.erase(key);
    result["action"] = "host_profile";
    return result;
}
cv::Mat full_image() {
    cv::Mat result(480, 640, CV_8UC3);
    cv::RNG random(572);
    random.fill(result, cv::RNG::UNIFORM, 0, 255);
    return result;
}
HostCaptureBundle publish(const std::filesystem::path &inbox, const std::string &id, const Json &settings = profile()) {
    HostCaptureBundle bundle;
    std::string error;
    check(make_host_capture_bundle(full_image(), settings, id, "2026-10-07T00:00:00.000Z", bundle, error),
          "纯像素夹具无法生成截图包");
    std::filesystem::path published;
    check(write_host_capture_bundle(inbox / "captures", bundle, published, error), "截图包夹具原子发布失败");
    return bundle;
}
void live(Service &service, std::uint64_t sequence, bool verified = false, int width = 640) {
    CapturedFrame frame;
    frame.width = frame.height = frame.encoded_width = frame.encoded_height = 320;
    frame.source_width = width;
    frame.source_height = 480;
    frame.roi_x = (width - 320) / 2;
    frame.roi_y = 80;
    frame.source_mapping_verified = verified;
    frame.bgr = full_image()(cv::Rect(160, 80, 320, 320)).clone();
    frame.timing.sequence = sequence;
    frame.timing.captured_at = Clock::now();
    service.submit(frame, "fixture-receiver");
}
Json settle(Service &service, const std::string &status) {
    for (int attempt = 0; attempt < 300; ++attempt) {
        service.poll_host_captures();
        const auto state = service.state();
        if (!state.at("busy").get<bool>() && state.at("host_capture").at("status") == status) return state;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error("主机导入未达到预期状态：" + service.state().at("host_capture").dump());
}
Json observe(Service &service) {
    for (int attempt = 0; attempt < 300; ++attempt) {
        auto state = service.state();
        if (!state.at("preview").empty()) return state;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error("观察帧等待超时");
}
Json read_json(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    Json result;
    input >> result;
    return result;
}
std::size_t reference_count(const std::filesystem::path &root) {
    std::size_t count = 0;
    for (const auto &entry : std::filesystem::directory_iterator(root))
        if (entry.is_directory() && std::filesystem::exists(entry.path() / "raw.png")) ++count;
    return count;
}
void configure(Service &service, const std::filesystem::path &inbox) {
    check(service.enable_host_capture(inbox, {640, 480}), "合法主机收件配置失败");
    send(service, {{"action", "start"}});
}
} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("xen-host-service-test-" + std::to_string(Clock::now().time_since_epoch().count()));
    try {
        const auto catalog_root = root / "catalog", inbox = root / "inbox";
        std::string recipe_id;
        {
            Service service(catalog_root, ReferenceMode::ROI, "fixture-ndi");
            check(command(service, profile_command()).status == 409, "未显式启用的服务拒绝主机配置");
            service.poll_host_captures();
            check(service.state()["host_capture"]["enabled"] == false, "普通入口默认禁用主机导入");
            configure(service, inbox);
            check(!service.enable_host_capture(root / "other", {640, 480}), "收件路径不能二次更换");
            for (const auto fields : {Json{{"path", "E:/outside"}}, Json{{"source_id", "forged"}},
                    Json{{"source_width", 1920}}, Json{{"team", "TERRORIST"}}, Json{{"map", false}},
                    Json{{"name", 42}}, Json{{"throw_action", Json{{"schema", 2}}}}}) {
                auto request = profile_command();
                request.update(fields);
                const auto before = service.state();
                const auto result = command(service, request);
                check(result.status == 400 && result.json["revision"] == before["revision"] &&
                    !std::filesystem::exists(inbox / "profile.json"), "无效主机配置不能产生文件或修改版本");
            }
            send(service, profile_command());
            check(read_json(inbox / "profile.json") == profile(), "CLI 来源和表单配置正确原子保存");
            auto update = profile_command();
            update["throw_instructions"] = "更新后的投掷说明";
            send(service, update);
            check(read_json(inbox / "profile.json")["throw_instructions"] == "更新后的投掷说明",
                  "同一主机配置文件可原子替换");
            send(service, profile_command());
            const auto bundle = publish(inbox, "capture-first");
            service.poll_host_captures();
            check(service.state()["host_capture"]["status"] == "waiting_geometry" &&
                  service.state()["recipes"].empty(), "没有实际接收帧不能导入");
            live(service, 1, false, 800);
            service.poll_host_captures();
            check(service.state()["host_capture"]["status"] == "waiting_geometry", "实时几何不匹配不能导入");
            live(service, 2);
            service.disconnect("夹具断连");
            service.poll_host_captures();
            check(service.state()["host_capture"]["status"] == "waiting_geometry", "断连不能复用旧几何");
            live(service, 3);
            const auto state = settle(service, "imported");
            check(state["recipes"].size() == 1 && state["mode"] == "browse" && !state["locating"].get<bool>(),
                  "导入只保存并切换浏览");
            const auto recipe = state["recipes"][0];
            recipe_id = recipe.at("id").get<std::string>();
            check(state["locked_id"] == recipe_id && recipe["name"] == "主机采集 capture-first" &&
                  recipe["target"] == "当前瞄点" && recipe["grenade"] == "smoke" && !recipe["draft"].get<bool>(),
                  "导入默认名字和目标并锁定完整配方");
            check(recipe["throw_action"] == profile()["throw_action"] &&
                  !recipe["action_status"]["design"]["timing_complete"].get<bool>() &&
                  service.control_request().mode == control::Mode::CANCEL,
                  "未填写投掷时长不会被导入或锁定变成定位输出");
            const auto metadata = read_json(catalog_root / recipe_id / "reference.json");
            check(metadata["captured_steady_ns"].is_null() && metadata["source_time_valid"] == false &&
                  metadata["source_timestamp_valid"] == false && metadata["source_sequence_valid"] == false &&
                  metadata["source_mapping_verified"] == false && metadata["capture_origin"] == "desktop_duplication",
                  "静态主机照片不能伪造 NDI 时钟或已验证映射");
            const auto raw = service.reference(recipe_id), full = service.reference("full:" + recipe_id),
                       overview = service.reference("overview:" + recipe_id);
            check(raw && full && overview && cv::norm(cv::imdecode(*raw, cv::IMREAD_COLOR), bundle.roi, cv::NORM_INF) == 0 &&
                  cv::norm(cv::imdecode(*full, cv::IMREAD_COLOR), bundle.full, cv::NORM_INF) == 0 &&
                  cv::norm(cv::imdecode(*overview, cv::IMREAD_COLOR), bundle.overview, cv::NORM_INF) == 0,
                  "clean320、全屏原图和标框展示图分别保真");
            check(recipe["overview_url"] == "/api/overview/" + recipe_id && recipe["full_url"] == "/api/full/" + recipe_id,
                  "全屏图片 URL 可供网页映射");
            for (int attempt = 0; attempt < 4; ++attempt) service.poll_host_captures();
            check(service.state()["recipes"].size() == 1 && reference_count(catalog_root) == 1,
                  "同一截图包不会重复导入或留下额外参考");

            publish(inbox, "capture-after-f8");
            service.set_execution_status({{"state", "holding"}, {"active", true}, {"cleanup_required", true}});
            service.poll_host_captures();
            check(service.state()["host_capture"]["status"] == "waiting_idle" && service.state()["recipes"].size() == 1,
                  "F7 不打断 Runtime 持有输出");
            service.reset_control("夹具 Runtime 断连");
            service.poll_host_captures();
            check(service.state()["execution"]["state"] == "unknown" &&
                  service.state()["host_capture"]["status"] == "waiting_idle",
                  "输出未确认结束时断连不能将导入状态降为空闲");
            service.set_execution_status({{"state", "unknown"}, {"active", false}, {"cleanup_required", false}});
            service.poll_host_captures();
            check(service.state()["host_capture"]["status"] == "waiting_idle", "物理结果未知不能开放导入");
            service.set_execution_status({{"state", "idle"}, {"active", false}});
            check(service.request_locate(42), "明确序号定位事件被接受");
            bool preparing = false;
            check(service.control_request(&preparing).mode == control::Mode::CANCEL && preparing,
                  "首次观察前的准备窗口可由发布端原子辨认");
            live(service, 4);
            observe(service);
            const auto locate = service.control_request(&preparing);
            check(locate.mode == control::Mode::LOCATE && locate.trigger_sequence == 42 && !preparing,
                  "LOCATE 携带该次触发序号并结束准备窗口");
            const auto observation = service.control_request();
            check(observation.mode == control::Mode::OBSERVATION && observation.trigger_sequence == 42,
                  "同次观察保留触发序号且不重复 LOCATE");
            const auto generation = locate.observation.identity.selection_generation;
            Json completed = {{"state", "aligned"}, {"active", false}, {"cleanup_required", false},
                {"trigger_sequence", 41}, {"selection_generation", generation}};
            service.set_execution_status(completed);
            check(service.state()["locating"] == true, "旧序号终态不能结束新定位");
            completed["trigger_sequence"] = 42;
            completed["selection_generation"] = generation + 1;
            service.set_execution_status(completed);
            check(service.state()["locating"] == true, "错误配方代次终态不能结束定位");
            completed["selection_generation"] = generation;
            completed["state"] = "unknown";
            service.set_execution_status(completed);
            check(service.state()["locating"] == true, "未知终态保持定位阻断");
            completed["state"] = "aligned";
            service.set_execution_status(completed);
            check(service.state()["locating"] == false && service.control_request().trigger_sequence == 0,
                  "F8 的准确终态关闭旧定位并清除触发序号");
            live(service, 5);
            settle(service, "imported");
            check(service.state()["recipes"].size() == 2 && !service.state()["locating"].get<bool>(),
                  "F8 完成后下一次 F7 无需网页取消");
            check(service.request_locate(43), "新的定位事件被接受");
            service.set_execution_status(completed);
            check(service.state()["locating"] == true, "延迟到达的上次终态不会撤销下一次定位");
            send(service, {{"action", "cancel"}});
            check(service.control_request(&preparing).mode == control::Mode::CANCEL && !preparing,
                  "真正取消不能被当成准备窗口跳过");
        }
        {
            Service restored(catalog_root, ReferenceMode::ROI, "fixture-ndi");
            configure(restored, inbox);
            check(restored.state()["host_capture"]["profile"] == profile() && restored.state()["recipes"].size() == 2,
                  "重启恢复配置和完整目录");
            live(restored, 1);
            for (int attempt = 0; attempt < 4; ++attempt) restored.poll_host_captures();
            check(restored.state()["recipes"].size() == 2 && !restored.state()["busy"].get<bool>() &&
                  restored.control_request().mode == control::Mode::CANCEL,
                  "重启通过目录中的截图 ID 去重且不自动定位");
            check(restored.reference("full:" + recipe_id).has_value(), "重启仍能读取主机全屏图");
        }
        for (const auto kind : {"source", "polluted", "metadata"}) {
            const auto case_root = root / kind;
            Service service(case_root / "catalog", ReferenceMode::ROI, "fixture-ndi");
            configure(service, case_root / "inbox");
            auto settings = profile();
            if (std::string(kind) == "source") settings["source_id"] = "wrong-source";
            const auto bundle = publish(case_root / "inbox", "invalid", settings);
            const auto directory = case_root / "inbox" / "captures" / "invalid";
            if (std::string(kind) == "polluted") {
                auto pixels = bundle.roi.clone();
                pixels.at<cv::Vec3b>(0, 0)[0] ^= 255;
                check(cv::imwrite((directory / "roi.png").string(), pixels), "污染夹具写入失败");
            }
            if (std::string(kind) == "metadata") {
                auto metadata = bundle.metadata;
                metadata["roi"]["x"] = 159;
                std::ofstream output(directory / "capture.json", std::ios::binary);
                output << metadata.dump();
            }
            live(service, 1);
            const auto state = settle(service, "error");
            check(state["recipes"].empty() && !state["locating"].get<bool>() &&
                  reference_count(case_root / "catalog") == 0 && service.control_request().mode == control::Mode::CANCEL,
                  "错误来源、污染裁剪和几何篡改均不能生成可用配方");
        }
        {
            const auto case_root = root / "atomic-failure";
            Service service(case_root / "catalog", ReferenceMode::ROI, "fixture-ndi");
            configure(service, case_root / "inbox");
            publish(case_root / "inbox", "atomic-failure");
            std::filesystem::create_directory(case_root / "catalog" / "catalog.json");
            live(service, 1);
            const auto state = settle(service, "error");
            check(state["recipes"].empty() && reference_count(case_root / "catalog") == 0 &&
                  std::filesystem::is_directory(case_root / "catalog" / "catalog.json"),
                  "目录原子发布失败会回收本次参考且保留原目标");
            for (const auto &entry : std::filesystem::directory_iterator(case_root / "catalog"))
                check(!entry.path().filename().string().starts_with(".host-pending-"), "失败没有残留导入临时目录");
        }
        {
            Service service(root / "named-capture", ReferenceMode::ROI, "fixture-ndi");
            send(service, {{"action", "start"}});
            send(service, {{"action", "mode"}, {"value", "capture"}});
            check(command(service, {{"action", "capture"}, {"name", 3}}).status == 400,
                  "辅机采集新名称字段排队前拒绝非法类型");
            send(service, {{"action", "capture"}, {"name", "命名采集"}, {"target", "A"}, {"grenade", "smoke"}});
            live(service, 1);
            bool saved = false;
            for (int attempt = 0; attempt < 300; ++attempt) {
                const auto state = service.state();
                if (state["capture_status"] == "saved") {
                    check(state["recipes"][0]["name"] == "命名采集" && state["recipes"][0]["target"] == "A" &&
                          state["recipes"][0]["grenade"] == "smoke" && state["recipes"][0]["draft"] == false,
                          "辅机采集保存可选显示字段并计算草稿状态");
                    saved = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            check(saved, "辅机命名采集保存超时");
        }
        std::cout << "主机配置、同帧校验、静态导入、去重、原子重载和定位隔离通过\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << "；夹具目录=" << root << '\n';
        return 1;
    }
}
