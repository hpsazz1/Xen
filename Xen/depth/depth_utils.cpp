#ifdef USE_CUDA

#include "depth_utils.h"

namespace depth_anything
{
    // 深度图像尺寸调整：保持原始宽高比，缩放到目标尺寸，居中放置并填充灰色边框
    // 返回值为(调整后的图像, x偏移量, y偏移量)
    std::tuple<cv::Mat, int, int> resize_depth(const cv::Mat& img, int w, int h)
    {
        int nw;
        int nh;
        float aspectRatio = static_cast<float>(img.cols) / static_cast<float>(img.rows);

        if (aspectRatio >= 1.0f)
        {
            nw = w;
            nh = static_cast<int>(h / aspectRatio);
        }
        else
        {
            nw = static_cast<int>(w * aspectRatio);
            nh = h;
        }

        cv::Mat resized;
        cv::resize(img, resized, cv::Size(nw, nh));

        cv::Mat rgb;
        cv::cvtColor(resized, rgb, cv::COLOR_BGR2RGB);

        cv::Mat out(h, w, CV_8UC3, cv::Scalar(128, 128, 128));
        const int xOffset = (w - rgb.cols) / 2;
        const int yOffset = (h - rgb.rows) / 2;
        rgb.copyTo(out(cv::Rect(xOffset, yOffset, rgb.cols, rgb.rows)));

        return std::make_tuple(out, xOffset, yOffset);
    }
}

#endif
