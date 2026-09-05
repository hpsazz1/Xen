#include "detector/detector.h"
#include "log/log.h"

#include <onnxruntime_cxx_api.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "[失败] " << message << '\n';
}

// 只监测 SDK 外部所有权契约，不替换 Xen Session、模型解析或返回的 shape。
// ReleaseTypeInfo 到达时视为 owner 已结束；真实销毁延迟到观测结束，避免
// 为诊断旧代码而解引用 DLL 内部已释放内存。它不是 ASan/堆 UAF 检测器。
class TypeInfoLifetimeObserver {
public:
    struct Owner {
        OrtTypeInfo* value = nullptr;
        const OrtTensorTypeAndShapeInfo* borrowed = nullptr;
        bool input = false;
        size_t index = 0;
        bool released = false;
        bool shape_read = false;
        bool element_type_read = false;
    };

    struct InvalidAccess {
        bool input = false;
        size_t index = 0;
        const char* operation = nullptr;
    };

    explicit TypeInfoLifetimeObserver(const OrtApi& real)
        : real_(real), forwarding_(real) {
        active_ = this;
        forwarding_.SessionGetInputTypeInfo = &get_input_type_info;
        forwarding_.SessionGetOutputTypeInfo = &get_output_type_info;
        forwarding_.CastTypeInfoToTensorInfo = &cast_tensor_info;
        forwarding_.ReleaseTypeInfo = &release_type_info;
        forwarding_.GetDimensionsCount = &get_dimensions_count;
        forwarding_.GetDimensions = &get_dimensions;
        forwarding_.GetTensorElementType = &get_element_type;
        Ort::InitApi(&forwarding_);
    }

    ~TypeInfoLifetimeObserver() {
        Ort::InitApi(&real_);
        for (size_t index = 0; index < owner_count_; ++index) {
            real_.ReleaseTypeInfo(owners_[index].value);
        }
        active_ = nullptr;
    }

    TypeInfoLifetimeObserver(const TypeInfoLifetimeObserver&) = delete;
    TypeInfoLifetimeObserver& operator=(const TypeInfoLifetimeObserver&) = delete;

    void verify() const {
        expect(!overflow_, "SDK 生命周期监测容量不足，不能声称没有无效访问");
        bool input_observed = false;
        std::array<bool, 2> outputs_observed{};
        for (size_t index = 0; index < owner_count_; ++index) {
            const Owner& owner = owners_[index];
            expect(owner.released, "模型加载结束后每个 SDK TypeInfo owner 应已释放");
            expect(owner.borrowed && owner.shape_read && owner.element_type_read,
                   "模型输入与每个输出的真实 shape/type 必须进入生命周期监测");
            if (owner.input) {
                input_observed = true;
            } else if (owner.index < outputs_observed.size()) {
                outputs_observed[owner.index] = true;
            }
        }
        expect(input_observed && outputs_observed[0] && outputs_observed[1],
               "公有模型加载必须覆盖输入与两个不同输出的借用视图");
        for (size_t index = 0; index < invalid_access_count_; ++index) {
            const InvalidAccess& access = invalid_accesses_[index];
            expect(false,
                   std::string(access.input ? "input[" : "output[") +
                   std::to_string(access.index) + "] owner 释放后访问 " +
                   access.operation);
        }
        std::cout << "SDK 生命周期: owners=" << owner_count_
                  << ", invalid_borrowed_accesses=" << invalid_access_count_
                  << '\n';
    }

private:
    void remember(OrtTypeInfo* value, bool input, size_t index) noexcept {
        if (owner_count_ == owners_.size()) {
            overflow_ = true;
            return;
        }
        owners_[owner_count_++] = {value, nullptr, input, index};
    }

    Owner* find_owner(const OrtTypeInfo* value) noexcept {
        for (size_t index = 0; index < owner_count_; ++index) {
            if (owners_[index].value == value) return &owners_[index];
        }
        return nullptr;
    }

    void observe_read(const OrtTensorTypeAndShapeInfo* value,
                      const char* operation, bool element_type) noexcept {
        for (size_t index = 0; index < owner_count_; ++index) {
            Owner& owner = owners_[index];
            if (owner.borrowed != value) continue;
            if (element_type) owner.element_type_read = true;
            else owner.shape_read = true;
            if (owner.released) {
                if (invalid_access_count_ == invalid_accesses_.size()) {
                    overflow_ = true;
                    return;
                }
                invalid_accesses_[invalid_access_count_++] = {
                    owner.input, owner.index, operation};
            }
            return;
        }
        // Run() 的 owning TensorTypeAndShapeInfo 不是 TypeInfo 的借用视图。
    }

    static OrtStatus* ORT_API_CALL get_input_type_info(
            const OrtSession* session, size_t index,
            OrtTypeInfo** output) noexcept {
        auto* status = active_->real_.SessionGetInputTypeInfo(
            session, index, output);
        if (!status) active_->remember(*output, true, index);
        return status;
    }

