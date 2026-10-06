#ifndef XEN_LINEUP_CALIBRATION_INTERNAL_H
#define XEN_LINEUP_CALIBRATION_INTERNAL_H
#include "lineup/execution_internal.h"
#include <filesystem>
namespace lineup::detail {
struct CalibrationReadResult {
    bool valid = false;
    std::string reason;
    ExecutionCalibration calibration;
};
// 只读量测记录；模拟记录永不成为生产标定，增益从样本计算。
CalibrationReadResult read_calibration(const std::filesystem::path &path,
    const std::string &source_id, const ExecutionGeometry &geometry,
    const std::string &backend, const std::string &context_identity) noexcept;
}
#endif
