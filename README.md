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

### 自动急停模块

独立“辅助”菜单提供自动急停配置、允许键及会话暂停状态，配置保存到 `[auto_stop]`。
默认关闭，只支持 KMBOX NET。四个单键和八条有序相邻双键保留各轴历史，按连续意图估计
分别结束反向按键。允许键只授予许可；开关或按键本身不触发制动。

Runtime 已提供 `request_auto_stop(request_id)` / `cancel_auto_stop(request_id)` 供后续自动扳机调用，
请求编号在会话中严格递增。自动扳机、压枪尚未实现，因此正常应用当前没有自动制动请求来源。
独立调度在没有图像或目标时仍推进释放；暂停、End、输入异常、改向与请求到期均取消并清理。
每请求最多保持 500 毫秒后开始归还；已在途设备应答可能推迟实际清理，不能视作硬实时保证。

反向软件键持有期间暂缓 Aim 物理发送，预计完成且零键报告确认后恢复 Aim，随后按请求归还物理键。
调试报告 `auto_stop.schema=2` 记录请求、清理、应答等待、释放调用迟到及 Aim 跳过次数；无测量项为 null。
模型结果仅为预计完成，`fire_permitted` 保持 false。按当前安排，实体屏蔽/归还、停稳效果与混合负载
实机复测留到自动扳机接入阶段，软件测试和协议 ACK 不替代实机结论。

仅检查界面时可构建 `auxiliary_ui_preview` 目标，运行
`build/Release/auxiliary_ui_preview.exe`（可加 `--minimum`、`--dark`）。该入口只创建窗口，
不创建 Runtime 或设备，保存仅保留在内存中。

设备实验使用独立 `auto_stop_probe` 目标。它复用生产 KMBOX owner，但不创建 Aim/Runtime，
仅接受 WASD 单键或相邻组合；每步最多 2 秒、每计划最多 15 秒，支持 End 取消与退出清理。
计划格式为 `{"steps":[{"held_mask":1,"hold_ms":150},{"held_mask":0,"hold_ms":1500}]}`，
W/A/S/D 对应位值 1/2/4/8。`--plan <计划> --output <新报告> --dry-run` 只校验计划，不连接设备。
实际执行另需 `--config <配置>`、`--allow-physical-output --confirm AUTO_STOP_WASD_PHYSICAL`。
无需人工按 Ctrl 初始化。默认在设备连接与独占 owner 确认后执行有界计划；尚未收到物理报告时
键态仍为未知，报告 `monitor_initial_state_known=false`。首个 End 报告即可取消；已收到有效键态后
监听失效会取消。`--monitor-ready-timeout-ms` 默认 0，显式设置 1～30000 只增加有界观察时间。

连续实验可用 `--session-dir <新目录> --session-seconds <1～600>` 替代单计划参数，复用同一连接。
按序原子放入 `001.plan.json`、`002.plan.json` 等请求，工具生成同序号结果和 `session.status.json`；
目录中的 `STOP`、End 或时限结束会清理并关闭。空闲不发送运动，失败终止整次会话。
设备 ACK、实际命令间隔和释放结果均写入报告，不能据此宣称角色停稳。
`--mask-check --config <配置> --output <新报告>` 是独立实体 W 检查模式，仍要求上述物理授权参数。
它等待有效的实体 W 持有报告，150 毫秒后仅屏蔽 W，保持 1500 毫秒后解除，再观察 250 毫秒；
不会合成非零软件键，也不需要 Ctrl。屏蔽期间正常松开/重按 W 可取证新的物理报告；
开始观察的时间不冒充 W 实际按下时刻。其它方向、End、监听异常或回执未知均取消并清理。

只读画面取证使用 `xen_auto_stop_capture` 目标，输出 `XenAutoStopCapture.exe`，支持
`--output <新目录> --seconds <1～30> --fps <1～240>`；默认采样左侧 650×310 HUD 区域，
可通过 `--roi-x/--roi-y/--roi-width/--roi-height` 调整。PNG 与 `frames.csv` 保留实际帧时间，
请求 FPS 不代表实际采样率。该工具没有物理输出能力，不改变生产采集 ROI。
`scripts/read_auto_stop_hud.ps1` 使用 Windows PowerShell 5.1 的本地 WinRT OCR，保留原文和缺失值；
`scripts/analyze_auto_stop_hud.py` 分析客户端可见运动、零显示与位置稳定区间，
不将两台机器时钟直接相减，不将 HUD 括号中的 3 秒峰值当作当前速度。
这些实验工具不等于生产急停已完成；持续物理持键时的屏蔽、归还及共享调度仍需分别验证。

