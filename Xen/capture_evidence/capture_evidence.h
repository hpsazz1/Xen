#ifndef CAPTURE_EVIDENCE_H
#define CAPTURE_EVIDENCE_H

#include "capture/capture.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace capture_evidence {

struct CaptureEvidenceConfig {
    std::filesystem::path output_directory;
    std::filesystem::path source_binding_path;
    CaptureConfig capture;
    bool require_source_timing = false;
    std::uint64_t requested_frame_count = 0;
};

// required 录制与 CLI 等待有效帧共用 NDI submission 映射完整性合同。
// 质量决策仍由 mapper 的 VALID 表示；optional 仍保留缺失或无效时钟事实。
bool has_required_source_timing(const FrameTiming& timing) noexcept;

class CaptureEvidenceRecorder {
public:
    CaptureEvidenceRecorder() noexcept;
    ~CaptureEvidenceRecorder();

    CaptureEvidenceRecorder(const CaptureEvidenceRecorder&) = delete;
    CaptureEvidenceRecorder& operator=(const CaptureEvidenceRecorder&) = delete;

    bool start(const CaptureEvidenceConfig& config,
               std::string& error) noexcept;
    bool record(const CapturedFrame& frame, std::string& error) noexcept;
    bool finish(std::string& error) noexcept;
    std::uint64_t recorded_frame_count() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace capture_evidence

#endif // CAPTURE_EVIDENCE_H
