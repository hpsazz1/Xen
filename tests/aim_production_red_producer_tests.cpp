#include "aim_production_red/aim_production_red.h"
#include "aim_production_red/aim_production_red_internal.h"
#include "config/config.h"

#include <nlohmann/json.hpp>

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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

std::string read_text(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {};
    return {std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>()};
}

void require_plain_path_chain(const std::filesystem::path& path) {
    for (auto current = path; !current.empty();) {
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            throw std::runtime_error("测试路径不存在或包含 reparse point");
        }
        const auto parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }
}

class OwnedTestDirectory {
public:
    OwnedTestDirectory() {
        parent_ = std::filesystem::absolute(
            std::filesystem::temp_directory_path()).lexically_normal();
        if (parent_ != parent_.root_path() && parent_.filename().empty()) {
            parent_ = parent_.parent_path();
        }
        require_plain_path_chain(parent_);
        static unsigned int sequence = 0;
        const auto unique = std::to_string(GetCurrentProcessId()) + "-" +
            std::to_string(std::chrono::steady_clock::now()
                               .time_since_epoch().count()) + "-" +
            std::to_string(++sequence);
        root_ = parent_ / ("xen-aim-production-red-producer-" + unique);
        if (!std::filesystem::create_directory(root_)) {
            throw std::runtime_error("未取得测试目录所有权");
        }
        std::cout << "自有测试目录: " << root_.string() << '\n';
    }

    ~OwnedTestDirectory() noexcept {
        try {
            // 只清理本构造函数原子创建的直属子目录；整条路径和目录树不得跳转。
            if (!root_.is_absolute() || root_.lexically_normal() != root_ ||
                root_.parent_path() != parent_) {
                throw std::runtime_error("测试清理路径越过自有目录边界");
            }
            require_plain_path_chain(root_);
            for (const auto& entry :
                 std::filesystem::recursive_directory_iterator(root_)) {
                const DWORD attributes = GetFileAttributesW(entry.path().c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES ||
                    (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                    throw std::runtime_error("测试目录树包含 reparse point");
                }
            }
            std::filesystem::remove_all(root_);
        } catch (const std::exception& exception) {
            expect(false, "拒绝或未能清理自有测试目录: " +
                   std::string(exception.what()));
        }
    }

    OwnedTestDirectory(const OwnedTestDirectory&) = delete;
    OwnedTestDirectory& operator=(const OwnedTestDirectory&) = delete;

    const std::filesystem::path& path() const noexcept { return root_; }

private:
    std::filesystem::path parent_;
    std::filesystem::path root_;
};

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
    const OwnedTestDirectory directory;
    const auto& root = directory.path();
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

}

void test_producer_preserves_existing_incoming() {
    const OwnedTestDirectory directory;
    const auto output = directory.path() / "bundle";
    const auto incoming = directory.path() / "bundle.incoming";
    std::filesystem::create_directories(incoming / "nested");
    const auto sentinel = incoming / "nested" / "sentinel.bin";
    const std::string original_bytes("existing\0incoming\r\n", 19);
    write_text(sentinel, original_bytes);

    aim_production_red::ProduceResult result;
    std::string error;
    const bool produced = aim_production_red::produce_output_off_bundle(
        {{}, {}, {}, {}, output}, result, error);
    expect(!produced && !error.empty() && result.manifest_path.empty() &&
               result.trace_count == 0 && result.sample_count == 0 &&
               !std::filesystem::exists(output) &&
               std::filesystem::is_directory(incoming) &&
               read_text(sentinel) == original_bytes,
           "拒绝既有 incoming 必须保留目录及原始 bytes，且不得发布 output");
}

