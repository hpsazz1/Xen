#include "detector/preprocess.h"
#include <cstring>
#include <algorithm>

namespace detector::detail {

void letterbox(const cv::Mat& src, cv::Mat& dst,
               int target_w, int target_h,
               LetterBoxInfo& info) {
    int sw = src.cols, sh = src.rows;

    float scale = std::min((float)target_w / sw, (float)target_h / sh);
    int nw = (int)(sw * scale);
    int nh = (int)(sh * scale);
    float pad_x = (target_w - nw) / 2.0f;
    float pad_y = (target_h - nh) / 2.0f;

    info.scale_x = 1.0f / scale;
    info.scale_y = 1.0f / scale;
    info.pad_x   = pad_x;
    info.pad_y   = pad_y;
    info.orig_w  = sw;
    info.orig_h  = sh;

    cv::Mat resized;
    cv::resize(src, resized, cv::Size(nw, nh), 0, 0, cv::INTER_LINEAR);

    cv::Mat canvas(target_h, target_w, CV_8UC3, cv::Scalar(114, 114, 114));
    resized.copyTo(canvas(cv::Rect((int)pad_x, (int)pad_y, nw, nh)));

    // BGR → RGB + HWC → CHW + [0,1]
    cv::Mat rgb_float;
    canvas.convertTo(rgb_float, CV_32FC3, 1.0f / 255.0f);
    cv::cvtColor(rgb_float, rgb_float, cv::COLOR_BGR2RGB);

    std::vector<cv::Mat> ch(3);
    cv::split(rgb_float, ch);

    dst = cv::Mat(1, 3 * target_h * target_w, CV_32F);
    float* ptr = dst.ptr<float>();
    for (int c = 0; c < 3; ++c)
        std::memcpy(ptr + c * target_h * target_w,
                    ch[c].data, target_h * target_w * sizeof(float));
}

} // namespace detector::detail
