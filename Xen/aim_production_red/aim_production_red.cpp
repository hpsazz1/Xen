#include "aim_production_red/aim_production_red.h"
#include "aim_production_red/aim_production_red_internal.h"

#include "aim/aim.h"
#include "config/config.h"

#include <nlohmann/json.hpp>

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace aim_production_red {
namespace {

using json = nlohmann::json;

constexpr int kPlanSchemaVersion = 1;
constexpr int kRedSchemaVersion = 1;
constexpr std::string_view kPlanEvidenceType =
    "aim_production_red_output_off_plan";
constexpr std::string_view kManifestEvidenceType =
    "aim_production_red_manifest";
constexpr std::size_t kMaximumSamplesPerBlock = 1000000;
constexpr int kMaximumPlantDelaySamples = 64;
constexpr std::int64_t kBackendCompletionDelayNs = 100000;

struct CompletedCommand {
    int dx = 0;
    int dy = 0;
};

struct PlantProfile {
    std::string id;
    int delay_samples = 0;
    double gain_x = 0.0;
    double gain_y = 0.0;
};

bool is_sha256(std::string_view value) noexcept {
    return value.size() == 64 && std::all_of(
        value.begin(), value.end(), [](char character) {
            return (character >= '0' && character <= '9') ||
                (character >= 'a' && character <= 'f') ||
                (character >= 'A' && character <= 'F');
        });
}

bool is_safe_identifier(std::string_view value) noexcept {
    return !value.empty() && value.size() <= 96 && std::all_of(
        value.begin(), value.end(), [](char character) {
            return (character >= 'a' && character <= 'z') ||
                (character >= 'A' && character <= 'Z') ||
                (character >= '0' && character <= '9') ||
                character == '-' || character == '_' || character == '.';
        });
}

bool compute_file_sha256(const std::filesystem::path& path,
                         std::string& value,
                         std::string& error) {
    value.clear();
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = "无法读取 SHA-256 输入: " + path.string();
        return false;
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_size = 0;
    DWORD hash_size = 0;
    DWORD returned = 0;
    std::vector<UCHAR> object;
    std::vector<UCHAR> digest;
    auto cleanup = [&]() noexcept {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    };
    const auto succeeded = [](NTSTATUS status) noexcept {
        return status >= 0;
    };
    if (!succeeded(BCryptOpenAlgorithmProvider(
            &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) ||
        !succeeded(BCryptGetProperty(
            algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size),
            &returned, 0)) ||
        !succeeded(BCryptGetProperty(
            algorithm, BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&hash_size), sizeof(hash_size),
            &returned, 0))) {
        cleanup();
        error = "初始化 SHA-256 失败";
        return false;
    }
    object.resize(object_size);
    digest.resize(hash_size);
    if (!succeeded(BCryptCreateHash(
            algorithm, &hash, object.data(), object_size,
            nullptr, 0, 0))) {
        cleanup();
        error = "创建 SHA-256 状态失败";
        return false;
    }

    // Windows 可执行文件默认线程栈通常不足以再容纳 1 MiB 局部数组；哈希
    // 缓冲属于流式工作区，放到堆上可保持固定内存上限且不改变证据字节。
    std::vector<char> buffer(1024 * 1024);
    while (stream) {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = stream.gcount();
        if (count > 0 && !succeeded(BCryptHashData(
                hash, reinterpret_cast<PUCHAR>(buffer.data()),
                static_cast<ULONG>(count), 0))) {
            cleanup();
            error = "计算 SHA-256 失败";
            return false;
        }
    }
    if (!stream.eof() || !succeeded(BCryptFinishHash(
            hash, digest.data(), hash_size, 0))) {
        cleanup();
        error = "完成 SHA-256 失败";
        return false;
    }
    cleanup();

    std::ostringstream text;
    text << std::hex << std::setfill('0');
    for (const UCHAR byte : digest) {
        text << std::setw(2) << static_cast<unsigned int>(byte);
    }
    value = text.str();
    return true;
}

bool copy_verified(const std::filesystem::path& source,
                   const std::filesystem::path& destination,
                   std::string& sha256,
                   std::string& error) {
    if (!std::filesystem::is_regular_file(source)) {
        error = "输入文件不存在: " + source.string();
        return false;
    }
    std::string source_sha256;
    if (!compute_file_sha256(source, source_sha256, error)) return false;
    std::filesystem::create_directories(destination.parent_path());
    std::filesystem::copy_file(
        source, destination, std::filesystem::copy_options::none);
    std::string destination_sha256;
    if (!compute_file_sha256(destination, destination_sha256, error)) {
        return false;
    }
    if (source_sha256 != destination_sha256) {
        error = "输入文件复制后 SHA-256 不一致: " + source.string();
        return false;
    }
    sha256 = std::move(destination_sha256);
    return true;
}

json read_json(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("无法读取 plan");
    json value;
    stream >> value;
    return value;
}

void write_json(const std::filesystem::path& path, const json& value) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << value.dump(2) << '\n';
    if (!stream) throw std::runtime_error("写入 JSON 失败: " + path.string());
}

