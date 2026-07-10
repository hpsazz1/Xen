// 确保仅包含 Windows 核心 API，避免 Winsock 重复定义
#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#ifdef USE_CUDA

#include <opencv2/opencv.hpp>
#include <opencv2/core/utils/logger.hpp>
#include <cuda_runtime_api.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "config.h"
#include "other_tools.h"
#include "provider_benchmark.h"
#include "trt_detector.h"

// 全局配置与 TensorRT 检测器实例
extern Config config;
extern TrtDetector trt_detector;

namespace benchmarks
{
namespace
{
// 高精度时钟别名（毫秒级稳态时钟）
using Clock = std::chrono::steady_clock;

// CLI 命令行选项结构体 —— 存储用户通过命令行传入的所有基准测试参数
struct CliOptions
{
    bool help = false;                    // 是否显示帮助信息
    bool listDevices = false;             // 是否列出可用 CUDA 设备
    bool saveResults = true;              // 是否将结果追加到 CSV 文件
    std::string providersRequested = "cuda";  // 请求的 provider 类型
    std::string modelPath;                // 通用模型路径别名
    std::string cudaModelPath;            // CUDA（TensorRT）模型路径
    std::string imagePath;                // 输入图像路径（可选）
    std::string resultsPath = "benchmark_results/provider_benchmark_cuda.csv";  // CSV 输出路径
    int runs = 100;                       // 计时运行次数
    int warmupRuns = 10;                  // 预热运行次数
    int resolution = 0;                   // 输入分辨率（0 表示使用配置默认值）
};

// 基准测试结果结构体 —— 存储单次 CUDA 基准测试的完整时间统计
struct BenchmarkResult
{
    std::string provider = "cuda";        // provider 名称
    std::string providerModel;            // 模型路径
    std::string status = "ok";            // 运行状态（"ok" / "failed"）
    std::string error;                    // 错误信息（失败时）
    int inputW = 0;                       // 输入宽度
    int inputH = 0;                       // 输入高度
    int runs = 0;                         // 实际运行次数
    int warmupRuns = 0;                   // 实际预热次数
    size_t lastDetections = 0;            // 最后一次运行的检测框数量
    double loadSeconds = 0.0;             // 模型加载耗时（秒）
    double warmupSeconds = 0.0;           // 预热阶段总耗时（秒）
    double totalSeconds = 0.0;            // 计时运行总耗时（秒）
    double preprocessSeconds = 0.0;       // 预处理累计耗时（秒）
    double inferenceSeconds = 0.0;        // 推理累计耗时（秒）
    double postprocessSeconds = 0.0;      // 后处理累计耗时（秒）
};

// 判断字符串是否以指定前缀开头
bool StartsWith(const std::string& value, const std::string& prefix)
{
    return value.rfind(prefix, 0) == 0;
}

// 解析整数（严格模式：字符串必须完全匹配整数格式）
bool ParseInt(const std::string& text, int* out)
{
    if (!out)
        return false;
    try
    {
        size_t used = 0;
        int value = std::stoi(text, &used, 10);
        if (used != text.size())
            return false;
        *out = value;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

// 消费命令行参数值 —— 支持 "--opt=value" 和 "--opt value" 两种格式
bool ConsumeValue(int argc, char** argv, int* index, const std::string& arg, const std::string& option, std::string* value)
{
    // 尝试 "--opt=value" 格式
    const std::string prefix = option + "=";
    if (StartsWith(arg, prefix))
    {
        *value = arg.substr(prefix.size());
        return true;
    }

    // 尝试 "--opt value" 格式
    if (arg == option)
    {
        if (*index + 1 >= argc)
            return false;
        const std::string next = argv[*index + 1] ? argv[*index + 1] : "";
        if (StartsWith(next, "--"))
            return false;
        *value = argv[++(*index)];
        return true;
    }

    return false;
}

// 解析命令行参数，填充 CliOptions 结构体
CliOptions ParseCli(int argc, char** argv)
{
    CliOptions options;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i] ? argv[i] : "";
        std::string value;

        // 帮助选项
        if (arg == "--benchmark-help" || arg == "--bench-help")
        {
            options.help = true;
            continue;
        }
        // 列出设备选项
        if (arg == "--bench-list-devices")
        {
            options.listDevices = true;
            continue;
        }
        // provider 选择
        if (ConsumeValue(argc, argv, &i, arg, "--benchmark-providers", &value) ||
            ConsumeValue(argc, argv, &i, arg, "--bench-providers", &value) ||
            ConsumeValue(argc, argv, &i, arg, "--providers", &value))
        {
            options.providersRequested = value.empty() ? "cuda" : value;
            continue;
        }
        // 通用模型路径
        if (ConsumeValue(argc, argv, &i, arg, "--bench-model", &value) ||
            ConsumeValue(argc, argv, &i, arg, "--model", &value))
        {
            options.modelPath = value;
            continue;
        }
        // CUDA/TensorRT 专用模型路径
        if (ConsumeValue(argc, argv, &i, arg, "--bench-cuda-model", &value) ||
            ConsumeValue(argc, argv, &i, arg, "--cuda-model", &value))
        {
            options.cudaModelPath = value;
            continue;
        }
        // 输入图像路径
        if (ConsumeValue(argc, argv, &i, arg, "--bench-image", &value) ||
            ConsumeValue(argc, argv, &i, arg, "--image", &value))
        {
            options.imagePath = value;
            continue;
        }
        // 结果 CSV 路径
        if (ConsumeValue(argc, argv, &i, arg, "--bench-results", &value) ||
            ConsumeValue(argc, argv, &i, arg, "--results", &value))
        {
            options.resultsPath = value;
            continue;
        }
        // 计时运行次数
        if (ConsumeValue(argc, argv, &i, arg, "--bench-runs", &value) ||
            ConsumeValue(argc, argv, &i, arg, "--runs", &value))
        {
            ParseInt(value, &options.runs);
            continue;
        }
        // 预热运行次数
        if (ConsumeValue(argc, argv, &i, arg, "--bench-warmup", &value) ||
            ConsumeValue(argc, argv, &i, arg, "--warmup", &value))
        {
            ParseInt(value, &options.warmupRuns);
            continue;
        }
        // 输入分辨率
        if (ConsumeValue(argc, argv, &i, arg, "--bench-resolution", &value) ||
            ConsumeValue(argc, argv, &i, arg, "--resolution", &value))
        {
            ParseInt(value, &options.resolution);
            continue;
        }
        // 不保存结果（仅打印不写 CSV）
        if (arg == "--bench-no-save")
        {
            options.saveResults = false;
            continue;
        }
    }

    // 参数合法性修正
    options.runs = std::max(1, options.runs);
    options.warmupRuns = std::max(0, options.warmupRuns);
    return options;
}

// 打印命令行帮助信息
void PrintHelp()
{
    std::cout
        << "Usage:\n"
        << "  Xen.exe --benchmark-providers [cuda] [options]\n\n"
        << "Options:\n"
        << "  --bench-cuda-model <path>        TensorRT .engine or source .onnx model.\n"
        << "  --bench-model <path>             Alias used when --bench-cuda-model is omitted.\n"
        << "  --bench-image <path>             Optional image used as benchmark input.\n"
        << "  --bench-results <path>           CSV append path. Default: benchmark_results/provider_benchmark_cuda.csv.\n"
        << "  --bench-runs <n>                 Measured runs. Default: 100.\n"
        << "  --bench-warmup <n>               Warmup runs before timing. Default: 10.\n"
        << "  --bench-resolution <n>           Input size. Default: config detection_resolution.\n"
        << "  --bench-no-save                  Do not append the summary row to CSV.\n"
        << "  --bench-list-devices             Print CUDA devices and exit.\n"
        << "  --benchmark-help                 Show this help.\n";
}

// 从当前目录向上查找包含 .git 文件夹的仓库根目录
std::filesystem::path FindRepoRoot()
{
    std::error_code ec;
    std::filesystem::path path = std::filesystem::current_path(ec);
    if (ec)
        return {};

    while (!path.empty())
    {
        if (std::filesystem::exists(path / ".git"))
            return path;
        if (!path.has_parent_path() || path.parent_path() == path)
            break;
        path = path.parent_path();
    }
    return {};
}

// 为路径添加引号，用于命令行拼接（转义内部双引号）
std::string QuoteForCommand(const std::filesystem::path& path)
{
    std::string value = path.string();
    std::string quoted = "\"";
    for (char ch : value)
        quoted += (ch == '"') ? "\\\"" : std::string(1, ch);
    quoted += '"';
    return quoted;
}

// 执行系统命令并捕获其标准输出（使用 _popen）
std::string CaptureCommandOutput(const std::string& command)
{
    std::array<char, 256> buffer{};
    std::string output;
    FILE* pipe = _popen(command.c_str(), "r");
    if (!pipe)
        return {};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe))
        output += buffer.data();
    _pclose(pipe);

    // 去除尾部空白字符
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r' || output.back() == ' ' || output.back() == '\t'))
        output.pop_back();
    return output;
}

// 获取 Git 仓库的短提交 ID（commit hash）
std::string GetGitCommitId(const std::filesystem::path& repoRoot)
{
    if (repoRoot.empty())
        return "unknown";
    std::string commit = CaptureCommandOutput("git -C " + QuoteForCommand(repoRoot) + " rev-parse --short HEAD 2>NUL");
    return commit.empty() ? "unknown" : commit;
}

// 检查 Git 仓库是否有未提交的修改
bool GetGitDirty(const std::filesystem::path& repoRoot)
{
    if (repoRoot.empty())
        return false;
    return !CaptureCommandOutput("git -C " + QuoteForCommand(repoRoot) + " status --porcelain 2>NUL").empty();
}