### 启动应用

1. 启动一次 `build/Release/Xen.exe`。程序会在同目录创建 `models/` 和默认 `config.ini`。
2. 把 ONNX 模型放入 `models/` 根目录，在“检测”页刷新、选择并应用模型。
3. 配置 Capture 与 Provider，启动 Runtime；先保持物理输出关闭，确认预览、日志和 Provider 状态。
4. 需要真实设备验收时，使用正式 Prepare/Launch 脚本生成独立 Run，并由用户手动执行带确认令牌的
   Launch 命令。

正式多 Provider 包只从根目录 `XenLauncher.exe` 进入。Launcher 负责 manifest 路由、路径安全、
后端归属和 Worker 存在性；日常启动不会扫描或哈希整包。完整文件集合与 SHA-256 校验只保留在
新完整发布、跨机复制、运行库拓扑变化或明确供应链审计边界。

### 普通界面调参

新建配置采用已接受的基线：平滑更新比例 `0.475`，X/Y 移动比例 `0.425/0.400`，
二维最大步长 `14`，延迟补偿开启、控制延迟 `15 ms`、最大补偿延迟 `44 ms`、
最大补偿距离 `15%`，预测关闭。已有配置保留用户保存值，不被默认值覆盖。

在普通界面停止 Runtime 后，可独立修改 X/Y 比例、平滑、死区、延迟和预测参数，保存后再启动。
两个开关可分别组合；关闭的功能保留参数供下次开启使用。最大补偿延迟应不小于控制延迟，
其他数值仍遵守界面和配置校验范围。自由调参不取消武装、按住启用和急停。

普通界面每次启动产生的 `cache/runtime/` 报告会保存本次实际 Aim 参数：JSON 的
`aim_config` 和 CSV 的同名注释可用于对应各次开关组合。报告不依赖随后修改的配置值。

预测关闭时，公有瞄点保持基础点。预测开启且世界运动来源可靠时，可独立产生提前点，
不要求同时开启延迟补偿；来源暂时失效后，已有提前量按原有速率回到当前基础点，恢复来源后
重新建立预测。控制延迟参数仍用于自身已发命令记账，不因两个开关关闭而失效。
四种组合的软件专项检查不代替人工闭环测试。

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

## 日志输出设置

设置页的“日志输出”提供无（不输出）、错误、警告及以上、信息及以上四档。
切换立即生效，Runtime 运行中也可调整；停止运行后点击“保存配置”，下次启动沿用
`[log].global_level`。默认仍为 INFO，已有 INI 的模块等级和输出目的地设置保持有效。

“无”停止接收新日志，不清除已有记录，异步队列中已接收的消息仍可能完成写入。
最近日志/控制台可显示 INFO 及以上，常规轮转日志文件只收 WARN/ERROR，前提是对应输出已启用。
默认 Release 中 TRACE/DEBUG 宏已裁剪，设置页不提供无效的详细日志档位；开发者需启用相应
编译选项，再通过 INI 设置全局及模块等级。
Debug CSV/JSON 运行报告与崩溃诊断独立于 Log，不随此开关关闭。

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

## 自动扳机与压枪

辅助页提供独立自动扳机，默认关闭。先配置长按键、头部/身体内部范围及按下时长，
运行后仍需全局武装、新按键边沿、健康物理输入、新鲜源图像与源端焦点才能发出按钮命令。
“要求急停完成”需要独立的观察停稳证据；当前急停只有估算状态，此模式会等待，不会把键盘 ACK 当作停稳。