void write_json_lines(const std::filesystem::path& path,
                      const std::vector<json>& rows) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    for (const auto& row : rows) stream << row.dump() << '\n';
    if (!stream) {
        throw std::runtime_error("写入 JSONL 失败: " + path.string());
    }
}

double finite_number(const json& object,
                     std::string_view field,
                     std::string_view context) {
    const auto iterator = object.find(field);
    if (iterator == object.end() || !iterator->is_number()) {
        throw std::runtime_error(
            std::string(context) + " 缺少数值字段 " + std::string(field));
    }
    const double value = iterator->get<double>();
    if (!std::isfinite(value)) {
        throw std::runtime_error(
            std::string(context) + " 含非有限字段 " + std::string(field));
    }
    return value;
}

std::int64_t integer(const json& object,
                     std::string_view field,
                     std::string_view context) {
    const auto iterator = object.find(field);
    if (iterator == object.end() ||
        !(iterator->is_number_integer() || iterator->is_number_unsigned())) {
        throw std::runtime_error(
            std::string(context) + " 缺少整数字段 " + std::string(field));
    }
    return iterator->get<std::int64_t>();
}

bool boolean(const json& object,
             std::string_view field,
             std::string_view context) {
    const auto iterator = object.find(field);
    if (iterator == object.end() || !iterator->is_boolean()) {
        throw std::runtime_error(
            std::string(context) + " 缺少布尔字段 " + std::string(field));
    }
    return iterator->get<bool>();
}

std::string text(const json& object,
                 std::string_view field,
                 std::string_view context) {
    const auto iterator = object.find(field);
    if (iterator == object.end() || !iterator->is_string()) {
        throw std::runtime_error(
            std::string(context) + " 缺少字符串字段 " + std::string(field));
    }
    return iterator->get<std::string>();
}

std::array<double, 2> vector2(const json& object,
                              std::string_view field,
                              std::string_view context) {
    const auto iterator = object.find(field);
    if (iterator == object.end() || !iterator->is_array() ||
        iterator->size() != 2 ||
        !(*iterator)[0].is_number() || !(*iterator)[1].is_number()) {
        throw std::runtime_error(
            std::string(context) + " 的 " + std::string(field) +
            " 必须是二维数值");
    }
    const std::array<double, 2> value{
        (*iterator)[0].get<double>(), (*iterator)[1].get<double>()};
    if (!std::isfinite(value[0]) || !std::isfinite(value[1])) {
        throw std::runtime_error(
            std::string(context) + " 的 " + std::string(field) +
            " 含非有限值");
    }
    return value;
}

std::array<int, 2> integer_vector2(const json& object,
                                   std::string_view field,
                                   std::string_view context) {
    const auto iterator = object.find(field);
    if (iterator == object.end() || !iterator->is_array() ||
        iterator->size() != 2 ||
        !(*iterator)[0].is_number_integer() ||
        !(*iterator)[1].is_number_integer()) {
        throw std::runtime_error(
            std::string(context) + " 的 " + std::string(field) +
            " 必须是二维整数");
    }
    return {(*iterator)[0].get<int>(), (*iterator)[1].get<int>()};
}

