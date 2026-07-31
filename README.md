# Xen — Windows AI 辅助瞄准工具

Xen 是基于 C++20 原生实现的 Windows AI 辅助瞄准工具。核心管线为：截图采集 → YOLO 目标检测推理 → 瞄准控制。

当前仓库已形成 P0 单机最小闭环：

```text
Desktop Duplication → Detector → Aim → Runtime SafetyGate → Win32 SendInput
```

物理鼠标输出默认禁用。只有配置显式允许、Runtime 已启动、用户完成武装、按住启用键且
急停未触发时，Pipeline 才会调用 Mouse。Capture、Detector 或 Aim 失败均不会沿用旧结果。

## 目录结构

```text
Xen/                           # 仓库根目录
├── CMakeLists.txt             # 顶层构建文件
├── README.md                  # 项目入口
├── Xen/                       # C++ 源码根目录
│   ├── detector/              # Detector 模块（.h + .cpp 平铺）
│   ├── log/                   # Log 模块（.h + .cpp 平铺）
│   ├── capture/               # DXGI Desktop Duplication + 中心 ROI
│   ├── aim/                   # 观测归并、追踪、目标选择和移动控制
│   ├── mouse/                 # 设备无关命令与 Win32 SendInput 后端
│   ├── keyboard/              # 按住启用与急停键轮询
│   ├── config/                # SimpleIni 静态配置与校验
│   ├── runtime/               # 生命周期、三槽最新帧队列和安全门控
│   ├── overlay/               # Codex 浅色/深色 ImGui 控制台
│   └── app/                   # Windows 应用入口与 Xen 品牌资源
├── AGENTS.md                  # 本地开发规范，不纳入 Git
└── docs/                      # 本地设计文档，不纳入 Git
```

文档和构建文件中的源码路径均以仓库根目录为基准，例如 `Xen/detector/detector.cpp`。源码内部的 `#include "detector/detector.h"` 以 `Xen/` 为包含目录基准。

## P0 运行模型

- 主线程负责 Win32/D3D11/ImGui 消息循环、只读快照渲染和意图提交。
- Capture 线程从 Desktop Duplication 获取中心 ROI，并发布到三个可复用槽组成的最新帧队列。
- Pipeline 线程依次执行 Detector、Aim、安全门控和 Mouse，不增加独立控制线程。
- 当前兼容链为 `GPU 纹理 → CPU BGR ROI → Detector CPU 前处理 → Provider`；GPU 互操作待实测后再实施。
- Overlay 采用 168 px 简洁标签栏和带 8 px 外边距的独立工作区。程序身份只在随主题切换的原生标题栏展示，侧栏仅保留概览、检测、瞄准、输入和设置五个标签；配置页保存操作固定在页头，运行期间锁定需重建资源的配置。
- Codex 浅色与深色主题可在偏好设置中切换并保存到 `config.ini`。连续数值使用“滑块粗调 + 数值框精确输入”，手填值在 Enter 或失焦时按合法范围校验。
- `Xen/app/xen.ico` 提供 16–256 px Windows 图标，`Xen/app/xen-brand.svg` 是可编辑品牌母版；标志以 Xen 的 X 和锁定点表达“精确控制”。

应用目标为 `xen_app`，Release 输出名为 `Xen.exe`。首次运行缺少 `config.ini` 时，界面会显示配置错误；填写模型路径并保存后方可启动 Runtime。

## 构建状态

源码路径已同步到 `Xen/`，并提供 Detector 纯算法测试。Windows 推荐使用 VS 2026 的多配置生成器：

```bat
set ONNXRUNTIME_ROOT=C:\path\to\onnxruntime-win-x64-gpu_cuda13-1.27.1
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 ^
  -DOpenCV_DIR=C:\path\to\opencv\build\x64\vc16\lib ^
  -DBUILD_TESTING=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

本项目已用 ONNX Runtime 1.27.1、OpenCV 4.14.0、spdlog 1.17.0 和 VS 2026 Release 配置完成构建。测试所需的非系统运行库由 CMake 部署到可执行文件旁，不依赖开发机额外配置 `PATH`。

也可以使用仓库内的可复用脚本完成配置、构建和测试：

```powershell
.\scripts\build.ps1 `
  -OnnxRuntimeRoot "C:\path\to\onnxruntime-win-x64-gpu_cuda13-1.27.1" `
  -OpenCvDir "C:\path\to\opencv\build\x64\vc16\lib" `
  -TensorRtRoot "C:\path\to\TensorRT" `
  -CudnnRoot "C:\path\to\cudnn" `
  -CudaRoot "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.2" `
  -ModelPath "C:\path\to\model.onnx"
```

`ModelPath` 可省略；提供后会额外执行真实模型加载与单帧推理测试，模型不会复制进构建目录或纳入 Git。
构建测试可执行文件时，CMake 会把其实际使用的 OpenCV、ONNX Runtime、TensorRT、
cuDNN 和 CUDA DLL 复制到对应的 `build/<Configuration>/`，测试程序可直接启动，
无需手工修改系统 `PATH`。TensorRT 只复制核心、ONNX 解析器和 SDK 提供的各架构
Builder Resource，使同一构建可在不同 NVIDIA GPU 上首次生成对应 Engine；不复制完整 SDK。

需要单独验证 TensorRT EP 与缓存时，应使用正式脚本重新配置、构建并检查本地运行库部署：

```powershell
.\scripts\test_tensorrt.ps1 `
  -ModelPath "C:\path\to\model.onnx" `
  -OnnxRuntimeRoot "C:\path\to\onnxruntime" `
  -OpenCvDir "C:\path\to\opencv\build\x64\vc16\lib" `
  -TensorRtRoot "C:\path\to\TensorRT-10.16.1.11" `
  -CudnnRoot "C:\path\to\cudnn-9.21.0.82-cuda13" `
  -CudaRoot "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.2"
