#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>
#include <dshow.h>

#include <algorithm>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "virtual_camera.h"
#include "other_tools.h"

#pragma comment(lib, "strmiids.lib")

namespace
{
// 确保值为偶数（某些摄像头硬件要求偶数尺寸）
inline int even(int v) { return (v % 2 == 0) ? v : v + 1; }

// 摄像头候选信息：显示名称、OpenCV 索引、后端类型
struct CameraCandidate
{
    std::string displayName;
    int index = -1;
    int backend = cv::CAP_ANY;
};

// 摄像头缓存的互斥锁
std::mutex& CameraCacheMutex()
{
    static std::mutex m;
    return m;
}

// 摄像头候选列表缓存
std::vector<CameraCandidate>& CameraCandidateCache()
{
    static std::vector<CameraCandidate> cache;
    return cache;
}

// 摄像头名称缓存
std::vector<std::string>& CameraNameCache()
{
    static std::vector<std::string> cache;
    return cache;
}

// 将后端类型转换为字符串表示
std::string BackendToString(int backend)
{
    if (backend == cv::CAP_DSHOW)
        return "DSHOW";
    if (backend == cv::CAP_MSMF)
        return "MSMF";
    return "ANY";
}

// 使用 DirectShow API 枚举摄像头设备，获取显示名称和设备索引
std::vector<CameraCandidate> EnumerateDirectShowCandidates()
{
    std::vector<CameraCandidate> out;

    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninit = SUCCEEDED(hrCo);
    if (FAILED(hrCo) && hrCo != RPC_E_CHANGED_MODE)
        return out;

    ICreateDevEnum* devEnum = nullptr;
    IEnumMoniker* enumMoniker = nullptr;

    HRESULT hr = CoCreateInstance(
        CLSID_SystemDeviceEnum,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_ICreateDevEnum,
        reinterpret_cast<void**>(&devEnum)
    );

    if (SUCCEEDED(hr) && devEnum)
        hr = devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &enumMoniker, 0);

    if (hr == S_OK && enumMoniker)
    {
        IMoniker* moniker = nullptr;
        ULONG fetched = 0;
        int index = 0;

        while (enumMoniker->Next(1, &moniker, &fetched) == S_OK)
        {
            std::string name = "Camera " + std::to_string(index);

            IPropertyBag* propBag = nullptr;
            if (SUCCEEDED(moniker->BindToStorage(0, 0, IID_IPropertyBag, reinterpret_cast<void**>(&propBag))) && propBag)
            {
                VARIANT varName;
                VariantInit(&varName);

                bool gotName = false;
                if (SUCCEEDED(propBag->Read(L"FriendlyName", &varName, 0)) && varName.vt == VT_BSTR && varName.bstrVal)
                {
                    name = WideToUtf8(varName.bstrVal);
                    gotName = !name.empty();
                }
                VariantClear(&varName);

                if (!gotName)
                {
                    VARIANT varDesc;
                    VariantInit(&varDesc);
                    if (SUCCEEDED(propBag->Read(L"Description", &varDesc, 0)) && varDesc.vt == VT_BSTR && varDesc.bstrVal)
                    {
                        std::string desc = WideToUtf8(varDesc.bstrVal);
                        if (!desc.empty())
                            name = std::move(desc);
                    }
                    VariantClear(&varDesc);
                }

                propBag->Release();
            }

            moniker->Release();
            moniker = nullptr;

            name = OtherTools::TrimAscii(name);
            if (name.empty())
                name = "Camera " + std::to_string(index);

            CameraCandidate c;
            c.displayName = std::move(name);
            c.index = index;
            c.backend = cv::CAP_DSHOW;
            out.push_back(std::move(c));
            ++index;
        }
    }

    if (enumMoniker)
        enumMoniker->Release();
    if (devEnum)
        devEnum->Release();

    if (shouldUninit)
        CoUninitialize();

    return out;
}

// 检查候选列表中是否已存在指定索引和后端的摄像头
bool CandidateAlreadyExists(
    const std::vector<CameraCandidate>& list,
    int index,
    int backend)
{
    return std::any_of(list.begin(), list.end(), [&](const CameraCandidate& c) {
        return c.index == index && c.backend == backend;
    });
}

