#include "lineup/service.h"
#include "lineup/practice_internal.h"
#include "lineup/action_internal.h"
#include "lineup/host_capture_internal.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cmath>
#include <deque>
#include <fstream>
#include <mutex>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <thread>
#include <unordered_set>
namespace lineup {
namespace {
std::string unique_id() {
    static std::atomic<unsigned long long> n{0};
    return std::to_string(Clock::now().time_since_epoch().count()) + "-" + std::to_string(++n);
}
std::vector<unsigned char> read_bytes(const std::filesystem::path &p) {
    std::ifstream f(p, std::ios::binary);
    if (!f)
        throw std::runtime_error("image unavailable");
    return {std::istreambuf_iterator<char>(f), {}};
}
void write_json(const std::filesystem::path &p, const Json &j) {
    auto temp = p;
    temp += "." + unique_id() + ".tmp";
    try {
        {
            std::ofstream f(temp, std::ios::binary);
            f << j.dump(2);
            f.flush();
            if (!f) throw std::runtime_error("write failed");
            f.close();
            if (f.fail()) throw std::runtime_error("close failed");
        }
        std::filesystem::rename(temp, p);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
        throw;
    }
}
void replace_json(const std::filesystem::path &path, const Json &value) {
    auto temporary = path;
    temporary += "." + unique_id() + ".tmp";
    try {
        write_json(temporary, value);
        if (!MoveFileExW(temporary.c_str(), path.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("atomic JSON replacement failed");
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}
void write_png(const std::filesystem::path &path, const cv::Mat &image) {
    std::vector<unsigned char> bytes;
    if (!cv::imencode(".png", image, bytes)) throw std::runtime_error("image encoding failed");
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.flush();
    if (!output) throw std::runtime_error("image write failed");
    output.close();
    if (output.fail()) throw std::runtime_error("image close failed");
}
bool host_safe_id(const std::string &id) {
    return !id.empty() && id.size() <= 96 && std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}
bool redirected(const std::filesystem::path &path) {
    const auto attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}
std::string lower_status(Status s) {
    auto v = std::string(status_name(s));
    for (auto &c : v)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return v;
}
bool valid_team(const std::string &v) {
    return v == "T" || v == "CT" || v == "ANY" || v == "UNCLASSIFIED";
}
std::string compatibility(const Json &r, const Json &context, const Json &scope) {
    auto map = context.value("map", "");
    if (map.empty()) map = scope.value("map", "");
    if (!map.empty() && weapon::canonical_map_id(r.value("map", "")) != weapon::canonical_map_id(map))
        return "map_mismatch";
    auto region = scope.value("region", "");
    if (!region.empty() && r.value("region", "") != region) return "region_mismatch";
    auto team = r.value("team", "UNCLASSIFIED");
    if (team == "UNCLASSIFIED") return "unclassified";
    auto current = context.value("team", "UNKNOWN");
    if (current == "UNKNOWN") return "context_unknown";
    if (team != "ANY" && team != current) return "team_mismatch";
    return "compatible";
}
bool selectable(const std::string &reason) {
    return reason == "compatible" || reason == "unclassified" || reason == "context_unknown";
}
} // namespace
struct Service::Impl {
    std::filesystem::path root;
    detail::PracticeStore practice;
    ReferenceMode frame_mode;
    std::string source_identity;
    bool locating = false;
    control::Request control_state;
    std::uint64_t sent_control_generation = 0;
    std::uint64_t locate_trigger_sequence = 0;
    Json execution_status = {{"state","unavailable"},{"reason","runtime_disconnected"},{"real_verified",false}};
    cv::Size last_encoded_size;
    double last_roi_x = 0, last_roi_y = 0;
    bool last_mapping_verified = false;
    bool live_geometry_available = false;
    Clock::time_point live_geometry_at{};
    bool host_enabled = false;
    std::filesystem::path host_inbox;
    cv::Size host_source_size;
    Json host_profile = Json::object();
    std::string host_status = "disabled", host_error, host_last_id;
    std::unordered_set<std::string> host_imported, host_rejected;
    std::mutex host_poll_mutex;
    std::optional<std::filesystem::directory_iterator> host_iterator;
    std::mutex mutex;
    std::condition_variable cv;
    std::thread worker;
    bool closed = false, running = false, busy = false, storage_valid = true;
    Clock::time_point capture_deadline{}, capture_requested{};
    std::string last_session;
    std::uint64_t last_sequence = 0;
    cv::Size last_size, last_source_size;
    double last_scale_x = 0, last_scale_y = 0;
    Json records = Json::array();
    struct Sample {
        std::string id;
        cv::Mat raw;
        std::vector<unsigned char> marked;
    };
    std::deque<Sample> samples;
    std::string mode = "browse", epoch = unique_id(), locked, source = "disconnected", error,
                export_path, capture_status = "idle";
    std::uint64_t revision = 0, generation = 0, dropped = 0, submitted = 0, capture_after = 0;
    Json recipes = Json::array(), suggestions = Json::array(), preview_state = Json::object();
    Json scope = {{"map", ""}, {"region", ""}};
    Json context = {{"mode", "auto"}, {"map", ""}, {"team", "UNKNOWN"},
                    {"auto_map", ""}, {"auto_team", "UNKNOWN"},
                    {"gsi_status", "UNAVAILABLE"}, {"age_ms", nullptr},
                    {"identity_confirmed", false}, {"reason", "GSI 不可用，可手动浏览选择"}};
    std::string lock_reason;
    Clock::time_point gsi_received{}, gsi_until{};
    std::uint64_t gsi_context_epoch = 0;
    bool search_limited = false;
    std::size_t search_cursor = 0;
    std::optional<Json> job, capture_request;
    struct Frame {
        CapturedFrame frame;
        std::string session;
        std::uint64_t serial, generation;
    };
    std::optional<Frame> pending;
    std::vector<unsigned char> preview_bytes;
    Clock::time_point preview_time{};
    std::deque<std::string> request_order;
    std::unordered_set<std::string> requests;
    explicit Impl(std::filesystem::path p, ReferenceMode fm, std::string identity)
        : root(std::move(p)), practice(root), frame_mode(fm), source_identity(std::move(identity)) {
        try {
            std::filesystem::create_directories(root);
            auto path = root / "catalog.json";
            if (std::filesystem::exists(path)) {
                std::ifstream f(path);
                f >> recipes;
                if (!recipes.is_array() || recipes.size() > 4096)
                    throw std::runtime_error("invalid catalog");
                for (auto &r : recipes) {
                    for (auto key : {"id", "reference_id", "standpoint_reference_id"}) {
                        auto v = r.at(key).get<std::string>();
                        if (v.empty() || v.size() > 96 ||
                            !std::all_of(v.begin(), v.end(), [](unsigned char c) {
                                return std::isalnum(c) || c == '-' || c == '_';
                            }))
                            throw std::runtime_error("invalid catalog path");
                    }
                    for (auto key : {"name", "target", "grenade", "map", "region", "standpoint_id"})
                        if (!r.at(key).is_string())
                            throw std::runtime_error("invalid catalog text");
                    if (!r.at("draft").is_boolean())
                        throw std::runtime_error("invalid catalog draft");
                    if (!r.contains("recipe_version")) r["recipe_version"] = 1;
                    if (!r["recipe_version"].is_number_unsigned() && !r["recipe_version"].is_number_integer())
                        throw std::runtime_error("invalid recipe version");
                    if (r["recipe_version"].get<std::int64_t>() < 1) throw std::runtime_error("invalid recipe version");
                    if (!r.contains("team")) r["team"] = "UNCLASSIFIED";
                    if (!r["team"].is_string() || !valid_team(r["team"].get<std::string>()))
                        throw std::runtime_error("invalid recipe team");
                    if (r.contains("host_capture_id")) {
                        if (!host_safe_id(r.at("host_capture_id").get<std::string>()) ||
                            !host_safe_id(r.at("host_reference_id").get<std::string>()))
                            throw std::runtime_error("invalid host capture identity");
                        host_imported.insert(r.at("host_capture_id").get<std::string>());
                    }
                    Engine e;
                    std::string why;
                    if (!e.load_reference(root / r.at("reference_id").get<std::string>(), why))
                        throw std::runtime_error("reference load: " + why);
                }
            }
        } catch (const std::exception &e) {
            recipes = Json::array();
            storage_valid = false;
            error = e.what();
        }
        worker = std::thread([this] { loop(); });
    }
    bool reference_compatible(const Json &r) const {
        return r.value("frame_mode", "full_frame") == (frame_mode == ReferenceMode::ROI ? "roi" : "full_frame") &&
            (frame_mode != ReferenceMode::ROI || (!source_identity.empty() && r.value("source_id", "") == source_identity));
    }
    void clear_preview() {
        locating = false;
        locate_trigger_sequence = 0;
        control_state = {};
        preview_bytes.clear();
        preview_state = Json::object();
        suggestions = Json::array();
        ++generation;
        pending.reset();
    }
    void invalidate_context_lock(const std::string &why, bool force = false) {
        if (locked.empty()) return;
        for (const auto &r : recipes) {
            if (r.at("id") == locked && (force || !selectable(compatibility(r, context, scope)))) {
                locked.clear();
                lock_reason = why;
                clear_preview();
                return;
            }
        }
    }
    void expire_context() {
        if (gsi_until == Clock::time_point{} || Clock::now() < gsi_until || context.value("auto_map", "").empty()) return;
        context["auto_map"] = "";
        context["auto_team"] = "UNKNOWN";
        context["identity_confirmed"] = false;
        context["gsi_status"] = "EXPIRED";
        if (context["mode"] == "auto") {
            context["map"] = "";
            context["team"] = "UNKNOWN";
            context["reason"] = "GSI 已过期，可手动浏览选择";
            scope["map"] = "";
            scope["region"] = "";
            invalidate_context_lock("GSI 上下文已过期，已撤销锁定", true);
            capture_request.reset();
            clear_preview();
        }
        ++revision;
    }
    Json snapshot() {
        expire_context();
        auto p = preview_state;
        if (!p.empty()) {
            auto age =
                std::chrono::duration<double, std::milli>(Clock::now() - preview_time).count();
            p["age_ms"] = age;
            if (age > 1000) {
                p["status"] = "expired";
                p["url"] = "";
            }
        }
        auto listed = recipes;
        Json eligible = Json::array();
        std::size_t unclassified = 0;
        for (auto &r : listed) {
            auto reason = reference_compatible(r) ? compatibility(r, context, scope) : "reference_mode_or_source_mismatch";
            r["team"] = r.value("team", "UNCLASSIFIED");
            r["compatibility"] = reason;
            std::optional<detail::ActionCapabilities> capabilities;
            if (execution_status.contains("capabilities")) {
                const auto &c = execution_status.at("capabilities");
                if (c.is_object()) capabilities = detail::ActionCapabilities{c.value("left_button",false), c.value("right_button",false), c.value("movement",false), c.value("jump",false)};
            }
            r["action_status"] = detail::action_status(r.value("throw_action", Json()), capabilities);
            r["compatible"] = selectable(reason);
            if (r["team"] == "UNCLASSIFIED") ++unclassified;
            if (!r.value("draft", true) && selectable(reason)) eligible.push_back(r.at("id"));
        }
        auto current_context = context;
        if (gsi_received != Clock::time_point{})
            current_context["age_ms"] = std::chrono::duration<double, std::milli>(Clock::now() - gsi_received).count();
        return {{"epoch", epoch},
                {"revision", revision},
                {"mode", mode},
                {"frame_mode", frame_mode == ReferenceMode::ROI ? "roi" : "full_frame"},
                {"source_id", source_identity},
                {"host_capture", {{"enabled", host_enabled}, {"profile", host_profile},
                                  {"status", host_status}, {"error", host_error}, {"last_id", host_last_id}}},
                {"practice", practice.snapshot()},
                {"execution", execution_status},
                {"locating", locating},
                {"location_status", !locating ? "idle" : p.value("status", "pending")},
                {"running", running},
                {"source_status", source},
                {"locked_id", locked},
                {"capture_status", capture_status},
                {"error", error},
                {"recipes", listed},
                {"eligible_ids", eligible},
                {"unclassified_count", unclassified},
                {"context", current_context},
                {"lock_reason", lock_reason},
                {"suggestions", suggestions},
                {"preview", p},
                {"dropped", dropped},
                {"busy", busy},
                {"export_path", export_path},
                {"scope", scope},
                {"search_limited", search_limited},
                {"searching", frame_mode == ReferenceMode::FULL_FRAME && running && locked.empty()}};
    }
    void host_result(const std::string &status, const std::string &why = "") {
        if (host_status == status && host_error == why) return;
        host_status = status;
        host_error = why;
        ++revision;
    }
    bool host_idle() const {
        if (locating || capture_request || execution_status.value("cleanup_required", false) ||
            execution_status.value("active", false)) return false;
        const auto state = execution_status.value("state", "unknown");
        if (state == "aligned")
            return execution_status.contains("active") && execution_status.at("active") == false;
        return state == "idle" || state == "unavailable" || state == "blocked" ||
               state == "completed" || state == "cancelled";
    }
    bool host_geometry_ready() const {
        return running && live_geometry_available && !last_session.empty() &&
            Clock::now() - live_geometry_at <= std::chrono::seconds(2) &&
            last_size == cv::Size(320, 320) && last_source_size == host_source_size &&
            last_encoded_size.width >= 320 && last_encoded_size.height >= 320 &&
            last_roi_x == (host_source_size.width - 320) / 2 &&
            last_roi_y == (host_source_size.height - 320) / 2 &&
            last_scale_x == 1.0 && last_scale_y == 1.0;
    }
    void import_host_capture(const Json &task, Json values, std::uint64_t token) {
        const auto capture_id = task.at("host_id").get<std::string>();
        HostCaptureBundle bundle;
        std::string why;
        if (!read_host_capture_bundle(host_inbox / "captures" / capture_id, bundle, why))
            throw std::runtime_error("host bundle: " + why);
        const auto &metadata = bundle.metadata;
        const auto &profile = metadata.at("profile");
        if (metadata.at("id") != capture_id || profile.at("source_id") != source_identity ||
            profile.at("source_width") != host_source_size.width ||
            profile.at("source_height") != host_source_size.height || bundle.full.size() != host_source_size)
            throw std::runtime_error("host source or dimensions mismatch");
        if (!detail::inspect_action(profile.value("throw_action", Json())).valid)
            throw std::runtime_error("invalid host throw action");
        if (values.size() >= 4096) throw std::runtime_error("catalog capacity reached");
        CapturedFrame reference_frame;
        {
            std::lock_guard lock(mutex);
            if (closed || token != generation || !host_idle() || !host_geometry_ready()) {
                busy = false;
                host_result(!host_geometry_ready() ? "waiting_geometry" : "waiting_idle");
                return;
            }
            reference_frame.bgr = bundle.roi;
            reference_frame.width = reference_frame.height = 320;
            reference_frame.source_width = last_source_size.width;
            reference_frame.source_height = last_source_size.height;
            reference_frame.encoded_width = last_encoded_size.width;
            reference_frame.encoded_height = last_encoded_size.height;
            reference_frame.roi_x = last_roi_x;
            reference_frame.roi_y = last_roi_y;
            reference_frame.source_mapping_verified = last_mapping_verified;
        }
        // 这是离线参考的构造时刻，不代表主机源时钟或 NDI 帧的新鲜度。
        reference_frame.timing.sequence = 1;
        reference_frame.timing.captured_at = Clock::now();
        const auto reference_id = unique_id();
        struct OwnedFiles {
            std::filesystem::path stage, published, catalog;
            bool owns_stage = false, owns_published = false;
            ~OwnedFiles() {
                std::error_code ignored;
                // 三条路径只来自本次新建产物；失败不删除收件箱或已有参考。
                if (owns_stage) std::filesystem::remove_all(stage, ignored);
                if (owns_published) std::filesystem::remove_all(published, ignored);
                if (!catalog.empty()) std::filesystem::remove(catalog, ignored);
            }
        } files;
        files.stage = root / (".host-pending-" + unique_id());
        files.owns_stage = std::filesystem::create_directory(files.stage);
        if (!files.owns_stage) throw std::runtime_error("cannot create host import directory");
        Engine engine;
        cv::Mat mask(320, 320, CV_8U, cv::Scalar(0));
        cv::rectangle(mask, cv::Rect(32, 32, 256, 192), cv::Scalar(255), cv::FILLED);
        if (!engine.create_reference(reference_frame, "host-static-" + capture_id, reference_id,
                                     reference_id, {160, 160}, mask, Clock::now(), why, ReferenceMode::ROI) ||
            !engine.save_reference(files.stage, why))
            throw std::runtime_error("host reference: " + why);
        const auto staged_reference = files.stage / reference_id;
        write_png(staged_reference / "full.png", bundle.full);
        write_png(staged_reference / "overview.png", bundle.overview);
        write_json(staged_reference / "host_capture.json", metadata);
        Json reference_metadata;
        {
            std::ifstream input(staged_reference / "reference.json", std::ios::binary);
            input >> reference_metadata;
        }
        reference_metadata["captured_steady_ns"] = nullptr;
        reference_metadata["capture_origin"] = "desktop_duplication";
        reference_metadata["capture_timestamp"] = metadata.at("capture_timestamp");
        reference_metadata["static_reference"] = true;
        replace_json(staged_reference / "reference.json", reference_metadata);
        Engine verified;
        if (!verified.load_reference(staged_reference, why))
            throw std::runtime_error("host reference verification: " + why);
        Json recipe = {{"id", reference_id}, {"reference_id", reference_id}, {"recipe_version", 1},
            {"frame_mode", "roi"}, {"source_id", source_identity},
            {"name", profile.value("name", "")}, {"target", profile.value("target", "")},
            {"grenade", profile.value("grenade", "")}, {"team", profile.at("team")},
            {"map", profile.at("map")}, {"validation", "unverified"},
            {"throw_action", profile.value("throw_action", Json())},
            {"aim", Json::array({.5, .5})}, {"static_rect", Json::array({.1, .1, .8, .6})},
            {"standpoint_reference_id", reference_id}, {"host_capture_id", capture_id},
            {"host_reference_id", reference_id}, {"capture_origin", "desktop_duplication"},
            {"capture_timestamp", metadata.at("capture_timestamp")},
            {"reference_url", "/api/reference/" + reference_id},
            {"standpoint_url", "/api/standpoint/" + reference_id},
            {"overview_url", "/api/overview/" + reference_id}, {"full_url", "/api/full/" + reference_id}};
        if (recipe["name"] == "") recipe["name"] = "主机采集 " + capture_id;
        if (recipe["target"] == "") recipe["target"] = "当前瞄点";
        for (const auto key : {"region", "stance", "instructions", "standpoint_id", "notes", "conditions", "throw_instructions"})
            recipe[key] = profile.value(key, "");
        if (recipe["standpoint_id"] == "") recipe["standpoint_id"] = reference_id;
        recipe["draft"] = recipe["grenade"] == "";
        values.push_back(recipe);
        files.catalog = root / ("catalog-host-" + unique_id() + ".json");
        write_json(files.catalog, values);
        {
            std::lock_guard lock(mutex);
            expire_context();
            if (closed || token != generation || !host_idle() || !host_geometry_ready() ||
                last_encoded_size != cv::Size(reference_frame.encoded_width, reference_frame.encoded_height) ||
                last_mapping_verified != reference_frame.source_mapping_verified) {
                busy = false;
                host_result(!host_geometry_ready() ? "waiting_geometry" : "waiting_idle");
                return;
            }
            if (host_imported.contains(capture_id)) {
                busy = false;
                host_result("imported");
                return;
            }
            files.published = root / reference_id;
            std::filesystem::rename(staged_reference, files.published);
            files.owns_published = true;
            if (!MoveFileExW(files.catalog.c_str(), (root / "catalog.json").c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                throw std::runtime_error("atomic host catalog replacement failed");
            files.owns_published = false;
            recipes.swap(values);
            host_imported.insert(capture_id);
            host_last_id = capture_id;
            clear_preview();
            locked = reference_id;
            lock_reason.clear();
            mode = "browse";
            busy = false;
            capture_status = "saved";
            error.clear();
            host_result("imported");
            ++revision;
        }
    }
    void persist(Json values, std::optional<std::uint64_t> token = std::nullopt) {
        auto tmp = root / ("catalog-" + unique_id() + ".json");
        write_json(tmp, values);
        auto dst = root / "catalog.json";
        std::unique_lock guard(mutex, std::defer_lock);
        if (token) {
            guard.lock();
            expire_context();
            if (*token != generation || closed) {
                std::filesystem::remove(tmp);
                throw std::runtime_error("capture cancelled");
            }
        }
        if (!MoveFileExW(tmp.c_str(), dst.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("atomic catalog replacement failed");
        if (token) {
            // 文件发布与内存状态是一次提交；取消命令只能发生在此前或此后。
            recipes.swap(values);
            busy = false;
            capture_status = "saved";
            error.clear();
            ++revision;
        }
    }
    void loop() {
        ReferenceCache reference_cache;
        for (;;) {
            std::optional<Frame> frame;
            std::optional<Json> task;
            Json values, selected_scope, selected_context;
            std::string selection;
            std::uint64_t token = 0;
            {
                std::unique_lock l(mutex);
                cv.wait_for(l, std::chrono::milliseconds(100),
                            [&] { return closed || job || pending; });
                if (closed)
                    return;
                if (capture_request && Clock::now() > capture_deadline) {
                    capture_request.reset();
                    capture_status = "error";
                    error = "capture timeout";
                    ++revision;
                }
                if (!job && !pending)
                    continue;
                expire_context();
                token = generation;
                values = recipes;
                selection = locked;
                selected_scope = scope;
                selected_context = context;
                if (job) {
                    task = std::move(job);
                    job.reset();
                } else {
                    frame = std::move(pending);
                    pending.reset();
                    if (capture_request && frame->serial > capture_after &&
                        frame->frame.timing.captured_at >= capture_requested) {
                        task = capture_request;
                        capture_request.reset();
                        busy = true;
                        capture_status = "saving";
                    }
                }
            }
            try {
                if (task) {
                    reference_cache.clear();
                    auto action = task->at("action").get<std::string>();
                    if (action == "host_import") {
                        import_host_capture(*task, std::move(values), token);
                        continue;
                    } else if (action == "capture") {
                        std::string why;
                        Engine engine;
                        auto id = unique_id();
                        auto &f = frame->frame;
                        cv::Mat mask(f.bgr.size(), CV_8U, cv::Scalar(0));
                        cv::rectangle(mask,
                                      cv::Rect(f.width / 10, f.height / 10, f.width * 8 / 10,
                                               f.height * 6 / 10),
                                      cv::Scalar(255), cv::FILLED);
                        if (!engine.create_reference(f, frame->session, id, id,
                                                     {f.width * .5, f.height * .5}, mask,
                                                     Clock::now(), why, frame_mode) ||
                            !engine.save_reference(root, why))
                            throw std::runtime_error(why);
                        Json r = {{"id", id},
                                  {"reference_id", id},
                                  {"recipe_version", 1},
                                  {"frame_mode", frame_mode == ReferenceMode::ROI ? "roi" : "full_frame"},
                                  {"source_id", source_identity},
                                  {"name", task->value("name", "")},
                                  {"target", task->value("target", "")},
                                  {"grenade", task->value("grenade", "")},
                                  {"notes", ""},
                                  {"throw_instructions", task->value("throw_instructions", "")},
                                  {"validation", "unverified"},
                                  {"conditions", ""},
                                  {"draft", true},
                                  {"team", task->value("team", "UNCLASSIFIED")},
                                  {"aim", Json::array({.5, .5})},
                                  {"static_rect", Json::array({.1, .1, .8, .6})}};
                        r["draft"] = r["name"] == "" || r["target"] == "" || r["grenade"] == "";
                        for (auto key : {"map", "region", "stance", "instructions"})
                            r[key] = task->value(key, "");
                        if (task->contains("throw_action"))
                            r["throw_action"] = task->at("throw_action");
                        r["standpoint_id"] = task->value("standpoint_id", "");
                        if (r["standpoint_id"] == "")
                            r["standpoint_id"] = id;
                        r["standpoint_reference_id"] = id;
                        for (const auto &old : values)
                            if (reference_compatible(old) && old.value("standpoint_id", "") ==
                                r["standpoint_id"].get<std::string>() &&
                                weapon::canonical_map_id(old.value("map", "")) == weapon::canonical_map_id(r.value("map", "")) &&
                                old.value("region", "") == r.value("region", "") &&
                                old.value("team", "UNCLASSIFIED") == r.value("team", "UNCLASSIFIED")) {
                                r["standpoint_reference_id"] =
                                    old.value("standpoint_reference_id",
                                              old.at("reference_id").get<std::string>());
                                break;
                            }
                        r["reference_url"] = "/api/reference/" + id;
                        r["standpoint_url"] = "/api/standpoint/" + id;
                        values.push_back(r);
                        {
                            std::lock_guard l(mutex);
                            if (token != generation || closed) {
                                busy = false;
                                capture_status = "cancelled";
                                continue;
                            }
                        }
                        persist(std::move(values), token);
                        continue;
                    } else if (action == "update") {
                        for (auto &item : task->at("items")) {
                            bool found = false;
                            for (auto &r : values)
                                if (r["id"] == item.at("id")) {
                                    found = true;
                                    r["recipe_version"] = r.value("recipe_version", 1) + 1;
                                    if (item.contains("throw_action")) {
                                        const auto design = detail::inspect_action(item.at("throw_action"));
                                        if (!design.valid) throw std::runtime_error("invalid throw action: " + design.error);
                                        r["throw_action"] = item.at("throw_action");
                                    }
                                    for (auto key :
                                         {"name", "target", "grenade", "notes", "team",
                                          "throw_instructions", "validation", "conditions"})
                                        if (item.contains(key)) {
                                            if (!item[key].is_string())
                                                throw std::runtime_error("text required");
                                            if (std::string(key) == "team" && !valid_team(item[key].get<std::string>()))
                                            throw std::runtime_error("invalid recipe team");
                                        r[key] = item[key];
                                        }
                                    r["draft"] =
                                        r["name"] == "" || r["target"] == "" || r["grenade"] == "";
                                }
                            if (!found)
                                throw std::runtime_error("unknown recipe");
                        }
                        persist(values);
                    } else if (action == "annotate") {
                        bool found = false;
                        for (auto &r : values)
                            if (r["id"] == task->at("id")) {
                                found = true;
                                Engine old;
                                std::string why;
                                if (!old.load_reference(
                                        root / r.at("reference_id").get<std::string>(), why))
                                    throw std::runtime_error(why);
                                auto img = old.reference_image();
                                auto a = task->at("aim"), rect = task->at("static_rect");
                                if (a.size() != 2 || rect.size() != 4)
                                    throw std::runtime_error("invalid annotation");
                                for (auto v : a)
                                    if (!v.is_number() || v.get<double>() < 0 ||
                                        v.get<double>() > 1)
                                        throw std::runtime_error("invalid aim");
                                for (auto v : rect)
                                    if (!v.is_number() || v.get<double>() < 0 ||
                                        v.get<double>() > 1)
                                        throw std::runtime_error("invalid rectangle");
                                double x = rect[0], y = rect[1], w = rect[2], h = rect[3];
                                if (w <= 0 || h <= 0 || x + w > 1 || y + h > 1)
                                    throw std::runtime_error("invalid rectangle");
                                cv::Mat mask(img.size(), CV_8U, cv::Scalar(0));
                                cv::rectangle(mask,
                                              cv::Rect(int(x * img.cols), int(y * img.rows),
                                                       int(w * img.cols), int(h * img.rows)),
                                              cv::Scalar(255), cv::FILLED);
                                auto id = unique_id();
                                if (!old.annotate_reference({a[0].get<double>() * (img.cols - 1),
                                                             a[1].get<double>() * (img.rows - 1)},
                                                            mask, id, why) ||
                                    !old.save_reference(root, why))
                                    throw std::runtime_error(why);
                                r["recipe_version"] = r.value("recipe_version", 1) + 1;
                                r["reference_id"] = id;
                                r["aim"] = a;
                                r["static_rect"] = rect;
                            }
                        if (!found)
                            throw std::runtime_error("unknown recipe");
                        persist(values);
                    } else if (action == "export") {
                        auto dest = root / "exports" / unique_id();
                        std::filesystem::create_directories(dest);
                        write_json(dest / "catalog.json", values);
                        Json evidence;
                        std::deque<Sample> copies;
                        {
                            std::lock_guard l(mutex);
                            evidence = records;
                            copies = samples;
                        }
                        write_json(dest / "observations.json", evidence);
                        std::vector<double> timings;
                        for (auto &row : evidence)
                            timings.push_back(row.value("processing_ms", 0.0));
                        std::sort(timings.begin(), timings.end());
                        auto percentile = [&](double q) {
                            return timings.empty() ? 0.0
                                                   : timings[static_cast<std::size_t>(
                                                         (timings.size() - 1) * q)];
                        };
                        write_json(
                            dest / "summary.json",
                            {{"processing_p50_ms", percentile(.5)},
                             {"processing_p95_ms", percentile(.95)},
                             {"processing_p99_ms", percentile(.99)},
                             {"phone_display_latency", "unknown; requires manual measurement"},
                             {"synthetic_or_real", "source-dependent; no accuracy claim"},
                             {"observation_count", evidence.size()},
                             {"sample_count", copies.size()},
                             {"record_limit", 128},
                             {"sample_limit", 4}});
                        for (auto &sample : copies) {
                            std::vector<unsigned char> raw_png;
                            if (!cv::imencode(".png", sample.raw, raw_png))
                                throw std::runtime_error("export encoding failed");
                            {
                                std::ofstream raw_file(dest / (sample.id + "-raw.png"),
                                                       std::ios::binary);
                                raw_file.write(reinterpret_cast<const char *>(raw_png.data()),
                                               raw_png.size());
                                raw_file.flush();
                                if (!raw_file)
                                    throw std::runtime_error("export raw write failed");
                            }
                            std::ofstream f(dest / (sample.id + "-marked.jpg"), std::ios::binary);
                            f.write(reinterpret_cast<const char *>(sample.marked.data()),
                                    sample.marked.size());
                            if (!f)
                                throw std::runtime_error("export image failed");
                        }
                        for (auto &r : values) {
                            for (auto key : {"reference_id", "standpoint_reference_id"}) {
                                auto id = r.at(key).get<std::string>();
                                if (!std::filesystem::exists(dest / id))
                                    std::filesystem::copy(root / id, dest / id,
                                                          std::filesystem::copy_options::recursive);
                            }
                        }
                        std::lock_guard l(mutex);
                        auto path_utf8 = dest.u8string();
                        export_path.assign(reinterpret_cast<const char *>(path_utf8.data()),
                                           path_utf8.size());
                        error.clear();
                    }
                    {
                        std::lock_guard l(mutex);
                        recipes = std::move(values);
                        invalidate_context_lock("配方分类已变化，已撤销不兼容锁定");
                        busy = false;
                        capture_status = token == generation ? "saved" : "cancelled";
                        ++revision;
                    }
                    continue;
                }
                if (!frame)
                    continue;
                std::string why;
                if (!Engine::validate_frame(frame->frame, frame->session, Clock::now(), why, frame_mode))
                    throw std::runtime_error(why);
                const auto round_started = Clock::now();
                const auto cache_before = reference_cache.stats();
                double load_ms = 0, match_ms = 0;
                Json candidates = Json::array();
                std::optional<Observation> observation;
                std::vector<Json> eligible;
                for (const auto &r : values) {
                    if (r.value("draft", true) || !reference_compatible(r))
                        continue;
                    if (!selection.empty()) {
                        if (r.at("id") == selection)
                            eligible.push_back(r);
                        continue;
                    }
                    auto reason = compatibility(r, selected_context, selected_scope);
                    if (reason != "compatible") continue;
                    eligible.push_back(r);
                }
                const auto count = std::min<std::size_t>(16, eligible.size());
                std::size_t start = 0;
                {
                    std::lock_guard l(mutex);
                    start = eligible.empty() ? 0 : search_cursor % eligible.size();
                }
                auto prepared = count ? Engine::prepare(frame->frame, frame->session, frame_mode) : PreparedFrame{};
                for (std::size_t i = 0; i < count; ++i) {
                    const auto &r = eligible[(start + i) % eligible.size()];
                    const auto ref = r.at("reference_id").get<std::string>();
                    const auto load_started = Clock::now();
                    auto engine = reference_cache.get(root / ref, why);
                    load_ms += std::chrono::duration<double, std::milli>(Clock::now() - load_started).count();
                    if (!engine) continue;
                    auto o = engine->locate(frame->frame, frame->session, Clock::now(), prepared, frame_mode);
                    match_ms += o.processing_ms;
                    if (o.status == Status::VALID)
                        candidates.push_back({{"id", r.at("id")}, {"status", "suggested"}});
                    if (!selection.empty())
                        observation = o;
                }
                const auto encode_started = Clock::now();
                auto image = frame->frame.bgr.clone();
                if (observation && observation->status == Status::VALID && observation->aim) {
                    auto p = *observation->aim;
                    cv::drawMarker(image, cv::Point(cvRound(p.x), cvRound(p.y)),
                                   cv::Scalar(0, 255, 255), cv::MARKER_CROSS, 28, 2);
                }
                std::vector<unsigned char> bytes;
                cv::imencode(".jpg", image, bytes, {cv::IMWRITE_JPEG_QUALITY, 85});
                const auto finished = Clock::now();
                const double processing_ms = std::chrono::duration<double, std::milli>(finished - round_started).count();
                const auto cache_after = reference_cache.stats();
                Json performance = {{"round_ms", processing_ms}, {"load_ms", load_ms},
                    {"feature_ms", prepared.feature_ms}, {"match_ms", match_ms},
                    {"encode_ms", std::chrono::duration<double, std::milli>(finished - encode_started).count()},
                    {"candidate_count", count}, {"eligible_count", eligible.size()},
                    {"cache_hits", cache_after.hits - cache_before.hits},
                    {"cache_misses", cache_after.misses - cache_before.misses},
                    {"cache_entries", cache_after.entries}, {"cache_bytes", cache_after.bytes},
                    {"cache_evictions", cache_after.evictions - cache_before.evictions},
                    {"cache_entry_limit", 32}, {"cache_byte_limit", 128 * 1024 * 1024}};
                std::lock_guard l(mutex);
                expire_context();
                if (token != generation || !running)
                    continue;
                source = "live";
                error.clear();
                search_limited = eligible.size() > 16;
                search_cursor = eligible.empty() ? 0 : (start + count) % eligible.size();
                suggestions = std::move(candidates);
                if (locating && !selection.empty() && !eligible.empty()) {
                    const auto &f = frame->frame;
                    const auto &r = eligible.front();
                    control_state.mode = control::Mode::OBSERVATION;
                    auto &o = control_state.observation;
                    o = {};
                    o.identity = {selection, r.at("reference_id").get<std::string>(), source_identity, f.timing.source_clock_session_id > 0 ? "source-clock-" + std::to_string(f.timing.source_clock_session_id) : frame->session,
                        r.value("recipe_version",std::uint64_t{1}), token,
                        {f.width,f.height,f.source_width,f.source_height,f.encoded_width,f.encoded_height,
                         f.roi_x,f.roi_y,f.source_pixels_per_pixel_x,f.source_pixels_per_pixel_y,f.source_mapping_verified}};
                    o.sequence = f.timing.sequence; o.captured_at = f.timing.captured_at;
                    if (f.timing.source_time_timing_valid && f.timing.source_clock_status == SourceClockStatus::VALID &&
                        std::isfinite(f.timing.source_clock_uncertainty_ms) && f.timing.source_clock_uncertainty_ms >= 0 && f.timing.source_clock_uncertainty_ms <= 1000) {
                        o.source_at = f.timing.source_time_at;
                        o.source_uncertainty = std::chrono::milliseconds(static_cast<long long>(std::ceil(f.timing.source_clock_uncertainty_ms)));
                    }
                    // 准星原点只来自已证实的源映射；未知映射仍可显示局部定位，不可执行。
                    if (observation && observation->status == Status::VALID && observation->aim && f.source_mapping_verified) {
                        const double cx = (f.source_width * 0.5 - f.roi_x) / f.source_pixels_per_pixel_x;
                        const double cy = (f.source_height * 0.5 - f.roi_y) / f.source_pixels_per_pixel_y;
                        o.valid = cx >= 0 && cx < f.width && cy >= 0 && cy < f.height;
                        o.error_x = observation->aim->x - cx; o.error_y = observation->aim->y - cy;
                    }
                    control_state.reference_version = o.identity.recipe_version;
                    control_state.throw_action = r.value("throw_action", Json());
                }
                auto id = unique_id();
                preview_time = frame->frame.timing.captured_at;
                preview_bytes = std::move(bytes);
                records.push_back(
                    {{"frame_id", id},
                     {"session", frame->session},
                     {"sequence", frame->frame.timing.sequence},
                     {"recipe_id", selection},
                     {"status", observation ? lower_status(observation->status) : "no_reference"},
                     {"processing_ms", processing_ms},
                     {"performance", performance},
                     {"local_age_ms",
                      std::chrono::duration<double, std::milli>(Clock::now() - preview_time)
                          .count()},
                     {"dropped", dropped},
                     {"source_time_known", frame->frame.timing.source_time_timing_valid},
                     {"source_mapping_verified", frame->frame.source_mapping_verified},
                     {"frame_mode", frame_mode == ReferenceMode::ROI ? "roi" : "full_frame"}});
                if (records.size() > 128)
                    records.erase(records.begin());
                if (samples.size() >= 4)
                    samples.pop_front();
                samples.push_back({id, frame->frame.bgr.clone(), preview_bytes});
                preview_state = {
                    {"url", "/api/preview?frame_id=" + id},
                    {"frame_id", id},
                    {"source_sequence", frame->frame.timing.sequence},
                    {"session_id", frame->session},
                    {"recipe_id", selection},
                    {"status", observation ? lower_status(observation->status) : "no_reference"},
                    {"age_ms", 0},
                    {"max_age_ms", 1000},
                    {"processing_ms", processing_ms},
                     {"performance", performance},
                    {"source_time_known", frame->frame.timing.source_time_timing_valid},
                     {"source_mapping_verified", frame->frame.source_mapping_verified},
                     {"frame_mode", frame_mode == ReferenceMode::ROI ? "roi" : "full_frame"}};
            } catch (const std::exception &e) {
                std::lock_guard l(mutex);
                error = e.what();
                if (task) {
                    busy = false;
                    capture_status = "error";
                    if (task->value("action", "") == "host_import") {
                        host_rejected.insert(task->at("host_id").get<std::string>());
                        host_result("error", e.what());
                    }
                    ++revision;
                } else if (token == generation) {
                    clear_preview();
                    source = "invalid";
                }
            }
        }
    }
};
Service::Service(const std::filesystem::path &root, ReferenceMode mode, std::string source_identity)
    : impl_(std::make_unique<Impl>(root, mode, std::move(source_identity))) {}
Service::~Service() {
    close();
}
control::Request Service::control_request(bool *preparing) {
    auto &s = *impl_; std::lock_guard lock(s.mutex); s.expire_context();
    if (preparing) *preparing = s.running && s.locating && !s.locked.empty() && !s.closed &&
                                s.control_state.mode == control::Mode::CANCEL;
    auto request = s.control_state;
    if (!s.running || !s.locating || s.locked.empty() || s.closed) return {};
    if (request.mode != control::Mode::CANCEL && s.sent_control_generation != s.generation) {
        // 一次明确定位只产生一条 LOCATE；断线由主进程撤销，不自动重放。
        request.mode = control::Mode::LOCATE; s.sent_control_generation = s.generation;
    }
    request.trigger_sequence = s.locate_trigger_sequence;
    return request;
}
void Service::reset_control(const std::string &reason) {
    auto &s = *impl_; std::lock_guard lock(s.mutex);
    const bool unresolved = s.host_enabled && (s.locating || !s.host_idle());
    s.clear_preview();
    s.execution_status = {{"state", unresolved ? "unknown" : "unavailable"},
                          {"reason", reason}, {"real_verified", false}};
}
void Service::set_execution_status(const Json &status) {
    auto &s = *impl_; std::lock_guard lock(s.mutex);
    if (!status.is_object()) return;
    s.execution_status = status;
    // 仅独立测试的一次已确认终态能收起旧定位；旧回报和 UNKNOWN 不能解锁 F7。
    if (s.host_enabled && s.locating && s.locate_trigger_sequence != 0 &&
        status.contains("trigger_sequence") && status.at("trigger_sequence") == s.locate_trigger_sequence &&
        status.contains("selection_generation") && status.at("selection_generation") == s.generation &&
        status.contains("active") && status.at("active") == false &&
        status.value("cleanup_required", false) == false) {
        const auto state = status.value("state", "unknown");
        if (state == "aligned" || state == "completed" || state == "cancelled" || state == "blocked") {
            s.clear_preview();
            ++s.revision;
        }
    }
}
void Service::update_gsi(const weapon::WeaponSnapshot &snapshot, const std::string &receiver_error) {
    auto &s = *impl_;
    std::lock_guard l(s.mutex);
    if (s.closed) return;
    s.expire_context();
    auto old_map = s.context.value("map", "");
    auto now = Clock::now();
    bool valid = snapshot.context_valid && snapshot.identity_match && snapshot.player_playing &&
                 snapshot.valid_until > now && (snapshot.local_team == weapon::Team::T || snapshot.local_team == weapon::Team::CT);
    auto map = valid ? weapon::canonical_map_id(snapshot.map_name) : std::string();
    if (map.empty()) valid = false;
    auto team = valid ? std::string(weapon::team_name(snapshot.local_team)) : "UNKNOWN";
    bool changed = s.context.value("auto_map", "") != map || s.context.value("auto_team", "UNKNOWN") != team ||
                   s.context.value("identity_confirmed", false) != (valid && snapshot.identity_match);
    const bool hidden_boundary = !changed && s.gsi_context_epoch && snapshot.context_epoch &&
        s.gsi_context_epoch != snapshot.context_epoch;
    changed = changed || hidden_boundary;
    if (snapshot.context_epoch) s.gsi_context_epoch = snapshot.context_epoch;
    s.gsi_received = snapshot.received_at;
    s.gsi_until = snapshot.valid_until;
    s.context["auto_map"] = map;
    s.context["auto_team"] = team;
    s.context["gsi_status"] = weapon::status_name(snapshot.status);
    s.context["identity_confirmed"] = valid && snapshot.identity_match;
    if (s.context["mode"] == "auto") {
        s.context["map"] = map;
        s.context["team"] = team;
        if (old_map != map) {
            s.scope["map"] = map;
            s.scope["region"] = "";
        }
        s.context["reason"] = !receiver_error.empty() ? receiver_error : valid ? "已确认本人地图和阵营" : "GSI 未知或过期，可手动浏览选择";
        if (changed) {
            s.invalidate_context_lock(valid ? "GSI 地图或阵营已变化，已撤销不兼容锁定" : "GSI 身份或上下文失效，已撤销锁定", !valid || hidden_boundary);
            s.capture_request.reset();
            s.clear_preview();
        }
    }
    if (s.context["mode"] == "manual")
        s.context["reason"] = receiver_error.empty() ? "使用手动地图和阵营" : "使用手动地图和阵营；" + receiver_error;
    if (changed) ++s.revision;
}
bool Service::request_locate(std::uint64_t trigger_sequence) {
    auto &s = *impl_;
    std::lock_guard l(s.mutex);
    s.expire_context();
    if (s.closed || !s.running || s.mode != "browse" || s.locked.empty() || s.busy) return false;
    s.clear_preview();
    s.locate_trigger_sequence = trigger_sequence;
    s.locating = true;
    ++s.revision;
    return true;
}
bool Service::enable_host_capture(const std::filesystem::path &inbox, cv::Size source_size) {
    auto &s = *impl_;
    std::lock_guard lock(s.mutex);
    if (s.host_enabled || s.closed) return false;
    try {
        if (!s.storage_valid || s.frame_mode != ReferenceMode::ROI || s.source_identity.empty() ||
            inbox.empty() || source_size.width < 320 || source_size.height < 320 ||
            source_size.width > 7680 || source_size.height > 4320)
            throw std::runtime_error("invalid host capture configuration");
        auto path = std::filesystem::absolute(inbox).lexically_normal();
        if (redirected(path) || redirected(path / "captures"))
            throw std::runtime_error("redirected host inbox is not allowed");
        std::filesystem::create_directories(path / "captures");
        Json profile = {{"schema", 1}, {"source_id", s.source_identity},
            {"source_width", source_size.width}, {"source_height", source_size.height},
            {"map", ""}, {"team", ""}, {"throw_action", nullptr}, {"throw_instructions", ""}};
        const bool exists = std::filesystem::exists(path / "profile.json");
        if (exists) {
            std::string why;
            if (!read_host_capture_profile(path / "profile.json", profile, why))
                throw std::runtime_error("host profile: " + why);
            if (profile.at("source_id") != s.source_identity || profile.at("source_width") != source_size.width ||
                profile.at("source_height") != source_size.height)
                throw std::runtime_error("host profile source or dimensions mismatch");
            if (!detail::inspect_action(profile.value("throw_action", Json())).valid)
                throw std::runtime_error("invalid host throw action");
        }
        s.host_inbox = std::move(path);
        s.host_source_size = source_size;
        s.host_profile = std::move(profile);
        s.host_enabled = true;
        s.host_result(exists ? "ready" : "awaiting_profile");
        return true;
    } catch (const std::exception &e) {
        s.host_result("error", e.what());
        return false;
    }
}
void Service::poll_host_captures() {
    auto &s = *impl_;
    // 每次最多检查 64 个目录项并只排一个解码任务；图像 I/O 在既有 worker 完成。
    std::unique_lock polling(s.host_poll_mutex, std::try_to_lock);
    if (!polling.owns_lock()) return;
    try {
        std::filesystem::path captures;
        {
            std::lock_guard lock(s.mutex);
            if (!s.host_enabled || s.closed || !s.storage_valid || s.busy || s.job) return;
            if (!s.host_idle()) { s.host_result("waiting_idle"); return; }
            if (!s.host_geometry_ready()) { s.host_result("waiting_geometry"); return; }
            if (s.host_rejected.size() >= 4096) { s.host_result("error", "too many rejected host captures"); return; }
            captures = s.host_inbox / "captures";
        }
        if (redirected(captures)) throw std::runtime_error("redirected host captures directory");
        if (!s.host_iterator) s.host_iterator.emplace(captures);
        const std::filesystem::directory_iterator end;
        for (int checked = 0; checked < 64; ++checked) {
            if (*s.host_iterator == end) { s.host_iterator.reset(); return; }
            const auto entry = **s.host_iterator;
            ++(*s.host_iterator);
            const auto capture_id = entry.path().filename().string();
            if (!host_safe_id(capture_id) || !entry.is_directory() || redirected(entry.path())) continue;
            {
                std::lock_guard lock(s.mutex);
                if (s.host_imported.contains(capture_id) || s.host_rejected.contains(capture_id)) continue;
            }
            bool complete = true;
            for (const auto filename : {"capture.json", "full.png", "roi.png", "overview.png"})
                if (!std::filesystem::is_regular_file(entry.path() / filename)) { complete = false; break; }
            if (!complete) continue;
            std::lock_guard lock(s.mutex);
            if (s.closed || s.busy || s.job) return;
            if (!s.host_idle()) { s.host_result("waiting_idle"); return; }
            if (!s.host_geometry_ready()) { s.host_result("waiting_geometry"); return; }
            s.job = Json{{"action", "host_import"}, {"host_id", capture_id}};
            s.busy = true;
            s.host_result("importing");
            s.cv.notify_one();
            return;
        }
    } catch (const std::exception &e) {
        s.host_iterator.reset();
        std::lock_guard lock(s.mutex);
        s.host_result("error", e.what());
    }
}
bool Service::wants_input() const {
    std::lock_guard l(impl_->mutex);
    return impl_->running && !impl_->closed;
}
Json Service::state() {
    std::lock_guard l(impl_->mutex);
    return impl_->snapshot();
}
CommandResult Service::command(const Json &c) {
    auto &s = *impl_;
    std::lock_guard l(s.mutex);
    try {
        s.expire_context();
        auto id = c.at("request_id").get<std::string>();
        if (c.at("epoch") != s.epoch)
            return {409, s.snapshot()};
        if (s.requests.contains(id))
            return {200, s.snapshot()};
        if (c.at("revision") != s.revision || id.empty() || id.size() > 128 || s.closed)
            return {409, s.snapshot()};
        auto a = c.at("action").get<std::string>();
        if ((a == "capture" || a == "update" || a == "annotate" || a == "export" || a == "host_profile") &&
            (!s.storage_valid || s.busy || s.capture_request))
            return {409, s.snapshot()};
        if (a == "favorite" || a == "practice_queue" || a == "practice_record") {
            if (!s.storage_valid || s.busy) return {409, s.snapshot()};
            const auto target = c.at("id").get<std::string>();
            const auto r = std::find_if(s.recipes.begin(), s.recipes.end(), [&](const Json &item) { return item.at("id") == target && !item.value("draft", true); });
            if (r == s.recipes.end()) return {400, s.snapshot()};
            if (!s.practice.apply(c, *r, s.context)) return {200, s.snapshot()};
        } else if (a == "locate") {
            if (!s.running || s.mode != "browse" || s.locked.empty() || s.busy) return {409, s.snapshot()};
            s.clear_preview();
            s.locating = true;
        } else if (a == "context") {
            auto mode = c.at("mode").get<std::string>();
            if (mode != "auto" && mode != "manual") return {400, s.snapshot()};
            auto map = weapon::canonical_map_id(c.value("map", ""));
            auto team = c.value("team", "UNKNOWN");
            if (mode == "manual" && (map.empty() || (team != "T" && team != "CT" && team != "UNKNOWN")))
                return {400, s.snapshot()};
            auto old_map = s.context.value("map", "");
            s.context["mode"] = mode;
            s.context["map"] = mode == "manual" ? map : s.context.value("auto_map", "");
            s.context["team"] = mode == "manual" ? team : s.context.value("auto_team", "UNKNOWN");
            if (old_map != s.context.value("map", "")) {
                s.scope["map"] = s.context.at("map");
                s.scope["region"] = "";
            }
            s.context["reason"] = mode == "manual" ? "使用手动地图和阵营" : "自动读取 GSI 地图和阵营";
            s.invalidate_context_lock("地图或阵营选择已变化，已撤销不兼容锁定", mode == "auto" && s.context.value("auto_map", "").empty());
            s.capture_request.reset();
            s.clear_preview();
        } else if (a == "scope") {
            auto map = c.value("map", ""), region = c.value("region", "");
            if (map.size() > 256 || region.size() > 256)
                return {400, s.snapshot()};
            s.scope = {{"map", map}, {"region", region}};
            s.search_cursor = 0;
            s.search_limited = false;
            s.clear_preview();
        } else if (a == "lock") {
            auto target = c.at("id").get<std::string>();
            bool found = false;
            for (auto &r : s.recipes)
                if (r["id"] == target && !r.value("draft", true) && s.reference_compatible(r) && selectable(compatibility(r, s.context, s.scope)))
                    found = true;
            if (!found)
                return {400, s.snapshot()};
            s.locked = target;
            s.lock_reason.clear();
            s.clear_preview();
        } else if (a == "cancel") {
            s.locked.clear();
            s.capture_request.reset();
            s.capture_status = "idle";
            s.clear_preview();
        } else if (a == "mode") {
            auto v = c.at("value").get<std::string>();
            if (v != "browse" && v != "capture")
                return {400, s.snapshot()};
            s.mode = v;
            s.capture_request.reset();
            s.clear_preview();
        } else if (a == "start") {
            s.running = true;
            s.clear_preview();
        } else if (a == "stop") {
            s.running = false;
            s.live_geometry_available = false;
            s.capture_request.reset();
            s.clear_preview();
        } else if (a == "capture") {
            if (s.mode != "capture" || !s.running)
                return {409, s.snapshot()};
            // 排队前校验新增表单字段，拒绝时不能留下参考图或待保存请求。
            for (auto key : {"map", "team", "throw_instructions", "name", "target", "grenade"})
                if (c.contains(key) && !c.at(key).is_string())
                    return {400, s.snapshot()};
            if (c.contains("team") && !valid_team(c.at("team").get<std::string>()))
                return {400, s.snapshot()};
            if (c.contains("throw_action") && !detail::inspect_action(c.at("throw_action")).valid)
                return {400, s.snapshot()};
            s.capture_request = c;
            // 表单显式分类只属于本张配方；旧调用未填写时继续沿用游戏上下文。
            if (!c.contains("team")) {
                const auto effective_team = s.context.value("team", "UNKNOWN");
                (*s.capture_request)["team"] = effective_team == "T" || effective_team == "CT" ? effective_team : "UNCLASSIFIED";
            }
            if (c.value("map", "").empty() && !s.context.value("map", "").empty())
                (*s.capture_request)["map"] = s.context.at("map");
            s.capture_after = s.submitted;
            s.capture_requested = Clock::now();
            s.capture_deadline = s.capture_requested + std::chrono::seconds(5);
            s.capture_status = "waiting";
        } else if (a == "host_profile") {
            if (!s.host_enabled) return {409, s.snapshot()};
            static const std::unordered_set<std::string> allowed = {"action", "epoch", "revision", "request_id",
                "map", "team", "throw_action", "throw_instructions", "region", "stance", "instructions",
                "standpoint_id", "name", "target", "grenade", "notes", "conditions"};
            for (auto item = c.begin(); item != c.end(); ++item)
                if (!allowed.contains(item.key())) return {400, s.snapshot()};
            Json profile = {{"schema", 1}, {"source_id", s.source_identity},
                {"source_width", s.host_source_size.width}, {"source_height", s.host_source_size.height},
                {"map", c.value("map", "")}, {"team", c.value("team", "")},
                {"throw_action", c.value("throw_action", Json())},
                {"throw_instructions", c.value("throw_instructions", "")}};
            if (!detail::inspect_action(profile.at("throw_action")).valid) return {400, s.snapshot()};
            for (const auto key : {"region", "stance", "instructions", "standpoint_id", "name", "target", "grenade", "notes", "conditions"})
                if (c.contains(key)) profile[key] = c.at(key).get<std::string>();
            const auto temporary = s.host_inbox / ("profile-" + unique_id() + ".json");
            struct TemporaryProfile {
                std::filesystem::path path;
                ~TemporaryProfile() { std::error_code ignored; std::filesystem::remove(path, ignored); }
            } cleanup{temporary};
            write_json(temporary, profile);
            Json verified;
            std::string why;
            if (!read_host_capture_profile(temporary, verified, why)) return {400, s.snapshot()};
            if (!MoveFileExW(temporary.c_str(), (s.host_inbox / "profile.json").c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                throw std::runtime_error("atomic host profile replacement failed");
            s.host_profile = std::move(verified);
            s.host_result("ready");
        } else if (a == "update" || a == "annotate" || a == "export") {
            s.job = c;
            s.busy = true;
            s.capture_status = "saving";
            s.clear_preview();
            s.cv.notify_one();
        } else
            return {400, s.snapshot()};
        s.requests.insert(id);
        s.request_order.push_back(id);
        if (s.request_order.size() > 1024) {
            s.requests.erase(s.request_order.front());
            s.request_order.pop_front();
        }
        ++s.revision;
        return {200, s.snapshot()};
    } catch (...) {
        return {400, s.snapshot()};
    }
}
void Service::submit(const CapturedFrame &frame, const std::string &session) {
    auto &s = *impl_;
    std::lock_guard l(s.mutex);
    if (s.closed || !s.running)
        return;
    std::string why;
    if (!Engine::validate_frame(frame, session, Clock::now(), why, s.frame_mode)) {
        s.error = why;
        s.source = "invalid";
        s.live_geometry_available = false;
        s.clear_preview();
        return;
    }
    if (session == s.last_session && frame.timing.sequence <= s.last_sequence)
        return;
    if (session != s.last_session || frame.bgr.size() != s.last_size ||
        cv::Size(frame.source_width, frame.source_height) != s.last_source_size ||
        frame.source_pixels_per_pixel_x != s.last_scale_x ||
        frame.source_pixels_per_pixel_y != s.last_scale_y ||
        frame.roi_x != s.last_roi_x || frame.roi_y != s.last_roi_y ||
        cv::Size(frame.encoded_width, frame.encoded_height) != s.last_encoded_size ||
        frame.source_mapping_verified != s.last_mapping_verified) {
        if (!s.last_session.empty()) {
            ++s.revision;
            if (s.capture_request) {
                s.capture_request.reset();
                s.capture_status = "cancelled";
            }
        }
        if (!s.last_session.empty()) s.clear_preview();
    }
    s.last_session = session;
    s.last_sequence = frame.timing.sequence;
    s.last_size = frame.bgr.size();
    s.last_source_size = {frame.source_width, frame.source_height};
    s.last_scale_x = frame.source_pixels_per_pixel_x;
    s.last_scale_y = frame.source_pixels_per_pixel_y;
    s.last_encoded_size = {frame.encoded_width, frame.encoded_height};
    s.last_roi_x = frame.roi_x; s.last_roi_y = frame.roi_y;
    s.last_mapping_verified = frame.source_mapping_verified;
    s.live_geometry_available = true;
    s.live_geometry_at = Clock::now();
    if (s.frame_mode == ReferenceMode::ROI && !s.capture_request && (s.mode != "browse" || !s.locating || s.locked.empty())) return;
    CapturedFrame owned = frame;
    owned.bgr = frame.bgr.clone();
    owned.bgr_storage.reset();
    owned.native_storage.reset();
    if (s.pending)
        ++s.dropped;
    s.pending = Impl::Frame{std::move(owned), session, ++s.submitted, s.generation};
    s.cv.notify_one();
}
void Service::disconnect(const std::string &reason) {
    auto &s = *impl_;
    std::lock_guard l(s.mutex);
    ++s.revision;
    s.source = "disconnected";
    s.live_geometry_available = false;
    s.error = reason;
    s.clear_preview();
    s.capture_request.reset();
    s.capture_status = "idle";
}
std::optional<std::vector<unsigned char>> Service::preview(const std::string &id) {
    auto &s = *impl_;
    std::lock_guard l(s.mutex);
    s.expire_context();
    if (s.preview_state.value("frame_id", "") != id ||
        Clock::now() - s.preview_time > std::chrono::milliseconds(1000) || s.preview_bytes.empty())
        return {};
    return s.preview_bytes;
}
std::optional<std::vector<unsigned char>> Service::reference(const std::string &id) {
    auto &s = *impl_;
    std::string ref, filename = "raw.png";
    {
        std::lock_guard l(s.mutex);
        bool standpoint = id.starts_with("standpoint:");
        bool overview = id.starts_with("overview:"), full = id.starts_with("full:");
        auto key = standpoint ? id.substr(11) : overview ? id.substr(9) : full ? id.substr(5) : id;
        for (auto &r : s.recipes)
            if (r["id"] == key) {
                if (overview || full) {
                    ref = r.value("host_reference_id", "");
                    filename = overview ? "overview.png" : "full.png";
                } else ref = r.at(standpoint ? "standpoint_reference_id" : "reference_id").get<std::string>();
            }
    }
    if (ref.empty())
        return {};
    try {
        return read_bytes(s.root / ref / filename);
    } catch (...) {
        return {};
    }
}
void Service::close() {
    auto &s = *impl_;
    {
        std::lock_guard l(s.mutex);
        s.closed = true;
        s.running = false;
        s.pending.reset();
        s.job.reset();
        s.capture_request.reset();
        s.clear_preview();
        s.cv.notify_all();
    }
    if (s.worker.joinable())
        s.worker.join();
}
} // namespace lineup