// 获取当前本地时间的格式化字符串（YYYY-MM-DD HH:MM:SS）
std::string CurrentTimestampLocal()
{
    const std::time_t now = std::time(nullptr);
    std::tm localTime{};
    localtime_s(&localTime, &now);
    std::ostringstream oss;
    oss << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

// 对 CSV 字段进行转义处理 —— 去除不可见字符、对包含逗号/引号的字段加双引号
std::string CsvEscape(const std::string& value)
{
    std::string sanitized;
    sanitized.reserve(value.size());
    for (unsigned char ch : value)
    {
        if (ch == '\n' || ch == '\r' || ch == '\t')
            sanitized += ' ';
        else if (ch >= 32 && ch < 127)
            sanitized += static_cast<char>(ch);
    }

    // 如果不需要转义则直接返回
    if (sanitized.find_first_of(",\"") == std::string::npos)
        return sanitized;

    // 包含逗号或引号时，用双引号包裹并转义内部双引号
    std::string escaped = "\"";
    for (char ch : sanitized)
        escaped += (ch == '"') ? "\"\"" : std::string(1, ch);
    escaped += '"';
    return escaped;
}

// 根据 CLI 选项解析基准测试使用的模型路径
// 优先级：显式 cudaModelPath > 通用 modelPath > config.ai_model > 自动查找第一个可用模型
std::filesystem::path ResolveBenchmarkModelPath(const CliOptions& options)
{
    if (!options.cudaModelPath.empty())
        return options.cudaModelPath;
    if (!options.modelPath.empty())
        return options.modelPath;
    if (!config.ai_model.empty())
        return std::filesystem::path("models") / config.ai_model;

    std::vector<std::string> models = getAvailableModels();
    if (!models.empty())
        return std::filesystem::path("models") / models.front();
    return {};
}

// 加载基准测试输入帧 —— 优先加载指定图片，否则生成确定性合成图像
cv::Mat LoadBenchmarkFrame(const CliOptions& options)
{
    if (!options.imagePath.empty())
    {
        cv::Mat image = cv::imread(options.imagePath, cv::IMREAD_COLOR);
        if (!image.empty())
        {
            if (image.cols != options.resolution || image.rows != options.resolution)
                cv::resize(image, image, cv::Size(options.resolution, options.resolution), 0, 0, cv::INTER_LINEAR);
            return image;
        }
    }

    // 生成合成图像（确定性模式填充，保证每次测试输入一致）
    cv::Mat frame(options.resolution, options.resolution, CV_8UC3);
    for (int y = 0; y < frame.rows; ++y)
    {
        cv::Vec3b* row = frame.ptr<cv::Vec3b>(y);
        for (int x = 0; x < frame.cols; ++x)
        {
            row[x] = cv::Vec3b(
                static_cast<unsigned char>((x * 3 + y) % 256),
                static_cast<unsigned char>((x + y * 5) % 256),
                static_cast<unsigned char>((x * 7 + y * 11) % 256));
        }
    }
    return frame;
}

// 打印所有可用 CUDA 设备的编号、名称、计算能力和显存
void PrintCudaDevices()
{
    int count = 0;
    cudaError_t status = cudaGetDeviceCount(&count);
    if (status != cudaSuccess)
    {
        std::cout << "CUDA devices: unavailable (" << cudaGetErrorString(status) << ")\n";
        return;
    }

    std::cout << "CUDA devices:\n";
    for (int i = 0; i < count; ++i)
    {
        cudaDeviceProp prop{};
        if (cudaGetDeviceProperties(&prop, i) == cudaSuccess)
        {
            std::cout << "  id=" << i
                      << " name=" << prop.name
                      << " capability=" << prop.major << "." << prop.minor
                      << " memory_mb=" << (prop.totalGlobalMem / (1024 * 1024))
                      << "\n";
        }
    }
}

// 运行 CUDA（TensorRT）基准测试 —— 加载模型、预热、计时运行
BenchmarkResult RunCudaBenchmark(const CliOptions& options, const std::filesystem::path& modelPath, const cv::Mat& frame)
{
    BenchmarkResult result;
    result.providerModel = modelPath.string();
    result.inputW = frame.cols;
    result.inputH = frame.rows;
    result.runs = options.runs;
    result.warmupRuns = options.warmupRuns;

    auto loadStart = Clock::now();
    try
    {
        // 初始化 TensorRT 检测器
        trt_detector.initialize(modelPath.string());
        result.loadSeconds = std::chrono::duration<double>(Clock::now() - loadStart).count();
        if (!trt_detector.isInitialized())
            throw std::runtime_error("TensorRT detector did not initialize.");

        // 预热阶段
        auto warmupStart = Clock::now();
        for (int i = 0; i < options.warmupRuns; ++i)
        {
            auto detections = trt_detector.detect(frame);
            result.lastDetections = detections.size();
        }
        result.warmupSeconds = std::chrono::duration<double>(Clock::now() - warmupStart).count();

        // 计时运行阶段 —— 累计预处理、推理、后处理三段时间
        auto totalStart = Clock::now();
        for (int i = 0; i < options.runs; ++i)
        {
            auto detections = trt_detector.detect(frame);
            result.lastDetections = detections.size();
            result.preprocessSeconds += trt_detector.lastPreprocessTime.count() / 1000.0;
            result.inferenceSeconds +=
                (trt_detector.lastInferenceTime.count() + trt_detector.lastCopyTime.count()) / 1000.0;
            result.postprocessSeconds += trt_detector.lastPostprocessTime.count() / 1000.0;
        }
        result.totalSeconds = std::chrono::duration<double>(Clock::now() - totalStart).count();
    }
    // 异常处理 —— 捕获任何运行时错误并记录到结果
    catch (const std::exception& e)
    {
        result.status = "failed";
        result.error = e.what();
        result.loadSeconds = std::chrono::duration<double>(Clock::now() - loadStart).count();
    }

    trt_detector.requestStop();
    return result;
}

// 打印基准测试结果摘要到控制台（含平均耗时和 FPS）
void PrintSummary(const CliOptions& options, const BenchmarkResult& result)
{
    std::cout << "\nCUDA benchmark summary (seconds)\n";
    std::cout << "model=" << result.providerModel << "\n";
    std::cout << "providers_requested=" << options.providersRequested << "\n";
    std::cout << "resolution=" << options.resolution
              << " runs=" << options.runs
              << " warmup=" << options.warmupRuns
              << "\n\n";

    const double avgRun = (result.runs > 0 && result.totalSeconds > 0.0)
        ? result.totalSeconds / static_cast<double>(result.runs)
        : 0.0;
    const double fps = (result.totalSeconds > 0.0)
        ? static_cast<double>(result.runs) / result.totalSeconds
        : 0.0;

    std::cout << std::fixed << std::setprecision(6)
        << "provider,provider_model,status,runs,warmup,input_w,input_h,"
        << "load_s,warmup_s,total_s,preprocess_s,inference_s,postprocess_s,avg_run_s,fps,last_detections,error\n"
        << result.provider << ","
        << CsvEscape(result.providerModel) << ","
        << result.status << ","
        << result.runs << ","
        << result.warmupRuns << ","
        << result.inputW << ","
        << result.inputH << ","
        << result.loadSeconds << ","
        << result.warmupSeconds << ","
        << result.totalSeconds << ","
        << result.preprocessSeconds << ","
        << result.inferenceSeconds << ","
        << result.postprocessSeconds << ","
        << avgRun << ","
        << fps << ","
        << result.lastDetections << ","
        << CsvEscape(result.error)
        << "\n";
}

// 将基准测试结果追加写入 CSV 文件（自动创建目录、检查/更新表头）
bool AppendBenchmarkCsv(const CliOptions& options, const BenchmarkResult& result, std::filesystem::path* writtenPath)
{
    std::filesystem::path csvPath = options.resultsPath.empty()
        ? std::filesystem::path("benchmark_results/provider_benchmark_cuda.csv")
        : std::filesystem::path(options.resultsPath);
    const std::filesystem::path repoRoot = FindRepoRoot();
    if (csvPath.is_relative() && !repoRoot.empty())
        csvPath = repoRoot / csvPath;

    std::error_code ec;
    const std::filesystem::path parent = csvPath.parent_path();
    if (!parent.empty())
        std::filesystem::create_directories(parent, ec);
    if (ec)
        return false;

    // CSV 文件头定义
    const std::string header =
        "timestamp_local,commit_id,git_dirty,build_backend,providers_requested,provider,provider_model,status,"
        "runs,warmup,input_w,input_h,load_s,warmup_s,total_s,preprocess_s,inference_s,postprocess_s,"
        "avg_run_s,fps,last_detections,error";

    // 检测现有文件头是否匹配，避免重复写入
    bool writeHeader = true;
    if (std::filesystem::exists(csvPath, ec) && !ec && std::filesystem::file_size(csvPath, ec) > 0)
    {
        std::ifstream existing(csvPath);
        std::string firstLine;
        std::getline(existing, firstLine);
        if (!firstLine.empty() && firstLine.back() == '\r')
            firstLine.pop_back();
        writeHeader = firstLine != header;
    }

    std::ofstream file(csvPath, std::ios::app);
    if (!file)
        return false;

    if (writeHeader)
        file << header << "\n";

    const double avgRun = (result.runs > 0 && result.totalSeconds > 0.0)
        ? result.totalSeconds / static_cast<double>(result.runs)
        : 0.0;
    const double fps = (result.totalSeconds > 0.0)
        ? static_cast<double>(result.runs) / result.totalSeconds
        : 0.0;

    file << std::fixed << std::setprecision(6)
        << CsvEscape(CurrentTimestampLocal()) << ","
        << CsvEscape(GetGitCommitId(repoRoot)) << ","
        << (GetGitDirty(repoRoot) ? "true" : "false") << ","
        << "cuda,"
        << CsvEscape(options.providersRequested) << ","
        << result.provider << ","
        << CsvEscape(result.providerModel) << ","
        << result.status << ","
        << result.runs << ","
        << result.warmupRuns << ","
        << result.inputW << ","
        << result.inputH << ","
        << result.loadSeconds << ","
        << result.warmupSeconds << ","
        << result.totalSeconds << ","
        << result.preprocessSeconds << ","
        << result.inferenceSeconds << ","
        << result.postprocessSeconds << ","
        << avgRun << ","
        << fps << ","
        << result.lastDetections << ","
        << CsvEscape(result.error)
        << "\n";

    if (writtenPath)
        *writtenPath = csvPath;
    return true;
}
} // namespace

// 检测命令行中是否包含基准测试相关参数（用于早期短路判断）
bool IsProviderBenchmarkRequested(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i] ? argv[i] : "";
        if (arg == "--benchmark-providers" ||
            StartsWith(arg, "--benchmark-providers=") ||
            arg == "--bench-providers" ||
            StartsWith(arg, "--bench-providers=") ||
            arg == "--benchmark-help" ||
            arg == "--bench-help" ||
            arg == "--bench-list-devices")
        {
            return true;
        }
    }
    return false;
}

// CUDA 分支的基准测试 CLI 入口函数 —— 完成解析、配置、加载、运行、输出全流程
int RunProviderBenchmarkCli(int argc, char** argv)
{
    cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_FATAL);

    CliOptions options = ParseCli(argc, argv);
    if (options.help)
    {
        PrintHelp();
        return 0;
    }

    if (options.listDevices)
    {
        PrintCudaDevices();
        return 0;
    }

    // 加载配置文件并强制后端为 TensorRT
    if (!config.loadConfig())
    {
        std::cerr << "[Benchmark] Failed to load config.ini." << std::endl;
        return 2;
    }
    config.backend = "TRT";

    // 确定输入分辨率
    if (options.resolution <= 0)
        options.resolution = config.detection_resolution > 0 ? config.detection_resolution : 320;
    options.resolution = std::clamp(options.resolution, 32, 4096);

    // 解析模型路径
    std::filesystem::path modelPath = ResolveBenchmarkModelPath(options);
    if (modelPath.empty() || !std::filesystem::exists(modelPath))
    {
        std::cerr << "[Benchmark] TensorRT model was not found. Pass --bench-cuda-model <path> or put a model in models." << std::endl;
        return 2;
    }

    // 准备输入帧
    cv::Mat frame = LoadBenchmarkFrame(options);
    if (frame.empty())
    {
        std::cerr << "[Benchmark] Failed to prepare benchmark input frame." << std::endl;
        return 2;
    }

    // 执行基准测试
    BenchmarkResult result = RunCudaBenchmark(options, modelPath, frame);
    PrintSummary(options, result);
    if (options.saveResults)
    {
        std::filesystem::path csvPath;
        if (AppendBenchmarkCsv(options, result, &csvPath))
        {
            std::cout << "results_csv=" << csvPath.string() << "\n";
            std::cout << "results_csv_base=repo_root\n";
        }
    }

    return result.status == "ok" ? 0 : 3;
}
} // namespace benchmarks

#else
#include <dxgi1_6.h>
#include <wrl/client.h>

#ifdef AIMBOT_HAS_DIRECTML_EX
#include <d3d12.h>
#include <DirectML.h>
#endif

#include <dml_provider_factory.h>
#include <onnxruntime_cxx_api.h>
#include <opencv2/opencv.hpp>
#include <opencv2/core/utils/logger.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "config.h"
#include "postProcess.h"
#include "provider_benchmark.h"

