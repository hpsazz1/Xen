// ============================================================
// 绘制调试统计信息覆盖层
// 功能: 在游戏画面上覆盖显示 AI 检测管线的各项性能指标，
//       包括各阶段耗时、采集帧率、采集源详情及 GPU 统计。
// ============================================================

#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <string>

#include "imgui/imgui.h"
#include "Xen.h"
#include "overlay.h"
#include "capture.h"
#include "other_tools.h"
#include "overlay/ui_sections.h"

// ============================================================
// draw_stats() - 主绘制函数
// 在 ImGui 覆盖层上渲染所有统计信息。
// 包含三大区块：
//   1. Time Breakdown —— 推理管线各阶段耗时折线图
//   2. Capture FPS    —— 采集帧率折线图及显示器负载
//   3. Capture Details —— 采集方法、分辨率、WinRT/CUDA GPU 统计
// ============================================================
void draw_stats()
{
    // ---- 推理各阶段环形缓冲区（每个阶段保留最近 120 帧的耗时） ----
    static float preprocess_times[120] = {};    // 预处理耗时（毫秒）
    static float inference_times[120] = {};     // 推理耗时（毫秒）
    static float copy_times[120] = {};          // GPU 拷贝耗时（毫秒）
    static float postprocess_times[120] = {};   // 后处理耗时（毫秒）
    static float nms_times[120] = {};           // NMS 非极大值抑制耗时（毫秒）
    static int index_inf = 0;                   // 推理数据环形缓冲区的当前写入位置

    // ---- 采集帧率环形缓冲区 ----
    static float capture_fps_vals[120] = {};    // 每秒采集帧数历史记录
    static int index_fps = 0;                   // 帧率环形缓冲区的当前写入位置

    // ---- 各指标缓存平均值（每秒刷新一次） ----
    static float avg_preprocess_cached = 0.0f;  // 预处理的缓存平均值
    static float avg_inference_cached = 0.0f;   // 推理的缓存平均值
    static float avg_copy_cached = 0.0f;        // 拷贝的缓存平均值
    static float avg_post_cached = 0.0f;        // 后处理的缓存平均值
    static float avg_nms_cached = 0.0f;         // NMS 的缓存平均值
    static float avg_fps_cached = 0.0f;         // 帧率的缓存平均值
    static double last_avg_update_time = 0.0;   // 上次刷新缓存平均值的时间戳

    // ---- 当前帧各阶段原始耗时（来源：检测器对象） ----
    float current_preprocess = 0.0f;
    float current_inference = 0.0f;
    float current_copy = 0.0f;
    float current_post = 0.0f;
    float current_nms = 0.0f;

    // ---- 从当前激活的检测器读取该帧各阶段耗时 ----
#ifdef USE_CUDA
    // CUDA / TensorRT 路径：从 trt_detector 中读取各阶段计时
    current_preprocess = static_cast<float>(trt_detector.lastPreprocessTime.count());
    current_inference = static_cast<float>(trt_detector.lastInferenceTime.count());
    current_copy = static_cast<float>(trt_detector.lastCopyTime.count());
    current_post = static_cast<float>(trt_detector.lastPostprocessTime.count());
    current_nms = static_cast<float>(trt_detector.lastNmsTime.count());
#else
    // WinRT / DirectML 路径：从 dml_detector 中读取各阶段计时
    if (dml_detector)
    {
        current_preprocess = static_cast<float>(dml_detector->lastPreprocessTimeDML.count());
        current_inference = static_cast<float>(dml_detector->lastInferenceTimeDML.count());
        current_copy = static_cast<float>(dml_detector->lastCopyTimeDML.count());
        current_post = static_cast<float>(dml_detector->lastPostprocessTimeDML.count());
        current_nms = static_cast<float>(dml_detector->lastNmsTimeDML.count());
    }
#endif

    // ---- 将当前帧各阶段耗时写入环形缓冲区 ----
    preprocess_times[index_inf] = current_preprocess;
    inference_times[index_inf] = current_inference;
    copy_times[index_inf] = current_copy;
    postprocess_times[index_inf] = current_post;
    nms_times[index_inf] = current_nms;
    index_inf = (index_inf + 1) % IM_ARRAYSIZE(inference_times);

    // ---- 记录当前采集帧率到环形缓冲区 ----
    float current_fps = static_cast<float>(captureFps.load());
    capture_fps_vals[index_fps] = current_fps;
    index_fps = (index_fps + 1) % IM_ARRAYSIZE(capture_fps_vals);

    // ---- 计算非零有效数据的平均值（跳过未被填充的位置） ----
    auto avg = [](const float* arr, int n) -> float {
        float sum = 0.0f; int cnt = 0;
        for (int i = 0; i < n; ++i)
            if (arr[i] > 0.0f) { sum += arr[i]; ++cnt; }
        return cnt ? (sum / cnt) : 0.0f;
        };

    const double now = ImGui::GetTime();
    // ---- 每秒刷新一次缓存平均值，避免每帧重复计算 ----
    if (last_avg_update_time == 0.0 || (now - last_avg_update_time) >= 1.0)
    {
        avg_preprocess_cached = avg(preprocess_times, IM_ARRAYSIZE(preprocess_times));
        avg_inference_cached = avg(inference_times, IM_ARRAYSIZE(inference_times));
        avg_copy_cached = avg(copy_times, IM_ARRAYSIZE(copy_times));
        avg_post_cached = avg(postprocess_times, IM_ARRAYSIZE(postprocess_times));
        avg_nms_cached = avg(nms_times, IM_ARRAYSIZE(nms_times));
        avg_fps_cached = avg(capture_fps_vals, IM_ARRAYSIZE(capture_fps_vals));

        last_avg_update_time = now;
    }

    // ---- 判断当前采集方式是否需要使用显示器刷新率 ----
    // duplication_api 始终锁定显示器；winrt 采集全屏时也使用显示器刷新率
    const bool captureUsesMonitorRefresh =
        config.capture_method == "duplication_api" ||
        (config.capture_method == "winrt" && config.capture_target != "window");

    // ---- 显示器刷新率缓存（最多每 2 秒查询一次，避免重复调用） ----
    static int cachedRefreshMonitorIdx = -1;        // 上次查询的显示器索引
    static double cachedRefreshQueryTime = -100.0;   // 上次查询的时间戳
    static double cachedMonitorRefreshHz = 0.0;      // 缓存的显示器刷新率（Hz）
    if (captureUsesMonitorRefresh)
    {
        const int monitorIdx = std::max(0, config.monitor_idx);
        // 显示器切换或上次查询超过 2 秒时刷新缓存
        if (cachedRefreshMonitorIdx != monitorIdx || now - cachedRefreshQueryTime >= 2.0)
        {
            cachedMonitorRefreshHz = GetMonitorRefreshRateByIndex(monitorIdx);
            cachedRefreshMonitorIdx = monitorIdx;
            cachedRefreshQueryTime = now;
        }
    }

    // ================================================================
    // 区块一：Time Breakdown — 各阶段耗时折线图
    // ================================================================
    if (OverlayUI::BeginSection("耗时分解", "stats_section_time_breakdown"))
    {
        // ---- 预处理耗时折线图（范围 0-20 ms） ----
        ImGui::PlotLines("预处理", preprocess_times, IM_ARRAYSIZE(preprocess_times), index_inf, nullptr, 0.0f, 20.0f, ImVec2(0, 40));
        ImGui::SameLine(); ImGui::Text("%.2f | Avg: %.2f", current_preprocess, avg_preprocess_cached);

        // ---- 推理耗时折线图（范围 0-20 ms，超过 20 ms 时平均值显示为红色警告） ----
        ImGui::PlotLines("推理", inference_times, IM_ARRAYSIZE(inference_times), index_inf, nullptr, 0.0f, 20.0f, ImVec2(0, 40));
        ImGui::SameLine();

        ImGui::Text("%.2f | Avg:", current_inference);
        ImGui::SameLine();

        const bool inf_slow = (avg_inference_cached > 20.0f);
        if (inf_slow)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.2f, 0.2f, 1.0f));

        ImGui::Text("%.2f", avg_inference_cached);

        if (inf_slow)
            ImGui::PopStyleColor();

        // ---- 拷贝耗时折线图（范围 0-10 ms） ----
        ImGui::PlotLines("拷贝", copy_times, IM_ARRAYSIZE(copy_times), index_inf, nullptr, 0.0f, 10.0f, ImVec2(0, 40));
        ImGui::SameLine(); ImGui::Text("%.2f | Avg: %.2f", current_copy, avg_copy_cached);

        // ---- 后处理耗时折线图（范围 0-10 ms） ----
        ImGui::PlotLines("后处理", postprocess_times, IM_ARRAYSIZE(postprocess_times), index_inf, nullptr, 0.0f, 10.0f, ImVec2(0, 40));
        ImGui::SameLine(); ImGui::Text("%.2f | Avg: %.2f", current_post, avg_post_cached);

        // ---- NMS 非极大值抑制耗时折线图（范围 0-5 ms） ----
        ImGui::PlotLines("非极大值抑制(NMS)", nms_times, IM_ARRAYSIZE(nms_times), index_inf, nullptr, 0.0f, 5.0f, ImVec2(0, 40));
        ImGui::SameLine(); ImGui::Text("%.2f | Avg: %.2f", current_nms, avg_nms_cached);

        OverlayUI::EndSection();
    }

    // ================================================================
    // 区块二：Capture FPS — 采集帧率折线图及显示器负载
    // ================================================================
    if (OverlayUI::BeginSection("帧率监控", "stats_section_capture_fps"))
    {
        // ---- 帧率折线图：纵轴上限跟随显示器刷新率（有缓存时）或默认 360 FPS ----
        const float fpsPlotMax = (captureUsesMonitorRefresh && cachedMonitorRefreshHz > 1.0)
            ? static_cast<float>(cachedMonitorRefreshHz)
            : 360.0f;
        ImGui::PlotLines("##fps_plot", capture_fps_vals, IM_ARRAYSIZE(capture_fps_vals), index_fps, nullptr, 0.0f, fpsPlotMax, ImVec2(0, 60));
        ImGui::SameLine();
        ImGui::Text("当前：%.1f | Avg: %.1f", current_fps, avg_fps_cached);
        // ---- 显示器负载指示条：当前平均帧率占显示器刷新率的百分比 ----
        if (captureUsesMonitorRefresh && cachedMonitorRefreshHz > 1.0)
        {
            const float refreshHz = static_cast<float>(cachedMonitorRefreshHz);
            const float fpsLoad = std::clamp(avg_fps_cached / refreshHz, 0.0f, 1.0f);
            ImGui::Spacing();
            ImGui::Text("显示器负载");
            ImGui::SameLine();
            ImGui::TextDisabled("%.1f / %.1f Hz (%.0f%%)", avg_fps_cached, refreshHz, fpsLoad * 100.0f);
            ImGui::ProgressBar(fpsLoad, ImVec2(-1.0f, 18.0f), "");
        }
        OverlayUI::EndSection();
    }

    // ---- 从最新帧中读取分辨率及帧队列深度 ----
    int latestWidth = 0;
    int latestHeight = 0;
    size_t queueDepth = 0;
    {
        std::lock_guard<std::mutex> lk(frameMutex);
        if (!latestFrame.empty())
        {
            latestWidth = latestFrame.cols;
            latestHeight = latestFrame.rows;
        }
        queueDepth = frameQueue.size();
    }

    // ---- 计算帧间隔等衍生指标 ----
    const int captureFpsLimit = std::max(0, config.capture_fps);
    const float currentFrameTimeMs = (current_fps > 0.01f) ? (1000.0f / current_fps) : 0.0f;
    const float avgFrameTimeMs = (avg_fps_cached > 0.01f) ? (1000.0f / avg_fps_cached) : 0.0f;
    const int sourceWidth = screenWidth.load(std::memory_order_relaxed);
    const int sourceHeight = screenHeight.load(std::memory_order_relaxed);
    const CaptureSourceDiagnostics sourceDiagnostics = GetCaptureSourceDiagnostics();
    const int inferenceFps = detectionBuffer.getPublishFps();

    // ---- 根据配置的采集方法构建采集源描述文本 ----
    std::string captureSource = "Unknown";
    std::string sourceSizeLabel = "桌面尺寸";
    if (config.capture_method == "duplication_api")
    {
        captureSource = "Monitor " + std::to_string(std::max(0, config.monitor_idx) + 1);
    }
    else if (config.capture_method == "winrt")
    {
        if (config.capture_target == "window")
        {
            captureSource = config.capture_window_title.empty()
                ? "Window target is empty"
                : "Window: " + config.capture_window_title;
            sourceSizeLabel = "窗口尺寸";
        }
        else
        {
            captureSource = "Monitor " + std::to_string(std::max(0, config.monitor_idx) + 1);
        }
    }
    else if (config.capture_method == "virtual_camera")
    {
        captureSource =
            "Camera: " + config.virtual_camera_name + " (" +
            std::to_string(config.virtual_camera_width) + "x" +
            std::to_string(config.virtual_camera_height) + ")";
        sourceSizeLabel = "摄像头尺寸";
    }
    else if (config.capture_method == "udp_capture")
    {
        captureSource = "UDP " + config.udp_ip + ":" + std::to_string(config.udp_port);
        sourceSizeLabel = "流尺寸";
    }
    else if (config.capture_method == "ndi")
    {
        captureSource = "NDI: " + config.ndi_source_name;
        sourceSizeLabel = "完整FOV";
    }

    // ---- 区块三：Capture Details — 采集详情与 GPU 统计 ----
    if (OverlayUI::BeginSection("采集详情", "stats_section_capture_details"))
    {
        // ---- 采集方法、后端引擎、数据源描述 ----
        ImGui::Text("采集方式：%s", config.capture_method.c_str());
        ImGui::Text("推理后端：%s", config.backend.c_str());
        ImGui::TextWrapped("采集源：%s", captureSource.c_str());

        // ---- 屏幕/窗口/摄像头原始分辨率 ----
        if (sourceWidth > 0 && sourceHeight > 0)
            ImGui::Text("%s: %dx%d", sourceSizeLabel.c_str(), sourceWidth, sourceHeight);
        else
            ImGui::TextDisabled("%s: N/A", sourceSizeLabel.c_str());

        // ---- 显示器刷新率（仅对全屏采集有效） ----
        if (captureUsesMonitorRefresh)
        {
            if (cachedMonitorRefreshHz > 0.0)
                ImGui::Text("显示器刷新率：%.2f Hz", cachedMonitorRefreshHz);
            else
                ImGui::TextDisabled("显示器刷新率：N/A");
        }

        // ---- 最近一帧的实际分辨率 ----
        if (latestWidth > 0 && latestHeight > 0)
            ImGui::Text("最新帧：%dx%d", latestWidth, latestHeight);
        else
            ImGui::TextDisabled("最新帧：N/A");

        // ---- 检测分辨率及采集帧率上限 ----
        ImGui::Text("检测分辨率：%d", config.detection_resolution);
        if (captureFpsLimit > 0)
            ImGui::Text("捕获帧率上限：%d", captureFpsLimit);
        else
            ImGui::Text("捕获帧率上限：不限");

        // 所有采集方式使用相同的四级判断：声明值、真实输入、捕获处理、检测发布。
        // 桌面复制和 WinRT 不提供可靠的声明帧率，因此明确显示 N/A，而不是拿显示器刷新率代替。
        if (sourceDiagnostics.declaredFps > 0.0)
            ImGui::Text("输入源声明FPS：%.3f", sourceDiagnostics.declaredFps);
        else
            ImGui::TextDisabled("输入源声明FPS：N/A");
        ImGui::Text("输入源实际到达FPS：%d", sourceDiagnostics.receiveFps);
        ImGui::Text("捕获处理FPS：%d | 检测发布FPS：%d", captureFps.load(), inferenceFps);

        if (sourceDiagnostics.encodedWidth > 0 && sourceDiagnostics.encodedHeight > 0)
            ImGui::Text("输入编码帧：%dx%d", sourceDiagnostics.encodedWidth, sourceDiagnostics.encodedHeight);
        else
            ImGui::TextDisabled("输入编码帧：N/A");
        ImGui::Text("累计输入：%llu | 源帧淘汰：%llu",
            static_cast<unsigned long long>(sourceDiagnostics.receivedFrames),
            static_cast<unsigned long long>(sourceDiagnostics.droppedFrames));

        // ---- 当前帧和平均帧间隔时间 ----
        if (currentFrameTimeMs > 0.0f || avgFrameTimeMs > 0.0f)
            ImGui::Text("帧间隔：当前 %.2f 毫秒 | 平均 %.2f 毫秒", currentFrameTimeMs, avgFrameTimeMs);
        else
            ImGui::TextDisabled("帧间隔：N/A");

        // ---- 帧队列深度与圆形 FOV 开关 ----
        ImGui::Text("帧队列深度：%d", static_cast<int>(queueDepth));
        ImGui::Text("圆形视野：%s", config.circle_fov_enabled ? "开" : "关");

        // ============================================================
        // WinRT 采集统计追踪
        // 记录 WinRT 采集引擎的轮询、拉取、空轮询及内存拷贝耗时。
        // 每秒更新一次速率和平均值。
        // ============================================================
        static bool winrtStatsInitialized = false;
        static uint64_t lastWinrtPolls = 0;             // 上次的轮询总次数
        static uint64_t lastWinrtDrained = 0;            // 上次的拉取帧总数
        static uint64_t lastWinrtReturned = 0;           // 上次的成功返回帧数
        static uint64_t lastWinrtEmpty = 0;              // 上次的空轮询次数
        static uint64_t lastWinrtReadbackMicros = 0;     // 上次的 readback 累积微秒
        static uint64_t lastWinrtMapMicros = 0;          // 上次的 Map 累积微秒
        static uint64_t lastWinrtPixelCopyMicros = 0;    // 上次的像素拷贝累积微秒
        static double lastWinrtStatsTime = 0.0;          // 上次更新统计的时间戳
        static float winrtPollRate = 0.0f;               // 轮询速率（次/秒）
        static float winrtDrainedRate = 0.0f;            // 拉取速率（帧/秒）
        static float winrtReturnedRate = 0.0f;           // 成功返回速率（帧/秒）
        static float winrtEmptyRate = 0.0f;              // 空轮询速率（次/秒）
        static float winrtReadbackAvgMs = 0.0f;          // readback 平均耗时（毫秒）
        static float winrtMapAvgMs = 0.0f;               // Map 平均耗时（毫秒）
        static float winrtPixelCopyAvgMs = 0.0f;         // 像素拷贝平均耗时（毫秒）

        if (config.capture_method == "winrt")
        {
            // ---- 读取 WinRT 原子计数器的当前值 ----
            const uint64_t winrtPolls = captureWinrtPollAttemptsTotal.load(std::memory_order_relaxed);
            const uint64_t winrtDrained = captureWinrtFramesDrainedTotal.load(std::memory_order_relaxed);
            const uint64_t winrtReturned = captureWinrtFramesReturnedTotal.load(std::memory_order_relaxed);
            const uint64_t winrtEmpty = captureWinrtEmptyPollsTotal.load(std::memory_order_relaxed);
            const uint64_t winrtReadbackMicros = captureWinrtReadbackMicrosTotal.load(std::memory_order_relaxed);
            const uint64_t winrtMapMicros = captureWinrtMapMicrosTotal.load(std::memory_order_relaxed);
            const uint64_t winrtPixelCopyMicros = captureWinrtPixelCopyMicrosTotal.load(std::memory_order_relaxed);

            if (!winrtStatsInitialized)
            {
                // ---- 首次初始化：记录基准值 ----
                lastWinrtPolls = winrtPolls;
                lastWinrtDrained = winrtDrained;
                lastWinrtReturned = winrtReturned;
                lastWinrtEmpty = winrtEmpty;
                lastWinrtReadbackMicros = winrtReadbackMicros;
                lastWinrtMapMicros = winrtMapMicros;
                lastWinrtPixelCopyMicros = winrtPixelCopyMicros;
                lastWinrtStatsTime = now;
                winrtStatsInitialized = true;
            }
            else if (now - lastWinrtStatsTime >= 1.0)
            {
                // ---- 每秒更新：计算各项速率和平均耗时 ----
                const float dt = static_cast<float>(std::max(0.001, now - lastWinrtStatsTime));
                const uint64_t returnedDelta = winrtReturned - lastWinrtReturned;
                winrtPollRate = static_cast<float>(winrtPolls - lastWinrtPolls) / dt;
                winrtDrainedRate = static_cast<float>(winrtDrained - lastWinrtDrained) / dt;
                winrtReturnedRate = static_cast<float>(returnedDelta) / dt;
                winrtEmptyRate = static_cast<float>(winrtEmpty - lastWinrtEmpty) / dt;
                if (returnedDelta > 0)
                {
                    // ---- 仅在有成功返回帧时才计算平均耗时 ----
                    winrtReadbackAvgMs = static_cast<float>(winrtReadbackMicros - lastWinrtReadbackMicros) /
                        (1000.0f * static_cast<float>(returnedDelta));
                    winrtMapAvgMs = static_cast<float>(winrtMapMicros - lastWinrtMapMicros) /
                        (1000.0f * static_cast<float>(returnedDelta));
                    winrtPixelCopyAvgMs = static_cast<float>(winrtPixelCopyMicros - lastWinrtPixelCopyMicros) /
                        (1000.0f * static_cast<float>(returnedDelta));
                }

                // ---- 保存当前值作为下一次的基准 ----
                lastWinrtPolls = winrtPolls;
                lastWinrtDrained = winrtDrained;
                lastWinrtReturned = winrtReturned;
                lastWinrtEmpty = winrtEmpty;
                lastWinrtReadbackMicros = winrtReadbackMicros;
                lastWinrtMapMicros = winrtMapMicros;
                lastWinrtPixelCopyMicros = winrtPixelCopyMicros;
                lastWinrtStatsTime = now;
            }

            // ---- 渲染 WinRT 统计信息到覆盖层 ----
            ImGui::Text("WinRT 帧率：%.1f/秒 | 拉取：%.1f/秒", winrtReturnedRate, winrtDrainedRate);
            ImGui::Text("WinRT 空轮询：%.1f/秒 | 轮询：%.1f/秒", winrtEmptyRate, winrtPollRate);
            ImGui::Text("WinRT 回读平均：%.3f 毫秒 | 映射：%.3f 毫秒", winrtReadbackAvgMs, winrtMapAvgMs);
            ImGui::Text("WinRT 内存拷贝平均：%.3f 毫秒", winrtPixelCopyAvgMs);
        }
        else
        {
            // ---- 非 WinRT 模式：重置初始化的标记，下次进入时重新初始化 ----
            winrtStatsInitialized = false;
        }

        // ============================================================
        // CUDA / TensorRT GPU 直接采集统计跟踪
        // 仅在使用 TensorRT 后端且启用了 CUDA 直接采集时激活。
        // 跟踪 DDA (Desktop Duplication API) 的各种事件速率。
        // ============================================================
#ifdef USE_CUDA
        if (config.backend == "TRT")
        {
            // ---- 判断深度掩码、CUDA 采集是否可行及当前采集管线路径 ----
            const bool depthMaskEnabled = config.depth_inference_enabled && config.depth_mask_enabled;
            const bool canUseCudaCapture = (config.capture_method == "duplication_api");
            const bool directCaptureActive =
                canUseCudaCapture &&
                config.capture_use_cuda &&
                !depthMaskEnabled;

            // ---- 直接采集状态判断：
            //      仅 duplication_api + CUDA 开启 + 无深度掩码时走 GPU 直通路径 ----
            std::string directCaptureStatus;
            if (!canUseCudaCapture)
                directCaptureStatus = "N/A（需要 duplication_api）";
            else if (!config.capture_use_cuda)
                directCaptureStatus = "用户已禁用";
            else if (depthMaskEnabled)
                directCaptureStatus = "CPU回退（深度遮罩已启用）";
            else
                directCaptureStatus = "活跃";

            ImGui::Separator();
            ImGui::Text("CUDA 直接捕获：%s", config.capture_use_cuda ? "已启用" : "已禁用");
            ImGui::Text("深度遮罩：%s", depthMaskEnabled ? "开" : "关");
            ImGui::Text("捕获管线：%s", directCaptureActive ? "GPU直接路径" : "CPU回读");

            // ---- 使用颜色指示直接采集状态：绿色 = GPU 直通，橙色 = CPU readback ----
            if (directCaptureActive)
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 1.0f, 0.45f, 1.0f));
            else
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.78f, 0.28f, 1.0f));
            ImGui::TextWrapped("直接捕获状态：%s", directCaptureStatus.c_str());
            ImGui::PopStyleColor();

            // ---- CUDA GPU 统计：记录各项 DDA 事件的累计值（原子计数器） ----
            // 用于诊断 GPU 采集中的丢帧、超时、鼠标/元数据事件等问题。
            static uint64_t lastGpuAttempts = 0;            // 上轮 GPU 采集尝试次数
            static uint64_t lastGpuCaptured = 0;            // 上轮成功采集帧数
            static uint64_t lastGpuTimeouts = 0;            // 上轮超时事件数
            static uint64_t lastGpuAccumulated = 0;         // 上轮累积帧数
            static uint64_t lastGpuMissed = 0;              // 上轮丢帧数
            static uint64_t lastGpuPresent = 0;             // 上轮 Present 事件数
            static uint64_t lastGpuMouseOnly = 0;           // 上轮纯鼠标事件数
            static uint64_t lastGpuMetadataOnly = 0;        // 上轮纯元数据事件数
            static uint64_t lastGpuCoalesced = 0;           // 上轮合并事件数
            static uint64_t lastCpuFallbackAttempts = 0;    // 上轮 CPU 回退尝试次数
            static uint64_t lastCpuFallbackFrames = 0;      // 上轮 CPU 回退实际帧数
            static double lastGpuStatsTime = 0.0;           // 上次 GPU 统计更新时间
            static float gpuAttemptRate = 0.0f;             // 采集尝试速率（次/秒）
            static float gpuCapturedRate = 0.0f;            // 采集成功速率（帧/秒）
            static float gpuTimeoutRate = 0.0f;             // 超时速率（次/秒）
            static float gpuAccumulatedRate = 0.0f;         // 累积帧速率（帧/秒）
            static float gpuMissedRate = 0.0f;              // 丢帧速率（帧/秒）
            static float gpuPresentRate = 0.0f;             // Present 速率（次/秒）
            static float gpuMouseOnlyRate = 0.0f;           // 纯鼠标事件速率（次/秒）
            static float gpuMetadataOnlyRate = 0.0f;        // 纯元数据事件速率（次/秒）
            static float gpuCoalescedRate = 0.0f;           // 合并事件速率（次/秒）
            static float cpuFallbackAttemptRate = 0.0f;     // CPU 回退尝试速率（次/秒）
            static float cpuFallbackFrameRate = 0.0f;       // CPU 回退帧速率（帧/秒）

            // ---- 读取 CUDA GPU 原子计数器的当前累计值 ----
            const uint64_t gpuAttempts = captureGpuAttemptsTotal.load(std::memory_order_relaxed);
            const uint64_t gpuCaptured = captureGpuCapturedTotal.load(std::memory_order_relaxed);
            const uint64_t gpuTimeouts = captureGpuTimeoutTotal.load(std::memory_order_relaxed);
            const uint64_t gpuAccumulated = captureGpuAccumulatedFramesTotal.load(std::memory_order_relaxed);
            const uint64_t gpuMissed = captureGpuMissedFramesTotal.load(std::memory_order_relaxed);
            const uint64_t gpuPresent = captureGpuPresentFramesTotal.load(std::memory_order_relaxed);
            const uint64_t gpuMouseOnly = captureGpuMouseOnlyEventsTotal.load(std::memory_order_relaxed);
            const uint64_t gpuMetadataOnly = captureGpuMetadataOnlyEventsTotal.load(std::memory_order_relaxed);
            const uint64_t gpuCoalesced = captureGpuCoalescedEventsTotal.load(std::memory_order_relaxed);
            const uint64_t cpuFallbackAttempts = captureCpuFallbackAttemptsTotal.load(std::memory_order_relaxed);
            const uint64_t cpuFallbackFrames = captureCpuFallbackFramesTotal.load(std::memory_order_relaxed);

            if (lastGpuStatsTime <= 0.0)
            {
                // ---- 首次初始化：记录基准值 ----
                lastGpuAttempts = gpuAttempts;
                lastGpuCaptured = gpuCaptured;
                lastGpuTimeouts = gpuTimeouts;
                lastGpuAccumulated = gpuAccumulated;
                lastGpuMissed = gpuMissed;
                lastGpuPresent = gpuPresent;
                lastGpuMouseOnly = gpuMouseOnly;
                lastGpuMetadataOnly = gpuMetadataOnly;
                lastGpuCoalesced = gpuCoalesced;
                lastCpuFallbackAttempts = cpuFallbackAttempts;
                lastCpuFallbackFrames = cpuFallbackFrames;
                lastGpuStatsTime = now;
            }
            else if (now - lastGpuStatsTime >= 1.0)
            {
                // ---- 每秒更新：计算各项 GPU 事件速率 ----
                const float dt = static_cast<float>(std::max(0.001, now - lastGpuStatsTime));
                gpuAttemptRate = static_cast<float>(gpuAttempts - lastGpuAttempts) / dt;
                gpuCapturedRate = static_cast<float>(gpuCaptured - lastGpuCaptured) / dt;
                gpuTimeoutRate = static_cast<float>(gpuTimeouts - lastGpuTimeouts) / dt;
                gpuAccumulatedRate = static_cast<float>(gpuAccumulated - lastGpuAccumulated) / dt;
                gpuMissedRate = static_cast<float>(gpuMissed - lastGpuMissed) / dt;
                gpuPresentRate = static_cast<float>(gpuPresent - lastGpuPresent) / dt;
                gpuMouseOnlyRate = static_cast<float>(gpuMouseOnly - lastGpuMouseOnly) / dt;
                gpuMetadataOnlyRate = static_cast<float>(gpuMetadataOnly - lastGpuMetadataOnly) / dt;
                gpuCoalescedRate = static_cast<float>(gpuCoalesced - lastGpuCoalesced) / dt;
                cpuFallbackAttemptRate = static_cast<float>(cpuFallbackAttempts - lastCpuFallbackAttempts) / dt;
                cpuFallbackFrameRate = static_cast<float>(cpuFallbackFrames - lastCpuFallbackFrames) / dt;

                // ---- 保存当前值作为下一次的基准 ----
                lastGpuAttempts = gpuAttempts;
                lastGpuCaptured = gpuCaptured;
                lastGpuTimeouts = gpuTimeouts;
                lastGpuAccumulated = gpuAccumulated;
                lastGpuMissed = gpuMissed;
                lastGpuPresent = gpuPresent;
                lastGpuMouseOnly = gpuMouseOnly;
                lastGpuMetadataOnly = gpuMetadataOnly;
                lastGpuCoalesced = gpuCoalesced;
                lastCpuFallbackAttempts = cpuFallbackAttempts;
                lastCpuFallbackFrames = cpuFallbackFrames;
                lastGpuStatsTime = now;
            }

            // ---- 渲染 CUDA DDA GPU 统计信息到覆盖层 ----
            ImGui::Text("DDA 提交帧率：%.1f/秒 | 尝试：%.1f/秒", gpuCapturedRate, gpuAttemptRate);
            ImGui::Text("DDA 呈现帧率：%.1f/秒 | 仅鼠标：%.1f/秒", gpuPresentRate, gpuMouseOnlyRate);
            ImGui::Text("DDA 仅元数据：%.1f/秒 | 合并：%.1f/秒", gpuMetadataOnlyRate, gpuCoalescedRate);
            ImGui::Text("DDA GPU超时：%.1f/秒 | 累积：%.1f/秒", gpuTimeoutRate, gpuAccumulatedRate);
            ImGui::Text("DDA GPU丢失/合并：%.1f/秒", gpuMissedRate);
            ImGui::Text("DDA CPU回退：%.1f/秒 | 尝试：%.1f/秒", cpuFallbackFrameRate, cpuFallbackAttemptRate);
        }
#endif

        OverlayUI::EndSection();
    }
}
