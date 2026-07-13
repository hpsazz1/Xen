#include "runtime/basic_aim_controller.h"
#include "runtime/aim_coordinate_space.h"
#include "runtime/basic_target_filter.h"
#include "debug/pipeline_tracer.h"
#include "capture/ndi_frame_geometry.h"
#include "runtime/frame_rate_counter.h"
#include "runtime/latest_frame_queue.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <queue>
#include <string>

namespace
{
int failures = 0;

void expectNear(double actual, double expected, double tolerance, const char* name)
{
    if (std::abs(actual - expected) <= tolerance)
        return;

    std::cerr << name << ": expected " << expected << ", got " << actual << '\n';
    ++failures;
}

void expectTrue(bool condition, const char* name)
{
    if (condition)
        return;

    std::cerr << name << ": condition was false\n";
    ++failures;
}
}

int main()
{
    FrameRateCounter rateCounter;
    const auto rateStart = FrameRateCounter::TimePoint(std::chrono::milliseconds(1000));
    rateCounter.reset(rateStart);
    for (int frame = 1; frame <= 240; ++frame)
    {
        rateCounter.addFrame(rateStart + std::chrono::microseconds(frame * 1000000 / 240));
    }
    expectNear(rateCounter.value(rateStart + std::chrono::seconds(1)), 240.0, 0.0,
               "frame rate counter measures real event cadence");
    expectNear(rateCounter.value(rateStart + std::chrono::seconds(4)), 0.0, 0.0,
               "frame rate counter expires stale rate");

    std::queue<int> latestFrames;
    expectTrue(ReplaceWithLatestFrame(latestFrames, 1) == 0,
               "latest frame queue accepts first frame");
    expectTrue(ReplaceWithLatestFrame(latestFrames, 2) == 1,
               "latest frame queue counts superseded frame");
    int latestValue = 0;
    uint64_t skippedFrames = 0;
    expectTrue(TakeLatestFrame(latestFrames, latestValue, skippedFrames),
               "latest frame queue returns pending frame");
    expectNear(latestValue, 2.0, 0.0, "latest frame queue returns newest value");
    expectTrue(skippedFrames == 0, "latest frame queue has no hidden backlog");
    latestFrames.push(3);
    latestFrames.push(4);
    latestFrames.push(5);
    expectTrue(TakeLatestFrame(latestFrames, latestValue, skippedFrames),
               "latest frame queue drains defensive backlog");
    expectNear(latestValue, 5.0, 0.0, "latest frame queue selects newest backlog value");
    expectTrue(skippedFrames == 2, "latest frame queue reports defensive backlog");

    const auto ndiMetadataGeometry = ResolveNdiFrameGeometry(
        320, 320, "<xen source_width=\"2560\" source_height=\"1440\" roi_x=\"1120\" roi_y=\"560\"/>",
        1920, 1080);
    expectTrue(ndiMetadataGeometry.fromMetadata, "ndi metadata has priority");
    expectNear(ndiMetadataGeometry.sourceWidth, 2560.0, 0.0, "ndi metadata source width");
    expectNear(ndiMetadataGeometry.sourceHeight, 1440.0, 0.0, "ndi metadata source height");

    const auto ndiConfigGeometry = ResolveNdiFrameGeometry(
        320, 320, "<ndi source_width=\"9999\" source_height=\"9999\"/>", 2560, 1440);
    expectTrue(ndiConfigGeometry.fromConfig, "ndi config fallback for obs roi");
    expectNear(ndiConfigGeometry.sourceWidth, 2560.0, 0.0, "ndi config source width");

    const auto ndiSafeGeometry = ResolveNdiFrameGeometry(
        320, 320, "<xen source_width=\"100\" source_height=\"100\"/>", 0, 0);
    expectTrue(!ndiSafeGeometry.fromMetadata && !ndiSafeGeometry.fromConfig,
               "ndi rejects invalid source geometry");
    expectNear(ndiSafeGeometry.sourceWidth, 320.0, 0.0, "ndi falls back to encoded width");

    const double sourceSpan = AimCoordinateSpace::resolveFovPixelSpan(2560, 320.0);
    const double fallbackSpan = AimCoordinateSpace::resolveFovPixelSpan(0, 320.0);
    expectNear(sourceSpan, 2560.0, 0.0, "aim conversion uses capture source width");
    expectNear(fallbackSpan, 320.0, 0.0, "aim conversion falls back to detection width");
    const double sourceCountsPerPixel = AimCoordinateSpace::countsPerSourcePixel(
        106.0, sourceSpan, 1.4 * 0.022);
    const double wrongCropCountsPerPixel = AimCoordinateSpace::countsPerSourcePixel(
        106.0, 320.0, 1.4 * 0.022);
    expectNear(wrongCropCountsPerPixel / sourceCountsPerPixel, 8.0, 1e-9,
               "center crop must not multiply counts per pixel");

    PipelineTracer tracer;
    tracer.setMaxFrames(10);
    PipelineFrame pending = tracer.beginFrame(320);
    pending.rawPivotX = 123.0;
    expectTrue(tracer.getFrames().empty(), "trace frame is invisible before commit");
    tracer.commitFrame(std::move(pending));
    const auto committed = tracer.getFrames();
    expectTrue(committed.size() == 1, "trace frame commit count");
    expectNear(committed.front().rawPivotX, 123.0, 0.0, "trace frame committed value");

    const char* tracePath = "xen_basic_pipeline_test.csv";
    expectTrue(tracer.exportCSV(tracePath), "basic pipeline csv export");
    std::ifstream traceFile(tracePath, std::ios::binary);
    std::string traceHeader;
    std::getline(traceFile, traceHeader);
    if (traceHeader.size() >= 3 &&
        static_cast<unsigned char>(traceHeader[0]) == 0xEF)
        traceHeader.erase(0, 3);
    expectTrue(traceHeader.find("FilteredX") != std::string::npos,
               "basic pipeline contains filtered observation");
    expectTrue(traceHeader.find("SourceWidth,SourceHeight") != std::string::npos,
               "basic pipeline contains capture source dimensions");
    expectTrue(traceHeader.find(
        "InferenceFPS,SourceDeclaredFPS,SourceReceiveFPS,SourceReceivedFrames,SourceDroppedFrames") != std::string::npos,
        "basic pipeline contains generic source fps diagnostics");
    expectTrue(traceHeader.find("NdiDeclaredFPS,NdiReceiveFPS,NdiReceivedFrames,NdiDroppedFrames") != std::string::npos,
               "basic pipeline keeps ndi compatibility diagnostics");
    expectTrue(traceHeader.find("PredX") == std::string::npos,
               "basic pipeline excludes prediction stage");
    traceFile.close();
    std::remove(tracePath);

    for (int i = 0; i < 15; ++i)
        tracer.commitFrame(tracer.beginFrame(320));
    expectTrue(tracer.size() == 10, "trace ring capacity");

    BasicTargetFilter filter;
    const auto t0 = std::chrono::steady_clock::time_point(std::chrono::milliseconds(1000));
    filter.update(160.0, 160.0, t0, 1.0 / 120.0, 320.0);
    const auto filteredNoise = filter.update(
        160.3, 159.8, t0 + std::chrono::milliseconds(8), 1.0 / 120.0, 320.0);
    expectTrue(std::hypot(filteredNoise.x - 160.0, filteredNoise.y - 160.0) < 0.2,
               "basic filter suppresses detector quantization");
    const auto filteredMove = filter.update(
        168.0, 160.0, t0 + std::chrono::milliseconds(16), 1.0 / 120.0, 320.0);
    expectTrue(filteredMove.x > filteredNoise.x, "basic filter follows movement");
    expectTrue(filteredMove.x <= 168.0, "basic filter does not predict future position");

    BasicAimController controller;
    BasicAimController::Settings controllerSettings;
    controllerSettings.responseSeconds = 0.08;
    controllerSettings.maxCountsPerSecond = 240.0;
    controllerSettings.settleRadiusPixels = 5.0;
    controllerSettings.releaseRadiusPixels = 8.0;
    const auto farOutput = controller.update(
        -100.0, 0.0, 1.0 / 120.0, 15.0, 15.0, controllerSettings);
    expectTrue(farOutput.countsX < 0.0, "basic controller direction");
    expectTrue(std::hypot(farOutput.countsX, farOutput.countsY) <= 2.0 + 1e-9,
               "basic controller frame-rate speed limit");
    const auto settledOutput = controller.update(
        3.0, -2.0, 1.0 / 120.0, 15.0, 15.0, controllerSettings);
    expectTrue(settledOutput.settled, "basic controller settle state");
    expectNear(settledOutput.countsX, 0.0, 0.0, "basic controller settled x");

    // 生产速率必须使用捕获窗统计出的实际 FPS 换算单帧预算，而不是绑定某个设备或固定帧率。
    // 选取本轮 CSV 的约 131/155 FPS 和未来可能出现的 240 FPS，验证每秒总预算始终一致。
    BasicAimController::Settings productionSettings;
    productionSettings.settleRadiusPixels = 0.0;
    productionSettings.releaseRadiusPixels = 0.0;
    for (const double actualFps : { 131.0, 155.0, 240.0 })
    {
        BasicAimController actualFpsController;
        const auto actualFpsOutput = actualFpsController.update(
            1000.0, 0.0, 1.0 / actualFps, 15.0, 15.0, productionSettings);
        expectNear(actualFpsOutput.frameCountLimit * actualFps,
                   productionSettings.maxCountsPerSecond, 1e-9,
                   "production speed follows capture-window fps");
    }

    if (failures != 0)
    {
        std::cerr << failures << " basic algorithm test(s) failed\n";
        return 1;
    }

    std::cout << "basic algorithm tests passed\n";
    return 0;
}
