#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include <iostream>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <deque>
#include <random>
#include <array>
#include <cwchar>
#include <memory>

#include <opencv2/core/utils/logger.hpp>

#include "capture.h"
#include "mouse.h"
#include "Xen.h"
#include "keyboard_listener.h"
#include "overlay.h"
#include "Game_overlay.h"
#include "ghub.h"
#include "other_tools.h"
#include "virtual_camera.h"
#include "mem/cpu_affinity_manager.h"
#include "runtime/thread_loops.h"
#include "benchmarks/provider_benchmark.h"

#ifdef USE_CUDA
#include "mem/gpu_resource_manager.h"
#include "depth/depth_anything_trt.h"
#include "depth/depth_mask.h"
#include "tensorrt/nvinf.h"
#endif

// 条件变量，用于帧就绪通知（capture 线程通知其他等待帧数据的线程）
std::condition_variable frameCV;
// 原子标志：通知所有线程程序应退出
std::atomic<bool> shouldExit(false);
// 原子标志：当前是否正在进行瞄准（鼠标控制）
std::atomic<bool> aiming(false);
// 原子标志：是否暂停目标检测
std::atomic<bool> detectionPaused(false);
// 保护全局配置对象（config）的互斥锁
std::mutex configMutex;
// 保护全局输入设备指针（鼠标设备实例）的互斥锁
std::mutex inputDevicesMutex;

#ifdef USE_CUDA
// TensorRT 目标检测器（CUDA 后端），用于 GPU 加速的 AI 推理
TrtDetector trt_detector;
#else
// DirectML 目标检测器（非 CUDA 后端），使用 DirectX ML 进行推理
std::unique_ptr<DirectMLDetector> dml_detector;
#endif

// 全局鼠标线程指针，用于控制鼠标移动和点击
MouseThread* globalMouseThread = nullptr;
// 全局应用配置对象
Config config;


// 罗技 G HUB 鼠标设备接口（通过 G HUB 内存映射控制）
GhubMouse* gHub = nullptr;
// 雷蛇鼠标设备接口（通过 rzctl 控制）
RzctlMouse* razerControl = nullptr;
// Arduino 串口鼠标设备接口
Arduino* arduinoSerial = nullptr;
// RP2350 串口鼠标设备接口
RP2350* rp2350Serial = nullptr;
// Kmbox Net 网络鼠标设备接口
KmboxNetConnection* kmboxNetSerial = nullptr;
// Kmbox A 型串口鼠标设备接口
KmboxAConnection* kmboxASerial = nullptr;
// Makcu 连接鼠标设备接口
MakcuConnection* makcuSerial = nullptr;
// 当前活动的鼠标输入设备（RAII 所有权）
std::unique_ptr<IMouseInput> activeMouseInputOwner;

// 以下原子标志用于通知 capture 线程配置项已变更，需要重新初始化或调整：

// 检测分辨率已变更
std::atomic<bool> detection_resolution_changed(false);
// 捕获方法已变更（如 duplication_api / winrt 切换）
std::atomic<bool> capture_method_changed(false);
// 是否捕获鼠标指针的配置已变更
std::atomic<bool> capture_cursor_changed(false);
// 是否捕获窗口边框的配置已变更
std::atomic<bool> capture_borders_changed(false);
// 捕获帧率已变更
std::atomic<bool> capture_fps_changed(false);
// 捕获窗口标题已变更
std::atomic<bool> capture_window_changed(false);
// 检测模型已变更
std::atomic<bool> detector_model_changed(false);
// 是否显示预览窗口已变更
std::atomic<bool> show_window_changed(false);
// 输入方法（鼠标设备类型）已变更
std::atomic<bool> input_method_changed(false);

// 标志：当前是否正在缩放（狙击/高倍镜模式）
std::atomic<bool> zooming(false);
// 标志：是否正在自动射击
std::atomic<bool> shooting(false);


// 图标加载错误信息的存储字符串
std::string g_iconLastError;