extern Config config;

namespace benchmarks
{
namespace
{
// 高精度时钟别名（毫秒级稳态时钟）
using Clock = std::chrono::steady_clock;

// Provider 类型枚举 —— 指定 ONNX Runtime 的执行后端
enum class ProviderKind
{
    Cpu,      // CPU EP（执行提供程序）
    DmlGpu,   // DirectML GPU（默认 GPU）
    DmlCpu    // DirectML CPU（WARP 软渲染）
};

// CLI 命令行选项结构体 —— 存储用户通过命令行传入的所有基准测试参数
struct CliOptions
{
    bool benchmarkRequested = false;   // 是否请求了基准测试相关参数
    bool runBenchmark = false;         // 是否实际运行基准测试（vs 仅列举设备）
    bool help = false;                 // 是否显示帮助信息
    bool listDevices = false;          // 是否列出 DXGI 适配器
    bool postprocess = true;           // 是否运行后处理（影响计时统计）
    bool disableCpuFallback = false;   // 是否禁用 CPU EP 回退
    bool saveResults = true;           // 是否将结果追加到 CSV 文件
    std::vector<ProviderKind> providers;   // 需要测试的 provider 列表
    std::string modelPath;             // ONNX 模型路径
    std::string imagePath;             // 输入图像路径（可选）
    std::string resultsPath = "benchmark_results/provider_benchmark.csv";  // CSV 输出路径
    int runs = 100;                    // 计时运行次数
    int warmupRuns = 10;               // 预热运行次数
    int resolution = 0;                // 输入分辨率（0 表示使用配置默认值）
    int batch = 1;                     // 请求的批处理大小
    int dmlDeviceId = -1;              // DML GPU 设备 ID（-1 表示使用配置默认值）
};

// 模型元信息结构体 —— 存储从 ONNX 模型中读取的输入输出张量信息
struct ModelInfo
{
    std::string inputName;                      // 输入张量名称
    std::vector<std::string> outputNames;       // 输出张量名称列表
    std::vector<const char*> outputNamePtrs;    // 输出名称的 C 字符串指针数组（供 Ort API 使用）
    int batch = 1;                              // 批处理大小
    int channels = 3;                           // 通道数（固定为 3）
    int inputH = 0;                             // 输入高度
    int inputW = 0;                             // 输入宽度
};

// 预处理工作区结构体 —— 复用内存缓冲区以减少多次运行中的重复分配
struct PreprocessWorkspace
{
    cv::Mat bgrBuffer;          // BGR 转换临时缓冲区（4通道转3通道时使用）
    cv::Mat resizeBuffer;       // 缩放临时缓冲区
    cv::Mat floatBuffer;        // 浮点转换临时缓冲区
    cv::Mat grayResizeBuffer;   // 灰度缩放临时缓冲区
    cv::Mat grayFloatBuffer;    // 灰度浮点转换临时缓冲区
};

// Provider 结果结构体 —— 存储单个 provider 的完整基准测试时间统计
struct ProviderResult
{
    std::string provider;             // provider 名称（如 "cpu"、"dml-gpu"）
    std::string providerModel;        // 模型路径
    std::string status = "ok";        // 运行状态（"ok" / "failed" / "unavailable"）
    std::string error;                // 错误信息（失败或不可用时）
    int requestedBatch = 1;           // 请求的批处理大小
    int effectiveBatch = 1;           // 模型实际生效的批处理大小
    int inputW = 0;                   // 输入宽度
    int inputH = 0;                   // 输入高度
    int runs = 0;                     // 实际运行次数
    int warmupRuns = 0;               // 实际预热次数
    size_t lastDetections = 0;        // 最后一次运行的检测框数量
    double loadSeconds = 0.0;         // 模型加载耗时（秒）
    double warmupSeconds = 0.0;       // 预热阶段总耗时（秒）
    double totalSeconds = 0.0;        // 计时运行总耗时（秒）
    double preprocessSeconds = 0.0;   // 预处理累计耗时（秒）
    double inferenceSeconds = 0.0;    // 推理累计耗时（秒）
    double postprocessSeconds = 0.0;  // 后处理累计耗时（秒）
};

// 作用域流静默器 —— RAII 方式临时重定向 ostream 到空缓冲区（用于屏蔽第三方库日志输出）
class ScopedStreamSilencer
{
public:
    explicit ScopedStreamSilencer(std::ostream& stream)
        : stream_(stream),
          oldBuffer_(stream.rdbuf(sink_.rdbuf()))
    {
    }

    ~ScopedStreamSilencer()
    {
        stream_.rdbuf(oldBuffer_);
    }

    ScopedStreamSilencer(const ScopedStreamSilencer&) = delete;
    ScopedStreamSilencer& operator=(const ScopedStreamSilencer&) = delete;

private:
    std::ostream& stream_;           // 被静默的流对象
    std::streambuf* oldBuffer_;      // 保存原始缓冲区指针以便恢复
    std::ostringstream sink_;        // 丢弃数据的接收缓冲区
};

#ifdef AIMBOT_HAS_DIRECTML_EX
// DirectML WARP（CPU 软渲染）所需的 D3D12 / DirectML 资源结构体
struct DmlCpuResources
{
    Microsoft::WRL::ComPtr<ID3D12Device> d3d12Device;          // D3D12 设备（WARP 适配器）
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> commandQueue;   // 命令队列（COMPUTE 或 DIRECT 类型）
    Microsoft::WRL::ComPtr<IDMLDevice> dmlDevice;              // DirectML 设备
};
#else
// 当未链接 DirectML.lib 时的空资源占位
struct DmlCpuResources {};
#endif

// 字符串转小写
std::string ToLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

// 判断字符串是否以指定前缀开头
bool StartsWith(const std::string& value, const std::string& prefix)
{
    return value.rfind(prefix, 0) == 0;
}

// 将宽字符字符串（wchar_t*）转换为 UTF-8 编码的 std::string
std::string WideToUtf8Local(const wchar_t* value)
{
    if (!value)
        return {};

    const int required = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1)
        return {};

    std::string out(static_cast<size_t>(required - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, out.data(), required, nullptr, nullptr);
    return out;
}

// 将 HRESULT 错误码格式化为可读的十六进制字符串
std::string HResultToString(HRESULT hr)
{
    std::ostringstream oss;
    oss << "HRESULT 0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr);
    return oss.str();
}

// 将 ProviderKind 枚举转换为用户可读的字符串标识
std::string ProviderName(ProviderKind kind)
{
    switch (kind)
    {
    case ProviderKind::Cpu: return "cpu";
    case ProviderKind::DmlGpu: return "dml-gpu";
    case ProviderKind::DmlCpu: return "dml-cpu";
    }
    return "unknown";
}

// 从字符串解析 ProviderKind 枚举值（支持多种常见别名）
std::optional<ProviderKind> ParseProviderName(const std::string& value)
{
    const std::string name = ToLower(value);
    if (name == "cpu")
        return ProviderKind::Cpu;
    if (name == "dml" || name == "dml-gpu" || name == "dml_gpu" || name == "dml+gpu" || name == "directml")
        return ProviderKind::DmlGpu;
    if (name == "dml-cpu" || name == "dml_cpu" || name == "dml+cpu" || name == "warp" || name == "dml-warp")
        return ProviderKind::DmlCpu;
    return std::nullopt;
}

// 按逗号分割字符串，去除每个子项的前后空白
std::vector<std::string> SplitCommaList(const std::string& value)
{
    std::vector<std::string> out;
    std::stringstream ss(value);
    std::string item;
    while (std::getline(ss, item, ','))
    {
        item.erase(item.begin(), std::find_if(item.begin(), item.end(), [](unsigned char ch) {
            return !std::isspace(ch);
        }));
        item.erase(std::find_if(item.rbegin(), item.rend(), [](unsigned char ch) {
            return !std::isspace(ch);
        }).base(), item.end());
        if (!item.empty())
            out.push_back(item);
    }
    return out;
}

// 解析整数（严格模式：字符串必须完全匹配整数格式）
bool ParseInt(const std::string& text, int* out)
{
    if (!out)
        return false;

    try
    {
        size_t used = 0;
        const int value = std::stoi(text, &used, 10);
        if (used != text.size())
            return false;
        *out = value;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

// 消费命令行参数值 —— 支持 "--opt=value" 和 "--opt value" 两种格式
bool ConsumeValue(int argc, char** argv, int* index, const std::string& arg, const std::string& option, std::string* value)
{
    // 尝试 "--opt=value" 格式
    const std::string prefix = option + "=";
    if (StartsWith(arg, prefix))
    {
        *value = arg.substr(prefix.size());
        return true;
    }

    // 尝试 "--opt value" 格式
    if (arg == option)
    {
        if (*index + 1 >= argc)
            return false;
        const std::string next = argv[*index + 1] ? argv[*index + 1] : "";
        if (StartsWith(next, "--"))
            return false;
        *value = argv[++(*index)];
        return true;
    }

    return false;
}

// 从逗号分隔的字符串中解析 provider 列表并填入 vector
void AddProvidersFromList(const std::string& list, std::vector<ProviderKind>* providers)
{
    if (!providers)
        return;

    providers->clear();
    for (const std::string& item : SplitCommaList(list))
    {
        auto provider = ParseProviderName(item);
        if (provider)
            providers->push_back(*provider);
        else
            std::cerr << "[Benchmark] Unknown provider ignored: " << item << std::endl;
    }
}

// 解析命令行参数，填充 CliOptions 结构体
CliOptions ParseCli(int argc, char** argv)
{
    CliOptions options;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i] ? argv[i] : "";
        std::string value;

        // 帮助选项
        if (arg == "--benchmark-help" || arg == "--bench-help")
        {
            options.benchmarkRequested = true;
            options.help = true;
            continue;
        }
        // 列出设备选项
        if (arg == "--bench-list-devices")
        {
            options.benchmarkRequested = true;
            options.listDevices = true;
            continue;
        }
        // provider 选择（带值参数）
        if (ConsumeValue(argc, argv, &i, arg, "--benchmark-providers", &value) ||
            ConsumeValue(argc, argv, &i, arg, "--bench-providers", &value) ||
            ConsumeValue(argc, argv, &i, arg, "--providers", &value))
        {
            options.benchmarkRequested = true;
            options.runBenchmark = true;
            AddProvidersFromList(value, &options.providers);
            continue;
        }
        // provider 选择（无值参数，使用默认列表）
        if (arg == "--benchmark-providers" || arg == "--bench-providers")
        {
            options.benchmarkRequested = true;
            options.runBenchmark = true;
            continue;
        }
        // 模型路径
        if (ConsumeValue(argc, argv, &i, arg, "--bench-model", &value) ||
            ConsumeValue(argc, argv, &i, arg, "--model", &value))
        {
            options.modelPath = value;
            continue;
        }
        // 输入图像路径
        if (ConsumeValue(argc, argv, &i, arg, "--bench-image", &value) ||
            ConsumeValue(argc, argv, &i, arg, "--image", &value))
        {
            options.imagePath = value;
            continue;
        }
        // 结果 CSV 路径
        if (ConsumeValue(argc, argv, &i, arg, "--bench-results", &value) ||
            ConsumeValue(argc, argv, &i, arg, "--results", &value))
        {
            options.resultsPath = value;
            continue;
        }
        // 计时运行次数
        if (ConsumeValue(argc, argv, &i, arg, "--bench-runs", &value) ||
            ConsumeValue(argc, argv, &i, arg, "--runs", &value))
        {
            ParseInt(value, &options.runs);
            continue;
        }
        // 预热运行次数
        if (ConsumeValue(argc, argv, &i, arg, "--bench-warmup", &value) ||
            ConsumeValue(argc, argv, &i, arg, "--warmup", &value))
        {
            ParseInt(value, &options.warmupRuns);
            continue;
        }
        // 输入分辨率
        if (ConsumeValue(argc, argv, &i, arg, "--bench-resolution", &value) ||
            ConsumeValue(argc, argv, &i, arg, "--resolution", &value))
        {
            ParseInt(value, &options.resolution);
            continue;
        }
        // 批处理大小
        if (ConsumeValue(argc, argv, &i, arg, "--bench-batch", &value) ||
            ConsumeValue(argc, argv, &i, arg, "--batch", &value))
        {
            ParseInt(value, &options.batch);
            continue;
        }
        // DML 设备 ID
        if (ConsumeValue(argc, argv, &i, arg, "--bench-dml-device", &value) ||
            ConsumeValue(argc, argv, &i, arg, "--dml-device", &value))
        {
            ParseInt(value, &options.dmlDeviceId);
            continue;
        }
        // 禁用后处理
        if (arg == "--bench-no-postprocess")
        {
            options.postprocess = false;
            continue;
        }
        // 禁用 CPU EP 回退
        if (arg == "--bench-disable-cpu-fallback")
        {
            options.disableCpuFallback = true;
            continue;
        }
        // 不保存结果
        if (arg == "--bench-no-save")
        {
            options.saveResults = false;
            continue;
        }
    }

    // 如果未指定 provider，默认测试全部三种
    if (options.providers.empty())
    {
        options.providers = {
            ProviderKind::Cpu,
            ProviderKind::DmlGpu,
            ProviderKind::DmlCpu
        };
    }

    return options;
}

// 检查 ONNX Runtime 可用 provider 列表中是否包含指定 provider
bool HasOrtProvider(const std::vector<std::string>& availableProviders, const std::string& provider)
{
    return std::find(availableProviders.begin(), availableProviders.end(), provider) != availableProviders.end();
}

// 将字符串 vector 以指定分隔符拼接为单个字符串
std::string JoinStrings(const std::vector<std::string>& values, const std::string& separator)
{
    std::ostringstream oss;
    for (size_t i = 0; i < values.size(); ++i)
    {
        if (i != 0)
            oss << separator;
        oss << values[i];
    }
    return oss.str();
}

// 对 CSV 字段进行转义处理 —— 去除不可见字符、对包含逗号/引号的字段加双引号
std::string CsvEscape(const std::string& value)
{
    std::string sanitized;
    sanitized.reserve(value.size());
    for (unsigned char ch : value)
    {
        if (ch == '\n' || ch == '\r')
            sanitized += ' ';
        else if (ch >= 32 && ch < 127)
            sanitized += static_cast<char>(ch);
        else if (ch == '\t')
            sanitized += ' ';
    }

    // 特殊处理：为常见的 HRESULT 错误码附加可读说明
    if (value.find("80070057") != std::string::npos &&
        sanitized.find("invalid parameter") == std::string::npos)
    {
        sanitized += " (invalid parameter)";
    }

    // 如果不需要转义则直接返回
    if (sanitized.find_first_of(",\"") == std::string::npos)
        return sanitized;

    // 包含逗号或引号时，用双引号包裹并转义内部双引号
    std::string escaped = "\"";
    for (char ch : sanitized)
    {
        if (ch == '"')
            escaped += "\"\"";
        else
            escaped += ch;
    }
    escaped += '"';
    return escaped;
}

// 去除字符串首尾空白字符
std::string TrimWhitespace(std::string value)
{
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), value.end());
    return value;
}

