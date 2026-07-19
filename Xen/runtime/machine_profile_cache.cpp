#include "machine_profile_cache.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace
{
bool finitePositive(double value)
{
    return std::isfinite(value) && value > 0.0;
}

bool safeText(const std::string& value)
{
    return !value.empty() && value.find_first_of("\r\n=") == std::string::npos;
}

bool parseInt(const std::map<std::string, std::string>& values,
              const char* name, int& result, std::string& error)
{
    const auto it = values.find(name);
    if (it == values.end())
    {
        error = std::string("missing_field:") + name;
        return false;
    }
    try
    {
        size_t used = 0;
        const long long parsed = std::stoll(it->second, &used);
        if (used != it->second.size() || parsed < std::numeric_limits<int>::min() ||
            parsed > std::numeric_limits<int>::max())
        {
            throw std::out_of_range("integer range");
        }
        result = static_cast<int>(parsed);
        return true;
    }
    catch (...)
    {
        error = std::string("invalid_integer:") + name;
        return false;
    }
}

bool parseSize(const std::map<std::string, std::string>& values,
               const char* name, size_t& result, std::string& error)
{
    const auto it = values.find(name);
    if (it == values.end())
    {
        error = std::string("missing_field:") + name;
        return false;
    }
    try
    {
        size_t used = 0;
        const unsigned long long parsed = std::stoull(it->second, &used);
        if (used != it->second.size() || parsed > std::numeric_limits<size_t>::max())
            throw std::out_of_range("size range");
        result = static_cast<size_t>(parsed);
        return true;
    }
    catch (...)
    {
        error = std::string("invalid_size:") + name;
        return false;
    }
}

bool parseDouble(const std::map<std::string, std::string>& values,
                 const char* name, double& result, std::string& error)
{
    const auto it = values.find(name);
    if (it == values.end())
    {
        error = std::string("missing_field:") + name;
        return false;
    }
    try
    {
        size_t used = 0;
        result = std::stod(it->second, &used);
        if (used != it->second.size() || !std::isfinite(result))
            throw std::invalid_argument("non-finite double");
        return true;
    }
    catch (...)
    {
        error = std::string("invalid_double:") + name;
        return false;
    }
}

bool parseBool(const std::map<std::string, std::string>& values,
               const char* name, bool& result, std::string& error)
{
    const auto it = values.find(name);
    if (it == values.end())
    {
        error = std::string("missing_field:") + name;
        return false;
    }
    if (it->second == "0" || it->second == "false")
    {
        result = false;
        return true;
    }
    if (it->second == "1" || it->second == "true")
    {
        result = true;
        return true;
    }
    error = std::string("invalid_bool:") + name;
    return false;
}

bool getText(const std::map<std::string, std::string>& values,
             const char* name, std::string& result, std::string& error)
{
    const auto it = values.find(name);
    if (it == values.end() || !safeText(it->second))
    {
        error = std::string("invalid_text:") + name;
        return false;
    }
    result = it->second;
    return true;
}

bool sameDouble(double left, double right)
{
    return std::isfinite(left) && std::isfinite(right) &&
        std::abs(left - right) <= 1e-9 * std::max({1.0, std::abs(left), std::abs(right)});
}

std::string firstMismatch(const MachineProfileKey& expected,
                          const MachineProfileKey& current)
{
#define MATCH_TEXT(field) if (expected.field != current.field) return #field
#define MATCH_INT(field) if (expected.field != current.field) return #field
#define MATCH_DOUBLE(field) if (!sameDouble(expected.field, current.field)) return #field
    MATCH_TEXT(gameProfile);
    MATCH_TEXT(aimMode);
    MATCH_TEXT(captureMethod);
    MATCH_TEXT(captureSource);
    MATCH_INT(sourceWidth);
    MATCH_INT(sourceHeight);
    MATCH_INT(roiX);
    MATCH_INT(roiY);
    MATCH_INT(roiWidth);
    MATCH_INT(roiHeight);
    MATCH_TEXT(inferenceBackend);
    MATCH_TEXT(inputMethod);
    MATCH_TEXT(inputDeviceIdentity);
    MATCH_DOUBLE(sensitivity);
    MATCH_DOUBLE(yaw);
    MATCH_DOUBLE(pitch);
    MATCH_DOUBLE(fovXDegrees);
    MATCH_DOUBLE(fovYDegrees);
    MATCH_INT(fovScaled);
    MATCH_DOUBLE(baseFovDegrees);
    MATCH_INT(controllerRevision);
#undef MATCH_DOUBLE
#undef MATCH_INT
#undef MATCH_TEXT
    return {};
}

bool validEvidence(const MachineProfileEvidence& evidence)
{
    return finitePositive(evidence.pixelsPerCountX) &&
        finitePositive(evidence.pixelsPerCountY) &&
        finitePositive(evidence.degreesPerCountX) &&
        finitePositive(evidence.degreesPerCountY) &&
        finitePositive(evidence.t50MsX) && finitePositive(evidence.t50MsY) &&
        finitePositive(evidence.t90MsX) && finitePositive(evidence.t90MsY) &&
        evidence.t50MsX <= evidence.t90MsX && evidence.t50MsY <= evidence.t90MsY &&
        evidence.t90MsX <= 100.0 && evidence.t90MsY <= 100.0 &&
        std::isfinite(evidence.confidence) && evidence.confidence >= 0.90 &&
        evidence.confidence <= 1.0 && evidence.trialCount >= 120 &&
        safeText(evidence.protocol) && safeText(evidence.buildIdentity) &&
        safeText(evidence.evidenceDigest);
}

bool validKey(const MachineProfileKey& key)
{
    return safeText(key.gameProfile) && safeText(key.aimMode) &&
        safeText(key.captureMethod) && safeText(key.captureSource) &&
        key.sourceWidth > 0 && key.sourceHeight > 0 &&
        key.roiWidth > 0 && key.roiHeight > 0 && key.roiX >= 0 && key.roiY >= 0 &&
        safeText(key.inferenceBackend) && safeText(key.inputMethod) &&
        safeText(key.inputDeviceIdentity) && finitePositive(key.sensitivity) &&
        finitePositive(key.yaw) && finitePositive(key.pitch) &&
        finitePositive(key.fovXDegrees) && finitePositive(key.fovYDegrees) &&
        (!key.fovScaled || finitePositive(key.baseFovDegrees)) &&
        key.controllerRevision > 0;
}

void writeText(std::ostream& output, const char* name, const std::string& value)
{
    output << name << '=' << value << '\n';
}

void writeDouble(std::ostream& output, const char* name, double value)
{
    output << name << '=' << std::setprecision(17) << value << '\n';
}
}