// 打印致命错误消息并等待用户按回车后退出，返回 -1
static int FatalExit(const std::string& message)
{
    std::cerr << message << std::endl;
    std::cout << "Press Enter to exit...";
    std::cin.get();
    return -1;
}

// 将工作目录设置为可执行文件所在目录，确保相对路径资源（模型、配置等）可被正确加载
static void SetWorkingDirectoryToExecutable()
{
    wchar_t exePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0)
    {
        std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
        std::error_code ec;
        std::filesystem::current_path(exeDir, ec);
        if (ec && config.verbose)
        {
            std::cout << "[Config] Failed to set working dir: " << exeDir.u8string()
                      << " (" << ec.message() << ")" << std::endl;
        }
    }
}

// 选择与当前后端兼容的 AI 模型。优先使用 config.ai_model 指定的模型；
// 若不可用则自动选择 models 目录下第一个兼容的模型并更新配置
static bool SelectCompatibleAiModel()
{
    std::vector<std::string> availableModels = getAvailableModels();
    if (!config.ai_model.empty())
    {
        const std::string modelPath = "models/" + config.ai_model;
        if (!std::filesystem::exists(modelPath))
        {
            std::cerr << "[MAIN] Specified model does not exist: " << modelPath << std::endl;
        }
        else if (std::find(availableModels.begin(), availableModels.end(), config.ai_model) != availableModels.end())
        {
            return true;
        }
        else
        {
            std::cerr << "[MAIN] Specified model is not compatible with backend "
                      << config.backend << ": " << config.ai_model << std::endl;
        }
    }

    if (availableModels.empty())
    {
        std::cerr << "[MAIN] No compatible AI models found in 'models' directory for backend "
                  << config.backend << "." << std::endl;
        return false;
    }

    config.ai_model = availableModels[0];
    config.saveConfig("config.ini");
    std::cout << "[MAIN] Loaded first compatible " << config.backend
              << " 模型： " << config.ai_model << std::endl;
    return true;
}

// 处理线程崩溃：记录崩溃信息和线程名，设置退出标志，通知所有等待的线程
static void HandleThreadCrash(const char* name, const std::exception* ex)
{
    std::cerr << "[Thread] " << name << " thread crashed: "
              << (ex ? ex->what() : "unknown exception") << std::endl;
    shouldExit = true;
    gameOverlayShouldExit.store(true);
#ifdef USE_CUDA
    trt_detector.requestStop();
#endif
    frameCV.notify_all();
    detectionBuffer.cv.notify_all();
}

// 创建一个受异常保护的线程包装函数。若线程内抛出异常，自动调用 HandleThreadCrash 处理
template <typename Func>
static std::thread StartThreadGuarded(const char* name, Func func)
{
    return std::thread([name, func]() mutable {
        try
        {
            func();
        }
        catch (const std::exception& e)
        {
            HandleThreadCrash(name, &e);
        }
        catch (...)
        {
            HandleThreadCrash(name, nullptr);
        }
        });
}