// 为路径添加引号，用于命令行拼接（转义内部双引号）
std::string QuoteForCommand(const std::filesystem::path& path)
{
    std::string value = path.string();
    std::string quoted = "\"";
    for (char ch : value)
    {
        if (ch == '"')
            quoted += "\\\"";
        else
            quoted += ch;
    }
    quoted += '"';
    return quoted;
}

// 执行系统命令并捕获其标准输出（跨平台：Windows 用 _popen，Unix 用 popen）
std::string CaptureCommandOutput(const std::string& command)
{
    std::array<char, 256> buffer{};
    std::string output;

#ifdef _WIN32
    FILE* pipe = _popen(command.c_str(), "r");
#else
    FILE* pipe = popen(command.c_str(), "r");
#endif
    if (!pipe)
        return {};

    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe))
    {
        output += buffer.data();
    }

#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return TrimWhitespace(output);
}

// 从当前目录向上查找包含 .git 文件夹的仓库根目录
std::filesystem::path FindRepoRoot()
{
    std::error_code ec;
    std::filesystem::path path = std::filesystem::current_path(ec);
    if (ec)
        return {};

    while (!path.empty())
    {
        if (std::filesystem::exists(path / ".git"))
            return path;
        if (!path.has_parent_path() || path.parent_path() == path)
            break;
        path = path.parent_path();
    }

    return {};
}

// 获取 Git 仓库的短提交 ID（commit hash）
std::string GetGitCommitId(const std::filesystem::path& repoRoot)
{
    if (repoRoot.empty())
        return "unknown";

    std::string commit = CaptureCommandOutput("git -C " + QuoteForCommand(repoRoot) + " rev-parse --short HEAD 2>NUL");
    return commit.empty() ? "unknown" : commit;
}

// 检查 Git 仓库是否有未提交的修改
bool GetGitDirty(const std::filesystem::path& repoRoot)
{
    if (repoRoot.empty())
        return false;

    std::string status = CaptureCommandOutput("git -C " + QuoteForCommand(repoRoot) + " status --porcelain 2>NUL");
    return !status.empty();
}

// 获取当前本地时间的格式化字符串（YYYY-MM-DD HH:MM:SS，跨平台）
std::string CurrentTimestampLocal()
{
    const std::time_t now = std::time(nullptr);
    std::tm localTime{};
#ifdef _WIN32
    localtime_s(&localTime, &now);
#else
    localtime_r(&now, &localTime);
#endif
    std::ostringstream oss;
    oss << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

// 打印命令行帮助信息
void PrintHelp()
{
    std::cout
        << "Usage:\n"
        << "  Xen.exe --benchmark-providers [cpu,dml-gpu,dml-cpu] [options]\n\n"
        << "Options:\n"
        << "  --bench-model <path>             ONNX model path. Defaults to config ai_model if it is .onnx, otherwise first models/*.onnx.\n"
        << "  --bench-image <path>             Image to preprocess for every run. Defaults to deterministic synthetic input.\n"
        << "  --bench-results <path>           CSV append path. Relative paths resolve from the repository root.\n"
        << "                                   Default: benchmark_results/provider_benchmark.csv.\n"
        << "  --bench-runs <n>                 Measured runs. Default: 100.\n"
        << "  --bench-warmup <n>               Warmup runs before timing. Default: 10.\n"
        << "  --bench-resolution <n>           Dynamic input size. Default: config detection_resolution.\n"
        << "  --bench-batch <n>                Requested batch size. Static-batch models keep their own batch.\n"
        << "  --bench-dml-device <id>          DXGI adapter id for dml-gpu. Default: config dml_device_id.\n"
        << "  --bench-no-postprocess           Measure preprocess + session.Run only.\n"
        << "  --bench-disable-cpu-fallback     Disable ORT fallback to CPU EP for non-CPU providers.\n"
        << "  --bench-no-save                  Do not append the summary rows to CSV.\n"
        << "  --bench-list-devices             Print DXGI adapter ids and exit.\n"
        << "  --benchmark-help                 Show this help.\n\n"
        << "Examples:\n"
        << "  Xen.exe --benchmark-providers\n"
        << "  Xen.exe --benchmark-providers cpu,dml-gpu --bench-runs 200 --bench-warmup 20\n";
}

// 根据设备 ID 查询 DXGI 适配器名称
std::string AdapterName(int deviceId)
{
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return "unknown";

    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumAdapters1(static_cast<UINT>(deviceId), &adapter)))
        return "invalid";

    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(adapter->GetDesc1(&desc)))
        return "unknown";

    return WideToUtf8Local(desc.Description);
}

// 打印所有可用的 DXGI 适配器列表（供 DML GPU 使用），同时提示 WARP 选项
void PrintDxgiAdapters()
{
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr))
    {
        std::cout << "DXGI adapters: unavailable (" << HResultToString(hr) << ")\n";
        return;
    }

    std::cout << "DXGI adapters for DML GPU:\n";
    for (UINT i = 0;; ++i)
    {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        hr = factory->EnumAdapters1(i, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND)
            break;
        if (FAILED(hr))
            break;

        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc)))
            continue;

        const bool software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        std::cout << "  id=" << i
                  << " name=" << WideToUtf8Local(desc.Description)
                  << " dedicated_vram_mb=" << (desc.DedicatedVideoMemory / (1024 * 1024))
                  << (software ? " software" : "")
                  << "\n";
    }
    std::cout << "DML CPU uses the WARP software adapter through DirectML when available.\n";
}

// 收集所有候选的 ONNX 模型路径 —— 遍历配置文件、models 目录、以及显式指定的路径
std::vector<std::filesystem::path> CollectModelCandidates(const CliOptions& options)
{
    if (!options.modelPath.empty())
        return { std::filesystem::path(options.modelPath) };

    std::vector<std::filesystem::path> candidates;
    auto addCandidate = [&candidates](const std::filesystem::path& path)
    {
        if (path.empty() || ToLower(path.extension().string()) != ".onnx" || !std::filesystem::exists(path))
            return;
        const std::filesystem::path normalized = path.lexically_normal();
        if (std::find(candidates.begin(), candidates.end(), normalized) == candidates.end())
            candidates.push_back(normalized);
    };

    // 从 config.ai_model 添加候选
    if (!config.ai_model.empty())
    {
        std::filesystem::path configured = std::filesystem::path("models") / config.ai_model;
        if (ToLower(configured.extension().string()) == ".onnx")
        {
            addCandidate(configured);
        }
        else
        {
            addCandidate(configured.replace_extension(".onnx"));
        }
    }

    // 扫描 models 目录下所有 .onnx 文件
    std::filesystem::path modelsDir("models");
    if (std::filesystem::exists(modelsDir))
    {
        // 注意：这里 shadow 了外部 candidates，但后续通过 addCandidate 写入的是外部 vector
        std::vector<std::filesystem::path> candidates;
        for (const auto& entry : std::filesystem::directory_iterator(modelsDir))
        {
            if (entry.is_regular_file() && ToLower(entry.path().extension().string()) == ".onnx")
                candidates.push_back(entry.path());
        }

        if (!candidates.empty())
        {
            std::sort(candidates.begin(), candidates.end());
            for (const auto& candidate : candidates)
                addCandidate(candidate);
        }
    }

    return candidates;
}

// 加载基准测试输入帧 —— 优先加载指定图片，否则生成随机合成图像
cv::Mat LoadBenchmarkFrame(const CliOptions& options)
{
    if (!options.imagePath.empty())
    {
        cv::Mat image = cv::imread(options.imagePath, cv::IMREAD_COLOR);
        if (!image.empty())
            return image;
        std::cerr << "[Benchmark] Failed to read image, using synthetic input: " << options.imagePath << std::endl;
    }

    // 生成随机合成图像（确定种子以保证可复现性）
    cv::Mat synthetic(options.resolution, options.resolution, CV_8UC3);
    cv::RNG rng(0x5A17);
    rng.fill(synthetic, cv::RNG::UNIFORM, 0, 256);
    return synthetic;
}

