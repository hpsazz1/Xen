# 构建、发布与开发诊断

[返回 README](../../README.md) · [采集与训练](training.md) · [控制与弹道](controls.md) · [工具与输入评估](tools.md) · [开发与诊断](development.md)

本文中的命令均在仓库根目录执行；正式包路径另有说明。界面参数以当前配置校验为准。

## 目录

- [依赖与构建](#依赖与构建)
- [应用启动与发布布局](#应用启动与发布布局)
- [可执行目标](#可执行目标)
- [脚本与发布导航](#脚本与发布导航)
- [报告语义与时间边界](#报告语义与时间边界)
- [日志与全程归档](#日志与全程归档)
- [源码与验证](#源码与验证)
- [无设备界面预览](#无设备界面预览)

## 依赖与构建

不同 Provider 使用各自匹配的 ONNX Runtime 发行包和独立构建目录。请求严格后端时不会静默回退
到 CPU。固定 shape TensorRT 可启用 CUDA Graph；DirectML/OpenVINO 保持独立运行库闭包。

### 环境

- Windows x64
- Visual Studio 2026 / MSVC v14.51，安装 C++ 桌面开发组件
- CMake 4.2 或更高版本（本文 `Visual Studio 18 2026` 生成器所需；项目声明的最低 CMake 版本仍为 3.18）
- 与目标 Provider 匹配的 ONNX Runtime SDK
- OpenCV
- 开启 `BUILD_TESTING`：PowerShell 7，以及能导入 NumPy 和 OpenCV（`cv2`）的 Python 3
- 可选：CUDA、TensorRT、cuDNN、DirectML、NDI SDK

依赖版本和导入关系以 [CMakeLists.txt](../../CMakeLists.txt) 与实际构建报告为准；不要只替换一个 GPU
SDK 后沿用旧构建目录。

测试配置会先解析解释器，再用同一个 Python 实际导入 NumPy/`cv2`；缺失或加载失败会明确终止配置。
需要指定解释器时传入 `-DXEN_PYTHON_EXECUTABLE="C:\path\to\python.exe"`，测试使用该绝对路径。

### 最小 Release 构建

```powershell
$env:ONNXRUNTIME_ROOT = "C:\path\to\onnxruntime"
$env:OpenCV_DIR = "C:\path\to\opencv\build\x64\vc16\lib"
$buildDirectory = "build/nvidia" # 按所选 ORT 后端改为 build/directml 或 build/openvino

cmake -S . -B $buildDirectory -G "Visual Studio 18 2026" -A x64 `
  -DOpenCV_DIR="$env:OpenCV_DIR" `
  -DBUILD_TESTING=ON

cmake --build $buildDirectory --config Release --target xen_app --parallel
```

输出位于对应构建目录的 `Release/`，应用文件名为 `Xen.exe`。普通开发只构建受影响目标并运行相关专项测试；
依赖/ABI、共享 Provider 契约或正式发布候选才需要完整矩阵。

日常开发固定复用 `build/nvidia`、`build/directml`、`build/openvino` 中对应后端的目录；
一次性试验与临时产物集中到 `temp/<任务ID>/`，任务结束后清理。干净重建在固定目录原位完成，
不按提交号另建构建树。轻量 NVIDIA 构建和 NDI 时钟源脚本默认使用 `build/nvidia`；
`publish_aim_worker_delta.ps1` 必须显式传入 `PackageRoot`、`DestinationRoot` 和 `RemotePackageRoot`，
避免复用已经清理的旧包路径。

需要执行当前配置的完整构建、clean `PATH` 测试和运行库来源检查时，使用正式脚本：

```powershell
.\scripts\build.ps1 `
  -OnnxRuntimeRoot "C:\path\to\onnxruntime" `
  -OpenCvDir "C:\path\to\opencv\build\x64\vc16\lib"
```

GPU 或 NDI 构建再按脚本参数传入对应 SDK 根目录。正式脚本按 ORT 发行包默认使用
`build/nvidia`、`build/directml`、`build/openvino`，也可通过 `-BuildDirectory` 指定独立目录。
VC CRT 从所用 Visual Studio 的官方 Redist 目录解析；可用 `-MsvcRedistRoot` 明确指定，
不从系统目录补拷 DLL。
依赖 NDI 图像见证的 Prepare 只接受启用 NDI 且运行库完整的工具目录；未启用时明确拒绝，
不生成缺少采集能力的半成品任务。

## 应用启动与发布布局

1. 启动一次对应构建目录的 `Release/Xen.exe`。程序会在同目录创建 `models/`；`config.ini` 不存在时按代码发行默认生成完整配置。默认 CPU 推理、本机桌面采集，物理输出和辅助总开关关闭。
2. 把 ONNX 模型放入 `models/` 根目录，在“检测”页刷新、选择并应用模型。
3. 配置 Capture 与 Provider，启动 Runtime；先保持物理输出关闭，确认预览、日志和 Provider 状态。
4. 真实设备验收使用正式脚本生成独立 Run，按用户授权手动或自动执行有界任务，回收退出及输入释放结果。

删除 `config.ini` 后再次启动会按代码发行默认重建，不会恢复此前的私人连接信息；设备地址、NDI 源及双机焦点等需重新填写。已有有效配置不被覆盖，已有但无效的配置会报错而不是静默重置。

正式多 Provider 包只从根目录 `XenLauncher.exe` 进入。Launcher 负责 manifest 路由、路径安全、
后端归属和 Worker 存在性；日常启动不会扫描或哈希整包。完整文件集合与 SHA-256 校验只保留在
新完整发布、跨机复制、运行库拓扑变化或明确供应链审计边界。

发布目录按用途组织，三个 Worker 和必要工具均来自同一提交：

```text
Xen-unified-<版本>/
├── XenLauncher.exe、manifest.json、config.ini、Launcher 必需的 VC CRT
├── runtimes/{nvidia,directml,openvino}/  Worker、校准/优化 CLI、各自 DLL
├── tools/model-data/                  模型工作区脚本和固定依赖清单
├── tools/recoil/                      弹道导入与数据集整理脚本
├── tools/source/                      源端焦点、时钟、发送、画面取证工具及其 DLL
├── assets/recoil/                     导入清单与说明
├── models/、logs/、licenses/
└── cache/                            按用途保存运行与用户数据
```

运行数据以程序根为基准：统一包使用发布根，开发版使用 `Xen.exe` 所在目录，均不取决于启动时
工作目录。`cache/runtime` 保存执行报告，`cache/datasets` 保存素材，`cache/model-workspace`
保存设置与训练作业，`cache/recoil/{profiles,calibration,tuning}` 保存曲线、校准和优化结果。
这些用户数据不能按“缓存”一概删除。随包训练脚本使用相对工具路径；用户自选脚本、素材、权重和
已验证 Python 环境保留明确路径。升级时应复核绑定，不能直接搬移 venv 或复制旧 CMake 构建树。
发布包不包含用户旧弹道 CSV、训练权重或测试 EXE，旧包可保留供回退。
完整包固定携带同提交的 `tools/source/start_source_context_session.ps1`；根目录 `Start-Xen.cmd` 使用相对路径启动 Launcher，不包含设备凭据。旧机器上自定义的同名封装需单独保存，设备与源端连接信息在各机配置中填写。
单 Worker 更新来源保存在清单内的 `tools/acceptance/WORKER-UPDATE.json`，不扩展 Launcher
固定的清单顶层字段。发布回归使用 Launcher 同一生产解析器验证生成结果，再检查传输完整性。

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
| `xen_mouse_effect_probe` | `XenMouseEffectProbe.exe` | source-frame 驱动的 X-only 实际命令/背景响应证据入口；Physical 需要明确输出确认参数 |
| `xen_mouse_effect_probe_sequence` | `XenMouseEffectProbeSequence.exe` | 离线生成平衡净零的 A/A2 序列；S1 活性 profile 以固定 source-frame cadence 生成 X-only 回锚挑战，基线保持零命令 |

`XenSender.exe --report PATH` 必须声明至少一个非零退出上限：`--max-frames` 不超过
200000，或 `--fps × --max-seconds` 的理论样本数不超过 200000；两者都提供时任一安全上限即可。
其中 0 表示不限帧/不限时，不能单独证明报告有界。无安全容量边界的组合会在 Log、Capture、网络
和报告目录副作用前以命令行用法错误退出。

## 脚本与发布导航

| 入口 | 用途 |
|---|---|
| [scripts/build.ps1](../../scripts/build.ps1) | 当前 Provider 的完整构建与测试 |
| [scripts/build_aim_debug.ps1](../../scripts/build_aim_debug.ps1) | 固定 NVIDIA 环境下的 Aim/Runtime 轻量构建 |
| [scripts/review_run.py](../../scripts/review_run.py) | 读取已有 Run，生成离线复盘报告 |
| [scripts/publish_release_bundle.ps1](../../scripts/publish_release_bundle.ps1) | 生成隔离 Provider 的正式发布包 |
| [scripts/invoke_aim_manual_acceptance.ps1](../../scripts/invoke_aim_manual_acceptance.ps1) | 生成并执行受控 Aim 人工 Run |
| [scripts/run_ndi_clock_source.ps1](../../scripts/run_ndi_clock_source.ps1) | 在 NDI 源机前台启动时钟旁路；不会访问 KMBOX |
| [scripts/run_mouse_effect_probe_output_off.ps1](../../scripts/run_mouse_effect_probe_output_off.ps1) | 以零 Mouse 能力排练 probe/source/sidecar/像素绑定 |
| [scripts/prepare_mouse_effect_probe_a.ps1](../../scripts/prepare_mouse_effect_probe_a.ps1) | 固化 A 级 X-only Physical Run；只 Prepare，不启动设备或 sidecar |
| [scripts/prepare_mouse_effect_probe_a2_s1.ps1](../../scripts/prepare_mouse_effect_probe_a2_s1.ps1) | 固化 A2 S1 的自动 KMBOX 活性括号与零命令基线；只 Prepare，不执行 Physical Launch |
| [scripts/benchmark_runtime.ps1](../../scripts/benchmark_runtime.ps1) | Runtime 正式基准与原子报告 |
| [scripts/measure_process_resources.ps1](../../scripts/measure_process_resources.ps1) | 绑定既有进程身份，只读采集 CPU、内存、句柄和线程趋势 |
| [scripts/test_tensorrt.ps1](../../scripts/test_tensorrt.ps1) | TensorRT 专项正确性与变化输入验证 |
| [scripts/test_directml.ps1](../../scripts/test_directml.ps1) | DirectML 独立构建与专项验证 |
| [scripts/test_openvino.ps1](../../scripts/test_openvino.ps1) | OpenVINO 独立构建与专项验证 |

其余专项入口位于 [scripts/](../../scripts/)。脚本是参数和证据格式的事实源；不要长期维护一次性脚本。

仅更新审核脚本和页面时，使用 `publish_worker_delta.ps1 -ModelDataReviewOnly`，只投递两项审核资源与清单，保留既有 Worker 构建身份，无需重建二进制。

向既有主辅日常包投递长稳工具时，`publish_worker_delta.ps1 -IncludeSoakAcceptanceTools`
显式纳入验收入口、资源采集器、共享监督脚本和 source-session 启动脚本，绑定同一次源码提交；
不需要更新 source 可执行文件。差量发布逐端保留配置，准备 Run 本身不会启动真实输出。

配置校验变更的差量发布应使用 `publish_worker_delta.ps1 -IncludeLauncher`，同时更新主程序与启动器，避免启动器保留旧配置规则。

## 报告语义与时间边界

Runtime 启动阶段的采集故障保留 FAILED 和原始错误，不会被启动末尾的 RUNNING 覆盖；
停止或再次启动会先回收本轮线程与模块。NDI 持续无效视频以及 UDP/XUDP 空数据报均不刷新有效帧期限，
首次等待和断流仍按配置超时；期限内收到有效帧可恢复。

Runtime 滚动时延窗口只接收检测、Aim 成功的样本；已发送样本还要求鼠标后端 READY。
合法空检测计入成功，失败样本不会挤走此前的成功样本。此口径与 Debug 一致，不代表物理发送成功率。
同一窗口复用排序结果计算各分位数，插值定义不变；未测量实际帧率收益。

Benchmark 的模型和报告路径统一按 UTF-8 解释，支持中文与空格。中间文件位于本次独占的 staging 目录，
正式 CSV/JSON 和 Provider profile 均拒绝覆盖已有目标；失败或取消只清理本次拥有的输出，
期间出现的同名外部文件保留。成对报告顺序发布失败时，只回滚本次已发布文件；清理受阻会报告精确残留。
DebugReport 的实际写入、CSV 回滚副本、锁定标记删除以及报告内 CSV 文件名和路径错误信息同样使用 UTF-8，
不依赖 Windows ANSI 代码页。分段归档与视频报告在调用边界转换路径；压枪归档目录元数据同样使用 UTF-8。
保存失败后仍可重试；第二份报告发布失败时恢复旧 CSV，无旧 CSV 时移除本次 CSV。
回归覆盖中文含空格目录与文件名的保存、生产回读、正式发布，以及保存失败、发布碰撞后的生产清理和重试。
CSV/JSON 的顺序发布与补偿不提供跨文件事务或并发读者隔离。
离线 production-red 证据必须在 trace 和 manifest 均成功关闭后才发布，关闭失败不会产生成功 bundle。

Aim 尚未发送时错过输出时限，只取消当前帧并重置控制状态，下一张新鲜观测重新校验；
不会补发旧命令，也不会延长观测或设备等待期限。共享输出故障、实际发送失败和反馈账本异常仍保持
原有禁止输出规则。报告中的处理成功数不能代表持续物理输出成功；`final_snapshot.aim_dispatch_rejections`
独立保留整场拒绝计数及首末事件，区分进入前过期、等待截止、取得锁后过期、计算后过期和共享故障。

NDI `timestamp` 在报告中明确记为 SDK submission time，不称为桌面采集或曝光时刻。源机旁路以低频
四时间戳交换把该 UTC 时间映射到接收机 `steady_clock`，并逐帧输出 status、RTT、uncertainty、rate、
mapping age、sample count 和 source session；映射未就绪、过期或回跳时保持无效。Mouse 报告也分别
记录 backend completion、匹配协议响应的 protocol ACK 和独立 physical effect；当前后端没有物理效果
观测能力，因此不能由 API 返回或 ACK 推导真实鼠标已经移动。Mouse Benchmark schema 2 另行绑定
run UUID、completion semantic 与 peer/test boundary；正式脚本只在完整聚合键一致时复制 timing，
loopback/in-memory fake 不与真实设备报告合并，并始终显式记录 `physical_effect_observed=false`。
Runtime 报告将原始 source sequence/timecode/timestamp、映射后的 source、capture、
Aim observation 和 control 时刻及各自有效性绑定到实际处理帧；缺失值保持无效，不用本地序号补齐。
当前 schema 与字段定义以 [Debug 报告实现](../../Xen/debug/debug.cpp) 为准，分析旧报告时按其实际 schema 解释。
报告的原始逐帧表和原有时延摘要仍是有界尾窗。JSON 的 `session_aggregate` 另保存全部已摄入样本的
成功/失败计数，以及处理、采集到后端完成、控制到后端完成三项时延；成功口径包含实际发送时的鼠标
后端状态。`minute_trend` 保存最多1440个分钟摘要，窗口省略、无效时间和采集异常分别标记。
新增分位数是0.25毫秒固定桶的上下界，超过1024毫秒时上界为空；原尾窗精确分位数保持原语义。
Runtime丢样仍需单独核对，全部已摄入不等于全部Runtime帧；模型重载产生的分段须按同次Runtime
合并计数和桶，不能平均各段P95。尾部报告在正常结束时落盘；日常全程分段归档见下文。
这些 64 位标识、绝对时刻和 source clock session 在 JSON 中使用十进制字符串，CSV 保留整数文本，
避免解析器经过浮点数时损失相邻帧身份。`steady_ns` 只可在当前 Runtime 会话内比较；原始源时间
沿用 Capture 单位，源时钟 session 不等于 NDI 发送端身份，以上字段不提供曝光或设备应用位移证据。
Win32 的 execution boundary 内生为 `local_os_api`；KMBOX/MAKCU 不再从 endpoint 或脚本默认值推断
外部设备，必须显式传入 `ConfiguredExternalDevicePeer`，127/8 KMBOX fake 则必须显式传入
`LoopbackUdpFake`，且在创建报告目录或打开设备前完成拒绝。

## 日志与全程归档

设置页的“日志输出”提供无（不输出）、错误、警告及以上、信息及以上四档。
切换立即生效，Runtime 运行中也可调整；停止运行后点击“保存配置”，下次启动沿用
`[log].global_level`。默认仍为 INFO，已有 INI 的模块等级和输出目的地设置保持有效。
采集模块名为小写 `data`，可在 `[log_modules]` 中用 `data=OFF` 关闭该模块普通日志。

“无”停止接收新日志，不清除已有记录，异步队列中已接收的消息仍可能完成写入。
最近日志、控制台和常规轮转日志文件均可显示 INFO 及以上，仍受全局及模块等级过滤；前提是对应输出已启用。普通文件由后台线程及时刷新，无需等待警告或退出。
默认 Release 中 TRACE/DEBUG 宏已裁剪，设置页不提供无效的详细日志档位；开发者需启用相应
编译选项，再通过 INI 设置全局及模块等级。
详细运行数据与崩溃诊断独立于普通日志等级。设置页“日志与运行记录”中的“详细运行记录”单一开关
控制 Runtime CSV/JSON、逐帧分段归档、扳机事件及自动 recoil-batches；默认关闭，旧配置缺少
`[runtime].diagnostics_enabled` 时也按关闭处理，已有明确值继续保留。

运行中切换立即停止接收新记录，由后台完成已接收数据的有限封尾；不删除历史文件。再次启动、停止或
模型重载都不会自行重新开启。停止 Runtime 后保存配置，下次启动沿用该选择。显式采集素材、输入录制、
独立调试任务以及必要错误和崩溃反馈保持各自用途。

自动调试可只为本次进程设置 `XEN_RUNTIME_DIAGNOSTICS=1`。界面显示记录已启用，但保存其他设置时
仍写回原日常记录偏好；只有用户主动切换记录开关后，新偏好才可保存。HUD、Soak、原包压枪、Aim 人工验收
与实战 GUI 验收入口
在启动子进程时临时设置该变量，并恢复调用方环境；退出后日常偏好不变。

### 实战全程归档与异常标记

开启“详细运行记录”后，Runtime 后台分段保存逐帧数据和扳机事件，目录为
`cache/runtime/<会话>-g<模型代>-s<段号>-archive/`。约每5秒或累计1200帧生成
`segment-*.csv`、`segment-*.json`和对应`.meta.json`索引，`manifest.json`保存覆盖、缺口和错误。
不设置整场时长、文件总量或累计磁盘配额；已落盘分段不会因内存尾窗轮转被覆盖。
内存队列保持有界；相邻小批次在既有帧数和事件上限内合并，避免分段写盘时仅因界面刷新次数耗尽批次容量。
异常标记保持顺序边界，纯事件与最终状态更新也会保存。队列过载或磁盘失败会在界面及清单中显示丢样/错误，
清单分别记录合并次数、批次容量拒绝和帧数容量拒绝；不能把存在缺口的归档当作完整数据。
正常停止会排空队列并封尾；崩溃时已完成分段保留，尚未写出的尾部可能丢失，未封尾清单不算完整。
模型热重载使用独立目录，加载窗口沿用原报告边界而不归档，不能跨模型段假定连续覆盖。
逐帧诊断在发布时核对本次推理所属模型代际；旧模型的在途尾帧不会进入新段。
各个开段入口统一取得当前扳机、压枪事件水位。关闭时保留冻结事件范围，等待压枪归档封尾后只回填归档终态；
故障重启先完成旧报告，再重置 Runtime；保存配置触发的跨运行时重启也使用同一成功门槛。
后台任务回收和报告发布成功分别判断。发布失败保留原快照、样本和错误，取消自动重启/切段；显式重试不重新采样、
排空或过滤旧事件。关闭流程不会无限重试，尚未保存的数据仅保留在当前进程。
`app_report_lifecycle_tests` 通过 App 共用路由覆盖 LOADING 期间开启记录、重载成功/失败及取消；
同时验证最终报告真实发布失败、恢复重试、两类重启门槛与独立新段，
`runtime_model_reload_test` 使用确定性屏障覆盖跨代在途帧，均不代表真实设备验收。
主循环通过 `ReportLifecycle` 统一回收封尾与停止任务，再在本帧输入处理后消费重启请求。
`app_report_interleave_tests` 使用同一调度入口、真实报告文件和两阶段同步屏障，覆盖封尾后停止、
两类重启、保存失败显式重试，以及停止/急停在封尾中、停止中和停止刚回收时取消。
Runtime 停止使用无设备适配；此测试不启动完整窗口、实际 Provider 或 Launcher 子进程。

记录已开启时，发现异常可按默认 **F9**，或点击界面“标记异常”。关闭记录时不会写标记或暗中开启归档。
F9通过当前键鼠后端监听源机按键，
不依赖辅机获得游戏焦点；只记录时刻，不武装、不发送输入。在设置页“标记异常（按下）”可更改绑定。
旧配置若已把F9用于其他功能，新增默认绑定会禁用，保留原功能；设置一个不冲突的键并保存后即可使用。
`marker-*.json`同时记录本机单调时钟和UTC时刻，以前后各30秒范围关联分段索引，不复制另一套帧数据；
启动太晚或停止太早导致的窗口不足会明确标记。保存NVIDIA即时回放片段并记住回合，赛后可结合归档和Demo分析。

记录已开启时，普通CSV/JSON尾部报告最多保留最近10000帧（240 FPS约42秒），全程累计摘要语义保持；
分析整场或早期异常时应读取`-archive`目录的分段和覆盖清单，不能只看尾部报告。

## 源码与验证

先按下面的功能入口阅读，再沿调用链定位实现；完整目录可直接浏览 [Xen/](../../Xen/)。这些是源码模块，独立工具的可执行目标见[上方目标表](#可执行目标)。

| 想了解或修改什么 | 优先入口 | 相关模块 |
| :-- | :-- | :-- |
| 应用启动与主循环 | [app/main.cpp](../../Xen/app/main.cpp) | `app/`，配置与各模块生命周期 |
| 发布包选择推理后端 | [app/launcher.cpp](../../Xen/app/launcher.cpp) | `app/` 中的路由、发布合同与启动校验 |
| 页面、控件和状态展示 | [overlay/](../../Xen/overlay/) | `overlay.cpp` 及功能面板；配置定义在 `config/` |
| 图像从哪里进入 | [capture/](../../Xen/capture/) | `sender/` 发送端、`capture_evidence/` 画面取证 |
| 模型加载与推理结果 | [detector/](../../Xen/detector/) | Session、预处理、后处理及后端适配 |
| 目标如何转换成控制请求 | [aim/](../../Xen/aim/) | `runtime/` 组帧与控制协调 |
| 启停、队列与输出许可 | [runtime/runtime.h](../../Xen/runtime/runtime.h) | `runtime.cpp`、`runtime_queue.cpp`、`mouse/`、`keyboard/` |
| 扳机、急停与武器弹道 | [trigger/](../../Xen/trigger/)、[auto_stop/](../../Xen/auto_stop/)、[recoil/](../../Xen/recoil/) | `weapon/` 提供共享武器资料；`recoil_tuner/` 负责离线优化 |
| 旋转跳与 Long Jump | [movement/](../../Xen/movement/) | 动作阶段、方向与清理；`keyboard/` 和 `mouse/` 提供设备能力 |
| 素材保存与训练流程 | [data_collection/](../../Xen/data_collection/)、[model_workspace/](../../Xen/model_workspace/) | [scripts/model_data_pipeline.py](../../scripts/model_data_pipeline.py) 及训练脚本；C++ 工作台编排后台作业 |
| 双机焦点和时间同步 | [source_context/](../../Xen/source_context/)、[clock_sync/](../../Xen/clock_sync/) | `source_context_host/`、`clock_source/` 提供源端进程 |
| 日志、归档与离线评价 | [debug/](../../Xen/debug/)、[debug_session/](../../Xen/debug_session/) | `log/`、`crash/`、`input_training/`、`reference_assessment/` |

### 工具与测试的位置

- `benchmark/`、`mouse_benchmark/`：运行链与设备后端基准。
- `auto_stop_capture/`、`auto_stop_probe/`：急停画面取证与独立设备探针。
- `mouse_effect_probe/` 及同名前缀目录：鼠标效果取证、序列生成与结果封存。
- `recoil_calibration_cli/`、`recoil_tuner_cli/`：校准与优化命令行入口。
- `aim_landmark/`、`aim_production_red/`：Aim 诊断与回归支持。
- [tests/](../../tests/)：按模块查找测试，`tests/fixtures/` 保存测试素材；实际目标和登记以 [CMakeLists.txt](../../CMakeLists.txt) 为准。
- [scripts/](../../scripts/)：正式构建、数据处理、训练、发布和分析入口；先查看脚本参数或 `--help`。
- [review_run.py](../../scripts/review_run.py)：只读已有 Run，生成离线复盘报告；[tests/review_run_tests.py](../../tests/review_run_tests.py) 覆盖对应工具。


源码仓库中的 `assets/` 保存可分发资料；用户模型、采集内容和运行报告放在应用数据目录，不应混入测试夹具或公开截图目录。

模块内 `.h` 与 `.cpp` 平铺；源码 include 以 `Xen/` 为根，例如
`#include "detector/detector.h"`。

构建和测试应按变更影响选择受影响目标，真实设备验收独立于自动测试。

`aim_tests` 同时执行历史画面的同库存状态配对和独立延迟反馈质量检查。后者让每个分支自己的
输出驱动后续观测，覆盖减速、反向和停止；固定历史画面的整数请求量只作诊断，不作为物理过冲真值。
闭环包络是软件不退化基线，不能替代真实 KMBOX 验收。

## 辅机 PowerShell 管理

已配置严格主机校验及密钥登录的 SSH 别名可通过
`scripts/invoke_auxiliary_powershell.ps1 -SshHost xen-aux -ScriptPath <本地脚本路径>`
直接执行辅机 PowerShell，收集后返回中文输出，失败时在错误消息中保留远端退出码；管理操作加 `-RequireAdministrator`，
会在普通用户会话中明确失败。脚本编码只用于传输，不是加密，不得在脚本中写入凭据。

只有明确需要管理员维护时，用户才在辅机管理员 PowerShell 执行
`scripts/enable_auxiliary_ssh_admin.ps1 -UserName XenDeploy -PublicKeyPath <公钥文件> -ResultPath <结果JSON>`。
该入口将指定现有账户加入管理员组，并追加主机 Ed25519 公钥到 Windows OpenSSH 默认管理员公钥文件，
保留已有公钥，设置仅管理员与 SYSTEM 可访问的权限。适用于保留默认管理员 Match 规则的 SSH 配置；
自定义公钥路径需先核对。完成后使用新 SSH 会话验证权限，不关闭主机校验，不启动 Xen 或真实输出。

## 远程自动调试

已有任务授权覆盖自动调试时，可从主机调用同一个 PowerShell 入口。普通准备和只读检查用默认
`Execute`；GUI、采集和源服务用 `Start` 进入辅机已登录用户的桌面会话。远程脚本使用辅机本地路径，
不写入口令或令牌；沿用已有凭据目录和正式工具的物理输出参数。

```powershell
$run = 'C:\XenLab\runs\my-debug-run'
.\scripts\invoke_auxiliary_powershell.ps1 -Mode Start -ScriptPath .\temp\my-task\run.ps1 -RunDirectory $run -TimeoutSeconds 120
.\scripts\invoke_auxiliary_powershell.ps1 -Mode Status -RunDirectory $run
.\scripts\invoke_auxiliary_powershell.ps1 -Mode Stop -RunDirectory $run
.\scripts\invoke_auxiliary_powershell.ps1 -Mode Recover -RunDirectory $run
```

`ScriptPath` 是主机上的任务脚本；`RunDirectory` 是辅机上的唯一 Run。脚本通过
`XEN_AUXILIARY_RUN_DIRECTORY` 取得该目录，检查 `XEN_AUXILIARY_STOP_FILE` 后停止后续动作，并在
`finally` 收尾。复用已有有界工具，工具返回后核对原生报告和输入释放；反向轻点同时支持原有
`result/STOP`。超时或监督异常先请求取消，最多进行一次 5 秒清理等待。未确认子进程退出时保留
`*_cleanup_unconfirmed`；子进程可能仍在运行，须核对本 Run 的进程身份及原生报告，不能重新发送动作。
按需启动的计划任务不能依靠 `ExecutionTimeLimit` 保证终止子树；有限等待和监督器退出不代表输入已释放。
Windows PowerShell 的普通属性读取可能吞掉 getter 异常；runner 显式调用 `get_Handle()`，使句柄读取失败进入取消和退出确认路径。
`test_auxiliary_powershell.ps1` 使用真实 .NET getter 故障覆盖未退出与等待中退出两例，并检查 Running/Queued/Ready 下的回收门槛。

同一个 Run 再次 `Start` 只返回原任务状态；断线后先查 `Status`，避免重复真实动作。`Recover`
回传日志与结果，仅在确认子进程退出（或确认未尝试启动）且计划任务不再运行时注销，保留 Run 文件供既有 SMB/SCP 回收。
报告分别保存子进程身份、`child_exit_confirmed`、任务结束时间 `ended_utc` 与监督器结束时间 `supervision_ended_utc`。
旧报告只有 `ended_utc` 时保留清理未知，不自动注销。`completed` 只表示脚本退出成功；`timed_out`
及各种清理未知状态须结合工具报告排查，不能作为物理释放证据。
单纯启动 Launcher 不代表 Runtime 已运行或已停止，自动调试脚本仍须管理自己的运行结束条件。

## 无设备界面预览

预览沿用已完成 CMake 配置的后端构建目录。仅检查辅助界面时构建并运行 `auxiliary_ui_preview`：

```powershell
$buildDirectory = "build/nvidia" # 与已配置后端一致，也可为 build/directml 或 build/openvino
cmake --build $buildDirectory --config Release --target auxiliary_ui_preview --parallel
& "$buildDirectory/Release/auxiliary_ui_preview.exe"
```

可加 `--minimum`、`--dark`。该入口只创建窗口，不创建 Runtime 或设备，保存仅保留在内存中。

模型工作区完整截图使用目标 `model_workspace_ui_preview`：

```powershell
$buildDirectory = "build/nvidia" # 与已配置后端一致，也可为 build/directml 或 build/openvino
cmake --build $buildDirectory --config Release --target model_workspace_ui_preview --parallel
& "$buildDirectory/Release/model_workspace_ui_preview.exe" "C:\path\to\new-preview"
& "$buildDirectory/Release/model_workspace_ui_preview.exe" "C:\path\to\new-dark-preview" --minimum --dark
```

将示例截图路径替换为本次新目录。该程序以合成快照渲染生产Overlay，不构造Runtime或输入设备，不执行设备或模型业务动作；专项模式可编辑隔离配置。截图是界面预览，不是实战证据。

快捷键一致性回归使用同一目标的 `--hotkey-audit` 模式：

```powershell
& "$buildDirectory/Release/model_workspace_ui_preview.exe" "temp/<任务ID>/hotkey-ui" --hotkey-audit
```

该模式实际点击生产控件并注入本进程合成监听快照，覆盖合法共键 INI 加载后关闭身法（停止/运行两种
快照）、禁用功能旧键复用、启用后反向捕获拒绝和保存回读。失败返回非零退出码并保留
`hotkey-audit.txt` 与失败帧；成功仅证明界面和配置契约，不证明真实 Runtime 清理或设备释放。
配套运行 `config_tests`、`io_tests`、`overlay_tests` 和 `hotkey_device_capture_tests`；
`Config` 与 `Overlay` 共用 `Xen/config/hotkey_config_internal.h`，修改绑定占用规则时同时核对配置、
Keyboard 监听器和生产界面，不在面板或捕获分支另写一套占用判断。

技术栈：C++20、CMake、ONNX Runtime、OpenCV、spdlog、SimpleIni、Dear ImGui、nlohmann/json。
