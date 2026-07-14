#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "capture.h"
#include "capture/circle_fov.h"
#include "Game_overlay.h"
#include "mouse.h"
#include "other_tools.h"
#include "runtime/thread_loops.h"
#include "Xen.h"

#ifdef USE_CUDA
#include "depth/depth_anything_trt.h"
#include "depth/depth_mask.h"
#include "tensorrt/nvinf.h"
#endif

extern std::string g_iconLastError;

namespace
{
// 上一次成功加载的图标路径，用于缓存避免重复加载
std::string g_lastIconPath;
// 当前加载到覆盖层中的图标图像ID，0 表示未加载
int g_iconImageId = 0;
// 图标加载的互斥锁，保护图标路径和ID的线程安全访问
std::mutex g_iconMutex;

/**
 * 游戏覆盖层所在显示器的边界信息结构体
 */
struct GameOverlayMonitorBounds
{
    RECT rect{};        // 显示器矩形区域（屏幕坐标）
    int width = 1;      // 显示器宽度（像素）
    int height = 1;     // 显示器高度（像素）
};

/**
 * 判断两个显示器矩形区域是否相同
 * @param a 矩形A
 * @param b 矩形B
 * @return true 表示两个矩形完全相同
 */
bool sameGameOverlayMonitorRect(const RECT& a, const RECT& b)
{
    return a.left == b.left &&
        a.top == b.top &&
        a.right == b.right &&
        a.bottom == b.bottom;
}

/**
 * 根据显示器索引解析覆盖层绑定的显示器边界信息
 * 若指定索引无效则回退到主显示器（原点所在显示器）
 * @param overlayMonitorIndex 目标显示器索引
 * @return 显示器边界信息结构体
 */
GameOverlayMonitorBounds resolveGameOverlayMonitorBounds(int overlayMonitorIndex)
{
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);

    HMONITOR hTargetMonitor = GetMonitorHandleByIndex(overlayMonitorIndex);
    if (!hTargetMonitor)
        hTargetMonitor = MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);

    GameOverlayMonitorBounds bounds{};
    if (hTargetMonitor && GetMonitorInfo(hTargetMonitor, &mi))
    {
        bounds.rect = mi.rcMonitor;
    }
    else
    {
        bounds.rect.left = 0;
        bounds.rect.top = 0;
        bounds.rect.right = GetSystemMetrics(SM_CXSCREEN);
        bounds.rect.bottom = GetSystemMetrics(SM_CYSCREEN);
    }

    bounds.width = std::max(1, static_cast<int>(bounds.rect.right - bounds.rect.left));
    bounds.height = std::max(1, static_cast<int>(bounds.rect.bottom - bounds.rect.top));
    return bounds;
}

/**
 * 重置游戏覆盖层的图标缓存
 * 清空已加载的图标路径、图像ID和错误信息
 */
void resetGameOverlayIconCache()
{
    std::lock_guard<std::mutex> lk(g_iconMutex);
    g_lastIconPath.clear();
    g_iconImageId = 0;
    g_iconLastError.clear();
}

/**
 * 计算两个矩形的交并比（IoU），用于判断检测框与跟踪框的匹配程度
 * @param a 矩形A
 * @param b 矩形B
 * @return IoU 值（0.0 ~ 1.0），0 表示无重叠
 */
float rectIou(const cv::Rect& a, const cv::Rect& b)
{
    const cv::Rect intersection = a & b;
    if (intersection.width <= 0 || intersection.height <= 0)
        return 0.0f;

    const float intersectionArea = static_cast<float>(intersection.area());
    const float unionArea = static_cast<float>(a.area() + b.area()) - intersectionArea;
    if (unionArea <= 1e-6f)
        return 0.0f;

    return intersectionArea / unionArea;
}

/**
 * 判断某个检测框是否已被现有的跟踪轨迹所表示
 * 通过比较同类别检测框与跟踪框的IoU是否超过阈值来实现
 * 用于去除已经被跟踪器覆盖的冗余检测框，避免重复绘制
 *
 * @param box 检测框
 * @param classId 检测类别ID
 * @param tracks 当前跟踪轨迹列表
 * @return true 表示该检测已被跟踪轨迹覆盖
 */
bool detectionRepresentedByTrack(
    const cv::Rect& box,
    int classId,
    const std::vector<TrackDebugInfo>& tracks)
{
    // 同类框IoU判定阈值：超过此值视为已被跟踪器表示
    constexpr float kSameClassIouThreshold = 0.35f;

    for (const auto& t : tracks)
    {
        if (t.classId != classId)
            continue;

        if (rectIou(box, t.box) >= kSameClassIouThreshold)
            return true;
    }

    return false;
}
}
/**
 * 基础控制演示覆盖层——只显示自动推导的稳定/释放半径。
 *
 * @param overlay 游戏覆盖层指针
 * @param centerX 演示中心X坐标（屏幕像素）
 * @param centerY 演示中心Y坐标（屏幕像素）
 */