// 单帧预处理：将 OpenCV Mat 转换为 NCHW 布局的 float 张量
// 支持 BGR 3通道 / BGRA 4通道 / 灰度 1通道输入，自动缩放和归一化到 [0,1]
void PreprocessFrameToTensor(
    const cv::Mat& frame,
    float* dst,
    int targetW,
    int targetH,
    PreprocessWorkspace* workspace)
{
    if (!dst || targetW <= 0 || targetH <= 0 || !workspace)
        return;

    const size_t channelSize = static_cast<size_t>(targetW) * static_cast<size_t>(targetH);
    cv::Mat rgbPlanes[3] = {
        cv::Mat(targetH, targetW, CV_32F, dst),
        cv::Mat(targetH, targetW, CV_32F, dst + channelSize),
        cv::Mat(targetH, targetW, CV_32F, dst + channelSize * 2)
    };

    // 清空张量的 lambda 辅助函数
    auto clearTensor = [&]()
    {
        rgbPlanes[0].setTo(0.0f);
        rgbPlanes[1].setTo(0.0f);
        rgbPlanes[2].setTo(0.0f);
    };

    if (frame.empty())
    {
        clearTensor();
        return;
    }

    // 灰度图处理：缩放到目标尺寸后复制到所有 RGB 通道
    if (frame.channels() == 1)
    {
        cv::Mat grayResized;
        if (frame.cols != targetW || frame.rows != targetH)
        {
            cv::resize(frame, workspace->grayResizeBuffer, cv::Size(targetW, targetH), 0, 0, cv::INTER_LINEAR);
            grayResized = workspace->grayResizeBuffer;
        }
        else
        {
            grayResized = frame;
        }

        grayResized.convertTo(workspace->grayFloatBuffer, CV_32F, 1.0f / 255.0f);
        workspace->grayFloatBuffer.copyTo(rgbPlanes[0]);
        workspace->grayFloatBuffer.copyTo(rgbPlanes[1]);
        workspace->grayFloatBuffer.copyTo(rgbPlanes[2]);
        return;
    }

    // 处理 BGR / BGRA 多通道图像
    cv::Mat bgrFrame;
    switch (frame.channels())
    {
    case 4:
        cv::cvtColor(frame, workspace->bgrBuffer, cv::COLOR_BGRA2BGR);
        bgrFrame = workspace->bgrBuffer;
        break;
    case 3:
        bgrFrame = frame;
        break;
    default:
        clearTensor();
        return;
    }

    // 缩放到目标尺寸
    cv::Mat resizedBgr;
    if (bgrFrame.cols != targetW || bgrFrame.rows != targetH)
    {
        cv::resize(bgrFrame, workspace->resizeBuffer, cv::Size(targetW, targetH), 0, 0, cv::INTER_LINEAR);
        resizedBgr = workspace->resizeBuffer;
    }
    else
    {
        resizedBgr = bgrFrame;
    }

    // 归一化到 [0,1] 浮点范围
    resizedBgr.convertTo(workspace->floatBuffer, CV_32FC3, 1.0f / 255.0f);

    // BGR 转 RGB 并拆分为独立通道（NCHW 布局）
    cv::Mat bgrToRgbPlanes[3] = {
        rgbPlanes[2],
        rgbPlanes[1],
        rgbPlanes[0]
    };
    cv::split(workspace->floatBuffer, bgrToRgbPlanes);
}

// 批处理预处理：对 batch 个帧重复执行 PreprocessFrameToTensor
void PreprocessBatch(
    const cv::Mat& frame,
    int batch,
    int targetW,
    int targetH,
    std::vector<float>* tensor,
    PreprocessWorkspace* workspace)
{
    const size_t frameTensorSize = static_cast<size_t>(3) * static_cast<size_t>(targetH) * static_cast<size_t>(targetW);
    tensor->resize(static_cast<size_t>(batch) * frameTensorSize);
    for (int b = 0; b < batch; ++b)
    {
        float* dst = tensor->data() + static_cast<size_t>(b) * frameTensorSize;
        PreprocessFrameToTensor(frame, dst, targetW, targetH, workspace);
    }
}

// 安全地将 int64_t 转换为 int（检查范围溢出）
bool TryInt64ToInt(int64_t value, int* out)
{
    if (!out)
        return false;
    if (value < static_cast<int64_t>(std::numeric_limits<int>::min()) ||
        value > static_cast<int64_t>(std::numeric_limits<int>::max()))
    {
        return false;
    }
    *out = static_cast<int>(value);
    return true;
}

// Sigmoid 激活函数（数值稳定版本）
float SigmoidFloat(float x)
{
    if (x >= 0.0f)
    {
        const float z = std::exp(-x);
        return 1.0f / (1.0f + z);
    }
    const float z = std::exp(x);
    return z / (1.0f + z);
}

// Softplus 激活函数（数值稳定版本，用于边界框回归）
float SoftplusFloat(float x)
{
    if (x > 20.0f) return x;
    if (x < -20.0f) return std::exp(x);
    return std::log1p(std::exp(x));
}

// 检查张量形状是否为合法的 NCHW 4D 格式且所有维度正数
bool IsShape4(const std::vector<int64_t>& shape)
{
    return shape.size() == 4 && shape[0] > 0 && shape[1] > 0 && shape[2] > 0 && shape[3] > 0;
}

// 计算 NCHW 布局张量中元素的一维偏移量
size_t NchwOffset(int batch, int channel, int y, int x, int channels, int height, int width)
{
    return (((static_cast<size_t>(batch) * static_cast<size_t>(channels) + static_cast<size_t>(channel))
        * static_cast<size_t>(height) + static_cast<size_t>(y)) * static_cast<size_t>(width))
        + static_cast<size_t>(x);
}

// 在输出名称列表中查找目标名称的索引
int FindOutputIndex(const std::vector<std::string>& names, const char* wanted)
{
    for (int i = 0; i < static_cast<int>(names.size()); ++i)
    {
        if (names[i] == wanted)
            return i;
    }
    return -1;
}

// 解码 SunPoint 模型的原始输出 —— 热力图峰值检测 + 边界框回归 + 偏移量校正
// 输出经 NMS 后的 Detection 列表
std::vector<Detection> DecodeSunPointRaw(
    const float* heat,
    const std::vector<int64_t>& heatShape,
    const float* box,
    const std::vector<int64_t>& boxShape,
    const float* offset,
    const std::vector<int64_t>& offsetShape,
    int batchIndex,
    int targetW,
    int targetH,
    float confThreshold,
    float nmsThreshold)
{
    std::vector<Detection> detections;
    if (!heat || !box || !offset || !IsShape4(heatShape) || !IsShape4(boxShape) || !IsShape4(offsetShape))
        return detections;

    // 提取张量维度信息
    const int batch = static_cast<int>(heatShape[0]);
    const int classes = static_cast<int>(heatShape[1]);
    const int gridH = static_cast<int>(heatShape[2]);
    const int gridW = static_cast<int>(heatShape[3]);
    if (batchIndex < 0 || batchIndex >= batch || classes <= 0 || gridH <= 0 || gridW <= 0)
        return detections;
    if (boxShape[0] != heatShape[0] || boxShape[1] != 4 || boxShape[2] != heatShape[2] || boxShape[3] != heatShape[3])
        return detections;
    if (offsetShape[0] != heatShape[0] || offsetShape[1] != 2 || offsetShape[2] != heatShape[2] || offsetShape[3] != heatShape[3])
        return detections;

    // 计算网格到原始图像坐标的步长
    const float strideX = static_cast<float>(targetW) / static_cast<float>(gridW);
    const float strideY = static_cast<float>(targetH) / static_cast<float>(gridH);
    detections.reserve(static_cast<size_t>(std::max(config.max_detections, 16)));

    // 遍历每个类别和网格单元
    for (int c = 0; c < classes; ++c)
    {
        for (int y = 0; y < gridH; ++y)
        {
            for (int x = 0; x < gridW; ++x)
            {
                const size_t heatIdx = NchwOffset(batchIndex, c, y, x, classes, gridH, gridW);
                const float heatLogit = heat[heatIdx];
                const float score = SigmoidFloat(heatLogit);
                if (score <= confThreshold)
                    continue;

                // 3x3 局部非极大值抑制 —— 确保检测点是热力图局部峰值
                bool isPeak = true;
                for (int yy = std::max(0, y - 1); yy <= std::min(gridH - 1, y + 1) && isPeak; ++yy)
                {
                    for (int xx = std::max(0, x - 1); xx <= std::min(gridW - 1, x + 1); ++xx)
                    {
                        if (yy == y && xx == x)
                            continue;
                        const size_t neighborIdx = NchwOffset(batchIndex, c, yy, xx, classes, gridH, gridW);
                        if (heat[neighborIdx] > heatLogit)
                        {
                            isPeak = false;
                            break;
                        }
                    }
                }
                if (!isPeak)
                    continue;

                // 偏移量校正（Sigmoid 归一化到 [0,1]）
                const float offX = SigmoidFloat(offset[NchwOffset(batchIndex, 0, y, x, 2, gridH, gridW)]);
                const float offY = SigmoidFloat(offset[NchwOffset(batchIndex, 1, y, x, 2, gridH, gridW)]);
                const float centerX = (static_cast<float>(x) + offX) * strideX;
                const float centerY = (static_cast<float>(y) + offY) * strideY;

                // 边界框回归（Softplus 确保正数）
                const float left = SoftplusFloat(box[NchwOffset(batchIndex, 0, y, x, 4, gridH, gridW)]) * strideX;
                const float top = SoftplusFloat(box[NchwOffset(batchIndex, 1, y, x, 4, gridH, gridW)]) * strideY;
                const float right = SoftplusFloat(box[NchwOffset(batchIndex, 2, y, x, 4, gridH, gridW)]) * strideX;
                const float bottom = SoftplusFloat(box[NchwOffset(batchIndex, 3, y, x, 4, gridH, gridW)]) * strideY;

                const float x1 = std::clamp(centerX - left, 0.0f, static_cast<float>(targetW));
                const float y1 = std::clamp(centerY - top, 0.0f, static_cast<float>(targetH));
                const float x2 = std::clamp(centerX + right, 0.0f, static_cast<float>(targetW));
                const float y2 = std::clamp(centerY + bottom, 0.0f, static_cast<float>(targetH));
                if (x2 <= x1 || y2 <= y1)
                    continue;

                cv::Rect rect;
                rect.x = static_cast<int>(x1);
                rect.y = static_cast<int>(y1);
                rect.width = std::max(1, static_cast<int>(x2 - x1));
                rect.height = std::max(1, static_cast<int>(y2 - y1));
                detections.push_back(Detection{ rect, score, c });
            }
        }
    }

    // 超过最大检测数时按置信度截断
    if (config.max_detections > 0 && detections.size() > static_cast<size_t>(config.max_detections))
    {
        const auto kth = detections.begin() + config.max_detections;
        std::nth_element(
            detections.begin(),
            kth,
            detections.end(),
            [](const Detection& a, const Detection& b) { return a.confidence > b.confidence; });
        detections.resize(static_cast<size_t>(config.max_detections));
    }

    // 按置信度降序排列并执行 NMS
    std::sort(
        detections.begin(),
        detections.end(),
        [](const Detection& a, const Detection& b) { return a.confidence > b.confidence; });
    NMS(detections, nmsThreshold);
    return detections;
}

