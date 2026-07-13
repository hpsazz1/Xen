#include "runtime/application_threads.h"

#include "runtime/application_shutdown.h"

ApplicationThreads::~ApplicationThreads()
{
    RequestApplicationShutdown();
    joinAll();
}

void ApplicationThreads::joinAll() noexcept
{
    join(keyboard);
    join(capture);
    join(detector);
    join(mouse);
    join(overlay);
    join(gameOverlay);
}

void ApplicationThreads::join(std::thread& thread) noexcept
{
    if (thread.joinable())
        thread.join();
}

