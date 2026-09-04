#include "aim_production_red/aim_production_red.h"
#include "aim_production_red/aim_production_red_internal.h"
#include "config/config.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

using json = nlohmann::json;

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "[失败] " << message << '\n';
}

void write_text(const std::filesystem::path& path, std::string_view text) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << text;
    if (!stream) throw std::runtime_error("写入测试文件失败");
}

json make_plan() {
    json samples = json::array();
    for (int index = 0; index < 8; ++index) {
        samples.push_back({
            {"source_sequence", 100 + index},
            {"source_timestamp", 500000 + index * 41667},
            {"captured_at_ns", 1000000000LL + index * 4166700LL},
            {"control_at_ns", 1003000000LL + index * 4166700LL},
            {"controller_dt_ns", 4166700},
            {"world_delta_x", index == 0 ? 0.0 : 1.0},
            {"world_delta_y", 0.0},
            {"box_width", 20.0},
            {"box_height", 40.0},
            {"pose", "body"},
            {"visible", true},
            {"target_id", 7},
            {"lock_active", true},
            {"backend_failure", index == 6},
        });
    }
    return {
        {"plan_schema", 1},
        {"evidence_type", "aim_production_red_output_off_plan"},
        {"asset_id", "producer-native-ledger-test"},
        {"source_clock_session_id", "synthetic:test-clock-v1"},
        {"production_plant_profile_id", "P-TEST-D3"},
        {"plant_profiles", json::array({{
            {"plant_profile_id", "P-TEST-D3"},
            {"delay_samples", 3},
            {"pixels_per_completed_count_x", -0.5},
            {"pixels_per_completed_count_y", -0.5},
        }})},
        {"blocks", json::array({{
            {"block_id", "RED-NATIVE-LEDGER"},
            {"role", "development"},
            {"block_kind", "dynamic"},
            {"score_begin", 2},
            {"score_end", 8},
            {"initial_world_aim_point", json::array({180.0, 160.0})},
            {"control_center", json::array({160.0, 160.0})},
            {"roi_size", json::array({320, 320})},
            {"measured_reference", {
                {"source_identity_sha256", std::string(64, '0')},
                {"outside_sequence_sha256", std::string(64, '1')},
                {"metrics", {
                    {"outside_samples", 99},
                    {"outside_duration_ns", 99},
                    {"longest_outside_samples", 99},
                    {"longest_outside_ns", 99},
                    {"outside_area_px_ns", 99.0},
                    {"max_excess_x_px", 99.0},
                    {"max_abs_error_x_px", 99.0},
                }},
                {"absolute_uncertainty", {
                    {"outside_area_px_ns", 0.0},
                    {"max_excess_x_px", 0.0},
                    {"max_abs_error_x_px", 0.0},
                }},
            }},
            {"samples", std::move(samples)},
        }})},
    };
}

