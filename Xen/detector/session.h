#ifndef DETECTOR_SESSION_H
#define DETECTOR_SESSION_H

#include <string>
#include <vector>
#include <memory>
#include <onnxruntime_cxx_api.h>
#include "detector/detector.h"

namespace detector::detail {

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
    const std::vector<Ort::Value>* run(Ort::Value& input);

    size_t                        num_inputs() const;
    std::vector<int64_t>          input_shape() const;
    ONNXTensorElementDataType     input_type() const;
    size_t                        num_outputs() const;
    const std::vector<const char*>& output_names() const;
    Ort::MemoryInfo*              memory_info();
    std::string                   metadata_value(const char* key) const;
    const std::string&            active_provider() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace detector::detail

#endif // DETECTOR_SESSION_H
