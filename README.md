# Xen

Xen 是一个使用 C++20 编写的 Windows 实时视觉与控制项目，主链路为：

```text
Capture → Detector → Aim → Runtime SafetyGate → Mouse
```

项目支持本机与双机采集、多种 ONNX Runtime 执行后端、可视化控制台、离线基准和受控输入设备。
当前重点是稳定现有闭环与真实环境验收，不继续无边界扩展模型或后端。

> 物理鼠标输出默认关闭。任何真实输出都必须由用户在当轮前台明确授权、手动启动，并保留武装、
> 按住启用与急停门禁。请只在私有、离线或明确允许的环境中使用。

## 能力概览

| 层 | 当前能力 |
|---|---|
| Capture | DXGI Desktop Duplication、UDP MJPEG、XUDP JPEG、NDI |
| Detector | detect、segment、pose、OBB；CPU、CUDA、TensorRT、DirectML、OpenVINO |
| Aim | 观测归并、追踪、目标选择、延迟补偿与离线评价 |
| Runtime | 最新帧队列、生命周期、模型热重载、SafetyGate、原子调试报告 |
| Mouse/Input | Win32 SendInput、KMBOX NET、MAKCU；键盘和鼠标热键监听 |
| Tools | Sender、Runtime Benchmark、Mouse Benchmark、人工验收与发布脚本 |

不同 Provider 使用各自匹配的 ONNX Runtime 发行包和独立构建目录。请求严格后端时不会静默回退
到 CPU。固定 shape TensorRT 可启用 CUDA Graph；DirectML/OpenVINO 保持独立运行库闭包。

## 构建

### 环境

- Windows x64
- Visual Studio 2026 / MSVC v14.51，安装 C++ 桌面开发组件
- CMake 3.18 或更高版本
- 与目标 Provider 匹配的 ONNX Runtime SDK
- OpenCV
- 开启 `BUILD_TESTING`：PowerShell 7，以及能导入 NumPy 和 OpenCV（`cv2`）的 Python 3
- 可选：CUDA、TensorRT、cuDNN、DirectML、NDI SDK

依赖版本和导入关系以 [CMakeLists.txt](CMakeLists.txt) 与实际构建报告为准；不要只替换一个 GPU
SDK 后沿用旧构建目录。

测试配置会先解析解释器，再用同一个 Python 实际导入 NumPy/`cv2`；缺失或加载失败会明确终止配置。
需要指定解释器时传入 `-DXEN_PYTHON_EXECUTABLE="C:\path\to\python.exe"`，测试使用该绝对路径。

### 最小 Release 构建

```powershell
$env:ONNXRUNTIME_ROOT = "C:\path\to\onnxruntime"
$env:OpenCV_DIR = "C:\path\to\opencv\build\x64\vc16\lib"

cmake -S . -B build -G "Visual Studio 18 2026" -A x64 `
  -DOpenCV_DIR="$env:OpenCV_DIR" `
  -DBUILD_TESTING=ON

cmake --build build --config Release --target xen_app --parallel
```

输出位于 `build/Release/`，应用文件名为 `Xen.exe`。普通开发只构建受影响目标并运行相关专项测试；
依赖/ABI、共享 Provider 契约或正式发布候选才需要完整矩阵。

需要执行当前配置的完整构建、clean `PATH` 测试和运行库来源检查时，使用正式脚本：

```powershell
.\scripts\build.ps1 `
  -OnnxRuntimeRoot "C:\path\to\onnxruntime" `
  -OpenCvDir "C:\path\to\opencv\build\x64\vc16\lib"
```

GPU 或 NDI 构建再按脚本参数传入对应 SDK 根目录。DirectML、OpenVINO 与 NVIDIA 构建必须使用
不同的 `-BuildDirectory`。

## 运行

1. 启动一次 `build/Release/Xen.exe`。程序会在同目录创建 `models/` 和默认 `config.ini`。
2. 把 ONNX 模型放入 `models/` 根目录，在“检测”页刷新、选择并应用模型。
3. 配置 Capture 与 Provider，启动 Runtime；先保持物理输出关闭，确认预览、日志和 Provider 状态。
4. 需要真实设备验收时，使用正式 Prepare/Launch 脚本生成独立 Run，并由用户手动执行带确认令牌的
   Launch 命令。

正式多 Provider 包只从根目录 `XenLauncher.exe` 进入。Launcher 负责 manifest 路由、路径安全、
后端归属和 Worker 存在性；日常启动不会扫描或哈希整包。完整文件集合与 SHA-256 校验只保留在
新完整发布、跨机复制、运行库拓扑变化或明确供应链审计边界。

## 可执行目标

