#include "runtime/application_shutdown.h"

#include "Xen.h"
#include "capture.h"
#include "runtime/thread_loops.h"

void RequestApplicationShutdown() noexcept
{
    shouldExit.store(true);
    gameOverlayShouldExit.store(true);

#ifdef USE_CUDA
    trt_detector.requestStop();
#else
    if (dml_detector)
        dml_detector->requestStop();
#endif

    frameCV.notify_all();
    detectionBuffer.cv.notify_all();
}