void test_producer_preserves_existing_output_and_incoming() {
    const OwnedTestDirectory directory;
    const auto output = directory.path() / "bundle";
    const auto incoming = directory.path() / "bundle.incoming";
    std::filesystem::create_directory(output);
    std::filesystem::create_directory(incoming);
    const auto output_sentinel = output / "output.bin";
    const auto incoming_sentinel = incoming / "incoming.bin";
    write_text(output_sentinel, "existing published bytes\r\n");
    write_text(incoming_sentinel, "existing incoming bytes\r\n");

    aim_production_red::ProduceResult result{output / "stale.json", 9, 99};
    std::string error;
    const bool produced = aim_production_red::produce_output_off_bundle(
        {{}, {}, {}, {}, output}, result, error);
    expect(!produced && !error.empty() && result.manifest_path.empty() &&
               result.trace_count == 0 && result.sample_count == 0 &&
               read_text(output_sentinel) == "existing published bytes\r\n" &&
               read_text(incoming_sentinel) == "existing incoming bytes\r\n",
           "output 和 incoming 均存在时必须拒绝覆盖并保留双方原始 bytes");
}

void test_producer_preserves_file_at_incoming_path() {
    const OwnedTestDirectory directory;
    const auto output = directory.path() / "bundle";
    const auto incoming = directory.path() / "bundle.incoming";
    const std::string original_bytes("incoming\0file\r\n", 15);
    write_text(incoming, original_bytes);

    aim_production_red::ProduceResult result;
    std::string error;
    const bool produced = aim_production_red::produce_output_off_bundle(
        {{}, {}, {}, {}, output}, result, error);
    expect(!produced && !error.empty() && result.manifest_path.empty() &&
               result.trace_count == 0 && result.sample_count == 0 &&
               !std::filesystem::exists(output) &&
               std::filesystem::is_regular_file(incoming) &&
               read_text(incoming) == original_bytes,
           "incoming 位置已有普通文件时必须拒绝创建并保留原始 bytes，且不得发布 output");
}

void test_producer_preserves_parent_when_incoming_creation_fails() {
    const OwnedTestDirectory directory;
    const auto blocked_parent = directory.path() / "parent.bin";
    write_text(blocked_parent, "parent is a file\r\n");

    aim_production_red::ProduceResult result;
    std::string error;
    const bool produced = aim_production_red::produce_output_off_bundle(
        {{}, {}, {}, {}, blocked_parent / "bundle"}, result, error);
    expect(!produced && !error.empty() && result.manifest_path.empty() &&
               result.trace_count == 0 && result.sample_count == 0 &&
               std::filesystem::is_regular_file(blocked_parent) &&
               read_text(blocked_parent) == "parent is a file\r\n" &&
               std::distance(std::filesystem::directory_iterator(directory.path()),
                             std::filesystem::directory_iterator()) == 1,
           "incoming 无法创建时必须保留阻挡父路径的原始文件，且不留发布产物");
}

void test_producer_cleans_only_owned_incoming_after_failure() {
    const OwnedTestDirectory directory;
    const auto output = directory.path() / "bundle";
    const auto incoming = directory.path() / "bundle.incoming";
    const auto sibling = directory.path() / "unrelated.bin";
    write_text(sibling, "unrelated original bytes\r\n");

    aim_production_red::ProduceResult result;
    std::string error;
    const bool produced = aim_production_red::produce_output_off_bundle(
        {directory.path() / "missing-plan.json", {}, {}, {}, output},
        result, error);
    expect(!produced && !error.empty() && result.manifest_path.empty() &&
               result.trace_count == 0 && result.sample_count == 0 &&
               !std::filesystem::exists(output) &&
               !std::filesystem::exists(incoming) &&
               read_text(sibling) == "unrelated original bytes\r\n",
           "新建 incoming 后输入失败只可清理本次目录，必须保留同级既有文件");
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
    test_producer_preserves_existing_incoming();
    test_producer_preserves_existing_output_and_incoming();
    test_producer_preserves_file_at_incoming_path();
    test_producer_preserves_parent_when_incoming_creation_fails();
    test_producer_cleans_only_owned_incoming_after_failure();
    test_atomic_publish_retries_transient_access_denied();
    if (failures != 0) {
        std::cerr << failures << " 个 Aim production red producer 测试失败\n";
        return 1;
    }
    std::cout << "Aim production red producer 测试通过\n";
    return 0;
}