```

DirectML 使用官方独立 ORT 包，不能与 CUDA/TensorRT 版 `onnxruntime.dll`
混放。以下脚本固定使用 `build-dml/`，并严格校验实际 Provider；DML 不可用时
测试失败，不会静默回退 CPU：

```powershell
.\scripts\test_directml.ps1 `
  -ModelPath "C:\path\to\model.onnx" `
  -OnnxRuntimeRoot "C:\path\to\Microsoft.ML.OnnxRuntime.DirectML" `
  -DirectMlRoot "C:\path\to\Microsoft.AI.DirectML" `
  -OpenCvDir "C:\path\to\opencv\build\x64\vc16\lib"
```

固定 shape 的 TensorRT 模型默认启用 CUDA Graph。Detector 会复用 CUDA 输入输出
缓冲区，在图捕获之外执行每帧 H2D/D2H 复制，避免把首次输入错误地重复重放。
动态 shape 模型或诊断图捕获问题时设置 `enable_trt_cuda_graph = false`。

使用真实场景视频评估 Detector 时，可逐帧统计前处理、推理、后处理和总耗时，
以及有检测结果的帧比例和最长连续空检测帧数：

```powershell
.\scripts\benchmark_detector_videos.ps1 `
  -ModelPath "C:\path\to\model.onnx" `
  -VideoDirectory "C:\path\to\videos" `
  -OnnxRuntimeRoot "C:\path\to\onnxruntime" `
  -OpenCvDir "C:\path\to\opencv\build\x64\vc16\lib" `
  -TensorRtRoot "C:\path\to\TensorRT" `
  -CudnnRoot "C:\path\to\cudnn" `
  -CudaRoot "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.2"
```

脚本会在 CSV 旁生成同名前缀的 JSON 环境清单，记录 Git 状态、模型 SHA-256、
视频逐文件 SHA-256、可执行文件与部署 DLL 哈希、GPU/驱动、SDK 精确版本，以及
FP16、CUDA Graph、线程和缓存配置。脚本在运行前后复核输入与二进制快照；CSV 先写
临时文件，全部场景成功后才原子发布。性能报告和环境清单默认位于
`cache/benchmarks/`，均不提交 Git。

项目不迁入旧仓库的 `Xen/benchmarks/provider_benchmark.cpp`。旧实现会重复 Provider、
前处理、解码和统计逻辑，并通过编译期开关维护两套不同管线。当前基准始终复用生产
`Detector`，避免性能测试与真实运行的数据流、复制链和输出语义漂移。

CSV 中每段视频先重复第一帧预热 50 次，
正式统计覆盖全部视频帧，输出各阶段 mean/P50/P95/P99。视频没有人工标注时，
`detection_frame_rate` 只表示“存在检测结果的帧比例”，不能直接解释为 Recall；
目标在全屏范围移动而采集范围固定为中心 ROI 时，ROI 外的空检测是正确结果。
该指标同时受到目标进入 ROI 的时间占比与 ROI 内检测成功率影响；只有补充目标可见性
标注后，才能单独计算模型在 ROI 内的 Recall。

脚本默认使用 `-InputMode center`，从全屏录像中央裁取与模型输入相同大小的 ROI，
模拟实时游戏中只采集准星附近 FOV 的链路。只有需要测量“整幅录像缩放到模型输入”时
才使用 `-InputMode full`；两种模式的准确率结果不能直接混合比较。

## Detector 模型兼容性

Detector 不按“YOLOv5/v8/v10/11/26”等版本号硬编码分支，而按 ONNX 输出契约解码：

| 输出契约 | 典型形状 | 用途 |
|---|---|---|
| `CHANNEL_FIRST` | `[B, 4+C, A]` | 现代 Ultralytics raw detect 输出 |
| `ANCHOR_FIRST_OBJECTNESS` | `[B, A, 5+C]` | YOLOv5 类 raw 输出 |
| `END_TO_END` | `[B, N, 6]` | 已含 NMS 的 `xyxy, score, class` 输出 |

因此，后续 YOLO 版本只要继续使用上述任一契约即可直接支持；若导出布局变化，只需新增一个契约解码器，不必改动推理主流程。默认 `AUTO` 会结合 metadata 和形状识别。单类别 raw 输出与端到端输出都可能是 `[B,N,6]`，第三方导出模型遇到歧义时应显式设置 `DetectorConfig::output_format`。

当前仅支持单输入、单输出、NCHW、float32 的 detect 模型。分割、姿态、OBB、多输出 head 和真正的 batch inference 不在当前范围内。

### TensorRT 缓存

选择 `BackendType::TENSORRT` 时默认启用 Engine Cache 与 Timing Cache。首次加载仍需从 ONNX 构建 TensorRT Engine，缓存写入 `cache/tensorrt/`；后续创建 Session 会直接复用缓存，无需再次完整构建：

```cpp
DetectorConfig config;
config.model_path = "models/yolo.onnx";
config.backend = BackendType::TENSORRT;
config.enable_fp16 = true;
config.trt_cache_path = "cache/tensorrt";
```

缓存与模型、GPU、精度配置、ONNX Runtime 和 TensorRT 版本相关。上述任一项变化后，应删除对应缓存并重新生成。缓存目录已由 `.gitignore` 排除。

## 本地开发资料

`AGENTS.md` 与 `docs/` 仅用于本地开发和设计记录，已通过 `.gitignore` 排除，不随源码仓库发布。

## 技术栈

C++20 / ONNX Runtime / OpenCV / spdlog / CMake