const char* machineProfileLevelName(MachineProfileLevel level)
{
    switch (level)
    {
    case MachineProfileLevel::CalibratedAngle: return "calibrated_angle";
    case MachineProfileLevel::ConservativeAngle: return "conservative_angle";
    case MachineProfileLevel::NormalizedImage: return "normalized_image";
    case MachineProfileLevel::SafetyDirectPursuit:
    default: return "safety_direct_pursuit";
    }
}

void MachineProfileCache::clear()
{
    loaded_ = false;
    record_ = {};
    error_.clear();
}

bool MachineProfileCache::load(const std::string& path)
{
    clear();
    if (path.empty())
    {
        error_ = "cache_path_empty";
        return false;
    }
    if (!std::filesystem::path(path).is_absolute())
    {
        error_ = "cache_path_not_absolute";
        return false;
    }

    std::ifstream input(path);
    if (!input)
    {
        error_ = "cache_open_failed";
        return false;
    }

    std::map<std::string, std::string> values;
    std::string line;
    size_t lineNumber = 0;
    while (std::getline(input, line))
    {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line.front() == '#')
            continue;
        const size_t separator = line.find('=');
        if (separator == std::string::npos || separator == 0 || separator + 1 >= line.size())
        {
            error_ = "invalid_line:" + std::to_string(lineNumber);
            return false;
        }
        const std::string name = line.substr(0, separator);
        const std::string value = line.substr(separator + 1);
        if (!values.emplace(name, value).second)
        {
            error_ = "duplicate_field:" + name;
            return false;
        }
    }

    MachineProfileRecord parsed;
    auto text = [&](const char* name, std::string& value) {
        return getText(values, name, value, error_);
    };
    auto integer = [&](const char* name, int& value) {
        return parseInt(values, name, value, error_);
    };
    auto real = [&](const char* name, double& value) {
        return parseDouble(values, name, value, error_);
    };

    if (!integer("SchemaVersion", parsed.schemaVersion) ||
        !text("GameProfile", parsed.key.gameProfile) ||
        !text("AimMode", parsed.key.aimMode) ||
        !text("CaptureMethod", parsed.key.captureMethod) ||
        !text("CaptureSource", parsed.key.captureSource) ||
        !integer("SourceWidth", parsed.key.sourceWidth) ||
        !integer("SourceHeight", parsed.key.sourceHeight) ||
        !integer("RoiX", parsed.key.roiX) || !integer("RoiY", parsed.key.roiY) ||
        !integer("RoiWidth", parsed.key.roiWidth) ||
        !integer("RoiHeight", parsed.key.roiHeight) ||
        !text("InferenceBackend", parsed.key.inferenceBackend) ||
        !text("InputMethod", parsed.key.inputMethod) ||
        !text("InputDeviceIdentity", parsed.key.inputDeviceIdentity) ||
        !real("Sensitivity", parsed.key.sensitivity) ||
        !real("Yaw", parsed.key.yaw) || !real("Pitch", parsed.key.pitch) ||
        !real("FovXDegrees", parsed.key.fovXDegrees) ||
        !real("FovYDegrees", parsed.key.fovYDegrees) ||
        !parseBool(values, "FovScaled", parsed.key.fovScaled, error_) ||
        !real("BaseFovDegrees", parsed.key.baseFovDegrees) ||
        !integer("ControllerRevision", parsed.key.controllerRevision) ||
        !real("PixelsPerCountX", parsed.evidence.pixelsPerCountX) ||
        !real("PixelsPerCountY", parsed.evidence.pixelsPerCountY) ||
        !real("DegreesPerCountX", parsed.evidence.degreesPerCountX) ||
        !real("DegreesPerCountY", parsed.evidence.degreesPerCountY) ||
        !real("T50MsX", parsed.evidence.t50MsX) ||
        !real("T50MsY", parsed.evidence.t50MsY) ||
        !real("T90MsX", parsed.evidence.t90MsX) ||
        !real("T90MsY", parsed.evidence.t90MsY) ||
        !real("Confidence", parsed.evidence.confidence) ||
        !parseSize(values, "TrialCount", parsed.evidence.trialCount, error_) ||
        !text("Protocol", parsed.evidence.protocol) ||
        !text("BuildIdentity", parsed.evidence.buildIdentity) ||
        !text("EvidenceDigest", parsed.evidence.evidenceDigest))
    {
        return false;
    }

    if (parsed.schemaVersion != MachineProfileRecord::kSchemaVersion)
    {
        error_ = "unsupported_schema";
        return false;
    }
    if (!validKey(parsed.key))
    {
        error_ = "invalid_cache_key";
        return false;
    }
    if (!validEvidence(parsed.evidence))
    {
        error_ = "invalid_cache_evidence";
        return false;
    }

    record_ = std::move(parsed);
    loaded_ = true;
    return true;
}