// 创建/重建所有输入设备（鼠标）。先释放旧设备，然后根据当前配置创建新设备实例，
// 并更新全局设备指针（受 inputDevicesMutex 保护）
void createInputDevices()
{
    if (globalMouseThread)
        globalMouseThread->setMouseInput(nullptr);

    std::unique_ptr<IMouseInput> oldMouseInputOwner;
    {
        std::lock_guard<std::mutex> deviceLock(inputDevicesMutex);
        oldMouseInputOwner = std::move(activeMouseInputOwner);
        arduinoSerial = nullptr;
        rp2350Serial = nullptr;
        gHub = nullptr;
        razerControl = nullptr;
        kmboxNetSerial = nullptr;
        kmboxASerial = nullptr;
        makcuSerial = nullptr;
    }
    oldMouseInputOwner.reset();

    Config cfgSnapshot;
    {
        std::lock_guard<std::mutex> cfgLock(configMutex);
        cfgSnapshot = config;
    }

    auto newMouseInputOwner = CreateMouseInputDevice(cfgSnapshot);
    IMouseInput* newMouseInput = newMouseInputOwner.get();

    Arduino* newArduinoSerial = newMouseInput ? newMouseInput->arduino() : nullptr;
    RP2350* newRp2350Serial = newMouseInput ? newMouseInput->rp2350() : nullptr;
    GhubMouse* newGHub = newMouseInput ? newMouseInput->ghub() : nullptr;
    RzctlMouse* newRazerControl = newMouseInput ? newMouseInput->razer() : nullptr;
    KmboxNetConnection* newKmboxNetSerial = newMouseInput ? newMouseInput->kmboxNet() : nullptr;
    KmboxAConnection* newKmboxASerial = newMouseInput ? newMouseInput->kmboxA() : nullptr;
    MakcuConnection* newMakcuSerial = newMouseInput ? newMouseInput->makcu() : nullptr;

    std::string message = std::string("[Mouse] Using ") + (newMouseInput ? newMouseInput->name() : "unknown") + " input.";
    if (!newMouseInput || !newMouseInput->isOpen())
        message += " Device not connected; input disabled until the method becomes available.";

    {
        std::lock_guard<std::mutex> deviceLock(inputDevicesMutex);
        activeMouseInputOwner = std::move(newMouseInputOwner);
        arduinoSerial = newArduinoSerial;
        rp2350Serial = newRp2350Serial;
        gHub = newGHub;
        razerControl = newRazerControl;
        kmboxNetSerial = newKmboxNetSerial;
        kmboxASerial = newKmboxASerial;
        makcuSerial = newMakcuSerial;
    }

    std::cout << message << std::endl;
}

// 将当前活动鼠标输入设备绑定到全局鼠标线程
void assignInputDevices()
{
    if (globalMouseThread)
    {
        globalMouseThread->setMouseInput(activeMouseInputOwner.get());
    }
}