static void draw_target_correction_demo_game_overlay(Game_overlay* overlay, float centerX, float centerY)
{
    if (!overlay)
        return;

    const float scale = 4.0f;
    const float settle = std::max(2.0f, config.detection_resolution / 64.0f) * scale;
    const float release = settle * 1.6f;
    overlay->AddCircle({ centerX, centerY, release }, ARGB(180, 80, 120, 255), 2.0f);
    overlay->AddCircle({ centerX, centerY, settle }, ARGB(180, 255, 100, 100), 2.0f);
}

/**
 * 游戏覆盖层渲染主循环
 * 在独立线程中持续运行，负责：
 * - 监视配置变化和显示器边界变化
 * - 管理覆盖层窗口的生命周期（创建/重启/销毁）
 * - 从检测缓冲区获取检测框并进行延迟补偿投影
 * - 加载和缓存覆盖层图标
 * - 绘制深度推理可视化（仅 CUDA 版本）
 * - 绘制捕获帧边框、圆形FOV、检测框/跟踪框
 * - 绘制预测未来轨迹点、风偏调试轨迹
 * - 在每个目标上叠加图标
 * - 可选的目标校正速度演示
 */
void gameOverlayRenderLoop()
{
#ifdef USE_CUDA
    // ---- 深度调试模型的静态变量 ----
    static depth_anything::DepthAnythingTrt depthDebugModel;  // 深度估计推理引擎
    static std::string depthDebugModelPath;                    // 已加载的深度模型路径
    static int depthDebugColormap = -1;                        // 当前使用的深度着色方案
    static int depthDebugImageId = 0;                          // 深度图像在覆盖层中的ID
    static int depthMaskImageId = 0;                           // 深度遮罩在覆盖层中的ID
    static cv::Mat depthDebugFrame;                            // 最新深度估计结果帧
    static auto lastDepthUpdate = std::chrono::steady_clock::time_point::min(); // 上次深度更新的时间
    static bool lastDepthInferenceEnabled = true;              // 上一次的深度推理启用状态
#endif
    int lastDetectionVersion = -1;              // 上一次处理的检测版本号
    int lastOverlayMonitorIndex = 0;            // 上一次的覆盖层显示器索引
    RECT lastOverlayMonitorRect{};              // 上一次的覆盖层显示器矩形
    bool lastOverlayMonitorStateValid = false;  // 上一次的显示器状态是否有效

    while (!gameOverlayShouldExit.load())
    {
        // ========== 1. 配置更新与覆盖层生命周期管理 ==========

        // 覆盖层未启用：销毁现有覆盖层并重置所有状态
        if (!config.game_overlay_enabled)
        {
            lastOverlayMonitorStateValid = false;
            if (gameOverlayPtr)
            {
                gameOverlayPtr->Stop();
                gameOverlayPtr.reset();
                resetGameOverlayIconCache();
            }
#ifdef USE_CUDA
            depthDebugModel.reset();
            depthDebugModelPath.clear();
            depthDebugColormap = -1;
            depthDebugImageId = 0;
            depthMaskImageId = 0;
            depthDebugFrame.release();
            lastDepthUpdate = std::chrono::steady_clock::time_point::min();
#endif
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            continue;
        }

        // ---- 检测覆盖层绑定显示器是否发生变化 ----
        int overlayMonitorIndex = 0;
        {
            std::lock_guard<std::mutex> cfgLock(configMutex);
            overlayMonitorIndex = config.monitor_idx;
        }

        const GameOverlayMonitorBounds overlayBounds = resolveGameOverlayMonitorBounds(overlayMonitorIndex);
        RECT pr = overlayBounds.rect;
        const int pw = overlayBounds.width;
        const int ph = overlayBounds.height;
        const bool overlayMonitorChanged = lastOverlayMonitorStateValid &&
            (overlayMonitorIndex != lastOverlayMonitorIndex ||
                !sameGameOverlayMonitorRect(lastOverlayMonitorRect, pr));

        // 显示器变化且覆盖层存在：销毁旧覆盖层准备重新创建
        if (overlayMonitorChanged && gameOverlayPtr)
        {
            gameOverlayPtr->Stop();
            gameOverlayPtr.reset();
            resetGameOverlayIconCache();
        }

        lastOverlayMonitorIndex = overlayMonitorIndex;
        lastOverlayMonitorRect = pr;
        lastOverlayMonitorStateValid = true;

        // ---- 创建或重启覆盖层窗口 ----
        const unsigned overlayMaxFps = config.game_overlay_max_fps > 0 ? (unsigned)config.game_overlay_max_fps : 0;
        if (!gameOverlayPtr)
        {
            gameOverlayPtr = std::make_unique<Game_overlay>();
            gameOverlayPtr->SetWindowBounds(pr.left, pr.top, pw, ph);
            gameOverlayPtr->SetMaxFPS(overlayMaxFps);
            gameOverlayPtr->SetExcludeFromCapture(config.overlay_exclude_from_capture);
            gameOverlayPtr->Start();
        }
        else if (!gameOverlayPtr->IsRunning())
        {
            gameOverlayPtr->SetWindowBounds(pr.left, pr.top, pw, ph);
            gameOverlayPtr->SetMaxFPS(overlayMaxFps);
            gameOverlayPtr->SetExcludeFromCapture(config.overlay_exclude_from_capture);
            gameOverlayPtr->Start();
        }

        // 覆盖层启动失败：等待后重试
        if (!gameOverlayPtr || !gameOverlayPtr->IsRunning())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            continue;
        }

        // ---- 更新覆盖层的 FPS 和排除捕获设置 ----
        gameOverlayPtr->SetMaxFPS(overlayMaxFps);
        gameOverlayPtr->SetExcludeFromCapture(config.overlay_exclude_from_capture);

        // ---- 计算检测区域在覆盖层中的位置和缩放 ----
        const int detRes = config.detection_resolution;
        if (detRes <= 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        int regionW = detRes;
        int regionH = detRes;

        if (regionW > pw) regionW = pw;
        if (regionH > ph) regionH = ph;

        // 检测区域居中于覆盖层窗口
        const int baseX = (pw - regionW) / 2;
        const int baseY = (ph - regionH) / 2;

        // 检测分辨率到实际像素的缩放比例
        const float scaleX = detRes > 0 ? (static_cast<float>(regionW) / static_cast<float>(detRes)) : 1.0f;
        const float scaleY = detRes > 0 ? (static_cast<float>(regionH) / static_cast<float>(detRes)) : 1.0f;

        // ========== 2. 从检测缓冲区读取数据 ==========

        std::vector<cv::Rect> boxesCopy;
        std::vector<int> classesCopy;
        std::chrono::steady_clock::time_point detectionTimestamp{};
        int detectionVersion = lastDetectionVersion;
        {
            const unsigned fpsCap = (unsigned)config.game_overlay_max_fps;
            const int waitMs = (fpsCap > 0) ? static_cast<int>(std::max(1u, 1000u / fpsCap)) : 8;
            std::unique_lock<std::mutex> lk(detectionBuffer.mutex);
            detectionBuffer.cv.wait_for(lk, std::chrono::milliseconds(waitMs), [&] {
                return detectionBuffer.version != lastDetectionVersion || gameOverlayShouldExit.load();
            });
            std::vector<float> dummyConf;
            detectionBuffer.swapLocked(boxesCopy, classesCopy, dummyConf, detectionVersion, detectionTimestamp);
        }
        lastDetectionVersion = detectionVersion;

        // ---- 获取风偏调试数据 ----
        std::vector<std::pair<double, double>> windTailPts;
        if (config.game_overlay_draw_wind_tail && globalMouseThread)
            windTailPts = globalMouseThread->getWindDebugTrail();

        // ---- 获取跟踪器调试信息 ----
        std::vector<TrackDebugInfo> trackDebugCopy;
        int lockedTrackId = -1;
        {
            std::lock_guard<std::mutex> lk(g_trackerDebugMutex);
            trackDebugCopy = g_trackerDebugTracks;
            lockedTrackId = g_trackerLockedId;
        }

        // ========== 3. 检测框延迟补偿投影 Lambda ==========

        const auto overlayNow = std::chrono::steady_clock::now();

        /**
         * 对检测框进行延迟补偿投影——考虑检测到渲染之间的时间差，
         * 结合目标运动速度和相机运动补偿，将检测框修正到当前时间的位置
         *
         * @param b 原始检测框
         * @param velocityX 目标X方向速度（来自跟踪器）
         * @param velocityY 目标Y方向速度（来自跟踪器）
         * @param lastUpdate 跟踪器上次更新时间戳
         * @return 投影后的覆盖层矩形，若投影后无效则返回 nullopt
         */
        auto projectDetectionBox = [&](
            const cv::Rect& b,
            double velocityX = 0.0,
            double velocityY = 0.0,
            std::chrono::steady_clock::time_point lastUpdate = {}) -> std::optional<OverlayRect>
        {
            if (b.width <= 0 || b.height <= 0)
                return std::nullopt;

            // 若提供了跟踪器更新时间则使用它作为补偿基准，否则使用检测时间戳
            const auto compensationTimestamp =
                (lastUpdate.time_since_epoch().count() != 0) ? lastUpdate : detectionTimestamp;

            // 计算从补偿基准到当前时间的经过时间（秒）
            double ageSec = 0.0;
            if (compensationTimestamp.time_since_epoch().count() != 0)
            {
                ageSec = std::chrono::duration<double>(overlayNow - compensationTimestamp).count();
                if (!std::isfinite(ageSec) || ageSec < 0.0)
                    ageSec = 0.0;
                ageSec = std::clamp(ageSec, 0.0, 0.35);
            }

            // 获取相机运动补偿（自补偿基准时间以来的鼠标/视角移动量）
            std::pair<double, double> cameraCompensation{ 0.0, 0.0 };
            if (config.game_overlay_compensate_latency && globalMouseThread &&
                compensationTimestamp.time_since_epoch().count() != 0)
            {
                cameraCompensation = globalMouseThread->getMotionCompensationSince(compensationTimestamp);
            }

            // 应用运动补偿：目标速度 × 经过时间 - 相机运动补偿
            double left = static_cast<double>(b.x) + velocityX * ageSec - cameraCompensation.first;
            double top = static_cast<double>(b.y) + velocityY * ageSec - cameraCompensation.second;
            double right = left + static_cast<double>(b.width);
            double bottom = top + static_cast<double>(b.height);

            // 钳位到检测分辨率范围内
            left = std::clamp(left, 0.0, static_cast<double>(detRes));
            top = std::clamp(top, 0.0, static_cast<double>(detRes));
            right = std::clamp(right, 0.0, static_cast<double>(detRes));
            bottom = std::clamp(bottom, 0.0, static_cast<double>(detRes));

            const double w = right - left;
            const double h = bottom - top;
            if (w <= 0.0 || h <= 0.0)
                return std::nullopt;

            // 将检测坐标转换为覆盖层屏幕坐标
            OverlayRect rect{
                static_cast<float>(baseX + left * scaleX),
                static_cast<float>(baseY + top * scaleY),
                static_cast<float>(w * scaleX),
                static_cast<float>(h * scaleY)
            };

            // 检查投影后矩形是否在覆盖层可见区域内
            if (rect.x + rect.w < baseX || rect.y + rect.h < baseY ||
                rect.x > baseX + regionW || rect.y > baseY + regionH)
                return std::nullopt;

            return rect;
        };

        // ========== 4. 图标加载（带路径变化检测和错误处理） ==========

        if (config.game_overlay_icon_enabled)
        {
            std::lock_guard<std::mutex> lk(g_iconMutex);
            if (config.game_overlay_icon_path != g_lastIconPath)
            {
                // 路径变化：卸载旧图标
                if (g_iconImageId != 0)
                {
                    gameOverlayPtr->UnloadImage(g_iconImageId);
                    g_iconImageId = 0;
                }
                g_lastIconPath = config.game_overlay_icon_path;
                std::error_code fsErr;
                std::filesystem::path p;
                try
                {
                    p = std::filesystem::u8path(g_lastIconPath);
                }
                catch (const std::exception&)
                {
                    p = std::filesystem::path(g_lastIconPath);
                }
                const bool hasFile = !g_lastIconPath.empty() && p.has_filename() && std::filesystem::is_regular_file(p, fsErr);
                if (fsErr)
                {
                    g_iconImageId = 0;
                    g_iconLastError = "[GameOverlay] Failed to read icon path: " + g_lastIconPath + " (" + fsErr.message() + ")";
                    std::cerr << g_iconLastError << std::endl;
                }
                else if (hasFile)
                {
                    const std::wstring wpath = p.wstring();
                    g_iconLastError.clear();

                    UINT iw = 0, ih = 0;
                    std::string verr;
                    if (!IsValidImageFile(wpath, iw, ih, verr))
                    {
                        g_iconImageId = 0;
                        g_iconLastError = "[GameOverlay] Invalid image '" + g_lastIconPath + "': " + verr;
                        std::cerr << g_iconLastError << std::endl;
                    }
                    else
                    {
                        try
                        {
                            int id = gameOverlayPtr->LoadImageFromFile(wpath);
                            if (id != 0)
                            {
                                g_iconImageId = id;
                                std::cout << "[GameOverlay] Loaded icon (" << iw << "x" << ih << "): " << g_lastIconPath << std::endl;
                            }
                            else
                            {
                                g_iconImageId = 0;
                                g_iconLastError = "[GameOverlay] Failed to load icon (loader returned 0): " + g_lastIconPath;
                                std::cerr << g_iconLastError << std::endl;
                            }
                        }
                        catch (const std::exception& e)
                        {
                            g_iconImageId = 0;
                            g_iconLastError = std::string("[GameOverlay] Exception while loading icon: ") + e.what();
                            std::cerr << g_iconLastError << std::endl;
                        }
                        catch (...)
                        {
                            g_iconImageId = 0;
                            g_iconLastError = "[GameOverlay] Unknown exception while loading icon.";
                            std::cerr << g_iconLastError << std::endl;
                        }
                    }
                }
                else
                {
                    g_iconImageId = 0;
                    g_iconLastError = "[GameOverlay] Icon file not found: " + g_lastIconPath;
                    std::cerr << g_iconLastError << std::endl;
                }
            }
        }

        // ========== 5. 开始渲染帧 ==========

        gameOverlayPtr->BeginFrame();

        // ========== 6. 深度推理可视化（仅 CUDA） ==========

#ifdef USE_CUDA
        if (!config.depth_inference_enabled)
        {
            // 深度推理禁用：清理所有深度相关资源
            if (lastDepthInferenceEnabled)
            {
                if (gameOverlayPtr)
                {
                    if (depthDebugImageId != 0)
                    {
                        gameOverlayPtr->UnloadImage(depthDebugImageId);
                        depthDebugImageId = 0;
                    }
                    if (depthMaskImageId != 0)
                    {
                        gameOverlayPtr->UnloadImage(depthMaskImageId);
                        depthMaskImageId = 0;
                    }
                }

                depthDebugModel.reset();
                depthDebugModelPath.clear();
                depthDebugColormap = -1;
                depthDebugFrame.release();
                lastDepthUpdate = std::chrono::steady_clock::time_point::min();

                auto& depthMask = depth_anything::GetDepthMaskGenerator();
                depthMask.reset();
            }
            lastDepthInferenceEnabled = false;
        }
        else
        {
            lastDepthInferenceEnabled = true;

            // ---- 深度调试叠加层：实时深度估计结果 ----
            float depthW = 0.0f;
            float depthH = 0.0f;
            float maskW = 0.0f;
            float maskH = 0.0f;
            float maskOpacity = std::clamp(static_cast<float>(config.depth_mask_alpha) / 255.0f, 0.0f, 1.0f);
            bool maskHasBounds = false;
            cv::Rect maskBounds{};

            if (config.depth_debug_overlay_enabled)
            {
                cv::Mat frameCopy;
                {
                    std::lock_guard<std::mutex> lk(frameMutex);
                    if (!latestFrame.empty())
                        latestFrame.copyTo(frameCopy);
                }

                // 深度模型路径为空：卸载已有模型
                if (config.depth_model_path.empty())
                {
                    if (depthDebugModel.ready())
                        depthDebugModel.reset();
                    depthDebugModelPath.clear();
                }
                // 模型路径变化或模型未就绪：重新初始化
                else if (depthDebugModelPath != config.depth_model_path || !depthDebugModel.ready())
                {
                    if (depthDebugModel.initialize(config.depth_model_path, gLogger))
                    {
                        depthDebugModelPath = config.depth_model_path;
                    }
                }

                // 着色方案变化时更新
                if (config.depth_colormap != depthDebugColormap)
                {
                    depthDebugModel.setColormap(config.depth_colormap);
                    depthDebugColormap = config.depth_colormap;
                }

                // 模型就绪且有帧数据：按帧率限制执行推理
                if (depthDebugModel.ready() && !frameCopy.empty())
                {
                    auto now = std::chrono::steady_clock::now();
                    bool shouldUpdate = depthDebugFrame.empty();
                    if (config.depth_fps <= 0)
                    {
                        shouldUpdate = true;
                    }
                    else if (!shouldUpdate)
                    {
                        auto interval = std::chrono::milliseconds(1000 / config.depth_fps);
                        shouldUpdate = (now - lastDepthUpdate) >= interval;
                    }
                    if (shouldUpdate)
                    {
                        cv::Mat depthFrame = depthDebugModel.predict(frameCopy);
                        if (!depthFrame.empty())
                        {
                            depthDebugFrame = depthFrame;
                            lastDepthUpdate = now;
                        }
                    }
                }
                // 将深度结果转换为 BGRA 并上传到覆盖层
                if (!depthDebugFrame.empty())
                {
                    cv::Mat depthBGRA;
                    cv::cvtColor(depthDebugFrame, depthBGRA, cv::COLOR_BGR2BGRA);
                    int newId = gameOverlayPtr->UpdateImageFromBGRA(
                        depthBGRA.data,
                        depthBGRA.cols,
                        depthBGRA.rows,
                        static_cast<int>(depthBGRA.step),
                        depthDebugImageId);
                    if (newId != 0)
                        depthDebugImageId = newId;
                    depthW = static_cast<float>(regionW);
                    depthH = static_cast<float>(regionH);
                }
            }
            else if (depthDebugImageId != 0)
            {
                gameOverlayPtr->UnloadImage(depthDebugImageId);
                depthDebugImageId = 0;
            }

            // ---- 深度遮罩生成与渲染 ----
            if (config.depth_mask_enabled)
            {
                auto& depthMask = depth_anything::GetDepthMaskGenerator();
                cv::Mat mask = depthMask.getMask();

                if (mask.empty())
                {
                    cv::Mat frameCopy;
                    {
                        std::lock_guard<std::mutex> lk(frameMutex);
                        if (!latestFrame.empty())
                            latestFrame.copyTo(frameCopy);
                    }

                    if (!frameCopy.empty())
                    {
                        depth_anything::DepthMaskOptions maskOptions;
                        maskOptions.enabled = true;
                        maskOptions.fps = config.depth_mask_fps;
                        maskOptions.near_percent = config.depth_mask_near_percent;
                        maskOptions.expand = config.depth_mask_expand;
                        maskOptions.invert = config.depth_mask_invert;

                        depthMask.update(frameCopy, maskOptions, config.depth_model_path, gLogger);
                        mask = depthMask.getMask();

                        // 遮罩为空且模型已就绪：直接使用深度调试模型生成遮罩
                        if (mask.empty())
                        {
                            if (!config.depth_model_path.empty() &&
                                (depthDebugModelPath != config.depth_model_path || !depthDebugModel.ready()))
                            {
                                if (depthDebugModel.initialize(config.depth_model_path, gLogger))
                                {
                                    depthDebugModelPath = config.depth_model_path;
                                    depthDebugColormap = config.depth_colormap;
                                    depthDebugModel.setColormap(config.depth_colormap);
                                }
                            }

                            if (depthDebugModel.ready())
                            {
                                cv::Mat depthLocal = depthDebugModel.predictDepth(frameCopy);
                                if (!depthLocal.empty())
                                {
                                    const int nearPercent = std::clamp(config.depth_mask_near_percent, 1, 100);
                                    const bool invertMask = config.depth_mask_invert;
                                    mask = depth_anything::generateDepthMaskFallback(
                                        depthLocal, nearPercent, config.depth_mask_expand, invertMask);
                                }
                            }
                        }
                    }
                }

                // 将遮罩转为 BGRA 并上传到覆盖层
                if (!mask.empty())
                {
                    cv::Mat maskBGRA(mask.size(), CV_8UC4, cv::Scalar(0, 0, 0, 0));
                    maskBGRA.setTo(cv::Scalar(20, 90, 255, 255), mask);

                    cv::Mat nonZeroPoints;
                    cv::findNonZero(mask, nonZeroPoints);
                    if (!nonZeroPoints.empty())
                    {
                        maskBounds = cv::boundingRect(nonZeroPoints);
                        maskHasBounds = true;

                    }

                    int newId = gameOverlayPtr->UpdateImageFromBGRA(
                        maskBGRA.data,
                        maskBGRA.cols,
                        maskBGRA.rows,
                        static_cast<int>(maskBGRA.step),
                        depthMaskImageId);
                    if (newId != 0)
                        depthMaskImageId = newId;

                    maskW = static_cast<float>(regionW);
                    maskH = static_cast<float>(regionH);
                }
            }
            else if (depthMaskImageId != 0)
            {
                gameOverlayPtr->UnloadImage(depthMaskImageId);
                depthMaskImageId = 0;
            }

            // ---- 绘制深度图像和遮罩到覆盖层 ----
            if (depthDebugImageId != 0 || depthMaskImageId != 0 || (config.depth_debug_overlay_enabled && config.depth_mask_enabled))
            {
                float depthX = static_cast<float>(baseX);
                float depthY = static_cast<float>(baseY);
                float maskX = depthX;
                float maskY = depthY;

                if (depthDebugImageId != 0 && depthW > 0.0f && depthH > 0.0f)
                {
                    // 若同时显示遮罩，降低深度调试图的透明度
                    const float depthDebugOpacity = config.depth_mask_enabled ? 0.30f : 1.0f;
                    gameOverlayPtr->DrawImage(depthDebugImageId, depthX, depthY, depthW, depthH, depthDebugOpacity);
                    gameOverlayPtr->AddRect({ depthX, depthY, depthW, depthH }, ARGB(120, 255, 255, 255), 1.0f);
                }

                if (depthMaskImageId != 0 && maskW > 0.0f && maskH > 0.0f)
                {
                    gameOverlayPtr->DrawImage(depthMaskImageId, maskX, maskY, maskW, maskH, maskOpacity);

                    // 绘制遮罩边界框
                    if (maskHasBounds)
                    {
                        const float bx = maskX + static_cast<float>(maskBounds.x) * scaleX;
                        const float by = maskY + static_cast<float>(maskBounds.y) * scaleY;
                        const float bw = static_cast<float>(maskBounds.width) * scaleX;
                        const float bh = static_cast<float>(maskBounds.height) * scaleY;

                        gameOverlayPtr->AddRect({ bx, by, bw, bh }, ARGB(230, 255, 240, 120), 1.8f);
                    }
                }
            }
        }
#endif

        // ========== 7. 捕获帧边框绘制 ==========

        if (config.game_overlay_draw_frame)
        {
            int A = std::clamp(config.game_overlay_frame_a, 0, 255);
            int R = std::clamp(config.game_overlay_frame_r, 0, 255);
            int G = std::clamp(config.game_overlay_frame_g, 0, 255);
            int B = std::clamp(config.game_overlay_frame_b, 0, 255);
            const uint32_t col = (uint32_t(A) << 24) | (uint32_t(R) << 16) | (uint32_t(G) << 8) | uint32_t(B);

            float thickness = config.game_overlay_frame_thickness;
            if (thickness <= 0.f) thickness = 1.f;

            gameOverlayPtr->AddRect(
                { static_cast<float>(baseX),
                  static_cast<float>(baseY),
                  static_cast<float>(regionW),
                  static_cast<float>(regionH) },
                col, thickness);
        }

        // ========== 8. 圆形 FOV 绘制 ==========

        if (config.circle_fov_enabled && config.game_overlay_draw_circle_fov)
        {
            int A = std::clamp(config.game_overlay_frame_a, 0, 255);
            int R = std::clamp(config.game_overlay_frame_r, 0, 255);
            int G = std::clamp(config.game_overlay_frame_g, 0, 255);
            int B = std::clamp(config.game_overlay_frame_b, 0, 255);
            const uint32_t col = (uint32_t(A) << 24) | (uint32_t(R) << 16) | (uint32_t(G) << 8) | uint32_t(B);

            float thickness = config.game_overlay_frame_thickness;
            if (thickness <= 0.f) thickness = 1.f;

            const cv::Size regionSize(regionW, regionH);
            const cv::Point2f center = getCircleFovCenter(regionSize);
            const float radius = getCircleFovRadiusPixels(regionSize, config.circle_fov_radius_percent);
            gameOverlayPtr->AddCircle(
                { static_cast<float>(baseX) + center.x, static_cast<float>(baseY) + center.y, radius },
                col,
                thickness);
        }

        // ========== 9. 检测框和跟踪框绘制（带延迟补偿） ==========

        if (config.game_overlay_draw_boxes && (!boxesCopy.empty() || !trackDebugCopy.empty()))
        {
            int A = std::clamp(config.game_overlay_box_a, 0, 255);
            int R = std::clamp(config.game_overlay_box_r, 0, 255);
            int G = std::clamp(config.game_overlay_box_g, 0, 255);
            int B = std::clamp(config.game_overlay_box_b, 0, 255);
            const uint32_t col = (uint32_t(A) << 24) | (uint32_t(R) << 16) | (uint32_t(G) << 8) | uint32_t(B);

            float thickness = config.game_overlay_box_thickness;
            if (thickness <= 0.f) thickness = 1.f;

            // 是否使用延迟补偿的跟踪轨迹绘制
            const bool drawCompensatedTracks =
                config.game_overlay_compensate_latency && !trackDebugCopy.empty();

            if (drawCompensatedTracks)
            {
                // 绘制跟踪轨迹框（带速度和延迟补偿）
                for (const auto& t : trackDebugCopy)
                {
                    auto rect = projectDetectionBox(t.box, t.velocityX, t.velocityY, t.lastUpdate);
                    if (!rect)
                        continue;
                    gameOverlayPtr->AddRect(*rect, col, thickness);
                }

                // 绘制未被跟踪轨迹覆盖的原始检测框
                for (size_t i = 0; i < boxesCopy.size(); ++i)
                {
                    const int cls = (i < classesCopy.size()) ? classesCopy[i] : -1;
                    if (detectionRepresentedByTrack(boxesCopy[i], cls, trackDebugCopy))
                        continue;

                    auto rect = projectDetectionBox(boxesCopy[i]);
                    if (!rect)
                        continue;

                    gameOverlayPtr->AddRect(*rect, col, thickness);
                }
            }
            else
            {
                // 无补偿：直接绘制所有检测框
                for (const auto& b : boxesCopy)
                {
                    auto rect = projectDetectionBox(b);
                    if (!rect)
                        continue;
                    gameOverlayPtr->AddRect(*rect, col, thickness);
                }
            }

            // ---- 在每个跟踪轨迹上标注信息（ID、锁定标记、错过帧数） ----
            for (const auto& t : trackDebugCopy)
            {
                auto rect = projectDetectionBox(
                    t.box,
                    config.game_overlay_compensate_latency ? t.velocityX : 0.0,
                    config.game_overlay_compensate_latency ? t.velocityY : 0.0,
                    t.lastUpdate);
                if (!rect)
                    continue;

                std::wstring label = L"ID " + std::to_wstring(t.trackId);
                if (t.trackId == lockedTrackId || t.isLocked)
                    label += L" *";
                if (!t.observedThisFrame)
                    label += L" m" + std::to_wstring(t.missedFrames);

                const uint32_t textCol =
                    (t.trackId == lockedTrackId || t.isLocked)
                    ? ARGB(255, 255, 220, 70)
                    : ARGB(230, 180, 255, 180);

                gameOverlayPtr->AddText(
                    rect->x + 2.0f,
                    std::max(static_cast<float>(baseY), rect->y - 16.0f),
                    label,
                    15.0f,
                    textCol
                );
            }
        }

        // ========== 11. 风偏调试轨迹绘制 ==========

        if (config.game_overlay_draw_wind_tail && windTailPts.size() > 1)
        {
            const size_t n = windTailPts.size();
            const auto& anchor = windTailPts.back();
            const float centerX = static_cast<float>(baseX) + regionW * 0.5f;
            const float centerY = static_cast<float>(baseY) + regionH * 0.5f;
            for (size_t i = 1; i < n; ++i)
            {
                const auto& p0 = windTailPts[i - 1];
                const auto& p1 = windTailPts[i];

                // 将轨迹点转换为相对于锚点（最新点）的偏移
                const float rel0x = static_cast<float>(p0.first - anchor.first);
                const float rel0y = static_cast<float>(p0.second - anchor.second);
                const float rel1x = static_cast<float>(p1.first - anchor.first);
                const float rel1y = static_cast<float>(p1.second - anchor.second);

                const float x0 = centerX + rel0x * scaleX;
                const float y0 = centerY + rel0y * scaleY;
                const float x1 = centerX + rel1x * scaleX;
                const float y1 = centerY + rel1y * scaleY;

                // 越新的点透明度越低
                const uint8_t alpha = static_cast<uint8_t>(35 + (190 * i) / n);
                gameOverlayPtr->AddLine({ x0, y0, x1, y1 }, ARGB(alpha, 80, 210, 255), 1.3f);
            }

            // 绘制锚点中心标记和标签
            const float hx = centerX;
            const float hy = centerY;
            gameOverlayPtr->FillCircle({ hx, hy, 3.5f }, ARGB(230, 120, 230, 255));
            gameOverlayPtr->AddText(
                static_cast<float>(baseX) + 8.0f,
                static_cast<float>(baseY + regionH) - 22.0f,
                L"Wind tail",
                14.0f,
                ARGB(210, 120, 230, 255)
            );
        }

        // ========== 12. 目标图标叠加 ==========

        if (config.game_overlay_icon_enabled && g_iconImageId != 0 && !boxesCopy.empty())
        {
            const int iconW = config.game_overlay_icon_width;
            const int iconH = config.game_overlay_icon_height;
            const float offXIcon = config.game_overlay_icon_offset_x;
            const float offYIcon = config.game_overlay_icon_offset_y;
            std::string anchor = config.game_overlay_icon_anchor;
            const int wantedClass = config.game_overlay_icon_class;
            const size_t count = boxesCopy.size();
            for (size_t i = 0; i < count; ++i)
            {
                const auto& b = boxesCopy[i];
                int cls = (i < classesCopy.size()) ? classesCopy[i] : -1;
                // 类别过滤器（-1 表示显示所有类别）
                if (wantedClass >= 0 && cls != wantedClass)
                {
                    continue;
                }

                auto boxRect = projectDetectionBox(b);
                if (!boxRect)
                    continue;

                float drawX = boxRect->x;
                float drawY = boxRect->y;

                // 根据锚点位置计算图标绘制坐标
                if (anchor == "center")
                {
                    drawX = boxRect->x + boxRect->w / 2.0f - iconW / 2.0f;
                    drawY = boxRect->y + boxRect->h / 2.0f - iconH / 2.0f;
                }
                else if (anchor == "top" || anchor == "head")
                {
                    drawX = boxRect->x + boxRect->w / 2.0f - iconW / 2.0f;
                    drawY = boxRect->y - iconH;
                }
                else if (anchor == "bottom")
                {
                    drawX = boxRect->x + boxRect->w / 2.0f - iconW / 2.0f;
                    drawY = boxRect->y + boxRect->h;
                }
                else
                {
                    drawX = boxRect->x + boxRect->w / 2.0f - iconW / 2.0f;
                    drawY = boxRect->y + boxRect->h / 2.0f - iconH / 2.0f;
                }

                // 应用用户设置的偏移量
                drawX += offXIcon;
                drawY += offYIcon;

                gameOverlayPtr->DrawImage(g_iconImageId, drawX, drawY, (float)iconW, (float)iconH, 1.0f);
            }
        }

        // ========== 13. 目标校正演示 ==========

        if (config.game_overlay_show_target_correction)
        {
            draw_target_correction_demo_game_overlay(
                gameOverlayPtr.get(),
                static_cast<float>(baseX) + regionW * 0.5f,
                static_cast<float>(baseY) + regionH * 0.5f);
        }

        // ========== 14. 结束帧渲染 ==========

        gameOverlayPtr->EndFrame();
    }

    // ========== 循环退出后清理覆盖层资源 ==========

    if (gameOverlayPtr)
    {
        gameOverlayPtr->Stop();
        gameOverlayPtr.reset();
    }
}
