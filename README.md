# Xen — Windows AI 辅助瞄准工具

Xen 是基于 C++20 原生实现的 Windows AI 辅助瞄准工具。核心管线为：截图采集 → YOLO 目标检测推理 → 瞄准控制。

当前仓库已形成 P0 单机最小闭环，并完成 UDP MJPEG、XUDP JPEG、NDI 接收端与
KMBOX NET 鼠标后端的源码和自动回环测试：

```text
Desktop Duplication ──────┐
OBS/FFmpeg UDP MJPEG ─────┤
XUDP v1 自定义发送端 ──────┤
OBS/NDI Sender ───────────┴→ Detector → Aim → Runtime SafetyGate ─┬→ Win32 SendInput
                                                                 └→ KMBOX NET UDP
```

物理鼠标输出默认禁用。只有配置显式允许、Runtime 已启动、用户完成武装、按住启用键且
急停未触发时，Pipeline 才会调用 Mouse。Capture、Detector 或 Aim 失败均不会沿用旧结果。

KMBOX NET 只实现 Runtime 当前需要的相对移动，不包含旧项目的 monitor、按键屏蔽、LCD 或
自动轨迹接口。配置需填写设备 IPv4、端口和屏幕显示的 8 位十六进制 UUID；仓库不提供设备
凭据默认值。后端使用 16 字节 connect 和 72 字节 move 小端序数据报，并严格校验 ACK 的来源
地址、命令码和连续序号。任一发送、超时或 ACK 校验失败都会返回 Mouse 失败，Runtime 随即
急停。未开启物理输出时不会初始化 Winsock 或访问设备。

KMBOX 的 `dx_counts/dy_counts` 是 Aim 在主机 FOV 坐标下计算出的相对鼠标 counts，不是辅机
桌面坐标。主机 `2560x1440`、辅机 `1920x1080` 时仍以主机准星和 ROI 计算；辅机分辨率仅影响
Overlay 显示，不得对 KMBOX 命令再做一次 `1920/2560` 或 `1080/1440` 缩放。

## 目录结构

