#pragma once

#include <thread>

class ApplicationThreads
{
public:
    ApplicationThreads() = default;
    ~ApplicationThreads();

    ApplicationThreads(const ApplicationThreads&) = delete;
    ApplicationThreads& operator=(const ApplicationThreads&) = delete;

    void joinAll() noexcept;

    std::thread keyboard;
    std::thread capture;
    std::thread detector;
    std::thread mouse;
    std::thread overlay;
    std::thread gameOverlay;

private:
    static void join(std::thread& thread) noexcept;
};