void validate_measured_reference(const json& reference) {
    if (!reference.is_object()) {
        throw std::runtime_error("measured_reference 必须是对象");
    }
    for (const auto field : {
             "source_identity_sha256", "outside_sequence_sha256"}) {
        const std::string value = text(reference, field, "measured_reference");
        if (!is_sha256(value)) {
            throw std::runtime_error(
                std::string("measured_reference 的 ") + field +
                " 不是 SHA-256");
        }
    }
    const auto metrics = reference.find("metrics");
    const auto uncertainty = reference.find("absolute_uncertainty");
    if (metrics == reference.end() || !metrics->is_object() ||
        uncertainty == reference.end() || !uncertainty->is_object()) {
        throw std::runtime_error(
            "measured_reference 缺少 metrics/absolute_uncertainty");
    }
    for (const auto field : {
             "outside_samples", "outside_duration_ns",
             "longest_outside_samples", "longest_outside_ns"}) {
        if (integer(*metrics, field, "measured_reference.metrics") < 0) {
            throw std::runtime_error("measured reference 整数 metric 不得为负");
        }
    }
    for (const auto field : {
             "outside_area_px_ns", "max_excess_x_px",
             "max_abs_error_x_px"}) {
        if (finite_number(*metrics, field, "measured_reference.metrics") < 0.0 ||
            finite_number(*uncertainty, field,
                          "measured_reference.absolute_uncertainty") < 0.0) {
            throw std::runtime_error("measured reference 浮点 metric 不得为负");
        }
    }
}

std::vector<PlantProfile> parse_profiles(const json& plan) {
    const auto iterator = plan.find("plant_profiles");
    if (iterator == plan.end() || !iterator->is_array() || iterator->empty()) {
        throw std::runtime_error("plan 缺少 plant_profiles");
    }
    std::set<std::string> identities;
    std::vector<PlantProfile> profiles;
    for (const auto& value : *iterator) {
        PlantProfile profile;
        profile.id = text(value, "plant_profile_id", "plant profile");
        profile.delay_samples = static_cast<int>(
            integer(value, "delay_samples", "plant profile"));
        profile.gain_x = finite_number(
            value, "pixels_per_completed_count_x", "plant profile");
        profile.gain_y = finite_number(
            value, "pixels_per_completed_count_y", "plant profile");
        if (!is_safe_identifier(profile.id) ||
            profile.delay_samples <= 0 ||
            profile.delay_samples > kMaximumPlantDelaySamples ||
            !identities.insert(profile.id).second) {
            throw std::runtime_error("plant profile 身份或 delay 无效");
        }
        profiles.push_back(std::move(profile));
    }
    return profiles;
}

json make_extractor(std::string_view producer_sha256) {
    return {
        {"name", "XenAimProductionRedProducer"},
        {"version", 1},
        {"sha256", producer_sha256},
    };
}

json make_measured_reference(const json& source,
                             std::string_view relative_path,
                             std::string_view source_sha256) {
    return {
        {"source_identity_sha256", source.at("source_identity_sha256")},
        {"outside_sequence_sha256", source.at("outside_sequence_sha256")},
        {"metrics", source.at("metrics")},
        {"measurement_uncertainty", {
            {"source_relative_path", relative_path},
            {"source_sha256", source_sha256},
            {"absolute", source.at("absolute_uncertainty")},
        }},
    };
}