```text
Xen/                           # 仓库根目录
├── CMakeLists.txt             # 顶层构建文件
├── README.md                  # 项目入口
├── Xen/                       # C++ 源码根目录
│   ├── detector/              # Detector 模块（.h + .cpp 平铺）
│   ├── log/                   # Log 模块（.h + .cpp 平铺）
│   ├── capture/               # DXGI + UDP/XUDP + NDI + 主机 FOV 坐标契约
│   ├── aim/                   # 观测归并、追踪、目标选择和移动控制
│   ├── mouse/                 # 设备无关命令、Win32 与 KMBOX NET 后端
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
- Capture 线程从 Desktop Duplication、UDP/XUDP 或 NDI 获取 ROI，并发布到三个可复用槽组成的最新帧队列。
- Pipeline 线程依次执行 Detector、Aim、安全门控和 Mouse，不增加独立控制线程。
- 当前兼容链为 `GPU 纹理 → CPU BGR ROI → Detector CPU 前处理 → Provider`；GPU 互操作待实测后再实施。
- Overlay 采用 152 px 居中标签栏和无外框独立工作区。自绘标题栏在侧栏交界处仅用底色分区：左段与侧栏同色，右段与工作区同色，并随浅色/深色主题同步切换；标题栏和顶部控制条下方均不绘制分割线。侧栏仅保留概览、检测、瞄准、输入和设置五个标签，底部以无边框两行状态区展示输出门状态和版本。工作区底色从交界处连续铺满，顶部控制条保留 18 px、页面内容保留 28 px 左侧间距，只为状态卡、配置组等内部模块保留细边框；配置页保存操作固定在页头，运行期间锁定需重建资源的配置。
- 当前 Overlay 已经人工确认并作为后续 UI 扩展基线。新增部件必须复用现有语义色、间距、表单控件和内部模块样式，不得增加工作区外框、嵌套卡片或新的导航分组；改变整体布局或主题体系前必须重新人工复核。
- Codex 浅色与深色主题可在偏好设置中切换并保存到 `config.ini`。连续数值使用“滑块粗调 + 数值框精确输入”，手填值在 Enter 或失焦时按合法范围校验。
- `Xen/app/xen.ico` 提供 16–256 px Windows 图标，`Xen/app/xen-brand.svg` 是可编辑品牌母版；标志以 Xen 的 X 和锁定点表达“精确控制”。

## 双机网络坐标契约

双机模式下，主机游戏分辨率、UDP 编码尺寸、检测 ROI 和辅机显示器分辨率是四个独立空间。
辅机显示器只渲染 Overlay，不参与瞄准换算。例如主机为 `2560x1440`，OBS 以 1:1 像素发送
中心 `320x320` 时，Capture 必须发布主机 ROI `(1120, 560, 320, 320)`；即使辅机显示器为
`1920x1080`，该坐标也不改变。

推荐的低带宽 OBS 配置是对 `2560x1440` 源左右各裁 `1120`、上下各裁 `560`，保持
`320x320` 输出不再缩放；接收端选择 `CENTER_CROP_1_TO_1`，并显式配置
`udp_source_width=2560`、`udp_source_height=1440`。若 OBS 发送缩放后的完整画面，应改选
`FULL_FRAME_SCALED`，Capture 会把检测像素误差换算回主机 FOV 像素。

当前裸 MJPEG 兼容模式没有源帧号、发送时间戳、分片序号和校验，因此不能测量严格的发送端到
接收端帧龄，也不能把主动最新帧淘汰解释为网络丢包。真实双机上线前仍需完成有线局域网长时间
基准；详细方案见本地文档 `docs/009_双机网络采集坐标与方案评估_20260731.md`。

`XUDP_JPEG` v1 在每个大端序数据报中携带流/帧号、分片范围、编码尺寸、主机 FOV、主机 ROI、
源 FPS、发送时间戳和完整 JPEG SHA-256。接收端最多保留三个在途帧，支持乱序与相同重复分片，
并分别统计帧号缺口、协议异常、Capture 淘汰和 Runtime 覆盖。主机 `2560x1440` 的中心
`320x320` 由协议明确声明为 `(1120,560,320,320)`，辅机 `1920x1080` 从不进入换算。
SHA-256 只提供传输完整性，不提供发送端身份认证；未同步跨机时钟时，发送时间戳也不能直接
解释为严格端到端帧龄。当前仓库只有接收端和测试发送器，OBS 不能直接输出 XUDP，真实部署仍
需要自定义发送端或 OBS 后处理插件。协议详见本地文档
`docs/011_XUDP版本化帧协议设计与验证_20260731.md`。

NDI 后端使用 NDI 6 SDK 的 mDNS 发现与 BGRX/BGRA 接收，仍复用同一主机坐标契约。自定义
发送端可在每帧附加 Xen XML metadata；普通 OBS NDI 插件不能保证注入该 metadata，因此生产
配置必须保留显式主机 FOV 回退。`Auto` 仅在发现唯一源时连接，多源环境应填写完整 NDI 源名称。

```xml
<xen version="1" source_width="2560" source_height="1440"
     roi_x="1120" roi_y="560" roi_width="320" roi_height="320"/>
```

NDI SDK 可选。设置 `NDI_SDK_DIR` 或 `-DXEN_NDI_SDK_ROOT=...` 后编译真实后端，CMake 会把
`Processing.NDI.Lib.x64.dll` 和许可证复制到可执行文件旁；未安装 SDK 时项目仍可构建，但选择
NDI 会明确返回 `UNSUPPORTED`，不会切换 UDP、DXGI 或 CPU 路径。

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
  -NdiSdkRoot "C:\Program Files\NDI\NDI 6 SDK" `
  -ModelPath "C:\path\to\model.onnx"
```

`ModelPath` 可省略；提供后会额外执行真实模型加载与单帧推理测试，模型不会复制进构建目录或纳入 Git。
构建测试可执行文件时，CMake 会把其实际使用的 OpenCV、ONNX Runtime、TensorRT、
cuDNN 和 CUDA DLL 复制到对应的 `build/<Configuration>/`，测试程序可直接启动，
无需手工修改系统 `PATH`。TensorRT 只复制核心、ONNX 解析器和 SDK 提供的各架构
Builder Resource，使同一构建可在不同 NVIDIA GPU 上首次生成对应 Engine；不复制完整 SDK。
NDI 构建同时部署 SDK 运行 DLL 与许可证文件。

NDI 有 SDK/无 SDK 两条构建边界、真实 Sender/Receiver 回环、metadata 坐标、变化帧和断流可用
正式脚本重复验证：

```powershell
.\scripts\test_ndi.ps1 `
  -OnnxRuntimeRoot "C:\path\to\onnxruntime" `
  -OpenCvDir "C:\path\to\opencv\build\x64\vc16\lib" `
  -NdiSdkRoot "C:\Program Files\NDI\NDI 6 SDK"
```

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