双机焦点由 `xen_source_context.exe` 提供：源机显式指定 `--enable --host <源机绑定地址>
--port <端口> --process <游戏可执行文件名> --ttl-ms 200`，主程序辅助页填写对应地址、端口和进程名。
两端认证使用环境变量 `XEN_SOURCE_CONTEXT_TOKEN`（至少 32 字节），不写入 INI、命令行或报告。
程序不会自动运行源端工具；游戏持续发送数据也不等于游戏处于前台。该工具只提供焦点事实。

自动扳机与急停复用唯一 KMBOX owner；失焦、取消、帧过期或未知回执停止新动作并处理软件左键释放。
释放清理只把实际后端调用计入尝试次数，争锁单独受清理截止时间限制。按钮报告保留后端完成与
协议 ACK 原始时刻；压枪的软件触发起点仍是命令估计，不能当作游戏逐弹时间。
Debug 独立保存扳机有界事件历史、压枪拒绝原因及按 Aim/扳机/压枪分类的仲裁累计计数；普通 Log
关闭不影响这些执行证据。覆盖缺口显式记录，模型重载报告不重复导出已记录的扳机事件。
Release 专项与假设备测试不替代真实游戏/设备验收；真实输入只由用户前台触发。

压枪默认关闭，默认采用 GSI 自动识别武器并精确匹配已校准弹道。GSI 配置在辅助页：
本机默认只监听 `127.0.0.1`；双机显式设置主机绑定地址及允许的源机 IP，游戏 GSI URI
指向该 HTTP 地址。认证由 `XEN_GSI_TOKEN` 环境变量注入，玩家身份必须明确配置。
GSI 只用于武器、弹药及换弹上下文，不能提供弹道曲线、逐发时钟或焦点证明。

弹道使用显式“时间—累计 counts”曲线。已有 17 份旧数据提供可追溯导入入口，见
[本地导入说明](assets/recoil/README.md)；输出是待校准候选，不会自动激活。
在辅助页弹道编辑器中加载候选，常用只调横向/纵向强度；起压偏移和时间伸缩折叠显示。
也可直接编辑节点、撤销/重做并对比曲线。保存始终生成新版本；完成校准并填写实际证据后，
可选择独立试验引用或显式发布活动版本，原活动版本支持回退。运行期间不热换曲线。

独立压枪模式停止 Aim 的设备位移；混合模式保留 Aim 算法及原命令回执，另记已确认压枪位移。
两者串行发送，共用真实时间窗口额度，额度不足会停止该段压枪，不积存赶发。
实体左键与自动扳机按下都可建立估计起点；失焦、换枪、数据过期、未知回执后须重新释放和按下。
曲线耗尽不循环，恢复时间未经校准时不自动接续下一段。

“弹道自动优化”独立于实时输出。导入已结束的 Run 和测量后，模块检查环境/曲线/回执时序、
响应标定是否可辨识，再用稳健拟合生成有界候选，并显示留出数据的离线预测对比。
候选不会覆盖活动版；预测改善不等于新候选已取得实测改善。原始报告记录独立 `recoil.schema1`、
会话起点、逐命令回执、实际曲线版本及记录缺口，便于持续积累可复核的数据。

命令行也可独立分析：

```powershell
python -X utf8 scripts/build_recoil_dataset.py <人工测量清单manifest.json> <新的数据集.json>
build-nvidia/Release/xen_recoil_tuner.exe optimize <数据集.json> <基线profile.json> <新结果目录> <优化代数> <新版本号>
```

测量清单格式见 `python -X utf8 scripts/build_recoil_dataset.py --help`。脚本从 Debug 提取真实回执及
曲线身份，只接收用户明确提供的残差、噪声与测量时间；缺记录或未知回执不进入拟合。
UI 和 CLI 共用当前用户 `%LOCALAPPDATA%/Xen/recoil-tuner-usage-v1` 用途记录；已用于拟合、
响应标定或留出分析的数据不能重新作为独立留出。新一轮优化需新的测量资料。
成功时，新结果目录根部另存可由编辑器加载的 `SCHEMA_VALID` 弹道版本，分析报告位于
`analysis` 子目录。候选清除旧校准证据，需要重新校准后显式发布；命令不会修改活动版本。

## 技术栈

C++20、CMake、ONNX Runtime、OpenCV、spdlog、SimpleIni、Dear ImGui、nlohmann/json。