std::vector<json> run_trace(
    const json& plan,
    const json& block,
    const PlantProfile& profile,
    const AppConfig& app_config,
    std::string_view plan_sha256,
    std::string_view config_sha256,
    std::string_view producer_sha256) {
    const std::string block_id = text(block, "block_id", "block");
    const std::string role = text(block, "role", "block");
    const std::string block_kind = text(block, "block_kind", "block");
    if (!is_safe_identifier(block_id) ||
        (role != "development" && role != "deletion" &&
         role != "sealed_holdout") ||
        (block_kind != "dynamic" && block_kind != "static" &&
         block_kind != "y_only" && block_kind != "xy_limit")) {
        throw std::runtime_error("block 身份、role 或 kind 无效");
    }
    const auto samples_iterator = block.find("samples");
    if (samples_iterator == block.end() || !samples_iterator->is_array() ||
        samples_iterator->empty() ||
        samples_iterator->size() > kMaximumSamplesPerBlock) {
        throw std::runtime_error("block samples 数量无效");
    }
    const int score_begin = static_cast<int>(
        integer(block, "score_begin", "block"));
    const int score_end = static_cast<int>(
        integer(block, "score_end", "block"));
    if (score_begin < 0 || score_end <= score_begin ||
        score_end > static_cast<int>(samples_iterator->size())) {
        throw std::runtime_error("block score interval 无效");
    }
    const auto initial_world = vector2(
        block, "initial_world_aim_point", "block");
    const auto control_center = vector2(block, "control_center", "block");
    const auto roi_size = integer_vector2(block, "roi_size", "block");
    if (roi_size[0] <= 0 || roi_size[1] <= 0 ||
        app_config.aim.person_class_ids.empty()) {
        throw std::runtime_error("block ROI 或 Aim person class 无效");
    }
    const auto reference_iterator = block.find("measured_reference");
    if (reference_iterator == block.end()) {
        throw std::runtime_error("block 缺少 measured_reference");
    }
    validate_measured_reference(*reference_iterator);

    Aim aim(app_config.aim);
    std::vector<CompletedCommand> due_queue(
        static_cast<std::size_t>(profile.delay_samples));
    std::size_t due_slot = 0;
    double world_x = initial_world[0];
    double world_y = initial_world[1];
    double camera_x = 0.0;
    double camera_y = 0.0;
    std::uint64_t previous_source_sequence = 0;
    std::int64_t previous_source_timestamp = 0;
    std::int64_t previous_control_at_ns = 0;
    std::vector<json> rows;
    rows.reserve(samples_iterator->size());

    const json extractor = make_extractor(producer_sha256);
    const std::string source_session = text(
        plan, "source_clock_session_id", "plan");
    if (source_session.empty()) {
        throw std::runtime_error("source_clock_session_id 不得为空");
    }

    for (std::size_t index = 0; index < samples_iterator->size(); ++index) {
        const json& sample = (*samples_iterator)[index];
        const auto source_sequence_value = integer(
            sample, "source_sequence", "sample");
        const auto source_timestamp = integer(
            sample, "source_timestamp", "sample");
        const auto captured_at_ns = integer(
            sample, "captured_at_ns", "sample");
        const auto control_at_ns = integer(
            sample, "control_at_ns", "sample");
        const auto controller_dt_ns = integer(
            sample, "controller_dt_ns", "sample");
        if (source_sequence_value <= 0 || source_timestamp <= 0 ||
            captured_at_ns <= 0 || control_at_ns < captured_at_ns ||
            controller_dt_ns <= 0 ||
            source_sequence_value <=
                static_cast<std::int64_t>(previous_source_sequence) ||
            source_timestamp <= previous_source_timestamp ||
            (index != 0 &&
             control_at_ns - previous_control_at_ns != controller_dt_ns)) {
            throw std::runtime_error("sample source/time 顺序合同无效");
        }
        previous_source_sequence = static_cast<std::uint64_t>(
            source_sequence_value);
        previous_source_timestamp = source_timestamp;
        previous_control_at_ns = control_at_ns;

        const CompletedCommand due = due_queue[due_slot];
        due_queue[due_slot] = {};
        const double visible_effect_x =
            static_cast<double>(due.dx) * profile.gain_x;
        const double visible_effect_y =
            static_cast<double>(due.dy) * profile.gain_y;
        camera_x += visible_effect_x;
        camera_y += visible_effect_y;
        world_x += finite_number(sample, "world_delta_x", "sample");
        world_y += finite_number(sample, "world_delta_y", "sample");
        const double width = finite_number(sample, "box_width", "sample");
        const double height = finite_number(sample, "box_height", "sample");
        if (width <= 0.0 || height <= 0.0) {
            throw std::runtime_error("sample box size 无效");
        }
        const std::string pose = text(sample, "pose", "sample");
        if (pose != "body") {
            throw std::runtime_error("producer v1 只接受明确的 body pose");
        }
        const bool visible = boolean(sample, "visible", "sample");
        const bool lock_active = boolean(sample, "lock_active", "sample");
        const bool backend_failure = boolean(
            sample, "backend_failure", "sample");

        const double observed_aim_x = world_x + camera_x;
        const double observed_aim_y = world_y + camera_y;
        const double observed_center_y = observed_aim_y +
            (0.5 - static_cast<double>(
                app_config.aim.body_aim_height_ratio)) * height;
        const std::array<double, 4> observed_box{
            observed_aim_x - width * 0.5,
            observed_center_y - height * 0.5,
            observed_aim_x + width * 0.5,
            observed_center_y + height * 0.5,
        };

        AimFrame frame;
        frame.sequence = static_cast<std::uint64_t>(source_sequence_value);
        frame.captured_at = std::chrono::steady_clock::time_point(
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::nanoseconds(captured_at_ns)));
        frame.control_at = std::chrono::steady_clock::time_point(
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::nanoseconds(control_at_ns)));
        frame.roi_width = roi_size[0];
        frame.roi_height = roi_size[1];
        frame.control_center_x = static_cast<float>(control_center[0]);
        frame.control_center_y = static_cast<float>(control_center[1]);
        frame.lock_active = lock_active;
        if (visible) {
            frame.detections.push_back({
                static_cast<float>(observed_box[0]),
                static_cast<float>(observed_box[1]),
                static_cast<float>(observed_box[2]),
                static_cast<float>(observed_box[3]),
                0.95f,
                app_config.aim.person_class_ids.front(),
            });
        }

        const AimResult aim_result = aim.process(frame);
        const int issued_dx = aim_result.has_command
            ? aim_result.command.dx_counts : 0;
        const int issued_dy = aim_result.has_command
            ? aim_result.command.dy_counts : 0;
        CompletedCommand completed;
        if (aim_result.has_command && !backend_failure) {
            completed = {issued_dx, issued_dy};
        }
        const auto backend_completed_at = frame.control_at +
            std::chrono::nanoseconds(kBackendCompletionDelayNs);
        if (aim_result.has_command && !aim.record_backend_completed_command(
                frame.sequence, backend_completed_at,
                completed.dx, completed.dy)) {
            throw std::runtime_error(
                "Aim 拒绝同 source sequence 的 simulated completion");
        }
        due_queue[due_slot] = completed;
        due_slot = (due_slot + 1) % due_queue.size();

        json logical_queue = json::array();
        for (std::size_t offset = 0; offset < due_queue.size(); ++offset) {
            const auto& queued = due_queue[
                (due_slot + offset) % due_queue.size()];
            logical_queue.push_back(json::array({queued.dx, queued.dy}));
        }

        const bool matched = aim_result.has_target &&
            aim_result.target.matched_observation_valid;
        const json matched_box = matched
            ? json::array({
                aim_result.target.matched_observation_x1,
                aim_result.target.matched_observation_y1,
                aim_result.target.matched_observation_x2,
                aim_result.target.matched_observation_y2})
            : json::array({0.0, 0.0, 0.0, 0.0});
        const json base_point = aim_result.has_target
            ? json::array({aim_result.target.base_aim_x,
                           aim_result.target.base_aim_y})
            : json::array({control_center[0], control_center[1]});
        const json delay_point = aim_result.has_target
            ? json::array({aim_result.target.delay_compensated_aim_x,
                           aim_result.target.delay_compensated_aim_y})
            : base_point;
        const json prediction_point = aim_result.has_target
            ? json::array({aim_result.target.prediction_aim_x,
                           aim_result.target.prediction_aim_y})
            : delay_point;
        const double y_error = aim_result.has_target
            ? static_cast<double>(aim_result.target.base_aim_y) -
                control_center[1]
            : 0.0;
        const bool quantization_zero_y = aim_result.has_target &&
            std::abs(y_error) > app_config.aim.deadzone_pixels &&
            issued_dy == 0;
        const std::string completion_status = backend_failure &&
            aim_result.has_command
            ? "backend_failure"
            : (aim_result.has_command ? "completed" : "no_command");
        const std::string completion_zero_reason = backend_failure &&
            aim_result.has_command
            ? "backend_failure"
            : (aim_result.has_command ? "none" : "no_command");

        rows.push_back({
            {"red_schema", kRedSchemaVersion},
            {"asset_id", plan.at("asset_id")},
            {"source_relative_path", "sources/plan.json"},
            {"source_sha256", plan_sha256},
            {"extractor", extractor},
            {"configuration_sha256", config_sha256},
            {"block_id", block_id},
            {"role", role},
            {"reset", index == 0},
            {"sample_index_in_block", index},
            {"score_begin", score_begin},
            {"score_end", score_end},
            {"plant_profile_id", profile.id},
            {"source_sequence", source_sequence_value},
            {"source_timestamp", source_timestamp},
            {"source_clock_session_id", source_session},
            {"captured_at_ns", captured_at_ns},
            {"control_at_ns", control_at_ns},
            {"controller_dt_ns", controller_dt_ns},
            {"observation_age_ns", control_at_ns - captured_at_ns},
            {"backend_completed_at_ns",
             control_at_ns + kBackendCompletionDelayNs},
            {"completion_status", completion_status},
            {"completion_zero_reason", completion_zero_reason},
            {"protocol_acknowledged", false},
            {"backend_completed_dx", completed.dx},
            {"backend_completed_dy", completed.dy},
            {"aim_actual_history_dx", completed.dx},
            {"aim_actual_history_dy", completed.dy},
            {"plant_input_dx", completed.dx},
            {"plant_input_dy", completed.dy},
            {"world_delta_x", finite_number(
                sample, "world_delta_x", "sample")},
            {"world_delta_y", finite_number(
                sample, "world_delta_y", "sample")},
            {"box_width", width},
            {"box_height", height},
            {"pose", pose},
            {"visible", visible},
            {"target_id", sample.at("target_id")},
            {"aim_status", AimStatusName(aim_result.status)},
            {"matched_observation_box", matched_box},
            {"base_point", base_point},
            {"delay_compensated_point", delay_point},
            {"prediction_point", prediction_point},
            {"control_center", json::array(
                {control_center[0], control_center[1]})},
            {"controller_x", {
                {"proportional_counts",
                 aim_result.control.proportional_x_counts},
                {"feedforward_counts",
                 aim_result.control.feedforward_x_counts},
                {"desired_counts", aim_result.control.desired_x_counts},
                {"filtered_counts", aim_result.control.filtered_x_counts},
                {"shaped_counts", aim_result.control.shaped_x_counts},
            }},
            {"issued_dx", issued_dx},
            {"issued_dy", issued_dy},
            {"plant_due_queue", std::move(logical_queue)},
            {"plant_prefix", json::array({camera_x, camera_y})},
            {"camera_visible_effect", json::array(
                {visible_effect_x, visible_effect_y})},
            {"observed_box", json::array(
                {observed_box[0], observed_box[1],
                 observed_box[2], observed_box[3]})},
            {"quantization_zero_x", aim_result.control.quantization_zero_x},
            {"quantization_zero_y", quantization_zero_y},
            {"limit_signature", "shared-vector-limit:" +
                std::to_string(app_config.aim.max_counts_per_frame)},
            {"lock_active", lock_active},
            {"backend_failure", backend_failure},
            {"physical_dispatch_count", 0},
        });
    }
    return rows;
}

} // namespace

