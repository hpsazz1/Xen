#pragma once

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>

struct RawMouseMotionSample
{
    int rawDx = 0;
    int rawDy = 0;
    int manualDx = 0;
    int manualDy = 0;
    int selfSuppressedDx = 0;
    int selfSuppressedDy = 0;
    std::uint32_t rawPacketCount = 0;
    std::uint32_t selfSuppressedPacketCount = 0;
    double durationSeconds = 0.0;
    std::chrono::steady_clock::time_point time{};
};

class RawMouseInput
{
public:
    bool registerWindow(HWND window);
    void unregisterWindow(HWND window);
    bool handleMessage(LPARAM lParam);

    // 设备成功执行自动命令后登记期望回注包。完全相同的短时 Raw Input 包
    // 会被归为自身输出，避免自动命令递归触发手动接管。
    void recordAutomatedMove(
        int dx,
        int dy,
        std::chrono::steady_clock::time_point sendTime);

    // 无需构造 WM_INPUT 的可测试入口；生产代码仅由 handleMessage 调用。
    void ingestRelativeMotion(
        HANDLE device,
        int dx,
        int dy,
        std::chrono::steady_clock::time_point eventTime);

    RawMouseMotionSample consume(std::chrono::steady_clock::time_point now);
    void reset();

private:
    struct MotionEvent
    {
        HANDLE device = nullptr;
        int dx = 0;
        int dy = 0;
        bool selfGenerated = false;
        std::chrono::steady_clock::time_point time{};
    };

    struct ExpectedMove
    {
        int dx = 0;
        int dy = 0;
        std::chrono::steady_clock::time_point sendTime{};
    };

    void pruneLocked(std::chrono::steady_clock::time_point now);

    std::mutex mutex_;
    std::deque<MotionEvent> events_;
    std::deque<ExpectedMove> expectedMoves_;
    std::chrono::steady_clock::time_point lastConsume_{};
    HWND window_ = nullptr;
};

RawMouseInput& globalRawMouseInput();
