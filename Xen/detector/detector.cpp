#include "detector/detector.h"
#include "detector/session.h"
#include "detector/preprocess.h"
#include "detector/postprocess.h"
#include "log/log.h"
#include <chrono>

// ============================================================
// Impl
// ============================================================
struct Detector::Impl {
    detector::detail::Session session;
    bool loaded = false;
    int64_t channels = 3, height = 640, width = 640;

    bool load(const DetectorConfig& cfg) {
        LOG_INFO("detector", "Loading model: {}, backend={}",
                 cfg.model_path, BackendName(cfg.backend));

        if (!session.init_env(cfg)) {
            LOG_ERROR("detector", "ORT env init failed");
            return false;
        }
        session.setup_options(cfg);
        if (!session.load(cfg.model_path)) {
            LOG_ERROR("detector", "ORT load model failed: {}", cfg.model_path);
            return false;
        }

        auto shape = session.input_shape();
        if (shape.size() == 4) {
            channels = shape[1];
            height   = shape[2];
            width    = shape[3];
        }

        LOG_INFO("detector", "Model loaded: {}×{}, {} outputs",
                 width, height, session.num_outputs());
        loaded = true;
        return true;
    }

    std::vector<Detection> run(const cv::Mat& bgr_img,
                               const DetectorConfig& cfg,
                               InferenceProfile& prof_out) {
        using clk = std::chrono::steady_clock;

        // ── 前处理 ──
        auto t0 = clk::now();
        cv::Mat blob;
        detector::detail::LetterBoxInfo lb;
        detector::detail::letterbox(bgr_img, blob, (int)width, (int)height, lb);
        auto t1 = clk::now();

        // ── 输入张量 ──
        std::vector<int64_t> shape = {1, channels, height, width};
        size_t nbytes = 1 * (size_t)channels * height * width;
        auto mem = session.memory_info();
        Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
            *mem, blob.ptr<float>(), nbytes, shape.data(), shape.size());

        // ── 推理 ──
        auto outputs = session.run(input_tensor);
        auto t2 = clk::now();

        // ── 后处理 ──
        std::vector<Detection> dets;
        // 用 lambda 包裹让 early return 不绕晕
        auto decode = [&]() -> std::vector<Detection> {
            if (outputs.empty()) return {};

            // YOLOv10: 单输出 [1, N, 6]
            if (outputs.size() == 1) {
                auto info = outputs[0].GetTensorTypeAndShapeInfo();
                auto s = info.GetShape();
                if (s.size() == 3 && s[2] == 6) {
                    LOG_TRACE("detector", "Decode: YOLOv10 format, shape=[{}, {}, {}]",
                              s[0], s[1], s[2]);
                    std::vector<Detection> d;
                    detector::detail::decode_yolov10(
                        outputs[0].GetTensorData<float>(), (int)s[1], d);
                    return d;
                }
            }

            // YOLOv8/v11/v5: 多输出 [1, channels, anchors]
            std::vector<Detection> d;
            for (auto& out : outputs) {
                auto info = out.GetTensorTypeAndShapeInfo();
                auto s = info.GetShape();
                if (s.size() < 3 || s[0] != 1) continue;
                int ch = (int)s[1], anc = (int)s[2];
                auto* data = out.GetTensorData<float>();
                if (ch > 5) {
                    LOG_TRACE("detector", "Decode head: YOLOv8, ch={}, anchors={}", ch, anc);
                    detector::detail::decode_yolov8(data, anc, ch, d);
                } else {
                    LOG_TRACE("detector", "Decode head: YOLOv5, ch={}, anchors={}", ch, anc);
                    detector::detail::decode_yolov5(data, anc, ch, d);
                }
            }
            return d;
        };

        dets = decode();
        auto t3 = clk::now();

        // ── 过滤 + 坐标还原 + NMS ──
        dets.erase(std::remove_if(dets.begin(), dets.end(),
            [&](const Detection& d) { return d.confidence < cfg.conf_threshold; }),
            dets.end());
        detector::detail::scale_detections(dets, lb);
        detector::detail::nms(dets, cfg.nms_threshold, cfg.top_k);
        auto t4 = clk::now();

        // ── 统计 ──
        using ms = std::chrono::duration<double, std::milli>;
        prof_out.preprocess_ms  = std::chrono::duration_cast<ms>(t1 - t0).count();
        prof_out.inference_ms   = std::chrono::duration_cast<ms>(t2 - t1).count();
        prof_out.postprocess_ms = std::chrono::duration_cast<ms>(t4 - t2).count();
        prof_out.total_ms       = std::chrono::duration_cast<ms>(t4 - t0).count();

        LOG_DEBUG("detector",
                  "Pre={:.2f}ms Infer={:.2f}ms Post={:.2f}ms Total={:.2f}ms Det={}",
                  prof_out.preprocess_ms, prof_out.inference_ms,
                  prof_out.postprocess_ms, prof_out.total_ms, dets.size());

        if (dets.empty()) {
            static auto last_warn = clk::now();
            auto now = clk::now();
            constexpr auto min_interval = std::chrono::milliseconds(5000);
            if (now - last_warn >= min_interval) {
                LOG_WARN("detector", "detect() returned 0 detections (conf={:.2f})",
                         cfg.conf_threshold);
                last_warn = now;
            }
        }

        return dets;
    }
};

// ============================================================
// Detector 公有接口
// ============================================================

const char* BackendName(BackendType bt) noexcept {
    switch (bt) {
        case BackendType::CUDA:     return "CUDAExecutionProvider";
        case BackendType::TENSORRT: return "TensorrtExecutionProvider";
        case BackendType::DIRECTML: return "DmlExecutionProvider";
        case BackendType::CPU:      return "CPUExecutionProvider";
    }
    return "CPUExecutionProvider";
}

Detector::Detector(const DetectorConfig& cfg)
    : impl_(std::make_unique<Impl>()), config_(cfg) {}
Detector::~Detector() = default;

bool Detector::load() { return impl_->load(config_); }
bool Detector::loaded() const noexcept { return impl_->loaded; }

void Detector::reset() {
    impl_ = std::make_unique<Impl>();
}

std::vector<Detection> Detector::detect(const cv::Mat& bgr_image) {
    if (!impl_->loaded) {
        LOG_ERROR("detector", "detect() called before load()");
        return {};
    }
    InferenceProfile prof;
    auto dets = impl_->run(bgr_image, config_, prof);
    profile_ = prof;
    return dets;
}

std::vector<std::vector<Detection>> Detector::detect_batch(
    const std::vector<cv::Mat>& bgr_images) {
    std::vector<std::vector<Detection>> res;
    res.reserve(bgr_images.size());
    for (auto& img : bgr_images) res.push_back(detect(img));
    return res;
}

std::unique_ptr<Detector> Detector::clone() const {
    auto c = std::make_unique<Detector>(config_);
    c->load();
    return c;
}

const InferenceProfile& Detector::profile() const noexcept { return profile_; }
std::string Detector::backend_name() const { return BackendName(config_.backend); }
int Detector::input_width() const noexcept { return (int)impl_->width; }
int Detector::input_height() const noexcept { return (int)impl_->height; }

std::vector<std::string> Detector::available_providers() {
    try { return Ort::GetAvailableProviders(); }
    catch (...) { return {}; }
}
