#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include "config/config.h"
#include "runtime/basic_aim_controller.h"
#include "runtime/build_identity.h"
#include "runtime/machine_profile_cache.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

std::mutex configMutex;

namespace
{
struct Options
{
    std::filesystem::path config;
    std::filesystem::path dataRoot;
    std::filesystem::path output;
    std::string aimMode = "hipfire";
    int probeRoiX = 0;
    int probeRoiY = 0;
    int probeRoiWidth = 0;
    int probeRoiHeight = 0;
    bool confirmed = false;
};

bool parseInt(const std::string& text, int& value)
{
    try
    {
        size_t used = 0;
        value = std::stoi(text, &used);
        return used == text.size();
    }
    catch (...) { return false; }
}

bool parseDouble(const std::string& text, double& value)
{
    try
    {
        size_t used = 0;
        value = std::stod(text, &used);
        return used == text.size() && std::isfinite(value);
    }
    catch (...) { return false; }
}

bool parseOptions(int argc, char** argv, Options& options, std::string& error)
{
    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];
        auto next = [&]() -> std::string {
            return index + 1 < argc ? argv[++index] : std::string{};
        };
        if (argument == "--config") options.config = next();
        else if (argument == "--data-root") options.dataRoot = next();
        else if (argument == "--output") options.output = next();
        else if (argument == "--aim-mode") options.aimMode = next();
        else if (argument == "--probe-roi-x")
        {
            const std::string value = next();
            if (!parseInt(value, options.probeRoiX)) error = "--probe-roi-x无效";
        }
        else if (argument == "--probe-roi-y")
        {
            const std::string value = next();
            if (!parseInt(value, options.probeRoiY)) error = "--probe-roi-y无效";
        }
        else if (argument == "--probe-roi-width")
        {
            const std::string value = next();
            if (!parseInt(value, options.probeRoiWidth)) error = "--probe-roi-width无效";
        }
        else if (argument == "--probe-roi-height")
        {
            const std::string value = next();
            if (!parseInt(value, options.probeRoiHeight)) error = "--probe-roi-height无效";
        }
        else if (argument == "--confirm-manual-review")
            options.confirmed = next() == "YES";
        else error = "未知参数:" + argument;
        if (!error.empty()) return false;
    }

    if (!options.confirmed) error = "必须传入 --confirm-manual-review YES";
    else if (options.config.empty() || !options.config.is_absolute()) error = "--config必须是绝对路径";
    else if (options.dataRoot.empty() || !options.dataRoot.is_absolute()) error = "--data-root必须是绝对路径";
    else if (options.output.empty() || !options.output.is_absolute()) error = "--output必须是绝对路径";
    else if (options.aimMode.empty() || options.aimMode.find_first_of("\r\n=") != std::string::npos)
        error = "--aim-mode无效";
    else if (options.probeRoiX < 0 || options.probeRoiY < 0 ||
             options.probeRoiWidth < 8 || options.probeRoiHeight < 8)
        error = "探针ROI坐标必须非负且宽高至少8像素";
    return error.empty();
}

bool readKeyValues(const std::filesystem::path& path,
                   std::map<std::string, std::string>& values,
                   std::string& error)
{
    std::ifstream input(path);
    if (!input)
    {
        error = "decision_open_failed";
        return false;
    }
    std::string line;
    size_t lineNumber = 0;
    while (std::getline(input, line))
    {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // PowerShell UTF-8输出可能在首行带BOM；BOM不是协议字段名的一部分。
        if (lineNumber == 1 && line.size() >= 3 &&
            static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF)
        {
            line.erase(0, 3);
        }
        const size_t separator = line.find('=');
        if (separator == std::string::npos || separator == 0)
        {
            error = "decision_invalid_line:" + std::to_string(lineNumber);
            return false;
        }
        if (!values.emplace(line.substr(0, separator), line.substr(separator + 1)).second)
        {
            error = "decision_duplicate_field:" + line.substr(0, separator);
            return false;
        }
    }
    return true;
}

std::vector<std::string> split(const std::string& text, char separator)
{
    std::vector<std::string> values;
    std::stringstream stream(text);
    std::string value;
    while (std::getline(stream, value, separator)) values.push_back(value);
    return values;
}