bool MachineProfileCache::saveNew(const std::string& path,
                                  const MachineProfileRecord& record,
                                  std::string& error)
{
    error.clear();
    if (path.empty() || !std::filesystem::path(path).is_absolute() ||
        record.schemaVersion != MachineProfileRecord::kSchemaVersion ||
        !validKey(record.key) || !validEvidence(record.evidence))
    {
        error = "invalid_record_or_path";
        return false;
    }
    const std::filesystem::path target(path);
    std::error_code ec;
    if (std::filesystem::exists(target, ec))
    {
        error = "cache_already_exists";
        return false;
    }
    if (target.has_parent_path() && !std::filesystem::exists(target.parent_path(), ec))
    {
        error = "cache_parent_missing";
        return false;
    }

    std::ofstream output(target, std::ios::binary | std::ios::out);
    if (!output)
    {
        error = "cache_create_failed";
        return false;
    }
    output << "# Xen machine profile cache; explicit manual-review artifact\n";
    output << "SchemaVersion=" << record.schemaVersion << '\n';
    writeText(output, "GameProfile", record.key.gameProfile);
    writeText(output, "AimMode", record.key.aimMode);
    writeText(output, "CaptureMethod", record.key.captureMethod);
    writeText(output, "CaptureSource", record.key.captureSource);
    output << "SourceWidth=" << record.key.sourceWidth << '\n'
           << "SourceHeight=" << record.key.sourceHeight << '\n'
           << "RoiX=" << record.key.roiX << '\n' << "RoiY=" << record.key.roiY << '\n'
           << "RoiWidth=" << record.key.roiWidth << '\n'
           << "RoiHeight=" << record.key.roiHeight << '\n';
    writeText(output, "InferenceBackend", record.key.inferenceBackend);
    writeText(output, "InputMethod", record.key.inputMethod);
    writeText(output, "InputDeviceIdentity", record.key.inputDeviceIdentity);
    writeDouble(output, "Sensitivity", record.key.sensitivity);
    writeDouble(output, "Yaw", record.key.yaw);
    writeDouble(output, "Pitch", record.key.pitch);
    writeDouble(output, "FovXDegrees", record.key.fovXDegrees);
    writeDouble(output, "FovYDegrees", record.key.fovYDegrees);
    output << "FovScaled=" << (record.key.fovScaled ? 1 : 0) << '\n';
    writeDouble(output, "BaseFovDegrees", record.key.baseFovDegrees);
    output << "ControllerRevision=" << record.key.controllerRevision << '\n';
    writeDouble(output, "PixelsPerCountX", record.evidence.pixelsPerCountX);
    writeDouble(output, "PixelsPerCountY", record.evidence.pixelsPerCountY);
    writeDouble(output, "DegreesPerCountX", record.evidence.degreesPerCountX);
    writeDouble(output, "DegreesPerCountY", record.evidence.degreesPerCountY);
    writeDouble(output, "T50MsX", record.evidence.t50MsX);
    writeDouble(output, "T50MsY", record.evidence.t50MsY);
    writeDouble(output, "T90MsX", record.evidence.t90MsX);
    writeDouble(output, "T90MsY", record.evidence.t90MsY);
    writeDouble(output, "Confidence", record.evidence.confidence);
    output << "TrialCount=" << record.evidence.trialCount << '\n';
    writeText(output, "Protocol", record.evidence.protocol);
    writeText(output, "BuildIdentity", record.evidence.buildIdentity);
    writeText(output, "EvidenceDigest", record.evidence.evidenceDigest);
    output.close();
    if (!output)
    {
        std::filesystem::remove(target, ec);
        error = "cache_write_failed";
        return false;
    }
    return true;
}

