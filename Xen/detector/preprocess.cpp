#include "detector/preprocess.h"
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>
#include <limits>

namespace detector::detail {
namespace {

bool letterbox_impl(const cv::Mat& src, cv::Mat& dst,
                    cv::Mat& resize_buffer,
                    int target_w, int target_h,
                    LetterBoxInfo& info) noexcept {
    info = {};

    // 明确限制输入契约，避免空图、灰度图或浮点图在 OpenCV 内部触发异常，
    // 并防止除零或构造负尺寸画布。
    if (src.empty() || src.type() != CV_8UC3 ||
        target_w <= 0 || target_h <= 0) {
        dst.release();
        return false;
    }

    try {
        const size_t pixel_count = static_cast<size_t>(target_w) *
            static_cast<size_t>(target_h);
        if (pixel_count >
            static_cast<size_t>(std::numeric_limits<int>::max()) / 3U) {
            dst.release();
            return false;
        }

        const int sw = src.cols;
        const int sh = src.rows;
        const float scale = std::min(
            static_cast<float>(target_w) / static_cast<float>(sw),
            static_cast<float>(target_h) / static_cast<float>(sh));
        if (!(scale > 0.0f) || !std::isfinite(scale)) return false;

        // 四舍五入后使用整数 left/top 作为真实 copyTo 偏移，坐标还原必须使用
        // 同一组值，不能保留理论上的 0.5 像素填充而与实际画布错位。
        const int nw = std::clamp(
            static_cast<int>(std::round(sw * scale)), 1, target_w);
        const int nh = std::clamp(
            static_cast<int>(std::round(sh * scale)), 1, target_h);
        const int left = (target_w - nw) / 2;
        const int top = (target_h - nh) / 2;

        info.scale = scale;
        info.pad_x = static_cast<float>(left);
        info.pad_y = static_cast<float>(top);
        info.orig_w = sw;
        info.orig_h = sh;
        info.target_w = target_w;
        info.target_h = target_h;

        const cv::Mat* resized = &src;
        if (nw != sw || nh != sh) {
            resize_buffer.create(nh, nw, CV_8UC3);
            cv::resize(src, resize_buffer, cv::Size(nw, nh),
                       0, 0, cv::INTER_LINEAR);
            resized = &resize_buffer;
        }

        // 一次循环完成填充、BGR→RGB、uint8→float32、归一化和 HWC→CHW。
        // 相比 canvas/convertTo/cvtColor/split/memcpy 链路，避免四个整图临时
        // 缓冲区和多次内存遍历。dst.create() 在尺寸不变时保留原有分配。
        const size_t element_count = pixel_count * 3U;
        dst.create(1, static_cast<int>(element_count), CV_32F);
        float* output = dst.ptr<float>();
        constexpr float kScale = 1.0f / 255.0f;
        constexpr float kPadding = 114.0f / 255.0f;

        float* red = output;
        float* green = output + pixel_count;
        float* blue = output + pixel_count * 2U;
        // ROI 已与模型输入同尺寸时不存在填充区域，后续像素循环会覆盖整个
        // 张量；跳过这次整块写入可避免每帧先写后覆写 3×H×W 个 float。
        if (nw != target_w || nh != target_h) {
            std::fill_n(output, element_count, kPadding);
        }
        for (int y = 0; y < nh; ++y) {
            const cv::Vec3b* source_row = resized->ptr<cv::Vec3b>(y);
            const size_t destination_row =
                static_cast<size_t>(top + y) * target_w + left;
            for (int x = 0; x < nw; ++x) {
                const size_t destination = destination_row + x;
                const cv::Vec3b& pixel = source_row[x];
                red[destination] = static_cast<float>(pixel[2]) * kScale;
                green[destination] = static_cast<float>(pixel[1]) * kScale;
                blue[destination] = static_cast<float>(pixel[0]) * kScale;
            }
        }
        return true;
    } catch (...) {
        dst.release();
        info = {};
        return false;
    }
}

} // namespace

bool letterbox(const cv::Mat& src, cv::Mat& dst,
               int target_w, int target_h,
               LetterBoxInfo& info) noexcept {
    cv::Mat resize_buffer;
    return letterbox_impl(
        src, dst, resize_buffer, target_w, target_h, info);
}

bool letterbox_reuse(const cv::Mat& src, cv::Mat& dst,
                     cv::Mat& resize_buffer,
                     int target_w, int target_h,
                     LetterBoxInfo& info) noexcept {
    return letterbox_impl(
        src, dst, resize_buffer, target_w, target_h, info);
}

bool letterbox_bgr_reuse(const cv::Mat& src, cv::Mat& dst,
                         cv::Mat& resize_buffer, cv::Mat& padding_buffer,
                         int target_w, int target_h,
                         LetterBoxInfo& info) noexcept {
    info = {};
    if (src.empty() || src.type() != CV_8UC3 ||
        target_w <= 0 || target_h <= 0) {
        dst.release();
        return false;
    }

    try {
        const int sw = src.cols;
        const int sh = src.rows;
        const float scale = std::min(
            static_cast<float>(target_w) / static_cast<float>(sw),
            static_cast<float>(target_h) / static_cast<float>(sh));
        if (!(scale > 0.0f) || !std::isfinite(scale)) return false;

        const int nw = std::clamp(
            static_cast<int>(std::round(sw * scale)), 1, target_w);
        const int nh = std::clamp(
            static_cast<int>(std::round(sh * scale)), 1, target_h);
        const int left = (target_w - nw) / 2;
        const int top = (target_h - nh) / 2;

        info.scale = scale;
        info.pad_x = static_cast<float>(left);
        info.pad_y = static_cast<float>(top);
        info.orig_w = sw;
        info.orig_h = sh;
        info.target_w = target_w;
        info.target_h = target_h;

        const cv::Mat* resized = &src;
        if (nw != sw || nh != sh) {
            resize_buffer.create(nh, nw, CV_8UC3);
            cv::resize(src, resize_buffer, cv::Size(nw, nh),
                       0, 0, cv::INTER_LINEAR);
            resized = &resize_buffer;
        }

        if (nw == target_w && nh == target_h) {
            // 这里只创建浅引用，真正的跨帧稳定地址由 CudaPreprocessor 的 pinned
            // staging 提供；同尺寸 ROI 不需要额外复制一份普通 CPU cv::Mat。
            dst = *resized;
            return true;
        }

        // dst 可能仍借用上一帧输入；只向独立持有的 padding 缓冲写入，
        // 避免同尺寸 create 复用借用存储，同时保留固定目标尺寸的跨帧分配。
        padding_buffer.create(target_h, target_w, CV_8UC3);
        padding_buffer.setTo(cv::Scalar(114, 114, 114));
        resized->copyTo(padding_buffer(cv::Rect(left, top, nw, nh)));
        dst = padding_buffer;
        return true;
    } catch (...) {
        dst.release();
        info = {};
        return false;
    }
}

} // namespace detector::detail
