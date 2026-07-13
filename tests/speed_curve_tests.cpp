#include "runtime/basic_aim_controller.h"
#include "runtime/aim_coordinate_space.h"
#include "runtime/basic_target_filter.h"
#include "debug/pipeline_tracer.h"
#include "capture/ndi_frame_geometry.h"
#include "capture/network_frame_geometry.h"
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

    const auto udpConfiguredGeometry = ResolveConfiguredNetworkFrameGeometry(
        320, 320, 2560, 1440);
    expectTrue(udpConfiguredGeometry.fromConfig, "udp roi uses configured full fov geometry");
    expectNear(udpConfiguredGeometry.sourceWidth, 2560.0, 0.0,
               "udp roi reports full fov width");
    expectNear(udpConfiguredGeometry.sourceHeight, 1440.0, 0.0,
               "udp roi reports full fov height");
    const auto udpInvalidGeometry = ResolveConfiguredNetworkFrameGeometry(
        320, 320, 2560, 100);
    expectTrue(!udpInvalidGeometry.fromConfig, "udp rejects undersized source geometry");
    expectNear(udpInvalidGeometry.sourceWidth, 320.0, 0.0,
               "udp invalid geometry falls back to encoded width");

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
    expectTrue(traceHeader.find("ObservedVelocityX,ObservedVelocityY,ObservedSpeed") != std::string::npos,
               "basic pipeline contains signed target velocity diagnostics");
    expectTrue(traceHeader.find("SourceWidth,SourceHeight") != std::string::npos,
               "basic pipeline contains capture source dimensions");
    expectTrue(traceHeader.find(
        "InferenceFPS,SourceDeclaredFPS,SourceReceiveFPS,SourceReceivedFrames,SourceDroppedFrames") != std::string::npos,
        "basic pipeline contains generic source fps diagnostics");
    expectTrue(traceHeader.find("NdiDeclaredFPS,NdiReceiveFPS,NdiReceivedFrames,NdiDroppedFrames") != std::string::npos,
               "basic pipeline keeps ndi compatibility diagnostics");
    expectTrue(traceHeader.find("FrameCountLimit,SpeedLimited,Settled") != std::string::npos,
               "basic pipeline reports controller speed limiting");
    expectTrue(traceHeader.find("IntegralCountsX,IntegralCountsY") != std::string::npos &&
               traceHeader.find("ResponseSeconds,IntegralTimeSeconds") != std::string::npos,
               "basic pipeline reports moving-target integral diagnostics");
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
    expectNear(filteredMove.observedVelocityX, 962.5, 1e-9,
               "basic filter reports signed horizontal observation velocity");
    expectNear(filteredMove.observedVelocityY, 25.0, 1e-9,
               "basic filter reports signed vertical observation velocity");
    expectNear(filteredMove.observedSpeed,
               std::hypot(filteredMove.observedVelocityX, filteredMove.observedVelocityY),
               1e-9, "basic filter speed matches signed velocity magnitude");

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
    expectTrue(farOutput.speedLimited, "basic controller reports speed limiting");
    const auto settledOutput = controller.update(
        3.0, -2.0, 1.0 / 120.0, 15.0, 15.0, controllerSettings);
    expectTrue(settledOutput.settled, "basic controller settle state");
    expectNear(settledOutput.countsX, 0.0, 0.0, "basic controller settled x");

    // 生产速率必须使用捕获窗统计出的实际 FPS 换算单帧预算，而不是绑定某个设备或固定帧率。
    // 选取本轮 CSV 的约 127/143 FPS 和未来可能出现的 240 FPS，验证每秒总预算始终一致。
    BasicAimController::Settings productionSettings;
    productionSettings.settleRadiusPixels = 0.0;
    productionSettings.releaseRadiusPixels = 0.0;
    expectNear(productionSettings.maxCountsPerSecond, 1440.0, 0.0,
               "production max speed uses four-chain nine-grid result");
    for (const double actualFps : { 127.0, 143.0, 240.0 })
    {
        BasicAimController actualFpsController;
        const auto actualFpsOutput = actualFpsController.update(
            1000.0, 0.0, 1.0 / actualFps, 15.0, 15.0, productionSettings);
        expectNear(actualFpsOutput.frameCountLimit * actualFps,
                   productionSettings.maxCountsPerSecond, 1e-9,
                   "production speed follows capture-window fps");
    }

    // 本轮只放宽远距离设备速率上限。近中心未触发限速时，两档速度必须产生完全相同的
    // 一阶响应，证明优化不会改变已经通过 36 段实测验证的稳定半径与近中心收敛形态。
    BasicAimController baselineNearController;
    BasicAimController optimizedNearController;
    BasicAimController::Settings baselineSettings = productionSettings;
    baselineSettings.maxCountsPerSecond = 1200.0;
    const auto baselineNearOutput = baselineNearController.update(
        12.0, 0.0, 1.0 / 120.0, 1.344, 1.668, baselineSettings);
    const auto optimizedNearOutput = optimizedNearController.update(
        12.0, 0.0, 1.0 / 120.0, 1.344, 1.668, productionSettings);
    expectTrue(!baselineNearOutput.speedLimited && !optimizedNearOutput.speedLimited,
               "near-center response remains outside speed limiting");
    expectNear(optimizedNearOutput.countsX, baselineNearOutput.countsX, 1e-12,
               "max speed optimization preserves near-center response");

    // 远距离请求在两档速度下均受限，1440 cps 的单帧预算应严格比 1200 cps 高 20%，
    // 从而只缩短大误差阶段，不通过修改响应时间或稳定回差掩盖问题。
    BasicAimController baselineFarController;
    BasicAimController optimizedFarController;
    const auto baselineFarOutput = baselineFarController.update(
        160.0, 0.0, 1.0 / 120.0, 1.344, 1.668, baselineSettings);
    const auto optimizedFarOutput = optimizedFarController.update(
        160.0, 0.0, 1.0 / 120.0, 1.344, 1.668, productionSettings);
    expectTrue(baselineFarOutput.speedLimited && optimizedFarOutput.speedLimited,
               "far response remains explicitly speed limited");
    expectNear(optimizedFarOutput.frameCountLimit / baselineFarOutput.frameCountLimit,
               1.2, 1e-12, "far frame budget increases by twenty percent");

    // 匀速目标是斜坡输入，纯比例控制必然保留“目标速度 × 响应时间”的固定滞后。
    // 现场六轮数据折算闭环滞后约 84~85 ms，即使把 UI 响应压到 20 ms 也不能归零。
    // 使用标准 PI 积分后，设备需求仍低于 1440 cps 时应消除该结构性稳态误差。
    auto simulateMovingTarget = [](double integralTimeSeconds) {
        BasicAimController movingController;
        BasicAimController::Settings movingSettings;
        movingSettings.responseSeconds = 0.080;
        movingSettings.maxCountsPerSecond = 1440.0;
        movingSettings.integralTimeSeconds = integralTimeSeconds;
        movingSettings.settleRadiusPixels = 0.0;
        movingSettings.releaseRadiusPixels = 0.0;
        constexpr double dt = 1.0 / 120.0;
        constexpr double countsPerPixel = 1.344;
        constexpr double targetCountsPerSecond = 800.0;
        double targetPixels = 0.0;
        double cameraPixels = 0.0;
        for (int frame = 0; frame < 480; ++frame)
        {
            targetPixels += targetCountsPerSecond / countsPerPixel * dt;
            const auto output = movingController.update(
                targetPixels - cameraPixels, 0.0, dt,
                countsPerPixel, countsPerPixel, movingSettings);
            cameraPixels += output.countsX / countsPerPixel;
        }
        return targetPixels - cameraPixels;
    };
    const double proportionalRampError = simulateMovingTarget(0.0);
    const double integralRampError = simulateMovingTarget(0.320);
    expectTrue(std::abs(proportionalRampError) > 35.0,
               "proportional controller keeps ramp steady-state error");
    expectTrue(std::abs(integralRampError) < 5.0,
               "pi controller removes moving-target steady-state error");

    // DML 推理帧率较低时单帧积分输出更大，PI 追进 5 px 中心区后必须继续保留
    // 匀速目标所需的整数 count 输出，不能被静止目标稳定分支反复清零。
    BasicAimController dmlRateController;
    BasicAimController::Settings dmlRateSettings;
    dmlRateSettings.responseSeconds = 0.080;
    dmlRateSettings.maxCountsPerSecond = 1440.0;
    dmlRateSettings.integralTimeSeconds = 0.320;
    constexpr double dmlDt = 1.0 / 60.0;
    constexpr double dmlCountsPerPixel = 1.344;
    constexpr double dmlTargetCountsPerSecond = 800.0;
    double dmlTargetPixels = 0.0;
    double dmlCameraPixels = 0.0;
    int dmlSettledMovingFrames = 0;
    for (int frame = 0; frame < 360; ++frame)
    {
        dmlTargetPixels += dmlTargetCountsPerSecond / dmlCountsPerPixel * dmlDt;
        const auto output = dmlRateController.update(
            dmlTargetPixels - dmlCameraPixels, 0.0, dmlDt,
            dmlCountsPerPixel, dmlCountsPerPixel, dmlRateSettings);
        dmlCameraPixels += output.countsX / dmlCountsPerPixel;
        if (frame >= 120 && output.settled)
            ++dmlSettledMovingFrames;
    }
    expectTrue(dmlSettledMovingFrames == 0,
               "pi controller does not settle and clear integral on a moving target");
    expectTrue(std::abs(dmlTargetPixels - dmlCameraPixels) < 5.0,
               "pi controller tracks a moving target at dml inference rate");

    // 同侧进入中心应保留有效积分；真正越过中心时必须先清除旧方向积分，随后允许
    // 静止稳定逻辑接管，兼顾移动目标零稳态误差与静止目标不过冲。
    BasicAimController centerHoldController;
    BasicAimController::Settings centerHoldSettings = dmlRateSettings;
    for (int frame = 0; frame < 20; ++frame)
        centerHoldController.update(20.0, 0.0, dmlDt, 1.344, 1.344, centerHoldSettings);
    const auto sameSideCenter = centerHoldController.update(
        4.0, 0.0, dmlDt, 1.344, 1.344, centerHoldSettings);
    expectTrue(!sameSideCenter.settled && std::abs(sameSideCenter.integralCountsX) >= 0.5,
               "actionable integral survives same-side center entry");
    const auto crossedCenter = centerHoldController.update(
        -1.0, 0.0, dmlDt, 1.344, 1.344, centerHoldSettings);
    expectTrue(crossedCenter.settled,
               "error sign reversal clears integral before static settling");

    // 积分候选必须保持静止闭环安全：大误差阶段受设备限速时禁止 wind-up，
    // 接近中心后进入既有 5 px 稳定半径并清空积分，不得跨越目标持续反向输出。
    BasicAimController staticIntegralController;
    BasicAimController::Settings staticIntegralSettings;
    staticIntegralSettings.responseSeconds = 0.080;
    staticIntegralSettings.maxCountsPerSecond = 1440.0;
    staticIntegralSettings.integralTimeSeconds = 0.320;
    double staticCameraPixels = 0.0;
    bool staticSettled = false;
    for (int frame = 0; frame < 480; ++frame)
    {
        const auto output = staticIntegralController.update(
            140.0 - staticCameraPixels, 0.0, 1.0 / 120.0,
            1.344, 1.344, staticIntegralSettings);
        staticCameraPixels += output.countsX / 1.344;
        staticSettled = staticSettled || output.settled;
    }
    expectTrue(staticSettled, "pi controller settles a static step");
    expectTrue(std::abs(140.0 - staticCameraPixels) <= 5.0,
               "pi controller keeps static step inside settle radius");

    if (failures != 0)
    {
        std::cerr << failures << " basic algorithm test(s) failed\n";
        return 1;
    }

    std::cout << "basic algorithm tests passed\n";
    return 0;
}