bool parseCsvRow(const std::string& line, std::vector<std::string>& fields)
{
    fields.clear();
    size_t index = 0;
    while (index <= line.size())
    {
        std::string field;
        if (index < line.size() && line[index] == '"')
        {
            ++index;
            bool closed = false;
            while (index < line.size())
            {
                if (line[index] != '"')
                {
                    field.push_back(line[index++]);
                    continue;
                }
                if (index + 1 < line.size() && line[index + 1] == '"')
                {
                    field.push_back('"');
                    index += 2;
                    continue;
                }
                ++index;
                closed = true;
                break;
            }
            if (!closed || (index < line.size() && line[index] != ',')) return false;
        }
        else
        {
            while (index < line.size() && line[index] != ',')
            {
                if (line[index] == '"') return false;
                field.push_back(line[index++]);
            }
        }
        fields.push_back(field);
        if (index == line.size()) break;
        ++index;
        if (index == line.size())
        {
            fields.emplace_back();
            break;
        }
    }
    return true;
}

bool sha256File(const std::filesystem::path& path, std::string& digest, std::string& error)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        error = "hash_open_failed:" + path.filename().string();
        return false;
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectLength = 0;
    DWORD hashLength = 0;
    DWORD received = 0;
    std::vector<unsigned char> object;
    std::vector<unsigned char> bytes;
    auto cleanup = [&]() {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    };
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &received, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&hashLength), sizeof(hashLength), &received, 0) < 0)
    {
        cleanup();
        error = "sha256_initialization_failed";
        return false;
    }
    object.resize(objectLength);
    bytes.resize(hashLength);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectLength,
                         nullptr, 0, 0) < 0)
    {
        cleanup();
        error = "sha256_create_failed";
        return false;
    }

    std::vector<unsigned char> buffer(64 * 1024);
    while (input)
    {
        input.read(reinterpret_cast<char*>(buffer.data()),
                   static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0 && BCryptHashData(hash, buffer.data(),
            static_cast<ULONG>(count), 0) < 0)
        {
            cleanup();
            error = "sha256_update_failed";
            return false;
        }
    }
    if (!input.eof() || BCryptFinishHash(hash, bytes.data(), hashLength, 0) < 0)
    {
        cleanup();
        error = "sha256_finish_failed";
        return false;
    }
    cleanup();
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (unsigned char byte : bytes) output << std::setw(2) << static_cast<int>(byte);
    digest = output.str();
    return true;
}

bool validateSummary(const std::filesystem::path& path,
                     int expectedRuns,
                     int expectedTrials,
                     const std::vector<int>& requiredCounts,
                     std::string& error)
{
    std::ifstream input(path);
    if (!input)
    {
        error = "summary_open_failed";
        return false;
    }
    std::string header;
    if (!std::getline(input, header))
    {
        error = "summary_header_invalid";
        return false;
    }
    if (!header.empty() && header.back() == '\r') header.pop_back();
    if (header.size() >= 3 &&
        static_cast<unsigned char>(header[0]) == 0xEF &&
        static_cast<unsigned char>(header[1]) == 0xBB &&
        static_cast<unsigned char>(header[2]) == 0xBF)
    {
        header.erase(0, 3);
    }
    std::vector<std::string> columns;
    if (!parseCsvRow(header, columns))
    {
        error = "summary_header_invalid";
        return false;
    }
    std::map<std::string, size_t> indices;
    for (size_t index = 0; index < columns.size(); ++index)
    {
        if (!indices.emplace(columns[index], index).second)
        {
            error = "summary_duplicate_column:" + columns[index];
            return false;
        }
    }
    const std::vector<std::string> required = {
        "Run", "Counts", "Axis", "PositiveTrials", "NegativeTrials", "Passed" };
    for (const std::string& name : required)
    {
        if (!indices.count(name))
        {
            error = "summary_missing_column:" + name;
            return false;
        }
    }

    int totalTrials = 0;
    size_t rowCount = 0;
    std::set<std::string> combinations;
    std::string line;
    while (std::getline(input, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty())
        {
            error = "summary_row_invalid:" + std::to_string(rowCount + 2);
            return false;
        }
        std::vector<std::string> fields;
        if (!parseCsvRow(line, fields))
        {
            error = "summary_row_invalid:" + std::to_string(rowCount + 2);
            return false;
        }
        if (fields.size() != columns.size())
        {
            error = "summary_column_count_mismatch";
            return false;
        }
        int run = 0;
        int counts = 0;
        int positive = 0;
        int negative = 0;
        if (!parseInt(fields[indices["Run"]], run) ||
            !parseInt(fields[indices["Counts"]], counts) ||
            !parseInt(fields[indices["PositiveTrials"]], positive) ||
            !parseInt(fields[indices["NegativeTrials"]], negative) ||
            run < 1 || run > expectedRuns || positive < 1 || negative < 1 ||
            std::find(requiredCounts.begin(), requiredCounts.end(), counts) == requiredCounts.end())
        {
            error = "summary_numeric_contract_failed";
            return false;
        }
        const std::string axis = fields[indices["Axis"]];
        const std::string passed = fields[indices["Passed"]];
        if ((axis != "x" && axis != "y") ||
            (passed != "True" && passed != "true" && passed != "1"))
        {
            error = "summary_gate_failed";
            return false;
        }
        const std::string combination = std::to_string(run) + ":" +
            std::to_string(counts) + ":" + axis;
        if (!combinations.insert(combination).second)
        {
            error = "summary_duplicate_combination:" + combination;
            return false;
        }
        totalTrials += positive + negative;
        ++rowCount;
    }
    const size_t expectedRows = static_cast<size_t>(expectedRuns) *
        requiredCounts.size() * 2;
    if (rowCount != expectedRows || combinations.size() != expectedRows ||
        totalTrials != expectedTrials)
    {
        error = "summary_completeness_failed";
        return false;
    }
    return true;
}