// 后处理管线：将 ONNX Runtime 输出张量转换为检测结果
// 支持 SunPoint 模型（heat/box/offset）和标准 YOLO 输出格式
size_t PostprocessOutputs(
    std::vector<Ort::Value>& outputTensors,
    const ModelInfo& info,
    int targetW,
    int targetH,
    int outputResolution)
{
    if (outputTensors.empty())
        return 0;

    const float confThreshold = config.confidence_threshold;
    const float nmsThreshold = config.nms_threshold;
    size_t detectionCount = 0;

    // 尝试 SunPoint 解码路径（三个命名输出: heat, box, offset）
    const int heatIdx = FindOutputIndex(info.outputNames, "heat");
    const int boxIdx = FindOutputIndex(info.outputNames, "box");
    const int offsetIdx = FindOutputIndex(info.outputNames, "offset");
    if (heatIdx >= 0 && boxIdx >= 0 && offsetIdx >= 0 &&
        heatIdx < static_cast<int>(outputTensors.size()) &&
        boxIdx < static_cast<int>(outputTensors.size()) &&
        offsetIdx < static_cast<int>(outputTensors.size()))
    {
        float* heatData = outputTensors[heatIdx].GetTensorMutableData<float>();
        float* boxData = outputTensors[boxIdx].GetTensorMutableData<float>();
        float* offsetData = outputTensors[offsetIdx].GetTensorMutableData<float>();
        const std::vector<int64_t> heatShape = outputTensors[heatIdx].GetTensorTypeAndShapeInfo().GetShape();
        const std::vector<int64_t> boxShape = outputTensors[boxIdx].GetTensorTypeAndShapeInfo().GetShape();
        const std::vector<int64_t> offsetShape = outputTensors[offsetIdx].GetTensorTypeAndShapeInfo().GetShape();
        const int batch = IsShape4(heatShape) ? static_cast<int>(heatShape[0]) : info.batch;

        for (int b = 0; b < batch; ++b)
        {
            auto detections = DecodeSunPointRaw(
                heatData, heatShape, boxData, boxShape, offsetData, offsetShape,
                b, targetW, targetH, confThreshold, nmsThreshold);
            detectionCount += detections.size();
        }
        return detectionCount;
    }

    // 通用 YOLO 格式输出解码路径（单输出张量）
    Ort::TensorTypeAndShapeInfo outInfo = outputTensors.front().GetTensorTypeAndShapeInfo();
    std::vector<int64_t> outShape = outInfo.GetShape();
    if (outShape.size() < 2)
        return 0;

    float* outData = outputTensors.front().GetTensorMutableData<float>();
    if (!outData)
        return 0;

    int batch = 1;
    int rows = 0;
    int cols = 0;
    if (outShape.size() == 3)
    {
        if (!TryInt64ToInt(outShape[0], &batch) ||
            !TryInt64ToInt(outShape[1], &rows) ||
            !TryInt64ToInt(outShape[2], &cols))
        {
            return 0;
        }
    }
    else
    {
        if (!TryInt64ToInt(outShape[0], &rows) ||
            !TryInt64ToInt(outShape[1], &cols))
        {
            return 0;
        }
    }

    if (batch <= 0 || rows <= 0 || cols <= 0)
        return 0;

    // 类别数 = 行数 - 4（边界框坐标）
    const int numClasses = rows - 4;
    const size_t frameOutputSize = static_cast<size_t>(rows) * static_cast<size_t>(cols);
    for (int b = 0; b < batch; ++b)
    {
        const float* ptr = outData + static_cast<size_t>(b) * frameOutputSize;
        std::vector<int64_t> shape2d = { rows, cols };
        auto detections = postProcessYoloDML(ptr, shape2d, numClasses, confThreshold, nmsThreshold);

        // 如果模型输出分辨率与目标分辨率不同，进行坐标缩放
        if (targetW != outputResolution || targetH != outputResolution)
        {
            const float scaleX = static_cast<float>(outputResolution) / static_cast<float>(targetW);
            const float scaleY = static_cast<float>(outputResolution) / static_cast<float>(targetH);
            for (auto& det : detections)
            {
                det.box.x = static_cast<int>(det.box.x * scaleX);
                det.box.y = static_cast<int>(det.box.y * scaleY);
                det.box.width = static_cast<int>(det.box.width * scaleX);
                det.box.height = static_cast<int>(det.box.height * scaleY);
            }
        }

        detectionCount += detections.size();
    }

    return detectionCount;
}

// 配置 ONNX Runtime 会话的通用选项（图优化、执行模式、日志级别等）
void ConfigureCommonSessionOptions(Ort::SessionOptions& sessionOptions, const CliOptions& options, ProviderKind kind)
{
    sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    sessionOptions.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    sessionOptions.SetLogSeverityLevel(static_cast<int>(ORT_LOGGING_LEVEL_FATAL));

    // DML provider 专用优化：禁用内存模式、单线程
    if (kind == ProviderKind::DmlGpu || kind == ProviderKind::DmlCpu)
    {
        sessionOptions.DisableMemPattern();
        sessionOptions.SetIntraOpNumThreads(1);
        sessionOptions.SetInterOpNumThreads(1);
    }

    // 非 CPU provider 可禁用 CPU EP 回退
    if (options.disableCpuFallback && kind != ProviderKind::Cpu)
    {
        sessionOptions.AddConfigEntry("session.disable_cpu_ep_fallback", "1");
    }
}

#ifdef AIMBOT_HAS_DIRECTML_EX
// 创建 DirectML WARP（CPU 软渲染）所需的 D3D12 + DirectML 资源
// 包括 WARP 适配器枚举、D3D12 设备创建、命令队列创建、DML 设备创建
bool CreateDmlWarpResources(DmlCpuResources* resources, std::string* error)
{
    if (!resources)
        return false;

    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr))
    {
        if (error) *error = "CreateDXGIFactory1 failed: " + HResultToString(hr);
        return false;
    }

    // 获取 WARP 软件适配器
    Microsoft::WRL::ComPtr<IDXGIAdapter> warpAdapter;
    hr = factory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter));
    if (FAILED(hr))
    {
        if (error) *error = "EnumWarpAdapter failed: " + HResultToString(hr);
        return false;
    }

    // 创建 D3D12 设备（WARP）
    hr = D3D12CreateDevice(warpAdapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&resources->d3d12Device));
    if (FAILED(hr))
    {
        if (error) *error = "D3D12CreateDevice(WARP) failed: " + HResultToString(hr);
        return false;
    }

    // 创建命令队列（优先 COMPUTE 类型，失败则回退 DIRECT 类型）
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    hr = resources->d3d12Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&resources->commandQueue));
    if (FAILED(hr))
    {
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr = resources->d3d12Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&resources->commandQueue));
    }
    if (FAILED(hr))
    {
        if (error) *error = "CreateCommandQueue(WARP) failed: " + HResultToString(hr);
        return false;
    }

    // 创建 DirectML 设备
    hr = DMLCreateDevice(resources->d3d12Device.Get(), DML_CREATE_DEVICE_FLAG_NONE, IID_PPV_ARGS(&resources->dmlDevice));
    if (FAILED(hr))
    {
        if (error) *error = "DMLCreateDevice(WARP) failed: " + HResultToString(hr);
        return false;
    }

    return true;
}
#endif

// 根据 provider 类型向 ONNX Runtime 会话选项追加对应的执行提供程序
void AppendProvider(Ort::SessionOptions& sessionOptions, ProviderKind kind, const CliOptions& options, DmlCpuResources* dmlCpuResources)
{
    switch (kind)
    {
    case ProviderKind::Cpu:
        // CPU provider 是默认的，无需显式追加
        return;
    case ProviderKind::DmlGpu:
    {
        const int deviceId = options.dmlDeviceId >= 0 ? options.dmlDeviceId : config.dml_device_id;
        Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_DML(sessionOptions, deviceId));
        return;
    }
    case ProviderKind::DmlCpu:
    {
#ifdef AIMBOT_HAS_DIRECTML_EX
        // DML CPU 使用 WARP 软渲染，需创建专用 D3D12/DML 资源
        std::string error;
        if (!CreateDmlWarpResources(dmlCpuResources, &error))
            throw std::runtime_error(error);
        Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProviderEx_DML(
            sessionOptions,
            dmlCpuResources->dmlDevice.Get(),
            dmlCpuResources->commandQueue.Get()));
        return;
#else
        throw std::runtime_error("DirectML WARP support is unavailable because DirectML.lib was not linked.");
#endif
    }
    }
}

