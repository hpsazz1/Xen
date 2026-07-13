#ifndef NDI_CAPTURE_H
#define NDI_CAPTURE_H

// NDI（Network Device Interface）网络视频流捕获类
// 通过网络接收 NDI 视频流作为程序输入源

#include "capture.h"

#include <Processing.NDI.Lib.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

class NDICapture : public IScreenCapture
{
public:
    NDICapture(int width, int height, const std::string& sourceName = "", int frameRate = 60,
               int sourceWidth = 0, int sourceHeight = 0);
    ~NDICapture();

    cv::Mat GetNextFrameCpu() override;

    // 初始化 NDI 接收器
    bool Initialize();
    // 清理资源
    void Cleanup();
    bool isInitialized() const { return initialized_.load(); }

    // 设置 NDI 源名称
    void SetSource(const std::string& sourceName);
    bool IsConnected() const { return is_connected_.load(); }
    std::string GetConnectedSourceName() const { return connected_source_name_; }

    // 获取可用 NDI 源列表。新建发现器需要等待 mDNS 首次返回，timeoutMs 为最长等待时间。
    static std::vector<std::string> GetAvailableSources(uint32_t timeoutMs = 1500);

private:
    struct NetworkFrame
    {
        cv::Mat image;       ///< 已按 1:1 像素中心裁剪的检测帧
        int sourceWidth = 0; ///< NDI 视频帧报告的完整传输宽度
        int sourceHeight = 0;///< NDI 视频帧报告的完整传输高度
    };
    // NDI 接收线程
    void ReceiveThread();

    int width_;
    int height_;
    int frame_rate_;
    int configured_source_width_;  ///< OBS 预裁剪 ROI 未携带元数据时的完整游戏 FOV 宽度
    int configured_source_height_; ///< OBS 预裁剪 ROI 未携带元数据时的完整游戏 FOV 高度
    std::string source_name_;
    std::string connected_source_name_;

    NDIlib_find_instance_t ndi_find_;  // NDI 源查找器
    NDIlib_recv_instance_t ndi_recv_;  // NDI 接收器
    bool ndi_sdk_initialized_;

    std::atomic<bool> is_connected_;
    std::atomic<bool> initialized_;
    std::atomic<bool> should_stop_;
    std::atomic<int> received_frames_;
    std::atomic<int> dropped_frames_;

    std::thread receive_thread_;
    std::mutex frame_mutex_;
    std::queue<NetworkFrame> frame_queue_;

    static const int MAX_QUEUE_SIZE = 5;  // 最大帧队列长度
};

#endif // NDI_CAPTURE_H