// 程序入口点。初始化控制台、加载配置、创建各模块线程，进入主循环等待线程结束
int main(int argc, char** argv)
{
    // 设置控制台输出编码为 UTF-8，支持中文显示
    SetConsoleOutputCP(CP_UTF8);
    // 设置随机控制台标题，防止被简单检测
    SetRandomConsoleTitle();
    // 抑制 OpenCV 日志输出，仅显示致命错误
    cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_FATAL);
    // 将工作目录设为 exe 所在目录
    SetWorkingDirectoryToExecutable();

    // 如果命令行参数请求了 provider benchmark，则以 CLI 模式运行基准测试并退出
    if (benchmarks::IsProviderBenchmarkRequested(argc, argv))
    {
        return benchmarks::RunProviderBenchmarkCli(argc, argv);
    }

    // 加载配置文件 (config.ini)
    if (!config.loadConfig())
    {
        std::cerr << "[Config] Error with loading config!" << std::endl;
        std::cin.get();
        return -1;
    }

    // 系统资源预留管理
    CPUAffinityManager cpuManager;

    // 预留指定数量的 CPU 核心（防止系统调度干扰）
    if (config.cpuCoreReserveCount > 0)
    {
        if (!cpuManager.reserveCPUCores(config.cpuCoreReserveCount))
            return FatalExit("[MAIN] Failed to reserve CPU cores.");
    }

    // 预留指定数量的系统内存（确保程序可用内存）
    if (config.systemMemoryReserveMB > 0)
    {
        if (!cpuManager.reserveSystemMemory(config.systemMemoryReserveMB))
            return FatalExit("[MAIN] Failed to reserve system memory.");
    }

    try
    {
#ifdef USE_CUDA
        // 检查 CUDA 运行时版本
        int cuda_runtime_version = 0;
        cudaError_t runtime_status = cudaRuntimeGetVersion(&cuda_runtime_version);

        if (runtime_status != cudaSuccess)
        {
            std::cerr << "[MAIN] CUDA runtime check failed: " << cudaGetErrorString(runtime_status) << std::endl;
            std::cin.get();
            return -1;
        }

        if (config.verbose)
            std::cout << "[CUDA] Version: " << cuda_runtime_version << std::endl;

        // 要求 CUDA 13.1 或更高版本
        const int required_cuda_version = 13010;
        if (cuda_runtime_version < required_cuda_version)
        {
            int required_major = required_cuda_version / 1000;
            int required_minor = (required_cuda_version % 1000) / 10;
            int runtime_major = cuda_runtime_version / 1000;
            int runtime_minor = (cuda_runtime_version % 1000) / 10;
            std::cerr << "[MAIN] CUDA " << required_major << "." << required_minor
                << " 需要但不可用。检测到 " << runtime_major << "." << runtime_minor << "." << std::endl;
            const wchar_t* title = L"CUDA Update Required";
            std::wstring message =
                L"An outdated CUDA version was detected. "
                L"Please update your graphics drivers to the latest version "
                L"and install CUDA 13.1.\n\n"
                L"The program will now attempt to continue.";
            MessageBoxW(nullptr, message.c_str(), title, MB_OK | MB_ICONWARNING);
        }

        // GPU 资源管理
        GPUResourceManager gpuManager;
        if (config.gpuMemoryReserveMB > 0)
        {
            if (!gpuManager.reserveGPUMemory(config.gpuMemoryReserveMB))
                return FatalExit("[MAIN] Failed to reserve GPU memory.");
        }

        // 尝试设置 GPU 独占模式
        if (config.enableGpuExclusiveMode)
        {
            if (!gpuManager.setGPUExclusiveMode())
                return FatalExit("[MAIN] Failed to set GPU exclusive mode.");
        }

        // 检查是否有可用的 CUDA 设备
        int cuda_devices = 0;
        if (cudaGetDeviceCount(&cuda_devices) != cudaSuccess || cuda_devices == 0)
        {
            std::cerr << "[MAIN] CUDA required but no devices found." << std::endl;
            std::cin.get();
            return -1;
        }
#endif
        // 确保 models 和 depth_models 目录存在
        if (!CreateDirectory(L"models", NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        {
            std::cout << "[MAIN] Error with models folder" << std::endl;
            std::cin.get();
            return -1;
        }
        if (!CreateDirectory(L"depth_models", NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        {
            std::cout << "[MAIN] Error with depth_models folder" << std::endl;
            std::cin.get();
            return -1;
        }

        // 如果使用虚拟摄像头，检查可用摄像头列表并验证配置
        if (config.capture_method == "virtual_camera")
        {
            auto cams = VirtualCameraCapture::GetAvailableVirtualCameras(true);
            if (!cams.empty())
            {
                if (config.virtual_camera_name != "None" &&
                    std::find(cams.begin(), cams.end(), config.virtual_camera_name) == cams.end())
                {
                    config.virtual_camera_name = "None";
                    config.saveConfig("config.ini");
                    std::cout << "[MAIN] virtual_camera_name reset to None (auto-select)." << std::endl;
                }
                std::cout << "[MAIN] Virtual cameras loaded: " << cams.size() << std::endl;
            }
            else
            {
                std::cerr << "[MAIN] No virtual cameras found" << std::endl;
            }
        }

        // 选择兼容的 AI 模型
        if (!SelectCompatibleAiModel())
        {
            std::cin.get();
            return -1;
        }

        // 创建鼠标输入设备（根据配置选择 Arduino / GHUB / 雷蛇 / Kmbox 等）
        createInputDevices();

        // 鼠标线程，负责 AI 预测后的鼠标移动和点击控制
        MouseThread mouseThread(
            config.detection_resolution,
            config.fovX,
            config.fovY,
            config.minSpeedMultiplier,
            config.maxSpeedMultiplier,
            config.predictionInterval,
            config.auto_shoot,
            config.bScope_multiplier,
            activeMouseInputOwner.get()
        );

        globalMouseThread = &mouseThread;
        assignInputDevices();

#ifdef USE_CUDA
        // 初始化 TensorRT 检测器并加载模型
        trt_detector.initialize("models/" + config.ai_model);
#else
        // 创建 DirectML 检测器并启动其推理线程
        std::thread dml_detThread;
        try
        {
            dml_detector = std::make_unique<DirectMLDetector>("models/" + config.ai_model);
            std::cout << "[MAIN] DML detector created"
                      << (dml_detector->isReady() ? "." : ", but no active model.") << std::endl;
            dml_detThread = StartThreadGuarded("DmlDetector", [] {
                dml_detector->dmlInferenceThread();
                });
        }
        catch (const std::exception& e)
        {
            std::cerr << "[MAIN] DML detector is unavailable: " << e.what()
                      << ". The application will continue without DML inference." << std::endl;
        }
        catch (...)
        {
            std::cerr << "[MAIN] DML detector is unavailable: unknown exception."
                      << ". The application will continue without DML inference." << std::endl;
        }
#endif

        detection_resolution_changed.store(true);

        // 启动各工作线程
        // 键盘监听线程：处理快捷键、配置热切换等
        std::thread keyThread = StartThreadGuarded("KeyboardListener", [] {
            keyboardListener();
            });
        // 屏幕捕获线程：从屏幕/摄像头/网络等源采集帧
        std::thread capThread = StartThreadGuarded("CaptureThread", [] {
            captureThread(config.detection_resolution, config.detection_resolution);
            });

#ifdef USE_CUDA
        // TensorRT 推理线程（CUDA 后端）
        std::thread trt_detThread = StartThreadGuarded("TrtDetector", [] {
            trt_detector.inferenceThread();
            });
#endif
        // 鼠标控制线程：执行瞄准和射击
        std::thread mouseMovThread = StartThreadGuarded("MouseThread", [&mouseThread] {
            mouseThreadFunction(mouseThread);
            });
        // 桌面叠加层 UI 线程
        std::thread overlayThread = StartThreadGuarded("OverlayThread", [] {
            OverlayThread();
            });

        // 游戏内叠加层 UI 线程（使用 DirectX 渲染）
        gameOverlayShouldExit.store(false);
        gameOverlayThread = StartThreadGuarded("GameOverlay", [] {
            gameOverlayRenderLoop();
            });

        // 显示欢迎消息
        welcome_message();

        // 等待各线程退出
        keyThread.join();
        capThread.join();
#ifdef USE_CUDA
        trt_detector.requestStop();
        trt_detThread.join();
#else
        if (dml_detThread.joinable())
        {
            if (dml_detector)
            {
                dml_detector->shouldExit = true;
                dml_detector->inferenceCV.notify_all();
            }
            dml_detThread.join();
        }
#endif

        gameOverlayShouldExit.store(true);
        if (gameOverlayThread.joinable()) gameOverlayThread.join();

        mouseMovThread.join();
        overlayThread.join();

        // 清理鼠标设备
        {
            std::lock_guard<std::mutex> deviceLock(inputDevicesMutex);
            activeMouseInputOwner.reset();
            arduinoSerial = nullptr;
            rp2350Serial = nullptr;
            gHub = nullptr;
            razerControl = nullptr;
            kmboxNetSerial = nullptr;
            kmboxASerial = nullptr;
            makcuSerial = nullptr;
        }

#ifndef USE_CUDA
        // 清理 DirectML 检测器
        if (dml_detector)
        {
            dml_detector.reset();
        }
#endif

        // 清理游戏叠加层
        if (gameOverlayPtr)
        {
            gameOverlayPtr->Stop();
            gameOverlayPtr.reset();
        }

        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[MAIN] An error has occurred in the main stream: " << e.what() << std::endl;
        std::cout << "Press Enter to exit...";
        std::cin.get();
        return -1;
    }
}