std::string configBackendIdentity(const Config& config)
{
    std::string backend = config.backend;
    std::transform(backend.begin(), backend.end(), backend.begin(),
        [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    return backend == "DML" ? "DML" : backend == "TRT" ? "CUDA" : backend;
}

std::string captureSource(const Config& config)
{
    if (config.capture_method == "ndi") return config.ndi_source_name;
    if (config.capture_method == "udp_capture")
        return config.udp_ip + ":" + std::to_string(config.udp_port);
    if (config.capture_target == "window") return config.capture_window_title;
    return "monitor:" + std::to_string(config.monitor_idx);
}

std::string inputIdentity(const Config& config)
{
    if (config.input_method == "KMBOX_NET") return config.kmbox_net_uuid;
    if (config.input_method == "KMBOX_A") return config.kmbox_a_pidvid;
    if (config.input_method == "MAKCU") return config.makcu_port;
    return config.input_method;
}

bool auditInvalidation(const MachineProfileCache& cache,
                       const MachineProfileKey& exact,
                       size_t& auditedFields,
                       std::string& error)
{
    std::vector<std::pair<std::string, MachineProfileKey>> variants;
    auto add = [&](const char* field, const MachineProfileKey& key) {
        variants.emplace_back(field, key);
    };
    MachineProfileKey key = exact; key.gameProfile += "_changed"; add("gameProfile", key);
    key = exact; key.aimMode += "_changed"; add("aimMode", key);
    key = exact; key.captureMethod += "_changed"; add("captureMethod", key);
    key = exact; key.captureSource += "_changed"; add("captureSource", key);
    key = exact; ++key.sourceWidth; add("sourceWidth", key);
    key = exact; ++key.sourceHeight; add("sourceHeight", key);
    key = exact; ++key.roiX; add("roiX", key);
    key = exact; ++key.roiY; add("roiY", key);
    key = exact; ++key.roiWidth; add("roiWidth", key);
    key = exact; ++key.roiHeight; add("roiHeight", key);
    key = exact; key.inferenceBackend += "_changed"; add("inferenceBackend", key);
    key = exact; key.inputMethod += "_changed"; add("inputMethod", key);
    key = exact; key.inputDeviceIdentity += "_changed"; add("inputDeviceIdentity", key);
    key = exact; key.sensitivity += 0.001; add("sensitivity", key);
    key = exact; key.yaw += 0.001; add("yaw", key);
    key = exact; key.pitch += 0.001; add("pitch", key);
    key = exact; key.fovXDegrees += 0.1; add("fovXDegrees", key);
    key = exact; key.fovYDegrees += 0.1; add("fovYDegrees", key);
    key = exact; key.fovScaled = !key.fovScaled; add("fovScaled", key);
    key = exact; key.baseFovDegrees += 0.1; add("baseFovDegrees", key);
    key = exact; ++key.controllerRevision; add("controllerRevision", key);

    for (const auto& [field, changed] : variants)
    {
        const MachineProfileDecision decision = cache.evaluate(changed, true, true);
        if (decision.level != MachineProfileLevel::ConservativeAngle ||
            decision.cacheMatched || decision.reason != "cache_key_mismatch:" + field)
        {
            error = "invalidation_audit_failed:" + field;
            return false;
        }
    }
    auditedFields = variants.size();
    return true;
}
}

int main(int argc, char** argv)
{
    Options options;
    std::string error;
    if (!parseOptions(argc, argv, options, error))
    {
        std::cerr << "ERROR=" << error << '\n';
        return 2;
    }
    const std::filesystem::path decisionPath =
        options.dataRoot / "active_profile_protocol_decision.txt";
    const std::filesystem::path summaryPath =
        options.dataRoot / "active_profile_protocol_summary.csv";
    if (!std::filesystem::is_regular_file(options.config) ||
        !std::filesystem::is_regular_file(decisionPath) ||
        !std::filesystem::is_regular_file(summaryPath))
    {
        std::cerr << "ERROR=required_input_missing\n";
        return 3;
    }

    std::map<std::string, std::string> decision;
    if (!readKeyValues(decisionPath, decision, error))
    {
        std::cerr << "ERROR=" << error << '\n';
        return 4;
    }
    const std::vector<std::string> requiredDecisionFields = {
        "ProtocolPassed", "Identity", "Runs", "Trials", "RequiredCounts",
        "AxisXPixelsPerCount", "AxisYPixelsPerCount", "AxisXT50Ms", "AxisYT50Ms",
        "AxisXT90Ms", "AxisYT90Ms", "Recommendation", "ProfileAutoWrite", "Issues" };
    for (const std::string& field : requiredDecisionFields)
    {
        if (!decision.count(field))
        {
            std::cerr << "ERROR=decision_missing_field:" << field << '\n';
            return 5;
        }
    }
    auto requiredValue = [&](const char* name) -> std::string {
        const auto it = decision.find(name);
        return it == decision.end() ? std::string{} : it->second;
    };
    if (requiredValue("ProtocolPassed") != "1" ||
        requiredValue("Recommendation") != "MANUAL_REVIEW_ONLY" ||
        requiredValue("ProfileAutoWrite") != "0" ||
        !requiredValue("Issues").empty())
    {
        std::cerr << "ERROR=decision_manual_review_gate_failed\n";
        return 5;
    }

    int runs = 0;
    int trials = 0;
    if (!parseInt(requiredValue("Runs"), runs) || !parseInt(requiredValue("Trials"), trials) ||
        runs < 3 || trials < 120)
    {
        std::cerr << "ERROR=decision_trial_contract_failed\n";
        return 6;
    }
    std::vector<int> requiredCounts;
    for (const std::string& value : split(requiredValue("RequiredCounts"), ','))
    {
        int counts = 0;
        if (!parseInt(value, counts) || counts <= 0)
        {
            std::cerr << "ERROR=decision_counts_invalid\n";
            return 7;
        }
        requiredCounts.push_back(counts);
    }
    if (requiredCounts.empty() ||
        !validateSummary(summaryPath, runs, trials, requiredCounts, error))
    {
        std::cerr << "ERROR=" << (error.empty() ? "summary_invalid" : error) << '\n';
        return 8;
    }

    const std::vector<std::string> identity = split(requiredValue("Identity"), '|');
    int evidenceControllerRevision = 0;
    if (identity.size() != 3 || identity[0].empty() || identity[1].empty() ||
        identity[2].size() < 2 || identity[2][0] != 'r' ||
        !parseInt(identity[2].substr(1), evidenceControllerRevision) ||
        evidenceControllerRevision != kBasicAimControllerRevision)
    {
        std::cerr << "ERROR=decision_identity_invalid_or_stale\n";
        return 9;
    }

    std::string configDigestBefore;
    std::string decisionDigest;
    std::string summaryDigest;
    if (!sha256File(options.config, configDigestBefore, error) ||
        !sha256File(decisionPath, decisionDigest, error) ||
        !sha256File(summaryPath, summaryDigest, error))
    {
        std::cerr << "ERROR=" << error << '\n';
        return 10;
    }

    Config config;
    if (!config.loadConfig(options.config.string()))
    {
        std::cerr << "ERROR=config_load_failed\n";
        return 11;
    }
    // 首版主动协议只在 NDI 接收帧上建立证据；拒绝为没有同源尺寸语义的采集方式猜测缓存键。
    if (config.capture_method != "ndi" || config.ndi_source_name.empty() ||
        config.ndi_source_width <= 0 || config.ndi_source_height <= 0)
    {
        std::cerr << "ERROR=unsupported_or_incomplete_capture_context\n";
        return 12;
    }
    const Config::GameProfile& game = config.currentProfile();
    const std::string backend = configBackendIdentity(config);
    if (backend != identity[0] || backend != BuildIdentity::backend())
    {
        std::cerr << "ERROR=backend_identity_mismatch\n";
        return 13;
    }

    MachineProfileRecord record;
    record.key.gameProfile = config.active_game;
    record.key.aimMode = options.aimMode;
    record.key.captureMethod = config.capture_method;
    record.key.captureSource = captureSource(config);
    record.key.sourceWidth = config.capture_method == "udp_capture"
        ? config.udp_source_width : config.ndi_source_width;
    record.key.sourceHeight = config.capture_method == "udp_capture"
        ? config.udp_source_height : config.ndi_source_height;
    record.key.roiWidth = config.detection_resolution;
    record.key.roiHeight = config.detection_resolution;
    record.key.roiX = std::max(0, (record.key.sourceWidth - record.key.roiWidth) / 2);
    record.key.roiY = std::max(0, (record.key.sourceHeight - record.key.roiHeight) / 2);
    record.key.inferenceBackend = backend;
    record.key.inputMethod = config.input_method;
    record.key.inputDeviceIdentity = inputIdentity(config);
    record.key.sensitivity = game.sens;
    record.key.yaw = game.yaw;
    record.key.pitch = game.pitch;
    record.key.fovXDegrees = config.fovX;
    record.key.fovYDegrees = config.fovY;
    record.key.fovScaled = game.fovScaled;
    record.key.baseFovDegrees = game.baseFOV;
    record.key.controllerRevision = kBasicAimControllerRevision;
    const double fovScale = game.fovScaled && game.baseFOV > 1.0
        ? static_cast<double>(config.fovX) / game.baseFOV : 1.0;

    record.evidence.probeRoiX = options.probeRoiX;
    record.evidence.probeRoiY = options.probeRoiY;
    record.evidence.probeRoiWidth = options.probeRoiWidth;
    record.evidence.probeRoiHeight = options.probeRoiHeight;
    if (!parseDouble(requiredValue("AxisXPixelsPerCount"), record.evidence.pixelsPerCountX) ||
        !parseDouble(requiredValue("AxisYPixelsPerCount"), record.evidence.pixelsPerCountY) ||
        !parseDouble(requiredValue("AxisXT50Ms"), record.evidence.t50MsX) ||
        !parseDouble(requiredValue("AxisYT50Ms"), record.evidence.t50MsY) ||
        !parseDouble(requiredValue("AxisXT90Ms"), record.evidence.t90MsX) ||
        !parseDouble(requiredValue("AxisYT90Ms"), record.evidence.t90MsY))
    {
        std::cerr << "ERROR=decision_measurement_invalid\n";
        return 14;
    }
    record.evidence.degreesPerCountX = game.sens * game.yaw * fovScale;
    record.evidence.degreesPerCountY = game.sens * game.pitch * fovScale;
    record.evidence.confidence = 1.0;
    record.evidence.trialCount = static_cast<size_t>(trials);
    record.evidence.protocol = "active_profile_v1";
    record.evidence.buildIdentity = requiredValue("Identity");
    record.evidence.evidenceDigest = "sha256:" + decisionDigest + ":" +
        summaryDigest + ":" + configDigestBefore;

    if (!MachineProfileCache::saveNew(options.output.string(), record, error))
    {
        std::cerr << "ERROR=" << error << '\n';
        return 15;
    }

    MachineProfileCache verification;
    const MachineProfileDecision exact = verification.load(options.output.string())
        ? verification.evaluate(record.key, true, true)
        : MachineProfileDecision{};
    std::string configDigestAfter;
    if (!sha256File(options.config, configDigestAfter, error) ||
        exact.level != MachineProfileLevel::CalibratedAngle ||
        !exact.cacheMatched || configDigestAfter != configDigestBefore)
    {
        std::error_code removeError;
        std::filesystem::remove(options.output, removeError);
        std::cerr << "ERROR=reverse_validation_failed\n";
        return 16;
    }

    size_t auditedFields = 0;
    if (!auditInvalidation(verification, record.key, auditedFields, error))
    {
        std::error_code removeError;
        std::filesystem::remove(options.output, removeError);
        std::cerr << "ERROR=" << error << '\n';
        return 17;
    }

    std::cout << "CandidateCreated=1\n"
              << "CandidateEnabled=0\n"
              << "ProfileAutoWrite=0\n"
              << "ReverseLoadLevel=3\n"
              << "InvalidationAuditFields=" << auditedFields << '\n'
              << "InvalidationAuditPassed=1\n"
              << "BuildIdentity=" << record.evidence.buildIdentity << '\n'
              << "RuntimeKeyBackend=" << record.key.inferenceBackend << '\n'
              << "RuntimeKeyRoi=" << record.key.roiX << ',' << record.key.roiY << ','
              << record.key.roiWidth << ',' << record.key.roiHeight << '\n'
              << "ProbeEvidenceRoi=" << record.evidence.probeRoiX << ','
              << record.evidence.probeRoiY << ',' << record.evidence.probeRoiWidth << ','
              << record.evidence.probeRoiHeight << '\n'
              << "Output=" << options.output.string() << '\n';
    return 0;
}
