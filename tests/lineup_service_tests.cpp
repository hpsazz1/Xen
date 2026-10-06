#include "lineup/service.h"
#include <fstream>
#include <iostream>
#include <opencv2/imgproc.hpp>
#include <stdexcept>
#include <thread>
using namespace lineup;
void check(bool c, const char *why) {
    if (!c)
        throw std::runtime_error(why);
}
Json send(Service &s, Json c) {
    auto state = s.state();
    c["epoch"] = state["epoch"];
    c["revision"] = state["revision"];
    static int n = 0;
    c["request_id"] = std::to_string(++n);
    auto r = s.command(c);
    check(r.status == 200, "command rejected");
    return r.json;
}
Json wait_saved(Service &s) {
    for (int i = 0; i < 300; ++i) {
        auto j = s.state();
        if (j["capture_status"] == "error")
            throw std::runtime_error(j["error"].get<std::string>());
        if (!j["busy"].get<bool>() && j["capture_status"] == "saved")
            return j;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error("worker timeout");
}
CapturedFrame fixture() {
    CapturedFrame f;
    f.width = f.source_width = f.encoded_width = 640;
    f.height = f.source_height = f.encoded_height = 480;
    f.bgr = cv::Mat(480, 640, CV_8UC3);
    cv::RNG random(42);
    random.fill(f.bgr, cv::RNG::UNIFORM, 0, 255);
    f.timing.sequence = 1;
    f.timing.captured_at = Clock::now();
    return f;
}
int main() {
    auto root =
        std::filesystem::temp_directory_path() /
        ("xen-lineup-service-test-" + std::to_string(Clock::now().time_since_epoch().count()));
    try {
        std::string id;
        {
            Service s(root);
            auto initial = s.state();
            check(initial["recipes"].empty(), "empty catalog");
            Json command = {{"action", "start"},
                            {"epoch", initial["epoch"]},
                            {"revision", initial["revision"]},
                            {"request_id", "repeat"}};
            check(s.command(command).status == 200, "start");
            auto rev = s.state()["revision"];
            check(s.command(command).status == 200 && s.state()["revision"] == rev, "idempotency");
            command["request_id"] = "stale";
            check(s.command(command).status == 409, "stale revision");
            send(s, {{"action", "mode"}, {"value", "capture"}});
            send(s, {{"action", "capture"}, {"map", "fixture-map"}, {"region", "fixture-region"}});
            auto f = fixture();
            s.submit(f, "synthetic-test");
            f.bgr.setTo(cv::Scalar(0));
            auto result = wait_saved(s);
            check(result["recipes"].size() == 1, "capture count");
            id = result["recipes"][0]["id"];
            check(result["recipes"][0]["draft"] == true, "draft");
            check(s.reference(id).has_value(), "reference image");
            check(s.reference("standpoint:" + id).has_value(), "standpoint image");
            send(s, {{"action", "update"},
                     {"items", Json::array({{{"id", id},
                                             {"name", "Synthetic"},
                                             {"target", "fixture"},
                                             {"grenade", "smoke"}}})}});
            result = wait_saved(s);
            check(result["recipes"][0]["draft"] == false, "promote");
            check(result["recipes"][0]["recipe_version"] == 2, "recipe revision advances");
            send(s, {{"action", "mode"}, {"value", "browse"}});
            send(s, {{"action", "lock"}, {"id", id}});
            f = fixture();
            f.timing.sequence = 2;
            s.submit(f, "synthetic-test");
            for (int i = 0; i < 200 && s.state()["preview"].empty(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            result = s.state();
            check(!result["preview"].empty(), "preview published");
            check(result["preview"]["processing_ms"].get<double>() > 0 &&
                  result["preview"]["performance"]["feature_ms"].get<double>() > 0 &&
                  result["preview"]["performance"]["cache_misses"] == 1, "real round metrics");
            auto frame_id = result["preview"]["frame_id"].get<std::string>();
            check(s.preview(frame_id).has_value(), "same frame bytes");
            std::this_thread::sleep_for(std::chrono::milliseconds(1050));
            check(!s.preview(frame_id), "expired preview rejected");
            check(s.state()["preview"]["status"] == "expired", "expired status");
            auto before_disconnect = s.state();
            s.disconnect("test disconnect");
            check(s.command({{"epoch", before_disconnect["epoch"]},
                             {"revision", before_disconnect["revision"]},
                             {"request_id", "delayed-across-disconnect"},
                             {"action", "stop"}}).status == 409,
                  "disconnect rejects delayed commands");
            check(!s.preview(frame_id), "disconnect retract");
            check(s.state()["preview"].empty(), "disconnect state");
            send(s, {{"action", "mode"}, {"value", "capture"}});
            send(s, {{"action", "capture"}});
            f = fixture();
            f.timing.sequence = 2;
            s.submit(f, "synthetic-test");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            check(s.state()["capture_status"] == "waiting",
                  "duplicate sequence cannot fulfill capture");
            send(s, {{"action", "cancel"}});
            f = fixture();
            f.timing.sequence = 3;
            s.submit(f, "synthetic-test");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            check(s.state()["recipes"].size() == 1, "cancel pending capture");
            send(s, {{"action", "capture"}});
            s.disconnect("capture-disconnect");
            f = fixture();
            f.timing.sequence = 4;
            s.submit(f, "synthetic-test");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            check(s.state()["recipes"].size() == 1, "disconnect cancels pending capture");
            send(s, {{"action", "capture"}});
            for (int i = 0; i < 600 && s.state()["capture_status"] == "waiting"; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            check(s.state()["capture_status"] == "error", "capture deadline");
            send(s, {{"action", "capture"}});
            f = fixture();
            auto before_session_change = s.state();
            s.submit(f, "replacement-session");
            check(s.command({{"epoch", before_session_change["epoch"]},
                             {"revision", before_session_change["revision"]},
                             {"request_id", "delayed-across-session"},
                             {"action", "stop"}}).status == 409,
                  "session change rejects delayed commands");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            check(s.state()["recipes"].size() == 1, "session change cancels capture");
            check(s.state()["capture_status"] == "cancelled",
                  "session change explicit cancellation");
            send(s, {{"action", "scope"}, {"map", "fixture-map"}, {"region", "fixture-region"}});
            check(s.state()["scope"]["map"] == "fixture-map", "scope state");
            f = fixture();
            auto before_geometry = s.state();
            f.source_width *= 2;
            f.source_pixels_per_pixel_x = 2;
            f.timing.sequence = 2;
            s.submit(f, "replacement-session");
            check(s.command({{"epoch", before_geometry["epoch"]},
                             {"revision", before_geometry["revision"]},
                             {"request_id", "delayed-across-geometry"},
                             {"action", "stop"}}).status == 409,
                  "geometry change rejects delayed commands without capture request");
            f = fixture();
            for (int i = 3; i < 103; ++i) {
                f.timing.sequence = i;
                f.timing.captured_at = Clock::now();
                s.submit(f, "replacement-session");
            }
            check(s.state()["dropped"].get<unsigned long long>() > 0,
                  "latest-only overload drops pending frames");
            s.close();
        }
        {
            Service s(root);
            check(s.state()["recipes"].size() == 1, "restart persisted");
            check(s.reference(id).has_value(), "restart image");
            send(s, {{"action", "export"}});
            wait_saved(s);
            check(std::filesystem::exists(root / "exports"), "export");
            s.close();
        }
        {
            auto broken = root / "broken";
            std::filesystem::create_directories(broken);
            {
                std::ofstream f(broken / "catalog.json");
                f << "[{\"id\":\"../escape\"}]";
            }
            Service s(broken);
            check(!s.state()["error"].get<std::string>().empty(), "corrupt catalog reported");
            auto st = s.state();
            auto r = s.command({{"epoch", st["epoch"]},
                                {"revision", st["revision"]},
                                {"request_id", "blocked"},
                                {"action", "update"},
                                {"items", Json::array()}});
            check(r.status == 409, "corrupt catalog writes blocked");
            s.close();
        }
        {
            auto blocked = root / "write-failure";
            Service s(blocked);
            std::filesystem::create_directory(blocked / "catalog.json");
            send(s, {{"action", "start"}});
            send(s, {{"action", "mode"}, {"value", "capture"}});
            send(s, {{"action", "capture"}});
            auto f = fixture();
            s.submit(f, "write-failure-fixture");
            for (int i = 0; i < 300 && s.state()["capture_status"] != "error"; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            check(s.state()["capture_status"] == "error", "disk error reported");
            check(s.state()["recipes"].empty(), "disk error not successful capture");
            s.close();
        }
        {
            Json catalog;
            { std::ifstream f(root / "catalog.json"); f >> catalog; }
            auto base = catalog.at(0);
            catalog = Json::array();
            for (const auto *team : {"T", "CT", "ANY", "UNCLASSIFIED"}) {
                auto r = base;
                r["id"] = std::string("gsi-") + team;
                r["map"] = "DE_DUST2";
                r["team"] = team;
                if (std::string(team) == "UNCLASSIFIED") r.erase("team");
                catalog.push_back(r);
            }
            { std::ofstream f(root / "catalog.json"); f << catalog.dump(); }
            Service s(root);
            check(s.state()["unclassified_count"] == 1, "legacy recipe stays unclassified");
            weapon::WeaponSnapshot gsi;
            gsi.context_valid = true;
            gsi.identity_match = true;
            gsi.player_playing = true;
            gsi.local_team = weapon::Team::T;
            gsi.map_name = "de_dust2";
            gsi.status = weapon::Status::READY;
            gsi.received_at = Clock::now();
            gsi.valid_until = Clock::now() + std::chrono::seconds(5);
            gsi.context_epoch = 1;
            s.update_gsi(gsi);
            auto state = s.state();
            check(state["context"]["map"] == "de_dust2" && state["context"]["team"] == "T", "automatic context");
            check(state["recipes"][0]["compatible"] == true && state["recipes"][1]["compatible"] == false && state["recipes"][2]["compatible"] == true, "T CT ANY compatibility");
            check(state["recipes"][3]["compatibility"] == "unclassified", "legacy classification visible");
            send(s, {{"action", "lock"}, {"id", "gsi-T"}});
            gsi.context_epoch = 3;
            s.update_gsi(gsi);
            check(s.state()["locked_id"] == "", "unobserved context discontinuity retracts old lock");
            send(s, {{"action", "lock"}, {"id", "gsi-T"}});
            gsi.local_team = weapon::Team::CT;
            s.update_gsi(gsi);
            check(s.state()["locked_id"] == "", "team switch retracts incompatible lock");
            send(s, {{"action", "lock"}, {"id", "gsi-ANY"}});
            gsi.local_team = weapon::Team::T;
            s.update_gsi(gsi);
            check(s.state()["locked_id"] == "gsi-ANY", "ANY remains compatible across team switch");
            send(s, {{"action", "context"}, {"mode", "manual"}, {"map", "DE_DUST2"}, {"team", "CT"}});
            gsi.context_valid = false;
            gsi.identity_match = false;
            s.update_gsi(gsi, "receiver unavailable");
            check(s.state()["context"]["team"] == "CT", "manual context survives unavailable GSI");
            send(s, {{"action", "context"}, {"mode", "auto"}});
            check(s.state()["context"]["map"] == "" && s.state()["context"]["team"] == "UNKNOWN", "auto never reuses stale context");
            check(s.state()["recipes"][0]["compatibility"] == "context_unknown", "unknown team explicitly labeled");
            gsi.context_valid = true;
            gsi.identity_match = true;
            gsi.valid_until = Clock::now() + std::chrono::milliseconds(100);
            s.update_gsi(gsi);
            send(s, {{"action", "lock"}, {"id", "gsi-T"}});
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            check(s.state()["context"]["gsi_status"] == "EXPIRED" && s.state()["locked_id"] == "", "TTL expires without update tick");
            send(s, {{"action", "context"}, {"mode", "manual"}, {"map", "de_dust2"}, {"team", "UNKNOWN"}});
            send(s, {{"action", "lock"}, {"id", "gsi-UNCLASSIFIED"}});
            check(s.state()["locked_id"] == "gsi-UNCLASSIFIED", "legacy recipe supports explicit manual selection");
            s.close();
        }
        {
            Service s(root / "standpoint-scope");
            send(s, {{"action", "start"}});
            send(s, {{"action", "mode"}, {"value", "capture"}});
            auto capture = [&](const char *map, const char *team, const char *region, int sequence) {
                send(s, {{"action", "context"}, {"mode", "manual"}, {"map", map}, {"team", team}});
                send(s, {{"action", "capture"}, {"region", region}, {"standpoint_id", "shared"}});
                auto f = fixture(); f.timing.sequence = sequence;
                s.submit(f, "scope-fixture");
                return wait_saved(s)["recipes"].back();
            };
            auto a = capture("de_dust2", "T", "A", 1);
            auto b = capture("de_dust2", "T", "A", 2);
            check(a["standpoint_reference_id"] == b["standpoint_reference_id"], "same scoped standpoint shares reference");
            auto c = capture("de_mirage", "T", "A", 3);
            auto d = capture("de_dust2", "CT", "A", 4);
            auto e = capture("de_dust2", "T", "B", 5);
            check(a["standpoint_reference_id"] != c["standpoint_reference_id"] &&
                  a["standpoint_reference_id"] != d["standpoint_reference_id"] &&
                  a["standpoint_reference_id"] != e["standpoint_reference_id"], "standpoint namespace includes map team region");
            send(s, {{"action", "mode"}, {"value", "browse"}});
            auto f = fixture(); f.timing.sequence = 6;
            s.submit(f, "scope-fixture");
            for (int i = 0; i < 200 && s.state()["preview"].empty(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            check(s.state()["preview"]["processing_ms"].get<double>() > 0, "unlocked round is measured");
            s.close();
        }
        std::cout << "lineup_service_tests passed (synthetic fixtures only)\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << "; evidence root=" << root << '\n';
        return 1;
    }
}