namespace detail {

bool rename_directory_with_retry(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    int maximum_attempts,
    std::chrono::milliseconds initial_delay,
    const RenameOperation& rename_operation,
    std::string& error) noexcept {
    error.clear();
    if (maximum_attempts <= 0 || !rename_operation) {
        error = "原子发布重试参数无效";
        return false;
    }
    auto delay = initial_delay.count() < 0
        ? std::chrono::milliseconds(0)
        : initial_delay;
    for (int attempt = 1; attempt <= maximum_attempts; ++attempt) {
        try {
            rename_operation(source, destination);
            error.clear();
            return true;
        } catch (const std::filesystem::filesystem_error& exception) {
            error = exception.what();
            if (attempt == maximum_attempts) return false;
        } catch (const std::exception& exception) {
            error = exception.what();
            return false;
        } catch (...) {
            error = "原子发布发生未知错误";
            return false;
        }
        if (delay.count() > 0) std::this_thread::sleep_for(delay);
        const auto doubled_delay = delay * 2;
        delay = doubled_delay > std::chrono::milliseconds(400)
            ? std::chrono::milliseconds(400)
            : doubled_delay;
    }
    return false;
}

} // namespace detail

bool produce_output_off_bundle(
    const ProduceOptions& options,
    ProduceResult& result,
    std::string& error) noexcept {
    result = {};
    error.clear();
    std::filesystem::path incoming;
    bool owns_incoming = false;
    try {
        if (options.output_directory.empty()) {
            throw std::runtime_error("output_directory 不得为空");
        }
        const auto output = std::filesystem::absolute(
            options.output_directory).lexically_normal();
        incoming = output;
        incoming += ".incoming";
        if (std::filesystem::exists(output)) {
            throw std::runtime_error("拒绝覆盖既有 output 或 incoming");
        }
        std::filesystem::create_directories(output.parent_path());
        // 只有本次原子新建的目录才归本次调用清理；既有目录返回 false，
        // 不能用先 exists 再无条件置 owner 的方式接管其他调用的产物。
        owns_incoming = std::filesystem::create_directory(incoming);
        if (!owns_incoming) {
            throw std::runtime_error("拒绝覆盖既有 output 或 incoming");
        }
        std::filesystem::create_directories(incoming / "sources");
        std::filesystem::create_directories(incoming / "generator");
        std::filesystem::create_directories(incoming / "traces");

        std::string plan_sha256;
        std::string config_sha256;
        std::string reference_sha256;
        std::string producer_sha256;
        if (!copy_verified(
                options.plan_path, incoming / "sources" / "plan.json",
                plan_sha256, error) ||
            !copy_verified(
                options.config_path, incoming / "sources" / "config.ini",
                config_sha256, error)) {
            throw std::runtime_error(error);
        }
        std::string reference_extension =
            options.measured_reference_source_path.extension().string();
        if (reference_extension.empty() || reference_extension.size() > 12) {
            reference_extension = ".bin";
        }
        const auto reference_relative = std::filesystem::path("sources") /
            ("measured-reference" + reference_extension);
        if (!copy_verified(
                options.measured_reference_source_path,
                incoming / reference_relative,
                reference_sha256, error) ||
            !copy_verified(
                options.producer_binary_path,
                incoming / "generator" / "producer.bin",
                producer_sha256, error)) {
            throw std::runtime_error(error);
        }

        const json plan = read_json(incoming / "sources" / "plan.json");
        if (!plan.is_object() ||
            integer(plan, "plan_schema", "plan") != kPlanSchemaVersion ||
            text(plan, "evidence_type", "plan") != kPlanEvidenceType) {
            throw std::runtime_error("plan schema/evidence_type 无效");
        }
        const std::string asset_id = text(plan, "asset_id", "plan");
        if (!is_safe_identifier(asset_id)) {
            throw std::runtime_error("plan asset_id 无效");
        }
        const std::string production_profile = text(
            plan, "production_plant_profile_id", "plan");
        const auto profiles = parse_profiles(plan);
        if (std::none_of(
                profiles.begin(), profiles.end(),
                [&production_profile](const PlantProfile& profile) {
                    return profile.id == production_profile;
                })) {
            throw std::runtime_error("production plant profile 未定义");
        }
        const auto blocks_iterator = plan.find("blocks");
        if (blocks_iterator == plan.end() || !blocks_iterator->is_array() ||
            blocks_iterator->empty()) {
            throw std::runtime_error("plan 缺少 blocks");
        }

        AppConfig app_config;
        std::string config_error;
        if (!load_app_config(
                (incoming / "sources" / "config.ini").string(),
                app_config, config_error)) {
            throw std::runtime_error("Aim config 无效: " + config_error);
        }

        json manifest = {
            {"red_schema", kRedSchemaVersion},
            {"evidence_type", kManifestEvidenceType},
            {"asset_id", asset_id},
            {"generator", make_extractor(producer_sha256)},
            {"configuration_sha256", config_sha256},
            {"physical_output_capability", false},
            {"physical_dispatch_count", 0},
            {"production_aim_changed", false},
            {"candidate_uses_f1", false},
            {"production_plant_profile_id", production_profile},
            {"mandatory_block_profiles", json::array()},
            {"traces", json::array()},
        };

        std::set<std::string> block_ids;
        std::size_t total_samples = 0;
        for (const auto& block : *blocks_iterator) {
            const std::string block_id = text(block, "block_id", "block");
            if (!block_ids.insert(block_id).second) {
                throw std::runtime_error("block_id 重复");
            }
            const std::string role = text(block, "role", "block");
            const std::string block_kind = text(
                block, "block_kind", "block");
            const int score_begin = static_cast<int>(
                integer(block, "score_begin", "block"));
            const int score_end = static_cast<int>(
                integer(block, "score_end", "block"));
            const auto& measured = block.at("measured_reference");
            validate_measured_reference(measured);
            for (const auto& profile : profiles) {
                std::vector<json> rows = run_trace(
                    plan, block, profile, app_config,
                    plan_sha256, config_sha256, producer_sha256);
                const std::string trace_id =
                    block_id + "--" + profile.id + "--B0";
                const auto relative_path = std::filesystem::path("traces") /
                    (trace_id + ".jsonl");
                const auto trace_path = incoming / relative_path;
                write_json_lines(trace_path, rows);
                std::string trace_sha256;
                if (!compute_file_sha256(trace_path, trace_sha256, error)) {
                    throw std::runtime_error(error);
                }
                manifest["mandatory_block_profiles"].push_back({
                    {"block_id", block_id},
                    {"plant_profile_id", profile.id},
                });
                manifest["traces"].push_back({
                    {"trace_id", trace_id},
                    {"block_id", block_id},
                    {"role", role},
                    {"block_kind", block_kind},
                    {"variant", "B0"},
                    {"plant_profile_id", profile.id},
                    {"relative_path", relative_path.generic_string()},
                    {"sha256", trace_sha256},
                    {"sample_count", rows.size()},
                    {"score_begin", score_begin},
                    {"score_end", score_end},
                    {"measured_reference", make_measured_reference(
                        measured, reference_relative.generic_string(),
                        reference_sha256)},
                });
                total_samples += rows.size();
            }
        }

        write_json(incoming / "manifest.json", manifest);
        std::string rename_error;
        if (!detail::rename_directory_with_retry(
                incoming, output, 5, std::chrono::milliseconds(50),
                [](const std::filesystem::path& source,
                   const std::filesystem::path& destination) {
                    std::filesystem::rename(source, destination);
                },
                rename_error)) {
            throw std::runtime_error(
                "原子发布目录失败: " + rename_error);
        }
        owns_incoming = false;
        result.manifest_path = output / "manifest.json";
        result.trace_count = manifest["traces"].size();
        result.sample_count = total_samples;
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
    } catch (...) {
        error = "output-off producer 发生未知错误";
    }
    if (owns_incoming) {
        std::error_code ignored;
        std::filesystem::remove_all(incoming, ignored);
    }
    result = {};
    return false;
}

} // namespace aim_production_red