void test_producer_records_native_source_and_completed_ledgers() {
    const auto unique = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto root = std::filesystem::temp_directory_path() /
        ("xen-aim-production-red-producer-" + unique);
    std::filesystem::create_directories(root);
    const auto plan_path = root / "plan.json";
    const auto config_path = root / "config.ini";
    const auto reference_path = root / "measured.csv";
    const auto binary_path = root / "producer.exe";
    const auto output_path = root / "bundle";

    write_text(plan_path, make_plan().dump(2) + "\n");
    AppConfig config;
    config.aim.smoothing = 0.475f;
    config.aim.counts_per_pixel_x = 0.425f;
    config.aim.counts_per_pixel_y = 0.4f;
    config.aim.max_counts_per_frame = 14.0f;
    config.aim.enable_delay_compensation = true;
    config.aim.control_delay_ms = 15.0f;
    config.aim.max_delay_compensation_ms = 44.0f;
    config.aim.enable_prediction = false;
    std::string config_error;
    expect(save_app_config(config_path.string(), config, config_error),
           "测试配置必须可写: " + config_error);
    write_text(reference_path, "independent measured reference\n");
    write_text(binary_path, "producer identity\n");

    aim_production_red::ProduceResult result;
    std::string error;
    const bool produced = aim_production_red::produce_output_off_bundle(
        {plan_path, config_path, reference_path, binary_path, output_path},
        result, error);
    expect(produced, "producer 必须生成 output-off bundle: " + error);
    expect(result.manifest_path == output_path / "manifest.json" &&
               result.trace_count == 1 && result.sample_count == 8,
           "producer 必须返回原子 bundle 的 trace/sample 身份");
    if (produced) {
        std::ifstream manifest_stream(result.manifest_path);
        json manifest;
        manifest_stream >> manifest;
        expect(manifest["physical_output_capability"] == false &&
                   manifest["physical_dispatch_count"] == 0 &&
                   manifest["production_aim_changed"] == false &&
                   manifest["traces"].size() == 1,
               "manifest 必须显式冻结 output-off 且只发布一个 B0 trace");

        const auto trace_path = output_path /
            manifest["traces"][0]["relative_path"].get<std::string>();
        std::ifstream trace_stream(trace_path);
        std::string line;
        int index = 0;
        int nonzero_completed = 0;
        bool saw_independent_failure = false;
        while (std::getline(trace_stream, line)) {
            const json row = json::parse(line);
            expect(row["source_sequence"] == 100 + index &&
                       row["source_timestamp"] ==
                           500000 + index * 41667 &&
                       row["source_clock_session_id"] ==
                           "synthetic:test-clock-v1",
                   "每行必须保留 plan 原生 source identity");
            expect(row["physical_dispatch_count"] == 0 &&
                       row["protocol_acknowledged"] == false,
                   "模拟 backend 不得伪造 physical dispatch 或 protocol ACK");
            expect(row["aim_actual_history_dx"] ==
                           row["backend_completed_dx"] &&
                       row["aim_actual_history_dy"] ==
                           row["backend_completed_dy"] &&
                       row["plant_input_dx"] ==
                           row["backend_completed_dx"] &&
                       row["plant_input_dy"] ==
                           row["backend_completed_dy"],
                   "Aim actual history 与 plant 只能消费独立 completed ledger");
            if (row["backend_completed_dx"].get<int>() != 0) {
                ++nonzero_completed;
            }
            if (index == 6) {
                saw_independent_failure =
                    row["backend_failure"] == true &&
                    row["issued_dx"].get<int>() != 0 &&
                    row["backend_completed_dx"] == 0 &&
                    row["backend_completed_dy"] == 0 &&
                    row["completion_status"] == "backend_failure" &&
                    row["completion_zero_reason"] == "backend_failure";
            }
            ++index;
        }
        expect(index == 8 && nonzero_completed > 0,
               "trace 必须逐样本保存，并包含实际模拟完成命令");
        expect(saw_independent_failure,
               "backend failure 必须保留 issued，但 completed/actual/plant 归零");
    }

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void test_atomic_publish_retries_transient_access_denied() {
    int attempts = 0;
    std::string error;
    const bool renamed =
        aim_production_red::detail::rename_directory_with_retry(
            "source.incoming", "destination", 3,
            std::chrono::milliseconds(0),
            [&attempts](const std::filesystem::path&,
                        const std::filesystem::path&) {
                ++attempts;
                if (attempts < 3) {
                    throw std::filesystem::filesystem_error(
                        "transient sharing violation",
                        std::make_error_code(std::errc::permission_denied));
                }
            },
            error);
    expect(renamed && attempts == 3 && error.empty(),
           "原子发布必须有限重试 transient access denied");
}

} // namespace

int main() {
    test_producer_records_native_source_and_completed_ledgers();
    test_atomic_publish_retries_transient_access_denied();
    if (failures != 0) {
        std::cerr << failures << " 个 Aim production red producer 测试失败\n";
        return 1;
    }
    std::cout << "Aim production red producer 测试通过\n";
    return 0;
}