// 读取 ONNX 模型的输入输出元信息，包括名称、形状、数据类型等
ModelInfo ReadModelInfo(Ort::Session& session, Ort::AllocatorWithDefaultOptions& allocator, const CliOptions& options)
{
    ModelInfo info;

    // 读取输入张量名称
    auto inputName = session.GetInputNameAllocated(0, allocator);
    info.inputName = inputName.get();

    // 检查输入张量类型是否为 float32
    Ort::TypeInfo inputTypeInfo = session.GetInputTypeInfo(0);
    auto inputTensorInfo = inputTypeInfo.GetTensorTypeAndShapeInfo();
    if (inputTensorInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
        throw std::runtime_error("Only float32 model inputs are supported by the provider benchmark.");

    // 检查输入形状是否为 4D NCHW
    std::vector<int64_t> inputShape = inputTensorInfo.GetShape();
    if (inputShape.size() != 4)
        throw std::runtime_error("Only NCHW 4D model inputs are supported by the provider benchmark.");

    // 解析 batch 维度（动态或静态）
    if (inputShape[0] > 0)
    {
        if (!TryInt64ToInt(inputShape[0], &info.batch) || info.batch <= 0)
            throw std::runtime_error("Invalid static batch size in model input.");
    }
    else
    {
        info.batch = options.batch;
    }

    // 解析 channels 维度（固定为 3）
    if (inputShape[1] > 0)
    {
        if (!TryInt64ToInt(inputShape[1], &info.channels) || info.channels != 3)
            throw std::runtime_error("Only 3-channel NCHW model inputs are supported by the provider benchmark.");
    }
    else
    {
        info.channels = 3;
    }

    // 解析输入高度（动态或静态）
    if (inputShape[2] > 0)
    {
        if (!TryInt64ToInt(inputShape[2], &info.inputH) || info.inputH <= 0)
            throw std::runtime_error("Invalid static input height in model.");
    }
    else
    {
        info.inputH = options.resolution;
    }

    // 解析输入宽度（动态或静态）
    if (inputShape[3] > 0)
    {
        if (!TryInt64ToInt(inputShape[3], &info.inputW) || info.inputW <= 0)
            throw std::runtime_error("Invalid static input width in model.");
    }
    else
    {
        info.inputW = options.resolution;
    }

    // 读取所有输出张量名称
    const size_t outputCount = session.GetOutputCount();
    if (outputCount == 0)
        throw std::runtime_error("Model has no outputs.");

    info.outputNames.reserve(outputCount);
    for (size_t i = 0; i < outputCount; ++i)
    {
        auto outputName = session.GetOutputNameAllocated(i, allocator);
        info.outputNames.emplace_back(outputName.get());
    }

    // 构建 C 字符串指针数组（供 Ort::Session::Run 使用）
    info.outputNamePtrs.reserve(info.outputNames.size());
    for (const auto& name : info.outputNames)
    {
        info.outputNamePtrs.push_back(name.c_str());
    }

    return info;
}

// 检查指定 provider 是否在 ONNX Runtime 的可用 provider 列表中
bool IsProviderAvailableForOrt(const std::vector<std::string>& availableProviders, ProviderKind provider)
{
    switch (provider)
    {
    case ProviderKind::Cpu:
        return true;
    case ProviderKind::DmlGpu:
    case ProviderKind::DmlCpu:
        return HasOrtProvider(availableProviders, "DmlExecutionProvider");
    }
    return false;
}

// 尝试用指定 provider 初始化模型，检测兼容性
bool CanInitializeModelForProvider(
    Ort::Env& env,
    const std::filesystem::path& modelPath,
    ProviderKind provider,
    const CliOptions& options)
{
    try
    {
        Ort::SessionOptions sessionOptions;
        ConfigureCommonSessionOptions(sessionOptions, options, provider);

        DmlCpuResources dmlCpuResources;
        AppendProvider(sessionOptions, provider, options, &dmlCpuResources);

        const std::wstring modelPathWide = modelPath.wstring();
        Ort::Session session(env, modelPathWide.c_str(), sessionOptions);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

// 自动选择最合适的 ONNX 模型路径
// 优先显式指定 -> 兼容所有请求 provider 的第一个候选 -> 第一个 ONNX
std::filesystem::path SelectModelPath(
    Ort::Env& env,
    const CliOptions& options,
    const std::vector<std::string>& availableProviders,
    std::string* selectionNote)
{
    const std::vector<std::filesystem::path> candidates = CollectModelCandidates(options);
    if (candidates.empty())
        return {};

    // 显式指定模型路径则直接返回
    if (!options.modelPath.empty())
    {
        if (selectionNote)
            *selectionNote = "explicit";
        return candidates.front();
    }

    // 获取需要探测的 provider 列表
    std::vector<ProviderKind> providersToProbe;
    for (ProviderKind provider : options.providers)
    {
        if (IsProviderAvailableForOrt(availableProviders, provider))
            providersToProbe.push_back(provider);
    }

    // 没有可用的非 CPU provider，直接返回第一个候选
    if (providersToProbe.empty())
    {
        if (selectionNote)
            *selectionNote = "auto:first-onnx";
        return candidates.front();
    }

    // 遍历候选模型，找到第一个能被所有请求 provider 成功初始化的模型
    for (const auto& candidate : candidates)
    {
        bool compatible = true;
        for (ProviderKind provider : providersToProbe)
        {
            if (!CanInitializeModelForProvider(env, candidate, provider, options))
            {
                compatible = false;
                break;
            }
        }

        if (compatible)
        {
            if (selectionNote)
            {
                *selectionNote = (candidate == candidates.front())
                    ? "auto:first-compatible"
                    : "auto:skipped-incompatible-onnx";
            }
            return candidate;
        }
    }

    // 没有找到完全兼容的模型，回退到第一个候选
    if (selectionNote)
        *selectionNote = "auto:no-compatible-candidate; using first ONNX";
    return candidates.front();
}

// 执行单次迭代 —— 预处理、推理、后处理，并记录各阶段耗时
void RunOneIteration(
    Ort::Session& session,
    const ModelInfo& info,
    const CliOptions& options,
    const cv::Mat& frame,
    std::vector<float>* inputTensorValues,
    PreprocessWorkspace* preprocessWorkspace,
    double* preprocessSeconds,
    double* inferenceSeconds,
    double* postprocessSeconds,
    size_t* detections)
{
    auto t0 = Clock::now();
    // 预处理：帧 -> NCHW float 张量
    PreprocessBatch(frame, info.batch, info.inputW, info.inputH, inputTensorValues, preprocessWorkspace);

    std::vector<int64_t> inputShape{
        static_cast<int64_t>(info.batch),
        static_cast<int64_t>(info.channels),
        static_cast<int64_t>(info.inputH),
        static_cast<int64_t>(info.inputW)
    };

    // 创建 ONNX Runtime 输入张量
    Ort::MemoryInfo memoryInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
        memoryInfo,
        inputTensorValues->data(),
        inputTensorValues->size(),
        inputShape.data(),
        inputShape.size());
    auto t1 = Clock::now();

    // ONNX Runtime 推理
    const char* inputNames[] = { info.inputName.c_str() };
    auto outputTensors = session.Run(
        Ort::RunOptions{ nullptr },
        inputNames,
        &inputTensor,
        1,
        info.outputNamePtrs.data(),
        info.outputNamePtrs.size());
    auto t2 = Clock::now();

    // 后处理（可选）
    size_t detectionCount = 0;
    if (options.postprocess)
    {
        detectionCount = PostprocessOutputs(outputTensors, info, info.inputW, info.inputH, options.resolution);
    }
    auto t3 = Clock::now();

    // 累加各阶段耗时
    if (preprocessSeconds)
        *preprocessSeconds += std::chrono::duration<double>(t1 - t0).count();
    if (inferenceSeconds)
        *inferenceSeconds += std::chrono::duration<double>(t2 - t1).count();
    if (postprocessSeconds)
        *postprocessSeconds += std::chrono::duration<double>(t3 - t2).count();
    if (detections)
        *detections = detectionCount;
}

// 运行指定 provider 的完整基准测试 —— 加载、预热、计时迭代
ProviderResult RunProviderBenchmark(
    Ort::Env& env,
    ProviderKind provider,
    const CliOptions& options,
    const std::filesystem::path& modelPath,
    const cv::Mat& frame,
    const std::vector<std::string>& availableProviders)
{
    ProviderResult result;
    result.provider = ProviderName(provider);
    result.providerModel = modelPath.string();
    result.requestedBatch = options.batch;
    result.runs = options.runs;
    result.warmupRuns = options.warmupRuns;

    // 检查 DML provider 是否可用
    if ((provider == ProviderKind::DmlGpu || provider == ProviderKind::DmlCpu) &&
        !HasOrtProvider(availableProviders, "DmlExecutionProvider"))
    {
        result.status = "unavailable";
        result.error = "DmlExecutionProvider is not available in the current ONNX Runtime package/runtime DLLs.";
        return result;
    }

    auto loadStart = Clock::now();
    try
    {
        // 创建并配置 ORT 会话
        Ort::SessionOptions sessionOptions;
        ConfigureCommonSessionOptions(sessionOptions, options, provider);

        DmlCpuResources dmlCpuResources;
        AppendProvider(sessionOptions, provider, options, &dmlCpuResources);

        // 加载模型
        const std::wstring modelPathWide = modelPath.wstring();
        Ort::Session session(env, modelPathWide.c_str(), sessionOptions);
        Ort::AllocatorWithDefaultOptions allocator;
        ModelInfo info = ReadModelInfo(session, allocator, options);

        result.effectiveBatch = info.batch;
        result.inputW = info.inputW;
        result.inputH = info.inputH;
        result.loadSeconds = std::chrono::duration<double>(Clock::now() - loadStart).count();

        std::vector<float> inputTensorValues;
        PreprocessWorkspace preprocessWorkspace;

        // 预热阶段
        auto warmupStart = Clock::now();
        for (int i = 0; i < options.warmupRuns; ++i)
        {
            RunOneIteration(
                session,
                info,
                options,
                frame,
                &inputTensorValues,
                &preprocessWorkspace,
                nullptr,
                nullptr,
                nullptr,
                &result.lastDetections);
        }
        result.warmupSeconds = std::chrono::duration<double>(Clock::now() - warmupStart).count();

        // 计时运行阶段
        auto totalStart = Clock::now();
        for (int i = 0; i < options.runs; ++i)
        {
            RunOneIteration(
                session,
                info,
                options,
                frame,
                &inputTensorValues,
                &preprocessWorkspace,
                &result.preprocessSeconds,
                &result.inferenceSeconds,
                &result.postprocessSeconds,
                &result.lastDetections);
        }
        result.totalSeconds = std::chrono::duration<double>(Clock::now() - totalStart).count();
    }
    // 异常处理 —— 捕获任何运行时错误并记录到结果
    catch (const std::exception& e)
    {
        result.status = "failed";
        result.error = e.what();
        result.loadSeconds = std::chrono::duration<double>(Clock::now() - loadStart).count();
    }

    return result;
}

// 前向声明：模型族名称提取函数
std::string ModelFamilyName(const std::filesystem::path& modelPath);

// 将所有 provider 的基准测试结果打印到控制台
void PrintSummary(
    const CliOptions& options,
    const std::filesystem::path& modelPath,
    const std::string& modelSelection,
    const std::vector<std::string>& availableProviders,
    const std::vector<ProviderResult>& results)
{
    std::cout << "\nProvider benchmark summary (seconds)\n";
    std::cout << "model_family=" << ModelFamilyName(modelPath) << "\n";
    std::cout << "onnx_model=" << modelPath.string() << "\n";
    if (!modelSelection.empty())
        std::cout << "model_selection=" << modelSelection << "\n";
    std::cout << "providers_requested=";
    for (size_t i = 0; i < options.providers.size(); ++i)
    {
        if (i != 0) std::cout << ",";
        std::cout << ProviderName(options.providers[i]);
    }
    std::cout << "\n";
    std::cout << "available_ort_providers=" << JoinStrings(availableProviders, "|") << "\n";
    std::cout << "resolution=" << options.resolution
              << " requested_batch=" << options.batch
              << " runs=" << options.runs
              << " warmup=" << options.warmupRuns
              << " postprocess=" << (options.postprocess ? "true" : "false")
              << " disable_cpu_fallback=" << (options.disableCpuFallback ? "true" : "false")
              << "\n";
    const int dmlDeviceId = options.dmlDeviceId >= 0 ? options.dmlDeviceId : config.dml_device_id;
    std::cout << "dml_gpu_device_id=" << dmlDeviceId
              << " dml_gpu_device_name=" << AdapterName(dmlDeviceId)
              << "\n\n";

    std::cout
        << "provider,provider_model,status,runs,warmup,requested_batch,effective_batch,input_w,input_h,"
        << "load_s,warmup_s,total_s,preprocess_s,inference_s,postprocess_s,avg_run_s,avg_frame_s,fps,last_detections,error\n";

    std::cout << std::fixed << std::setprecision(6);
    for (const ProviderResult& result : results)
    {
        const double avgRun = (result.runs > 0 && result.totalSeconds > 0.0)
            ? result.totalSeconds / static_cast<double>(result.runs)
            : 0.0;
        const double totalFrames = static_cast<double>(std::max(result.runs, 0)) * static_cast<double>(std::max(result.effectiveBatch, 1));
        const double avgFrame = (totalFrames > 0.0 && result.totalSeconds > 0.0)
            ? result.totalSeconds / totalFrames
            : 0.0;
        const double fps = (result.totalSeconds > 0.0) ? totalFrames / result.totalSeconds : 0.0;

        std::cout
            << result.provider << ","
            << CsvEscape(result.providerModel) << ","
            << result.status << ","
            << result.runs << ","
            << result.warmupRuns << ","
            << result.requestedBatch << ","
            << result.effectiveBatch << ","
            << result.inputW << ","
            << result.inputH << ","
            << result.loadSeconds << ","
            << result.warmupSeconds << ","
            << result.totalSeconds << ","
            << result.preprocessSeconds << ","
            << result.inferenceSeconds << ","
            << result.postprocessSeconds << ","
            << avgRun << ","
            << avgFrame << ","
            << fps << ","
            << result.lastDetections << ","
            << CsvEscape(result.error)
            << "\n";
    }
}

// 将请求的 provider 列表格式化为管道分隔的字符串
std::string ProvidersRequestedString(const std::vector<ProviderKind>& providers)
{
    std::ostringstream oss;
    for (size_t i = 0; i < providers.size(); ++i)
    {
        if (i != 0)
            oss << "|";
        oss << ProviderName(providers[i]);
    }
    return oss.str();
}

// 从模型路径中提取模型族名称（不含扩展名的文件名）
std::string ModelFamilyName(const std::filesystem::path& modelPath)
{
    return modelPath.stem().string();
}

// 生成 CSV 文件头字符串
std::string BenchmarkCsvHeader()
{
    return
        "timestamp_local,commit_id,git_dirty,build_backend,model_family,onnx_model,model_selection,providers_requested,"
        "available_ort_providers,resolution,postprocess,disable_cpu_fallback,dml_gpu_device_id,"
        "dml_gpu_device_name,provider,provider_model,status,runs,warmup,requested_batch,"
        "effective_batch,input_w,input_h,load_s,warmup_s,total_s,preprocess_s,inference_s,"
        "postprocess_s,avg_run_s,avg_frame_s,fps,last_detections,error";
}

// 为不兼容的旧 CSV 文件生成归档路径（添加 .legacy 后缀）
std::filesystem::path NextLegacyCsvPath(const std::filesystem::path& csvPath)
{
    const std::filesystem::path parent = csvPath.parent_path();
    const std::string stem = csvPath.stem().string();
    const std::string ext = csvPath.extension().string();

    for (int i = 1; i < 1000; ++i)
    {
        const std::string suffix = (i == 1) ? ".legacy" : ".legacy." + std::to_string(i);
        std::filesystem::path candidate = parent / (stem + suffix + ext);
        if (!std::filesystem::exists(candidate))
            return candidate;
    }

    return parent / (stem + ".legacy.latest" + ext);
}

// 准备 CSV 文件以追加写入 —— 检查表头兼容性，不兼容时自动重命名旧文件
bool PrepareCsvForAppend(
    const std::filesystem::path& csvPath,
    const std::string& expectedHeader,
    bool* writeHeader)
{
    *writeHeader = true;

    std::error_code ec;
    if (!std::filesystem::exists(csvPath, ec))
        return true;

    const uintmax_t size = std::filesystem::file_size(csvPath, ec);
    if (ec)
    {
        std::cerr << "[Benchmark] Failed to inspect results CSV: " << csvPath.string()
                  << " (" << ec.message() << ")" << std::endl;
        return false;
    }
    if (size == 0)
        return true;

    // 读取现有文件第一行（表头）
    std::ifstream existing(csvPath);
    if (!existing)
    {
        std::cerr << "[Benchmark] Failed to read results CSV header: " << csvPath.string() << std::endl;
        return false;
    }

    std::string firstLine;
    std::getline(existing, firstLine);
    if (!firstLine.empty() && firstLine.back() == '\r')
        firstLine.pop_back();

    // 表头匹配则无需写入新表头
    if (firstLine == expectedHeader)
    {
        *writeHeader = false;
        return true;
    }
    existing.close();

    // 表头不匹配：将旧文件重命名为 .legacy 并创建新文件
    const std::filesystem::path legacyPath = NextLegacyCsvPath(csvPath);
    std::filesystem::rename(csvPath, legacyPath, ec);
    if (ec)
    {
        std::cerr << "[Benchmark] Failed to rotate incompatible results CSV: " << csvPath.string()
                  << " -> " << legacyPath.string()
                  << " (" << ec.message() << ")" << std::endl;
        return false;
    }

    return true;
}

// 将所有 provider 的基准测试结果追加写入 CSV 文件
bool AppendBenchmarkCsv(
    const CliOptions& options,
    const std::filesystem::path& modelPath,
    const std::string& modelSelection,
    const std::vector<std::string>& availableProviders,
    const std::vector<ProviderResult>& results,
    std::filesystem::path* writtenPath)
{
    std::filesystem::path csvPath = options.resultsPath.empty()
        ? std::filesystem::path("benchmark_results/provider_benchmark.csv")
        : std::filesystem::path(options.resultsPath);
    const std::filesystem::path repoRoot = FindRepoRoot();
    if (csvPath.is_relative() && !repoRoot.empty())
        csvPath = repoRoot / csvPath;

    std::error_code ec;
    const std::filesystem::path parent = csvPath.parent_path();
    if (!parent.empty())
        std::filesystem::create_directories(parent, ec);
    if (ec)
    {
        std::cerr << "[Benchmark] Failed to create results directory: " << parent.string()
                  << " (" << ec.message() << ")" << std::endl;
        return false;
    }

    const std::string csvHeader = BenchmarkCsvHeader();
    bool writeHeader = true;
    if (!PrepareCsvForAppend(csvPath, csvHeader, &writeHeader))
        return false;

    std::ofstream file(csvPath, std::ios::app);
    if (!file)
    {
        std::cerr << "[Benchmark] Failed to open results CSV: " << csvPath.string() << std::endl;
        return false;
    }

    if (writeHeader)
    {
        file << csvHeader << "\n";
    }

    // 收集公共元数据
    const std::string commitId = GetGitCommitId(repoRoot);
    const bool gitDirty = GetGitDirty(repoRoot);
    const std::string timestamp = CurrentTimestampLocal();
    const std::string requestedProviders = ProvidersRequestedString(options.providers);
    const std::string availableProviderText = JoinStrings(availableProviders, "|");
    const std::string modelFamily = ModelFamilyName(modelPath);
    const int dmlDeviceId = options.dmlDeviceId >= 0 ? options.dmlDeviceId : config.dml_device_id;
    const std::string dmlDeviceName = AdapterName(dmlDeviceId);
    const char* buildBackend = "dml";

    file << std::fixed << std::setprecision(6);
    // 逐行写入每个 provider 的结果
    for (const ProviderResult& result : results)
    {
        const double avgRun = (result.runs > 0 && result.totalSeconds > 0.0)
            ? result.totalSeconds / static_cast<double>(result.runs)
            : 0.0;
        const double totalFrames = static_cast<double>(std::max(result.runs, 0)) * static_cast<double>(std::max(result.effectiveBatch, 1));
        const double avgFrame = (totalFrames > 0.0 && result.totalSeconds > 0.0)
            ? result.totalSeconds / totalFrames
            : 0.0;
        const double fps = (result.totalSeconds > 0.0) ? totalFrames / result.totalSeconds : 0.0;

        file
            << CsvEscape(timestamp) << ","
            << CsvEscape(commitId) << ","
            << (gitDirty ? "true" : "false") << ","
            << buildBackend << ","
            << CsvEscape(modelFamily) << ","
            << CsvEscape(modelPath.string()) << ","
            << CsvEscape(modelSelection) << ","
            << CsvEscape(requestedProviders) << ","
            << CsvEscape(availableProviderText) << ","
            << options.resolution << ","
            << (options.postprocess ? "true" : "false") << ","
            << (options.disableCpuFallback ? "true" : "false") << ","
            << dmlDeviceId << ","
            << CsvEscape(dmlDeviceName) << ","
            << result.provider << ","
            << CsvEscape(result.providerModel) << ","
            << result.status << ","
            << result.runs << ","
            << result.warmupRuns << ","
            << result.requestedBatch << ","
            << result.effectiveBatch << ","
            << result.inputW << ","
            << result.inputH << ","
            << result.loadSeconds << ","
            << result.warmupSeconds << ","
            << result.totalSeconds << ","
            << result.preprocessSeconds << ","
            << result.inferenceSeconds << ","
            << result.postprocessSeconds << ","
            << avgRun << ","
            << avgFrame << ","
            << fps << ","
            << result.lastDetections << ","
            << CsvEscape(result.error)
            << "\n";
    }

    if (writtenPath)
        *writtenPath = csvPath;
    return true;
}
} // namespace

// 检测命令行中是否包含基准测试相关参数（用于早期短路判断）
bool IsProviderBenchmarkRequested(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i] ? argv[i] : "";
        if (arg == "--benchmark-providers" ||
            StartsWith(arg, "--benchmark-providers=") ||
            arg == "--bench-providers" ||
            StartsWith(arg, "--bench-providers=") ||
            arg == "--benchmark-help" ||
            arg == "--bench-help" ||
            arg == "--bench-list-devices")
        {
            return true;
        }
    }
    return false;
}

