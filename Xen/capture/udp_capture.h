#ifndef UDP_CAPTURE_H
#define UDP_CAPTURE_H

// UDP 网络视频流捕获类
// 通过 UDP 协议接收 MJPEG 视频流作为程序输入源

#include "capture.h"

#include <opencv2/opencv.hpp>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

class UDPCapture : public IScreenCapture
{
public:
    UDPCapture(int width, int height, const std::string& ip = "0.0.0.0", int port = 1234);
    ~UDPCapture();

    cv::Mat GetNextFrameCpu() override;

    // 初始化 UDP 接收端
    bool Initialize();
    // 清理资源
    void Cleanup();
    bool isInitialized() const { return initialized_.load(); }

    // 设置 UDP 参数
    void SetUDPParams(const std::string& ip, int port);
    bool IsConnected() const { return is_connected_.load(); }
    int GetReceivedFrames() const { return received_frames_.load(); }
    int GetDroppedFrames() const { return dropped_frames_.load(); }

private:
    // UDP 接收线程
    void ReceiveThread();
    // 解析 MJPEG 帧数据
    bool ParseMJPEGFrame(const std::vector<uint8_t>& data, cv::Mat& frame);

    int width_;
    int height_;
    std::string ip_;
    int port_;

    SOCKET socket_;
    sockaddr_in server_addr_;

    std::atomic<bool> is_connected_;
    std::atomic<bool> initialized_;
    std::atomic<bool> should_stop_;
    std::atomic<int> received_frames_;
    std::atomic<int> dropped_frames_;

    std::thread receive_thread_;
    std::mutex frame_mutex_;
    std::queue<cv::Mat> frame_queue_;

    static const int MAX_FRAME_SIZE = 1024 * 1024;  // 最大帧字节数
    static const int MAX_QUEUE_SIZE = 5;              // 最大帧队列长度
};

#endif // UDP_CAPTURE_H
