#pragma once

// Requests a process-wide shutdown and wakes every worker that may be blocked
// on a condition variable. Safe to call repeatedly and from any worker thread.
void RequestApplicationShutdown() noexcept;