// DML 分支的基准测试 CLI 入口函数 —— 完成解析、配置、模型选择、运行、输出全流程
int RunProviderBenchmarkCli(int argc, char** argv)
{
    // 设置环境变量和日志级别
    SetEnvironmentVariableA("ORT_LOG_SEVERITY_LEVEL", "4");
    cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_FATAL);

    CliOptions options = ParseCli(argc, argv);
    if (options.help)
    {
        PrintHelp();
        return 0;
    }

    // 仅列举设备（不运行基准测试）
    if (options.listDevices && !options.runBenchmark)
    {
        PrintDxgiAdapters();
        return 0;
    }

    if (!config.loadConfig())
    {
        std::cerr << "[Benchmark] Failed to load config.ini." << std::endl;
        return 2;
    }

    // 如果同时也请求了运行，在加载配置后再次列举设备（打印更完整的上下文）
    if (options.listDevices)
    {
        PrintDxgiAdapters();
        return 0;
    }

    // 参数合法性修正
    if (options.resolution <= 0)
        options.resolution = config.detection_resolution > 0 ? config.detection_resolution : 320;
    options.resolution = std::clamp(options.resolution, 32, 4096);
    options.runs = std::max(1, options.runs);
    options.warmupRuns = std::max(0, options.warmupRuns);
    options.batch = std::max(1, options.batch);
    if (options.dmlDeviceId < 0)
        options.dmlDeviceId = config.dml_device_id;

    // 初始化 ONNX Runtime 环境并查询可用 provider
    Ort::Env env(ORT_LOGGING_LEVEL_FATAL, "provider_benchmark");
    std::vector<std::string> availableProviders;
    try
    {
        availableProviders = Ort::GetAvailableProviders();
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Benchmark] Failed to query ONNX Runtime providers: " << CsvEscape(e.what()) << std::endl;
    }

    // 自动选择模型
    std::string modelSelection;
    std::filesystem::path modelPath = SelectModelPath(env, options, availableProviders, &modelSelection);
    if (modelPath.empty() || !std::filesystem::exists(modelPath))
    {
        std::cerr << "[Benchmark] ONNX model was not found. Pass --bench-model <path> or put an .onnx model in models." << std::endl;
        return 2;
    }
    if (ToLower(modelPath.extension().string()) != ".onnx")
    {
        std::cerr << "[Benchmark] Provider benchmark requires an .onnx model: " << modelPath.string() << std::endl;
        return 2;
    }

    // 准备输入帧
    cv::Mat frame = LoadBenchmarkFrame(options);
    if (frame.empty())
    {
        std::cerr << "[Benchmark] Failed to prepare benchmark input frame." << std::endl;
        return 2;
    }

    // 对所有请求的 provider 逐一执行基准测试
    std::vector<ProviderResult> results;
    results.reserve(options.providers.size());
    for (ProviderKind provider : options.providers)
    {
        results.push_back(RunProviderBenchmark(env, provider, options, modelPath, frame, availableProviders));
    }

    // 打印和保存结果
    PrintSummary(options, modelPath, modelSelection, availableProviders, results);
    if (options.saveResults)
    {
        std::filesystem::path csvPath;
        if (AppendBenchmarkCsv(options, modelPath, modelSelection, availableProviders, results, &csvPath))
        {
            std::cout << "results_csv=" << csvPath.string() << "\n";
            std::cout << "results_csv_base=repo_root\n";
        }
    }
    return 0;
}
} // namespace benchmarks
#endif
