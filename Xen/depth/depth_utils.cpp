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

    cv::Mat generateDepthMaskFallback(
        const cv::Mat& depthGray,
        int nearPercent,
        int expandPixels,
        bool invert)
    {
        cv::Mat mask;
        if (depthGray.empty())
            return mask;

        const int total = depthGray.rows * depthGray.cols;
        if (total <= 0)
            return mask;

        // 计算深度直方图
        int hist[256] = {};
        for (int y = 0; y < depthGray.rows; ++y)
        {
            const uint8_t* row = depthGray.ptr<uint8_t>(y);
            for (int x = 0; x < depthGray.cols; ++x)
                hist[row[x]]++;
        }

        // 根据近端百分比确定深度阈值
        const int target = std::max(1, (total * nearPercent) / 100);
        int threshold = 0;
        if (!invert)
        {
            int count = 0;
            for (int i = 0; i < 256; ++i)
            {
                count += hist[i];
                if (count >= target) { threshold = i; break; }
            }
            cv::compare(depthGray, threshold, mask, cv::CMP_LE);
        }
        else
        {
            int count = 0;
            for (int i = 255; i >= 0; --i)
            {
                count += hist[i];
                if (count >= target) { threshold = i; break; }
            }
            cv::compare(depthGray, threshold, mask, cv::CMP_GE);
        }

        // 对遮罩进行椭圆膨胀扩展
        if (expandPixels > 0)
        {
            const int kernelSize = 2 * expandPixels + 1;
            cv::Mat kernel = cv::getStructuringElement(
                cv::MORPH_ELLIPSE, cv::Size(kernelSize, kernelSize));
            cv::dilate(mask, mask, kernel);
        }

        return mask;
    }
}

#endif