MachineProfileDecision MachineProfileCache::evaluate(
    const MachineProfileKey& current,
    bool cacheRequested,
    bool userProfileValid,
    bool severeMismatch) const
{
    MachineProfileDecision decision;
    decision.cacheRequested = cacheRequested;
    decision.cacheLoaded = loaded_;

    if (severeMismatch)
    {
        decision.reason = "severe_runtime_mismatch";
        return decision;
    }

    const bool geometryValid = current.sourceWidth > 0 && current.sourceHeight > 0 &&
        current.roiWidth > 0 && current.roiHeight > 0;
    if (!userProfileValid)
    {
        if (geometryValid)
        {
            decision.level = MachineProfileLevel::NormalizedImage;
            decision.normalizedImageEnabled = true;
            // 归一化估计器尚未接管设备；首版只记录降级状态，禁止沿用角度预测器。
            decision.predictionEnabled = false;
            decision.reason = "user_profile_invalid_normalized_space";
        }
        else
        {
            decision.reason = "user_profile_and_geometry_invalid";
        }
        return decision;
    }

    decision.level = MachineProfileLevel::ConservativeAngle;
    decision.angleSpaceEnabled = true;
    decision.predictionEnabled = true;
    decision.feedforwardConfidenceScale = 0.25;
    const double fovScale = current.fovScaled && current.baseFovDegrees > 1.0
        ? current.fovXDegrees / current.baseFovDegrees : 1.0;
    decision.degreesPerCountX = current.sensitivity * current.yaw * fovScale;
    decision.degreesPerCountY = current.sensitivity * current.pitch * fovScale;
    if (!cacheRequested)
    {
        decision.reason = "cache_disabled";
        return decision;
    }
    if (!loaded_)
    {
        decision.reason = error_.empty() ? "cache_not_loaded" : error_;
        return decision;
    }

    const std::string mismatch = firstMismatch(record_.key, current);
    if (!mismatch.empty())
    {
        decision.reason = "cache_key_mismatch:" + mismatch;
        return decision;
    }

    decision.level = MachineProfileLevel::CalibratedAngle;
    decision.cacheMatched = true;
    decision.calibratedViewResponseEnabled = true;
    decision.integralEnabled = true;
    decision.feedforwardConfidenceScale = 1.0;
    decision.degreesPerCountX = record_.evidence.degreesPerCountX;
    decision.degreesPerCountY = record_.evidence.degreesPerCountY;
    // 主动协议当前验证的是近似整步响应；以双轴t50均值作为shadow响应中心，
    // 不从t50/t90臆造未经协议验证的有限宽度。
    decision.commandToFrameDelayMs =
        (record_.evidence.t50MsX + record_.evidence.t50MsY) * 0.5;
    decision.commandResponseMs = 0.0;
    decision.reason = "cache_exact_match";
    return decision;
}
