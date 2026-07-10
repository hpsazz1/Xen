#pragma once
#include <vector>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <opencv2/opencv.hpp>

/**
 * DetectionBuffer - 线程安全的检测结果缓冲区
 *
 * 用于在 AI 推理线程和渲染/控制线程之间传递检测结果。
 * 通过互斥锁保证数据安全，通过条件变量通知消费者新数据就绪。
 *
 * 成员变量说明:
 *   mutex          - 互斥锁，保护所有数据的并发访问
 *   cv             - 条件变量，用于通知消费者新数据可用
 *   version        - 版本号，每次更新数据时递增，消费者可通过版本号判断是否为新数据
 *   boxes          - 检测框列表，每个框用 cv::Rect 表示 (x, y, width, height)
 *   classes        - 类别 ID 列表，与 boxes 一一对应（如 0=玩家, 1=头部）
 *   confidences    - 置信度列表，与 boxes 一一对应，范围为 [0.0, 1.0]
 *   frameTimestamp - 检测帧的时间戳（输入帧的采集时间）
 *   publishTimestamp - 结果发布的时间戳（检测完成写入缓冲区的时间）
 */
struct DetectionBuffer
{
    std::mutex mutex;
    std::condition_variable cv;
    int version = 0;
    std::vector<cv::Rect> boxes;
    std::vector<int> classes;
    std::vector<float> confidences;
    std::chrono::steady_clock::time_point frameTimestamp{};
    std::chrono::steady_clock::time_point publishTimestamp{};

    /**
     * set - 写入检测结果到缓冲区（无置信度版本）
     * @param newBoxes  检测框列表
     * @param newClasses类别 ID 列表
     */
    void set(const std::vector<cv::Rect>& newBoxes, const std::vector<int>& newClasses)
    {
        set(newBoxes, newClasses, std::vector<float>(), {});
    }

    /**
     * set - 写入完整的检测结果到缓冲区
     * 线程安全，加锁后复制数据，递增版本号并通知等待的消费者。
     * @param newBoxes          检测框列表
     * @param newClasses        类别 ID 列表
     * @param newConfidences    置信度列表
     * @param newFrameTimestamp 输入帧的时间戳（如果为默认值则使用当前时间）
     */
    void set(
        const std::vector<cv::Rect>& newBoxes,
        const std::vector<int>& newClasses,
        const std::vector<float>& newConfidences,
        std::chrono::steady_clock::time_point newFrameTimestamp = {})
    {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mutex);
        boxes = newBoxes;
        classes = newClasses;
        confidences = newConfidences;
        frameTimestamp = (newFrameTimestamp.time_since_epoch().count() != 0) ? newFrameTimestamp : now;
        publishTimestamp = now;
        ++version;
        cv.notify_all();
    }

    /**
     * clear - 清空缓冲区
     * 清空所有检测数据，重置时间戳，递增版本号并通知等待的消费者。
     * 在检测暂停时调用，确保消费者不会读取到过期数据。
     */
    void clear()
    {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mutex);
        boxes.clear();
        classes.clear();
        confidences.clear();
        frameTimestamp = now;
        publishTimestamp = now;
        ++version;
        cv.notify_all();
    }

    /**
     * get - 读取检测结果（无置信度版本）
     * 线程安全，加锁后复制数据返回。
     * @param outBoxes              [输出] 检测框列表
     * @param outClasses            [输出] 类别 ID 列表
     * @param outVersion            [输出] 当前版本号
     * @param outFrameTimestamp     [输出] 输入帧时间戳（可选）
     * @param outPublishTimestamp   [输出] 结果发布时间戳（可选）
     */
    void get(
        std::vector<cv::Rect>& outBoxes,
        std::vector<int>& outClasses,
        int& outVersion,
        std::chrono::steady_clock::time_point* outFrameTimestamp = nullptr,
        std::chrono::steady_clock::time_point* outPublishTimestamp = nullptr)
    {
        std::vector<float> ignoredConfidences;
        get(outBoxes, outClasses, ignoredConfidences, outVersion, outFrameTimestamp, outPublishTimestamp);
    }

    /**
     * get - 读取完整的检测结果
     * 线程安全，加锁后复制数据返回。
     * @param outBoxes              [输出] 检测框列表
     * @param outClasses            [输出] 类别 ID 列表
     * @param outConfidences        [输出] 置信度列表
     * @param outVersion            [输出] 当前版本号
     * @param outFrameTimestamp     [输出] 输入帧时间戳（可选）
     * @param outPublishTimestamp   [输出] 结果发布时间戳（可选）
     */
    void get(
        std::vector<cv::Rect>& outBoxes,
        std::vector<int>& outClasses,
        std::vector<float>& outConfidences,
        int& outVersion,
        std::chrono::steady_clock::time_point* outFrameTimestamp = nullptr,
        std::chrono::steady_clock::time_point* outPublishTimestamp = nullptr)
    {
        std::lock_guard<std::mutex> lock(mutex);
        outBoxes = boxes;
        outClasses = classes;
        outConfidences = confidences;
        outVersion = version;
        if (outFrameTimestamp)
            *outFrameTimestamp = frameTimestamp;
        if (outPublishTimestamp)
            *outPublishTimestamp = publishTimestamp;
    }

    /**
     * swapLocked - 在已持有 mutex 的前提下，通过 std::swap 将缓冲区数据移动到调用者的向量中
     * 比逐元素拷贝更快：锁内仅做指针交换（O(1)），真正的数据在锁外由调用者持有。
     * 注意：调用者必须已持有 mutex，否则产生数据竞争。
     * @param outBoxes          [输出] 检测框列表（与缓冲区交换）
     * @param outClasses        [输出] 类别 ID 列表
     * @param outConfidences    [输出] 置信度列表
     * @param outVersion        [输出] 当前版本号
     * @param outFrameTimestamp [输出] 输入帧时间戳
     */
    void swapLocked(
        std::vector<cv::Rect>& outBoxes,
        std::vector<int>& outClasses,
        std::vector<float>& outConfidences,
        int& outVersion,
        std::chrono::steady_clock::time_point& outFrameTimestamp)
    {
        std::swap(outBoxes, boxes);
        std::swap(outClasses, classes);
        std::swap(outConfidences, confidences);
        outVersion = version;
        outFrameTimestamp = frameTimestamp;
    }
};