// 通过 OpenCV 尝试打开摄像头来扫描指定后端的可用设备
void AppendScannedBackendCandidates(
    std::vector<CameraCandidate>& out,
    int backend,
    int maxIndex)
{
    for (int i = 0; i < maxIndex; ++i)
    {
        if (CandidateAlreadyExists(out, i, backend))
            continue;

        cv::VideoCapture test(i, backend);
        if (!test.isOpened())
            continue;

        CameraCandidate c;
        c.displayName = BackendToString(backend) + " Camera " + std::to_string(i);
        c.index = i;
        c.backend = backend;
        out.push_back(std::move(c));
        test.release();
    }
}

// 确保所有显示名称唯一，重名时添加后端和索引后缀
void EnsureDisplayNamesAreUnique(std::vector<CameraCandidate>& list)
{
    std::unordered_map<std::string, int> seen;
    for (auto& c : list)
    {
        if (c.displayName.empty())
            c.displayName = "Camera " + std::to_string(c.index);

        const std::string key = OtherTools::ToLowerAscii(c.displayName);
        int& count = seen[key];
        if (count > 0)
        {
            c.displayName += " [" + BackendToString(c.backend) + " #" + std::to_string(c.index) + "]";
        }
        ++count;
    }
}

// 枚举所有可用的摄像头候选
std::vector<CameraCandidate> EnumerateCameraCandidates()
{
    std::vector<CameraCandidate> out = EnumerateDirectShowCandidates();

    // 通过 OpenCV 探测的备用条目。在 DSHOW 列举失败或只有 MSMF 后端能打开摄像头时有用
    AppendScannedBackendCandidates(out, cv::CAP_MSMF, 20);
    if (out.empty())
        AppendScannedBackendCandidates(out, cv::CAP_DSHOW, 20);

    EnsureDisplayNamesAreUnique(out);
    return out;
}

// 获取摄像头候选列表（带缓存，可强制重新扫描）
std::vector<CameraCandidate> GetCameraCandidates(bool forceRescan)
{
    std::lock_guard<std::mutex> lock(CameraCacheMutex());

    auto& candidateCache = CameraCandidateCache();
    auto& nameCache = CameraNameCache();
    if (forceRescan || candidateCache.empty())
    {
        candidateCache = EnumerateCameraCandidates();
        nameCache.clear();
        nameCache.reserve(candidateCache.size());
        for (const auto& c : candidateCache)
            nameCache.push_back(c.displayName);
    }

    return candidateCache;
}

// 对摄像头自动选择进行评分：优先选择 OBS Virtual Camera，其次其他虚拟摄像头
int ScoreAutoCameraChoice(const CameraCandidate& c)
{
    const std::string n = OtherTools::ToLowerAscii(c.displayName);
    int score = 0;

    // OBS 虚拟摄像头评分最高
    if (n.find("obs virtual camera") != std::string::npos)
        score += 300;
    else if (n.find("obs-camera") != std::string::npos)
        score += 260;
    else if (n.find("obs") != std::string::npos)
        score += 180;

    // 虚拟摄像头设备加分
    if (n.find("virtual") != std::string::npos)
        score += 80;

    // DSHOW 后端优先
    if (c.backend == cv::CAP_DSHOW)
        score += 20;

    return score;
}

// 在候选列表中精确匹配摄像头名称
int FindExactNameMatch(const std::vector<CameraCandidate>& candidates, const std::string& requestedName)
{
    const std::string key = OtherTools::ToLowerAscii(OtherTools::TrimAscii(requestedName));
    if (key.empty())
        return -1;

    for (size_t i = 0; i < candidates.size(); ++i)
    {
        if (OtherTools::ToLowerAscii(candidates[i].displayName) == key)
            return static_cast<int>(i);
    }
    return -1;
}

