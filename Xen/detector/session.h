#ifndef DETECTOR_SESSION_H
#define DETECTOR_SESSION_H

#include <string>
#include <vector>
#include <memory>
#include <onnxruntime_cxx_api.h>
#include "detector/detector.h"

namespace detector::detail {

struct SessionRunProfile {
    double h2d_ms = 0.0;
    double gpu_preprocess_ms = 0.0;
    double execution_ms = 0.0;
    double d2h_ms = 0.0;
    bool explicit_device_copy = false;
    bool gpu_preprocess = false;
    std::uint64_t input_upload_bytes = 0;
};

/// ONNX Runtime 会话的轻量封装
class Session {
public:
    Session();
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&);
    Session& operator=(Session&&);

    bool init_env(const DetectorConfig& cfg);
    bool setup_options(const DetectorConfig& cfg);
    bool load(const std::string& path);
    // 返回值由 Session 持有，仅在下一次 run()/load() 或析构前有效。
    const std::vector<Ort::Value>* run(
        Ort::Value& input, SessionRunProfile& profile);
    bool gpu_preprocess_enabled() const noexcept;
    bool stage_gpu_input(const cv::Mat& bgr_image) noexcept;
    const std::vector<Ort::Value>* run_gpu_preprocessed(
        SessionRunProfile& profile);

    size_t                        num_inputs() const;
    std::vector<int64_t>          input_shape() const;
    ONNXTensorElementDataType     input_type() const;
    size_t                        num_outputs() const;
    std::vector<int64_t>          output_shape(size_t index) const;
    ONNXTensorElementDataType     output_type(size_t index) const;
    const std::vector<const char*>& output_names() const;
    Ort::MemoryInfo*              memory_info();
    std::string                   metadata_value(const char* key) const;
    const std::string&            active_provider() const noexcept;

private:
    const std::vector<Ort::Value>* run_cuda_graph(
        const void* host_input,
        size_t host_input_bytes,
        bool gpu_preprocessed,
        SessionRunProfile& profile);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace detector::detail

#endif // DETECTOR_SESSION_H