| CMake 目标 | 输出 | 用途 |
|---|---|---|
| `xen_app` | `Xen.exe` | 主应用与 Overlay |
| `xen_launcher` | `XenLauncher.exe` | 正式包 Provider 路由入口 |
| `xen_sender` | `XenSender.exe` | DXGI → XUDP 发送端 |
| `xen_clock_source` | `XenClockSource.exe` | NDI 源机四时间戳旁路；不接触图像或输入设备 |
| `xen_capture_evidence` | `XenCaptureEvidence.exe` | 不可武装的 Capture/NDI 像素证据录制入口 |
| `xen_benchmark` | `XenBenchmark.exe` | 无界面 Runtime 基准 |
| `xen_mouse_benchmark` | `XenMouseBenchmark.exe` | 鼠标后端性能与协议验证 |
| `xen_mouse_effect_probe` | `XenMouseEffectProbe.exe` | source-frame 驱动的 X-only 实际命令/背景响应证据入口；Physical 仅接受用户前台双授权 |
| `xen_mouse_effect_probe_sequence` | `XenMouseEffectProbeSequence.exe` | 离线生成平衡净零的 A/A2 序列；S1 活性 profile 以固定 source-frame cadence 生成 X-only 回锚挑战，基线保持零命令 |

`XenSender.exe --report PATH` 必须声明至少一个非零退出上限：`--max-frames` 不超过
200000，或 `--fps × --max-seconds` 的理论样本数不超过 200000；两者都提供时任一安全上限即可。
其中 0 表示不限帧/不限时，不能单独证明报告有界。无安全容量边界的组合会在 Log、Capture、网络
和报告目录副作用前以命令行用法错误退出。

## 常用脚本

| 入口 | 用途 |
|---|---|
| [scripts/build.ps1](scripts/build.ps1) | 当前 Provider 的完整构建与测试 |
| [scripts/build_aim_debug.ps1](scripts/build_aim_debug.ps1) | 固定 NVIDIA 环境下的 Aim/Runtime 轻量构建 |
| [scripts/publish_release_bundle.ps1](scripts/publish_release_bundle.ps1) | 生成隔离 Provider 的正式发布包 |
| [scripts/invoke_aim_manual_acceptance.ps1](scripts/invoke_aim_manual_acceptance.ps1) | 生成并执行受控 Aim 人工 Run |
| [scripts/run_ndi_clock_source.ps1](scripts/run_ndi_clock_source.ps1) | 在 NDI 源机前台启动时钟旁路；不会访问 KMBOX |
| [scripts/run_mouse_effect_probe_output_off.ps1](scripts/run_mouse_effect_probe_output_off.ps1) | 以零 Mouse 能力排练 probe/source/sidecar/像素绑定 |
| [scripts/prepare_mouse_effect_probe_a.ps1](scripts/prepare_mouse_effect_probe_a.ps1) | 固化 A 级 X-only Physical Run；只 Prepare，不启动设备或 sidecar |
| [scripts/prepare_mouse_effect_probe_a2_s1.ps1](scripts/prepare_mouse_effect_probe_a2_s1.ps1) | 固化 A2 S1 的自动 KMBOX 活性括号与零命令基线；只 Prepare，不执行 Physical Launch |
| [scripts/benchmark_runtime.ps1](scripts/benchmark_runtime.ps1) | Runtime 正式基准与原子报告 |
| [scripts/test_tensorrt.ps1](scripts/test_tensorrt.ps1) | TensorRT 专项正确性与变化输入验证 |
| [scripts/test_directml.ps1](scripts/test_directml.ps1) | DirectML 独立构建与专项验证 |
| [scripts/test_openvino.ps1](scripts/test_openvino.ps1) | OpenVINO 独立构建与专项验证 |

其余专项入口位于 [scripts/](scripts/)。脚本是参数和证据格式的事实源；不要长期维护一次性脚本。

`XenMouseEffectProbeCompositeSeal --study-scheduler <绝对新目录>` 提供独立的无鼠标输出调度诊断。
它先冻结采样协议，在 300/325/350 微秒 guard 上各记录 10 个 42-event 批次，再选择全部观测达标的
最小值，以另外 10 批新数据验证。迟到/marker 超限在表征阶段保留，硬 active、API、下一目标已错过、
停止或超时则中止；验证失败不再选替补或重试。150/100 微秒质量上限、每事件 350 微秒及每批
14.7 毫秒 active 上限保持不变，整个 campaign 最多 40 批，派生 active 上限 588 毫秒，经过时间
上限 30 秒（线程恢复执行时检查，Windows 调度不提供硬实时保证）。已有目录拒绝复用，Ctrl+C 请求
中止并保存已取得记录。产物仅为带插桩的有限经验筛查；后续数据不参与选值，但不宣称统计独立或
尾部可靠性。该工具不会发布正式 preflight/plan，也不会修改已有 sequence、Run 或系统调度设置。