// 在候选列表中部分匹配摄像头名称（大小写不敏感包含匹配）
int FindPartialNameMatch(const std::vector<CameraCandidate>& candidates, const std::string& requestedName)
{
    const std::string key = OtherTools::ToLowerAscii(OtherTools::TrimAscii(requestedName));
    if (key.empty())
        return -1;

    for (size_t i = 0; i < candidates.size(); ++i)
    {
        if (OtherTools::ContainsCaseInsensitive(candidates[i].displayName, key))
            return static_cast<int>(i);
    }
    return -1;
}

// 构建摄像头打开顺序：精确匹配 -> 部分匹配 -> 自动评分最高 -> 剩余所有候选
std::vector<int> BuildOpenOrder(const std::vector<CameraCandidate>& candidates, const std::string& requestedName)
{
    std::vector<int> order;
    order.reserve(candidates.size());

    auto addUnique = [&](int idx)
    {
        if (idx < 0 || idx >= static_cast<int>(candidates.size()))
            return;
        if (std::find(order.begin(), order.end(), idx) == order.end())
            order.push_back(idx);
    };

    const std::string trimmedRequest = OtherTools::TrimAscii(requestedName);
    const std::string lowerRequest = OtherTools::ToLowerAscii(trimmedRequest);
    const bool autoSelect = trimmedRequest.empty() || lowerRequest == "none" || lowerRequest == "auto";

    if (!autoSelect)
    {
        int exact = FindExactNameMatch(candidates, trimmedRequest);
        if (exact >= 0)
            addUnique(exact);
        else
        {
            int partial = FindPartialNameMatch(candidates, trimmedRequest);
            if (partial >= 0)
                addUnique(partial);
        }
    }

    int bestAutoIdx = -1;
    int bestScore = -1;
    for (int i = 0; i < static_cast<int>(candidates.size()); ++i)
    {
        const int score = ScoreAutoCameraChoice(candidates[i]);
        if (score > bestScore)
        {
            bestScore = score;
            bestAutoIdx = i;
        }
    }
    addUnique(bestAutoIdx);

    for (int i = 0; i < static_cast<int>(candidates.size()); ++i)
        addUnique(i);

    return order;
}

// 尝试通过 OpenCV 打开指定摄像头候选设备
bool TryOpenCandidate(
    const CameraCandidate& candidate,
    cv::VideoCapture& cap,
    bool verbose)
{
    cap.release();
    cap.open(candidate.index, candidate.backend);
    if (!cap.isOpened())
        return false;

    if (verbose)
    {
        std::cout << "[VirtualCamera] Opened '" << candidate.displayName
                  << "' using " << BackendToString(candidate.backend)
                  << " index=" << candidate.index << std::endl;
    }
    return true;
}
} // namespace

