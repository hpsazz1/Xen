#pragma once

#include <cstddef>
#include <string>

/**
 * @brief 机器标定缓存的严格运行上下文。
 *
 * 这里保存会改变counts到画面响应的全部已知身份。键采用字段逐项精确匹配，
 * 禁止只按游戏名或设备类型复用，避免分辨率、裁剪、NDI源、灵敏度或控制器
 * 修订变化后静默套用旧标定。
 */
struct MachineProfileKey
{
    std::string gameProfile;
    std::string aimMode;
    std::string captureMethod;
    std::string captureSource;
    int sourceWidth = 0;
    int sourceHeight = 0;
    int roiX = 0;
    int roiY = 0;
    int roiWidth = 0;
    int roiHeight = 0;
    std::string inferenceBackend;
    std::string inputMethod;
    std::string inputDeviceIdentity;
    double sensitivity = 0.0;
    double yaw = 0.0;
    double pitch = 0.0;
    double fovXDegrees = 0.0;
    double fovYDegrees = 0.0;
    bool fovScaled = false;
    double baseFovDegrees = 0.0;
    int controllerRevision = 0;
};

/** @brief 主动协议产生的只读证据；不包含任何自动应用或配置覆盖开关。 */
struct MachineProfileEvidence
{
    int probeRoiX = 0;
    int probeRoiY = 0;
    int probeRoiWidth = 0;
    int probeRoiHeight = 0;
    double pixelsPerCountX = 0.0;
    double pixelsPerCountY = 0.0;
    double degreesPerCountX = 0.0;
    double degreesPerCountY = 0.0;
    double t50MsX = 0.0;
    double t50MsY = 0.0;
    double t90MsX = 0.0;
    double t90MsY = 0.0;
    double confidence = 0.0;
    size_t trialCount = 0;
    std::string protocol;
    std::string buildIdentity;
    std::string evidenceDigest;
};

struct MachineProfileRecord
{
    static constexpr int kSchemaVersion = 1;
    int schemaVersion = kSchemaVersion;
    MachineProfileKey key;
    MachineProfileEvidence evidence;
};

enum class MachineProfileLevel
{
    SafetyDirectPursuit = 0,
    NormalizedImage = 1,
    ConservativeAngle = 2,
    CalibratedAngle = 3
};

const char* machineProfileLevelName(MachineProfileLevel level);

/** @brief 四级降级的可审计决策；首版只约束shadow，不解除active命令抑制。 */
struct MachineProfileDecision
{
    MachineProfileLevel level = MachineProfileLevel::SafetyDirectPursuit;
    bool cacheRequested = false;
    bool cacheLoaded = false;
    bool cacheMatched = false;
    bool angleSpaceEnabled = false;
    bool calibratedViewResponseEnabled = false;
    bool normalizedImageEnabled = false;
    bool predictionEnabled = false;
    bool integralEnabled = false;
    double feedforwardConfidenceScale = 0.0;
    double degreesPerCountX = 0.0;
    double degreesPerCountY = 0.0;
    double commandToFrameDelayMs = 0.0;
    double commandResponseMs = 0.0;
    std::string reason;
};

class MachineProfileCache
{
public:
    /** @brief 从显式路径加载一个缓存记录；失败时清除旧记录，禁止沿用陈旧值。 */
    bool load(const std::string& path);
    void clear();

    /**
     * @brief 显式创建新缓存文件。
     *
     * 运行时不会调用该函数；它拒绝覆盖已有文件，确保缓存写入必须由人工审核流程触发。
     */
    static bool saveNew(const std::string& path, const MachineProfileRecord& record,
                        std::string& error);

    /** @brief 依据用户profile、缓存键和严重失配信号计算四级降级。 */
    MachineProfileDecision evaluate(const MachineProfileKey& current,
                                    bool cacheRequested,
                                    bool userProfileValid,
                                    bool severeMismatch = false) const;

    bool loaded() const { return loaded_; }
    const MachineProfileRecord& record() const { return record_; }
    const std::string& error() const { return error_; }

private:
    bool loaded_ = false;
    MachineProfileRecord record_{};
    std::string error_;
};