NDI `timestamp` 在报告中明确记为 SDK submission time，不称为桌面采集或曝光时刻。源机旁路以低频
四时间戳交换把该 UTC 时间映射到接收机 `steady_clock`，并逐帧输出 status、RTT、uncertainty、rate、
mapping age、sample count 和 source session；映射未就绪、过期或回跳时保持无效。Mouse 报告也分别
记录 backend completion、匹配协议响应的 protocol ACK 和独立 physical effect；当前后端没有物理效果
观测能力，因此不能由 API 返回或 ACK 推导真实鼠标已经移动。Mouse Benchmark schema 2 另行绑定
run UUID、completion semantic 与 peer/test boundary；正式脚本只在完整聚合键一致时复制 timing，
loopback/in-memory fake 不与真实设备报告合并，并始终显式记录 `physical_effect_observed=false`。
Runtime 报告 schema 18 将原始 source sequence/timecode/timestamp、映射后的 source、capture、
Aim observation 和 control 时刻及各自有效性绑定到实际处理帧；缺失值保持无效，不用本地序号补齐。
这些 64 位标识、绝对时刻和 source clock session 在 JSON 中使用十进制字符串，CSV 保留整数文本，
避免解析器经过浮点数时损失相邻帧身份。`steady_ns` 只可在当前 Runtime 会话内比较；原始源时间
沿用 Capture 单位，源时钟 session 不等于 NDI 发送端身份，以上字段不提供曝光或设备应用位移证据。
Win32 的 execution boundary 内生为 `local_os_api`；KMBOX/MAKCU 不再从 endpoint 或脚本默认值推断
外部设备，必须显式传入 `ConfiguredExternalDevicePeer`，127/8 KMBOX fake 则必须显式传入
`LoopbackUdpFake`，且在创建报告目录或打开设备前完成拒绝。

## 源码结构

```text
Xen/
├── aim/                 # 追踪、选择、控制与评价
├── aim_landmark/        # 诊断用头部 landmark 关联
├── app/                 # 应用、Launcher、模型目录与资源
├── benchmark/           # 无界面 Runtime 基准
├── capture/             # DXGI、UDP、XUDP、NDI
├── capture_evidence/    # Capture 像素证据录制与发布
├── clock_source/        # NDI 源机时钟旁路进程
├── clock_sync/          # 双机四时间戳与 affine 映射
├── config/              # INI 聚合、保存与校验
├── crash/               # 崩溃报告与紧急日志尾部
├── debug/               # 有界样本与 CSV/JSON 报告
├── detector/            # ORT Session、前后处理、多任务输出
├── keyboard/            # 热键与急停事件
├── log/                 # 全局日志基础设施
├── mouse/               # Win32、KMBOX NET、MAKCU
├── mouse_benchmark/     # 鼠标后端基准与报告
├── mouse_effect_probe/  # 平衡 X 激励、逐 source-frame 执行与证据报告
├── mouse_effect_probe_runner/   # NDI/sidecar/deadman 编排入口
├── mouse_effect_probe_sequence/ # 离线序列生成入口
├── overlay/             # Win32/D3D11/ImGui 控制台
├── runtime/             # 生命周期、队列与安全门
└── sender/              # 生产发送端
```

模块内 `.h` 与 `.cpp` 平铺；源码 include 以 `Xen/` 为根，例如
`#include "detector/detector.h"`。

## 开发与文档

本地完整工作区包含 `AGENTS.md` 与 `docs/`：

- `AGENTS.md`：长期代理开发规则；开发任务会自动选择并调用相关 Skills。
- `docs/README.md`：全部本地文档的主题索引。
- `docs/000_项目状态与路线图.md`：当前事实、阶段和验证基线。
- `docs/todolist.md`：唯一任务注册表。
- `docs/001_*.md` 及后续编号文档：设计、排查和验证证据。

这些本地资料按当前仓库策略不随 Git 发布；跨机器克隆以本 README、源码、CMake 和正式脚本为准。
README 不再重复当前提交、测试总数、实验路径和历史基准，避免它们与事实源漂移。

开发验证按影响面分层：文档/纯配置不构建；普通代码运行最小相关 Release 目标和专项测试；
Provider、线程、复制链和发布变更升级到对应专项门禁；真实 KMBOX 视觉验收单独报告，不能由自动
测试或离线指标代替。

## 技术栈

C++20、CMake、ONNX Runtime、OpenCV、spdlog、SimpleIni、Dear ImGui、nlohmann/json。
