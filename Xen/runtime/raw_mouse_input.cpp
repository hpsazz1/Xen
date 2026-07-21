#include "raw_mouse_input.h"

#include <algorithm>
#include <vector>

namespace
{
RawMouseInput g_rawMouseInput;
constexpr auto kSelfMatchWindow = std::chrono::milliseconds(25);
constexpr auto kLateRegistrationWindow = std::chrono::milliseconds(10);
constexpr auto kMaximumInputHistory = std::chrono::milliseconds(100);
}

RawMouseInput& globalRawMouseInput()
{
    return g_rawMouseInput;
}

bool RawMouseInput::registerWindow(HWND window)
{
    if (!window)
        return false;

    RAWINPUTDEVICE device{};
    device.usUsagePage = 0x01; // Generic Desktop Controls
    device.usUsage = 0x02;     // Mouse
    device.dwFlags = RIDEV_INPUTSINK;
    device.hwndTarget = window;
    if (!RegisterRawInputDevices(&device, 1, sizeof(device)))
        return false;

    std::lock_guard<std::mutex> lock(mutex_);
    window_ = window;
    events_.clear();
    expectedMoves_.clear();
    lastConsume_ = std::chrono::steady_clock::now();
    return true;
}

void RawMouseInput::unregisterWindow(HWND window)
{
    bool shouldRemove = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shouldRemove = window && window_ == window;
        window_ = nullptr;
        events_.clear();
        expectedMoves_.clear();
        lastConsume_ = std::chrono::steady_clock::now();
    }
    if (!shouldRemove)
        return;

    RAWINPUTDEVICE device{};
    device.usUsagePage = 0x01;
    device.usUsage = 0x02;
    device.dwFlags = RIDEV_REMOVE;
    device.hwndTarget = nullptr;
    RegisterRawInputDevices(&device, 1, sizeof(device));
}

bool RawMouseInput::handleMessage(LPARAM lParam)
{
    UINT size = 0;
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT,
            nullptr, &size, sizeof(RAWINPUTHEADER)) != 0 || size == 0)
        return false;

    std::vector<std::uint8_t> buffer(size);
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT,
            buffer.data(), &size, sizeof(RAWINPUTHEADER)) != size)
        return false;

    const RAWINPUT* raw = reinterpret_cast<const RAWINPUT*>(buffer.data());
    if (raw->header.dwType != RIM_TYPEMOUSE ||
        (raw->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0)
        return false;

    ingestRelativeMotion(
        raw->header.hDevice,
        static_cast<int>(raw->data.mouse.lLastX),
        static_cast<int>(raw->data.mouse.lLastY),
        std::chrono::steady_clock::now());
    return true;
}

void RawMouseInput::recordAutomatedMove(
    int dx,
    int dy,
    std::chrono::steady_clock::time_point sendTime)
{
    if (dx == 0 && dy == 0)
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    pruneLocked(sendTime);

    // Raw Input 通常在驱动调用返回后才由 UI 线程消费；仍处理消息先到、发送
    // 确认后到的窄竞态，避免一个自身包泄漏为人工输入。
    for (auto it = events_.rbegin(); it != events_.rend(); ++it)
    {
        if (!it->selfGenerated && it->dx == dx && it->dy == dy &&
            it->time <= sendTime && sendTime - it->time <= kLateRegistrationWindow)
        {
            it->selfGenerated = true;
            return;
        }
    }

    expectedMoves_.push_back({ dx, dy, sendTime });
    while (expectedMoves_.size() > 256)
        expectedMoves_.pop_front();
}

void RawMouseInput::ingestRelativeMotion(
    HANDLE device,
    int dx,
    int dy,
    std::chrono::steady_clock::time_point eventTime)
{
    if (dx == 0 && dy == 0)
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    pruneLocked(eventTime);

    bool selfGenerated = false;
    for (auto it = expectedMoves_.begin(); it != expectedMoves_.end(); ++it)
    {
        if (it->dx == dx && it->dy == dy && eventTime >= it->sendTime &&
            eventTime - it->sendTime <= kSelfMatchWindow)
        {
            expectedMoves_.erase(it);
            selfGenerated = true;
            break;
        }
    }
    events_.push_back({ device, dx, dy, selfGenerated, eventTime });
    while (events_.size() > 2048)
        events_.pop_front();
}

RawMouseMotionSample RawMouseInput::consume(
    std::chrono::steady_clock::time_point now)
{
    std::lock_guard<std::mutex> lock(mutex_);
    pruneLocked(now);

    const auto windowStart = std::max(lastConsume_, now - kMaximumInputHistory);
    RawMouseMotionSample result;
    result.time = now;
    result.durationSeconds = std::clamp(
        std::chrono::duration<double>(now - windowStart).count(), 0.001, 0.100);
    for (const MotionEvent& event : events_)
    {
        if (event.time <= windowStart || event.time > now)
            continue;
        result.rawDx += event.dx;
        result.rawDy += event.dy;
        ++result.rawPacketCount;
        if (event.selfGenerated)
        {
            result.selfSuppressedDx += event.dx;
            result.selfSuppressedDy += event.dy;
            ++result.selfSuppressedPacketCount;
        }
        else
        {
            result.manualDx += event.dx;
            result.manualDy += event.dy;
            result.time = event.time;
        }
    }
    lastConsume_ = now;
    while (!events_.empty() && events_.front().time <= now)
        events_.pop_front();
    return result;
}

void RawMouseInput::reset()
{
    std::lock_guard<std::mutex> lock(mutex_);
    events_.clear();
    expectedMoves_.clear();
    lastConsume_ = std::chrono::steady_clock::now();
}

void RawMouseInput::pruneLocked(std::chrono::steady_clock::time_point now)
{
    while (!events_.empty() && now - events_.front().time > kMaximumInputHistory)
        events_.pop_front();
    while (!expectedMoves_.empty() &&
        now - expectedMoves_.front().sendTime > kSelfMatchWindow)
        expectedMoves_.pop_front();
}