    static OrtStatus* ORT_API_CALL get_output_type_info(
            const OrtSession* session, size_t index,
            OrtTypeInfo** output) noexcept {
        auto* status = active_->real_.SessionGetOutputTypeInfo(
            session, index, output);
        if (!status) active_->remember(*output, false, index);
        return status;
    }

    static OrtStatus* ORT_API_CALL cast_tensor_info(
            const OrtTypeInfo* owner,
            const OrtTensorTypeAndShapeInfo** output) noexcept {
        auto* status = active_->real_.CastTypeInfoToTensorInfo(owner, output);
        if (!status) {
            if (Owner* tracked = active_->find_owner(owner)) {
                tracked->borrowed = *output;
            }
        }
        return status;
    }

    static void ORT_API_CALL release_type_info(OrtTypeInfo* value) noexcept {
        if (Owner* owner = active_->find_owner(value)) {
            owner->released = true;
            return;
        }
        active_->real_.ReleaseTypeInfo(value);
    }

    static OrtStatus* ORT_API_CALL get_dimensions_count(
            const OrtTensorTypeAndShapeInfo* value, size_t* output) noexcept {
        active_->observe_read(value, "GetDimensionsCount", false);
        return active_->real_.GetDimensionsCount(value, output);
    }

    static OrtStatus* ORT_API_CALL get_dimensions(
            const OrtTensorTypeAndShapeInfo* value,
            int64_t* dimensions, size_t dimension_count) noexcept {
        active_->observe_read(value, "GetDimensions", false);
        return active_->real_.GetDimensions(value, dimensions, dimension_count);
    }

    static OrtStatus* ORT_API_CALL get_element_type(
            const OrtTensorTypeAndShapeInfo* value,
            ONNXTensorElementDataType* output) noexcept {
        active_->observe_read(value, "GetTensorElementType", true);
        return active_->real_.GetTensorElementType(value, output);
    }

    inline static TypeInfoLifetimeObserver* active_ = nullptr;
    const OrtApi& real_;
    OrtApi forwarding_;
    std::array<Owner, 16> owners_{};
    std::array<InvalidAccess, 128> invalid_accesses_{};
    size_t owner_count_ = 0;
    size_t invalid_access_count_ = 0;
    bool overflow_ = false;
};

void exercise_public_detector(const std::string& model_path) {
    DetectorConfig config;
    config.model_path = model_path;
    config.backend = BackendType::CPU;
    config.intra_threads = 1;
    config.enable_gpu_preprocess = false;
    config.enable_trt_cuda_graph = false;
    Detector detector(config);
    for (int reload = 0; reload < 2; ++reload) {
        const bool loaded = detector.load();
        expect(loaded && detector.loaded(), "真实双输出 ONNX 模型应加载成功");
        if (!loaded) return;
        expect(detector.input_width() == 4 && detector.input_height() == 4 &&
               detector.segmentation_supported(),
               "公有模型契约应保持 4×4 输入和双输出实例分割");

        const cv::Mat white(4, 4, CV_8UC3, cv::Scalar::all(255));
        const SegmentationResult foreground = detector.segment(white);
        expect(detector.profile().status == DetectionStatus::SUCCESS &&
               foreground.detections.size() == 1 && foreground.masks.size() == 1,
               "白图应由真实 ORT 输入运算生成一个框及掩码");
        if (foreground.detections.size() == 1) {
            const Detection& detection = foreground.detections.front();
            expect(std::abs(detection.confidence - 0.9f) < 1e-5f &&
                   std::abs(detection.x1 - 1.0f) < 1e-5f &&
                   std::abs(detection.y1 - 1.0f) < 1e-5f &&
                   std::abs(detection.x2 - 3.0f) < 1e-5f &&
                   std::abs(detection.y2 - 3.0f) < 1e-5f,
                   "fixture 的已知框和分数应通过公有结果保持不变");
        }

        const cv::Mat black(4, 4, CV_8UC3, cv::Scalar::all(0));
        const SegmentationResult background = detector.segment(black);
        expect(detector.profile().status == DetectionStatus::SUCCESS &&
               background.detections.empty() && background.masks.empty(),
               "换成黑图后真实模型的两个输出应改变并产生合法空结果");
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "需要极小 ONNX fixture 路径\n";
        return 2;
    }
    LogConfig config;
    config.enable_file = false;
    config.enable_debug_file = false;
    config.enable_ringbuf = false;
    Log::init(config);
    const OrtApi* real = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (!real) {
        std::cerr << "当前 ORT DLL 未提供构建时 API 版本\n";
        Log::shutdown();
        return 2;
    }
    {
        TypeInfoLifetimeObserver observer(*real);
        exercise_public_detector(argv[1]);
        observer.verify();
    }
    // 监测通过后再验证正常释放路径，避免隔离释放掩盖修复后的运行问题。
    if (failures == 0) exercise_public_detector(argv[1]);
    Log::shutdown();
    return failures == 0 ? 0 : 1;
}