// 构造函数：枚举可用摄像头并尝试打开指定设备（或自动选择）
// w/h: 期望宽度/高度（0 表示使用摄像头默认分辨率）
// cameraName: 摄像头名称（"None"/"Auto" 表示自动选择）
// captureFps: 目标帧率
// verbose: 是否输出详细日志
VirtualCameraCapture::VirtualCameraCapture(
    int w,
    int h,
    const std::string& cameraName,
    int captureFps,
    bool verbose)
    : targetWidth_(w)
    , targetHeight_(h)
    , selectedCameraName_(cameraName)
    , captureFps_(captureFps)
    , verbose_(verbose)
{
    auto candidates = GetCameraCandidates(false);
    if (candidates.empty())
        candidates = GetCameraCandidates(true);

    if (candidates.empty())
    {
        throw std::runtime_error("[VirtualCamera] No camera devices found");
    }

    const std::string requestedName = selectedCameraName_;
    if (!requestedName.empty() && OtherTools::ToLowerAscii(OtherTools::TrimAscii(requestedName)) != "none")
    {
        int exact = FindExactNameMatch(candidates, requestedName);
        int partial = (exact >= 0) ? exact : FindPartialNameMatch(candidates, requestedName);
        if (exact < 0 && partial < 0)
        {
            std::cerr << "[VirtualCamera] Requested camera not found: " << requestedName
                      << ". Will use fallback search." << std::endl;
        }
    }

    cap_ = std::make_unique<cv::VideoCapture>();
    bool opened = false;

    auto openByOrder = [&](const std::vector<CameraCandidate>& list) -> bool
    {
        const auto order = BuildOpenOrder(list, requestedName);
        for (int idx : order)
        {
            if (idx < 0 || idx >= static_cast<int>(list.size()))
                continue;

            if (TryOpenCandidate(list[idx], *cap_, verbose_))
            {
                selectedCameraName_ = list[idx].displayName;
                return true;
            }
        }
        return false;
    };

    opened = openByOrder(candidates);
    if (!opened)
    {
        candidates = GetCameraCandidates(true);
        opened = openByOrder(candidates);
    }

    if (!opened || !cap_ || !cap_->isOpened())
        throw std::runtime_error("[VirtualCamera] Unable to open any capture device");

    bool autoMode = (w <= 0 || h <= 0);
    if (autoMode)
    {
        w = static_cast<int>(cap_->get(cv::CAP_PROP_FRAME_WIDTH));
        h = static_cast<int>(cap_->get(cv::CAP_PROP_FRAME_HEIGHT));
    }
    else
    {
        cap_->set(cv::CAP_PROP_FRAME_WIDTH, even(w));
        cap_->set(cv::CAP_PROP_FRAME_HEIGHT, even(h));
        w = static_cast<int>(cap_->get(cv::CAP_PROP_FRAME_WIDTH));
        h = static_cast<int>(cap_->get(cv::CAP_PROP_FRAME_HEIGHT));
    }

    if (captureFps_ > 0)
        cap_->set(cv::CAP_PROP_FPS, captureFps_);

    cap_->set(cv::CAP_PROP_BUFFERSIZE, 1);

    roiW_ = even(std::max(2, w));
    roiH_ = even(std::max(2, h));
    SetSourceDimensions(roiW_, roiH_);

    if (verbose_)
    {
        std::cout << "[VirtualCamera] Selected camera: " << selectedCameraName_ << std::endl;
        std::cout << "[VirtualCamera] Actual capture: "
                  << roiW_ << 'x' << roiH_ << " @ "
                  << cap_->get(cv::CAP_PROP_FPS) << " FPS" << std::endl;
    }
}

// 析构函数：释放摄像头资源
VirtualCameraCapture::~VirtualCameraCapture()
{
    if (cap_)
    {
        if (cap_->isOpened())
            cap_->release();
        cap_.reset();
    }
}

// 获取下一帧：从摄像头读取图像，转换通道格式（灰度->BGR、BGRA->BGR），
// 按需缩放至目标尺寸
cv::Mat VirtualCameraCapture::GetNextFrameCpu()
{
    if (!cap_ || !cap_->isOpened())
        return cv::Mat();

    cv::Mat frame;
    if (!cap_->read(frame) || frame.empty())
        return cv::Mat();

    // 统一转换为 3 通道 BGR 格式
    switch (frame.channels())
    {
    case 1:
        cv::cvtColor(frame, frame, cv::COLOR_GRAY2BGR);
        break;
    case 4:
        cv::cvtColor(frame, frame, cv::COLOR_BGRA2BGR);
        break;
    case 3:
        break;
    default:
        std::cerr << "[VirtualCamera] Unexpected channel count: " << frame.channels() << std::endl;
        return cv::Mat();
    }

    frameCpu = frame;

    if (targetWidth_ > 0 && targetHeight_ > 0 && !frameCpu.empty())
        cv::resize(frameCpu, frameCpu, cv::Size(targetWidth_, targetHeight_));

    return frameCpu.clone();
}

// 静态方法：获取所有可用虚拟摄像头名称列表
std::vector<std::string> VirtualCameraCapture::GetAvailableVirtualCameras(bool forceRescan)
{
    auto candidates = GetCameraCandidates(forceRescan);
    std::vector<std::string> names;
    names.reserve(candidates.size());
    for (const auto& c : candidates)
        names.push_back(c.displayName);
    return names;
}

// 清除缓存的摄像头列表
void VirtualCameraCapture::ClearCachedCameraList()
{
    std::lock_guard<std::mutex> lock(CameraCacheMutex());
    CameraCandidateCache().clear();
    CameraNameCache().clear();
}
