#include "detector/preprocess.h"
#include <opencv2/imgproc.hpp>
#include <cstring>
#include <algorithm>
#include <cmath>

namespace detector::detail {

bool letterbox(const cv::Mat& src, cv::Mat& dst,
               int target_w, int target_h,
               LetterBoxInfo& info) noexcept {
    dst.release();
    info = {};

    // 明确限制输入契约，避免空图、灰度图或浮点图在 OpenCV 内部触发异常，
    // 并防止除零或构造负尺寸画布。
    if (src.empty() || src.type() != CV_8UC3 ||
        target_w <= 0 || target_h <= 0) {
        return false;
    }

    try {
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

        cv::Mat resized;
        cv::resize(src, resized, cv::Size(nw, nh), 0, 0, cv::INTER_LINEAR);

        cv::Mat canvas(target_h, target_w, CV_8UC3, cv::Scalar(114, 114, 114));
        resized.copyTo(canvas(cv::Rect(left, top, nw, nh)));

        // ONNX 检测模型通常接收 RGB、NCHW、float32、[0,1]。先转 float
        // 再拆通道，保证最终缓冲区按 R 平面、G 平面、B 平面连续排列。
        cv::Mat rgb_float;
        canvas.convertTo(rgb_float, CV_32FC3, 1.0f / 255.0f);
        cv::cvtColor(rgb_float, rgb_float, cv::COLOR_BGR2RGB);

        std::vector<cv::Mat> channels(3);
        cv::split(rgb_float, channels);

        dst = cv::Mat(1, 3 * target_h * target_w, CV_32F);
        float* ptr = dst.ptr<float>();
        const size_t plane_bytes =
            static_cast<size_t>(target_h) * target_w * sizeof(float);
        for (int c = 0; c < 3; ++c) {
            std::memcpy(ptr + static_cast<size_t>(c) * target_h * target_w,
                        channels[c].ptr<float>(), plane_bytes);
        }
        return true;
    } catch (...) {
        dst.release();
        info = {};
        return false;
    }
}

} // namespace detector::detail
