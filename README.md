# Xen — Windows AI 辅助瞄准工具

Xen 是基于 C++20 原生实现的 Windows AI 辅助瞄准工具。核心管线为：截图采集 → YOLO 目标检测推理 → 瞄准控制。

当前仓库已形成 P0 单机最小闭环，并完成 UDP MJPEG、XUDP JPEG、NDI 接收端、独立
XUDP 生产发送端、KMBOX NET 鼠标后端与独立鼠标性能基准工具的源码和自动回环测试：

```text
Desktop Duplication CPU/D3D11/CUDA/DirectML ─┐
OBS/FFmpeg UDP MJPEG ─────┤
XenSender DXGI/XUDP ────────┤
OBS/NDI Sender ───────────┴→ Detector → Aim → Runtime SafetyGate ─┬→ Win32 SendInput
                                                                 └→ KMBOX NET UDP
```

物理鼠标输出默认禁用。只有配置显式允许、Runtime 已启动、用户完成武装、按住启用键且
急停未触发时，Pipeline 才会调用 Mouse。Capture、Detector 或 Aim 失败均不会沿用旧结果。
按住启用键和急停键必须是 `1..255` 内两个不同的 Win32 虚拟键；按住键同时报告按下/释放
边沿，急停只在每次按下的上升沿报告一次，持续按住不会反复提交事件。

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
│   ├── crash/                 # Windows 未处理异常与紧急日志尾部落盘
│   ├── capture/               # DXGI + UDP/XUDP + NDI + 主机 FOV 坐标契约
│   ├── sender/                # DXGI ROI 到 JPEG/XUDP 的独立主机发送工具
│   ├── aim/                   # 观测归并、追踪、目标选择和移动控制
│   ├── mouse/                 # 设备无关命令、Win32 与 KMBOX NET 后端
│   ├── mouse_benchmark/       # 复用生产 Mouse 后端的独立性能基准入口
│   ├── keyboard/              # 按住启用与急停键轮询
│   ├── config/                # SimpleIni 静态配置与校验
│   ├── runtime/               # 生命周期、三槽最新帧队列和安全门控
│   ├── benchmark/             # 复用生产 Runtime 的无界面正式基准入口
│   ├── overlay/               # Codex 浅色/深色 ImGui 控制台
│   └── app/                   # Windows 应用入口与 Xen 品牌资源
├── AGENTS.md                  # 本地开发规范，不纳入 Git
└── docs/                      # 本地设计文档，不纳入 Git
```

文档和构建文件中的源码路径均以仓库根目录为基准，例如 `Xen/detector/detector.cpp`。源码内部的 `#include "detector/detector.h"` 以 `Xen/` 为包含目录基准。

## P0 运行模型

- 主线程负责 Win32/D3D11/ImGui 消息循环、只读快照渲染和意图提交。
- Capture 线程从 Desktop Duplication、UDP/XUDP 或 NDI 获取 ROI，并发布到三个可复用槽组成的最新帧队列。
- Desktop Duplication 收到 `DXGI_ERROR_ACCESS_LOST` 且 D3D11 设备仍有效时，会在 1 秒
  有界窗口内重枚举同一输出并原位重建 duplication；旋转或分辨率变化、设备移除、
  超时均仍进入 FAILED。成功重建返回 `NO_FRAME`，不发布旧帧，次数在 schema 6
  `duplication_recoveries` 中单独记录，不混入丢帧或失败帧。
- Pipeline 线程依次执行 Detector、Aim、安全门控和 Mouse，不增加独立控制线程。
- Detector 可在 Runtime 运行期间异步热重载。候选 Session 在独立线程加载，旧模型继续处理帧；
  只有候选完整加载成功后才在两帧之间交换指针。失败保持旧模型和 `RUNNING`，成功后强制解除
  输出武装并重置 Aim 状态；加载窗口拒绝新的武装请求。
- Runtime 每处理一帧把固定大小的 Pipeline 诊断样本写入有限环；样本同时固化主机 FOV、编码
  尺寸、ROI 和像素比例，避免网络重连或显示模式变化造成的瞬时坐标漂移被最终快照掩盖。主线程
  在会话结束时将其发布为 schema 6 的
  `cache/runtime/<进程-运行时钟-generation-segment>.csv/.json`。模型重载前结束当前报告，
  加载窗口不记录样本，完成后按实际活动模型和 Provider 开始新分段，避免新旧模型混合统计。
  报告按成功/失败状态隔离耗时，
  输出 capture、queue、preprocess、inference、H2D、D3D11→CUDA、D3D11→DirectML、GPU preprocess、execution、D2H、postprocess、aim、
  mouse、total 的 mean/P50/P95/P99/最大值；报告队列满载只覆盖最旧诊断样本，不反压核心线程。
- 默认兼容链仍从 Capture 交付 CPU BGR ROI。TensorRT CUDA Graph 默认用 OpenCV 在 CPU 完成
  resize/LetterBox，再经固定 pinned `uint8` staging 上传，由 CUDA kernel 完成 BGR→RGB、归一化和
  HWC→CHW；DirectML、CPU、CUDA 非 Graph 保持 CPU float 前处理。显式开启
  `capture.enable_d3d11_cuda_interop` 时，Desktop Duplication 改为发布三槽 BGRA8 D3D11 纹理，
  Runtime 启动期预注册 CUDA 资源，并通过 TensorRT CUDA Graph 消除 host upload。该路径要求 ROI
  等于模型静态输入、实际 Provider 为 TensorRT，且会关闭 ROI 预览和运行期模型热重载；默认关闭。
  独立的 `capture.enable_d3d11_directml_interop` 路径使用三槽共享 BGRA8 纹理、D3D11 共享 fence
  和 DML Session 的 D3D12 command queue，由 compute shader 直接写入固定 DML float CHW
  输入；它同样要求 Desktop Duplication、静态模型尺寸和实际 DirectML Provider，
  且与 CUDA 互操作互斥。两条实验路径均默认关闭。
- Overlay 采用 152 px 居中标签栏和无外框独立工作区。自绘标题栏在侧栏交界处仅用底色分区：左段与侧栏同色，右段与工作区同色，并随浅色/深色主题同步切换；标题栏和顶部控制条下方均不绘制分割线。侧栏仅保留概览、检测、瞄准、输入和设置五个标签，底部以无边框两行状态区展示输出门状态和版本。工作区底色从交界处连续铺满，顶部控制条保留 18 px、页面内容保留 28 px 左侧间距，只为状态卡、配置组等内部模块保留细边框；配置页保存操作固定在页头。运行期间仅 Detector 配置允许编辑、保存和热重载，Capture、Aim、Input 与 Runtime 配置仍锁定。
- 当前 Overlay 已经人工确认并作为后续 UI 扩展基线。新增部件必须复用现有语义色、间距、表单控件和内部模块样式，不得增加工作区外框、嵌套卡片或新的导航分组；改变整体布局或主题体系前必须重新人工复核。
- Codex 浅色与深色主题可在偏好设置中切换并保存到 `config.ini`。窗口尺寸的配置与实际拖拽下限统一为 `820×600`；连续数值使用“滑块粗调 + 数值框精确输入”，手填值在 Enter 或失焦时按合法范围校验。
- 概览页维护最近 256 个成功 UI 观测样本的采集 FPS、端到端延迟和目标置信度曲线。历史环只保存固定容量标量，失败帧不折叠为零值，不新增 Runtime 锁、队列或图像复制。
- 检测页提供显式启用的实时 ROI 诊断。Runtime 最多 10 FPS 发布最长边 512 的同帧 BGRA、
  Detection 和 Aim 快照，三个预分配槽只保留最新帧；Overlay 复用 D3D11 动态纹理绘制检测框、
  当前目标、轨迹 ID、预测状态、瞄点和主机准星。离开检测页、停止或故障会清除旧画面；关闭时
  不执行 resize、颜色转换或图像复制。`XenBenchmark.exe` 不创建 Overlay，因此正式基准不包含该支路。
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
解释为严格端到端帧龄。主机可直接运行 `XenSender.exe`，由 Desktop Duplication 采集 ROI 并
发送 XUDP；OBS 不能直接输出 XUDP，仍只用于裸 UDP/NDI 兼容链。协议详见本地文档
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

双机 XUDP 的主机发送目标为 `xen_sender`，Release 输出名为 `XenSender.exe`。目的地址必须显式
提供；默认采集主机中心 `320x320`、JPEG 质量 85、XUDP 数据报上限 1400 字节、发送上限及协议
声明 240 FPS：

```powershell
.\build\Release\XenSender.exe `
  --destination udp://192.168.1.20:5000
```

在 `2560x1440` 主机上，默认几何应记录为
`source=2560x1440, roi=(1120,560,320x320), encoded=320x320, scale=(1,1)`。
辅机 `1920x1080` 不传给发送器，也不进入 XUDP 几何或 Aim counts。可使用 `--adapter`、
`--output`、`--roi-width/--roi-height`、成对的 `--roi-x/--roi-y`、`--jpeg-quality`、`--fps`、
`--datagram-bytes`、`--max-frames`、`--max-seconds` 和 `--report` 调整；完整参数以
`XenSender.exe --help` 为准。正式发送使用包装脚本，默认连续 300 秒并生成逐帧发送报告与
环境清单：

```powershell
.\scripts\benchmark_xudp_sender.ps1 `
  -Destination udp://192.168.1.20:5000 `
  -BuildDirectory ".\build" `
  -ReportPrefix ".\cache\runtime-benchmark\xudp-host"
```

鼠标性能基准目标为 `xen_mouse_benchmark`，Release 输出名为 `XenMouseBenchmark.exe`。它在
独立进程中复用生产 `MouseDeviceFactory` 和 Win32/KMBOX NET 后端，分开记录 `open()`、首条
正向命令、首条反向补偿、预热和正式命令时延。默认执行 100 组预热和 10000 组正式样本；
每组都是相同 counts 的正反命令，尽量把累计物理位移归零。工具会发送真实输入，因此程序和
脚本都要求显式物理输出开关与固定确认令牌，缺少任一项都在设备打开前失败。正式 Win32
调用示例：

```powershell
.\scripts\benchmark_mouse.ps1 `
  -Backend Win32 `
  -BuildDirectory ".\build" `
  -ReportPrefix ".\cache\mouse-benchmark\win32" `
  -AllowPhysicalOutput `
  -PhysicalOutputConfirmation XEN_MOUSE_BENCHMARK_SENDS_REAL_INPUT
```

KMBOX NET 还需显式提供设备 IPv4、端口和 UUID。UUID 仅作为进程参数传给生产后端，不写入
程序报告或环境清单：

```powershell
.\scripts\benchmark_mouse.ps1 `
  -Backend KmboxNet `
  -KmboxIp 192.168.1.10 `
  -KmboxPort 12345 `
  -KmboxUuid A1B2C3D4 `
  -ReportPrefix ".\cache\mouse-benchmark\kmbox" `
  -AllowPhysicalOutput `
  -PhysicalOutputConfirmation XEN_MOUSE_BENCHMARK_SENDS_REAL_INPUT
```

脚本在 clean `PATH` 下执行，复核 Git、CMakeCache、可执行文件和输出目录 DLL 的前后
SHA-256，只在全部命令成功、最终状态为 `READY` 且每条正式样本严格正反成对时发布
`<prefix>.mouse.json` 与 `<prefix>.mouse.environment.json`。首个失败命令立即停止，失败样本不进入
分位数，也不会留下半份正式报告。自动测试只使用本机 UDP 假 KMBOX，不发送真实 Win32 输入；
真实设备的冷启动、5 分钟稳定性和断网急停仍需现场执行。

无界面正式基准目标为 `xen_benchmark`，Release 输出名为 `XenBenchmark.exe`。它复用生产
`Runtime`、`Capture`、`Detector`、`Aim` 和 `DebugReport`，不创建 Overlay；运行时强制
`allow_send_input=false`、使用禁用的 Win32 Mouse 且从不武装 SafetyGate。默认逐帧验收本机
`source=2560x1440, encoded=2560x1440, roi=(1120,560,320x320), scale=(1,1)`。在辅机接收
XUDP/UDP/NDI 时，主机 FOV 和 ROI 不变，只按实际传输内容覆盖 `--expect-encoded`，辅机显示器
`1920x1080` 仍不进入坐标换算。

正式运行应使用脚本，它会在 clean `PATH` 下执行，运行前后复核模型、配置、可执行文件和部署
DLL 的 SHA-256，逐样本校验 Provider、状态和几何，全部通过后才发布 CSV、JSON 与环境清单；
任一步失败都会清理本轮 pending 报告：

```powershell
.\scripts\benchmark_runtime.ps1 `
  -ModelPath "C:\path\to\model.onnx" `
  -Backend tensorrt `
  -OutputFormat channel_first `
  -BuildDirectory ".\build" `
  -ReportPrefix ".\cache\runtime-benchmark\dxgi-tensorrt"
```

TensorRT/CUDA 正式基准还必须为同一模型和后端提供独立的 ORT 节点级 profile；脚本会自动将
profile 作为第四个成组产物发布，并把最终路径、SHA-256 和每个 Provider 的 Node 数量写入环境清单。
直接调用 `XenBenchmark.exe` 时使用 `--provider-profile PATH`；DirectML/CPU 不接受该参数。

模型 AUTO 契约存在歧义时必须通过 `-OutputFormat` 显式指定，脚本不会猜测未知布局。默认门槛为
100 个 warmup、至少 10000 个正式成功样本、至少 300 秒且最大 600 秒。调试短冒烟
可显式降低门槛，但不能写成正式性能结论。FP16、CUDA Graph 和 GPU 前处理通过
`-EnableFp16 on|off`、`-EnableCudaGraph on|off`、`-EnableGpuPreprocess on|off` 控制，默认均为
`on`。`<prefix>.environment.json` 最后发布，是整组报告完成
标记；没有该文件或 `complete` 不为 `true` 的 CSV/JSON 不得视为有效报告。
TensorRT 和 CUDA 允许节点级回退，环境清单会通过独立 ORT profiling 记录实际 Node 归属：
TensorRT 允许 `TensorRT -> CUDA -> CPU`，CUDA 允许 `CUDA -> CPU`。profiling Session 与正式
性能 Session 分离，诊断开销不会混入正式样本。DirectML 会禁用 CPU 节点回退。

D3D11/CUDA 互操作可追加 `-EnableD3D11CudaInterop on`。脚本会强制要求 Desktop Duplication、
TensorRT、CUDA Graph 与 GPU 前处理，并逐样本核对 `h2d_ms=0`、`input_upload_bytes=0`、
`input_device_copy_bytes=roi_w*roi_h*4`。2026-08-01 在干净提交 `daecbdc` 上完成同机各 5 分钟
on/off A/B：默认 CPU BGR 为 78,007/78,007 成功，互操作为 78,003/78,003 成功，两侧失败和
诊断丢弃均为零。逐帧 `capture + total` P50/P95/P99 分别为 4.318/4.603/4.716 ms 和
8.065/8.440/8.597 ms；互操作虽把 307,200 B/帧 host upload 降为零，但 D3D11→CUDA 隐式同步
P95 为 3.789 ms，完整链路 P95 反而增加 3.838 ms。因此该路径只保留为实验/诊断开关，继续默认关闭。

D3D11/DirectML 互操作可追加 `-EnableD3D11DirectMlInterop on`。脚本会强制要求
Desktop Duplication 和 DirectML，逐样本核对 `d3d11_directml_interop=true`、
`input_upload_bytes=0`、`input_device_copy_bytes=0`。D3D11 copy 后的共享 fence 值是
D3D12 队列读取纹理的显式边界；DML 输出回传仍由 ORT 处理并归入
`execution_ms`。短冒烟已验证严格 DML Provider、零失败和零显式输入复制；
干净提交 `d3e055c` 的同机各 5 分钟 A/B 两侧均为 78,002/78,002 成功，零失败、
丢弃、覆盖、Mouse 命令和 duplication 恢复。逐帧 `capture + total` P50/P95/P99
从 5.133/5.458/5.582 ms 降至 5.012/5.346/5.474 ms，P95 改善 0.112 ms（约 2.04%）；
但互操作 max 由 6.898 ms 增至 8.511 ms，且当前只覆盖单机、单模型、固定 shape。
因此已验证的固定 shape DirectML 环境可显式启用，兼容默认值仍保持关闭。

UDP、XUDP 和 NDI 接收正式验收使用统一包装脚本。以下命令在辅机启动 XUDP 接收端；显式
`ReadyFilePath` 便于外部编排在 Capture 已绑定、Runtime 为 `RUNNING` 且实际 Provider 校验
通过后再启动主机 Sender。ready JSON 只声明接收端可收帧，其中几何名为
`expected_geometry`，不能替代首帧和逐帧几何验证；进程退出后该文件必须消失。

```powershell
.\scripts\benchmark_network_receiver.ps1 `
  -ModelPath "C:\path\to\model.onnx" `
  -CaptureBackend xudp_jpeg `
  -Backend directml `
  -BuildDirectory ".\build-dml" `
  -ListenUrl udp://0.0.0.0:5000 `
  -ReadyFilePath ".\cache\runtime-benchmark\xudp-aux.ready.json" `
  -ReportPrefix ".\cache\runtime-benchmark\xudp-aux"
```

未显式提供 `ReadyFilePath` 时，接收脚本会生成本轮随机路径。自动化必须使用唯一的新路径并轮询
JSON 内容，禁止靠固定延时或重定向日志中的“已监听”文本判断就绪。裸 UDP、XUDP、NDI 三种
后端均由同一脚本强制核对实际 Capture 后端、主机 FOV/ROI、Provider、失败状态、传输统计和
Runtime 覆盖；`<prefix>.network.json` 最后发布才表示网络接收报告完整。

2026-08-01 在 RTX 5070 Ti、本机 DXGI、`2560x1440` 中心 `320x320` ROI 上完成 5 分钟正式
基准；三组均为零失败、零报告/Runtime 丢弃且没有物理 Mouse 命令。`total` 从 Capture 发布完成
计到 control 结束，`capture + total` 是按同一帧逐样本相加后的完整采集到控制链路：

| Provider | 正式样本 | total P95 | total P99 | capture + total P95 |
|---|---:|---:|---:|---:|
| TensorRT | 78,007 | 0.574 ms | 0.631 ms | 4.612 ms |
| CUDA | 77,975 | 4.691 ms | 5.585 ms | 8.548 ms |
| DirectML | 78,001 | 1.148 ms | 1.195 ms | 5.164 ms |

结果仅适用于当前提交、模型和本机硬件。UDP/XUDP/NDI 辅机链路仍需分别完成同口径双机验收。

日志设施也由同一个 `config.ini` 静态加载。旧配置没有 `[log]` 节时使用默认值；日志等级支持
`trace`、`debug`、`info`、`warn`、`error` 和 `off`，未知等级会拒绝加载并在界面提示错误。
例如：

```ini
[log]
global_level=trace
enable_console=true
enable_file=true
enable_debug_file=false
enable_ringbuf=true
ringbuf_capacity=1024
log_dir=logs
file_max_size_mb=10
file_max_count=3

[log_modules]
detector=info
capture=warn
```

日志配置在进程启动时生效，运行中不会热更新；修改后保存并重新启动 Xen。
`[log_modules]` 可覆盖单个模块注册时的默认等级；最终阈值取模块等级与 `global_level` 中更严格者。
运行期间按 `F9` 可打开或关闭最近日志窗口；窗口只读取内存 ring buffer，不会读取日志文件或阻塞 Runtime 管线。

业务模块的 `#include "log/log.h"` 只依赖 C++20 标准库，格式串使用 `std::format` 语法并在
编译期校验；spdlog 仅作为 `log.cpp` 的私有 sink/队列后端。普通 `{}`、宽度、进制和浮点
精度格式与现有调用保持一致，不支持把 fmt 专属自定义 formatter 当作 Xen 公有接口。

应用还会在 Log 初始化后安装进程级崩溃处理器。正常受控诊断继续使用完整的 spdlog ring；
未处理 SEH 异常或 `std::terminate()` 只读取独立的 128 槽固定紧急尾部，并通过 Win32 API
同步追加到 `<log_dir>/crash_tail.log`。该异常路径不申请堆、不访问 `std::filesystem`、
spdlog ring 锁或 Log 生命周期锁；每条紧急记录最多 512 字节，超长消息按字节截断。

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

默认 `BUILD_TESTING=ON` 当前注册 15 个 CTest，按风险分为五层：

- 纯算法与内部契约：Detector 解码/前处理、Aim、真值解析、Runtime 队列、Overlay 标量环；
- 生产库回归：Log、Config、Debug、Benchmark 和 Mouse Benchmark 直接链接生产目标；
- OS/网络可复现替身：Crash 子进程、UDP/XUDP/NDI 回环、假 KMBOX 与 Keyboard 状态机；
- 可选真实模型集成：设置 `XEN_TEST_MODEL` 后增加 Detector 加载/变化输入和 Runtime 热重载；
- CUDA 真实模型构建还增加 D3D11/CUDA 纹理预注册、连续变化输入、Graph 输出指纹与 CPU 对照，
  因而当前 GPU 配置共注册 18 个 CTest；
- DirectML 真实模型构建还增加 D3D11/DirectML 共享纹理与 fence 测试，用饱和
  红/蓝变化输入核对通道、原始输出指纹、检测结果和零显式输入复制；
- 正式性能与部署验收：使用 `scripts/benchmark_*.ps1`，不把短单元测试耗时写成性能结论。

测试不得复制一套生产协议或算法，不得因环境缺失静默回退后端，也不得通过删除、跳过或放宽
断言处理失败。默认 CTest 必须能在无人值守环境重复运行；需要真实模型、GPU Provider、显示器、
双机或物理设备的场景由显式脚本承担，并在 clean `PATH` 下核对运行库和输入快照。详细规则见
本地文档 `docs/031_单元测试架构_20260801.md`。

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

固定 shape 的 TensorRT 模型默认启用 CUDA Graph。Detector 会复用 CUDA 输入输出、pinned
主机 staging 和 `uint8` 设备 staging；每帧显式上传模型尺寸 BGR 数据，由 CUDA kernel 直接写入
固定 float 输入，再执行 Graph 和 D2H，避免把首次输入错误地重复重放。320×320 输入由此把
每帧上传量从 1,228,800 字节降为 307,200 字节。动态 shape 或诊断图捕获问题时设置
`enable_trt_cuda_graph = false`；需要 A/B 时设置 `enable_gpu_preprocess = false`。

Desktop Duplication 的可选 CUDA 互操作还会复用三张固定 BGRA8 D3D11 ROI 纹理和一块固定
CUDA BGRA staging。D3D copy/Flush 与 CUDA map/copy/unmap 使用同一个短提交锁，防止两个业务
线程把隐式同步调用交错成驱动死锁；高成本 registration 只在 Runtime 启动期执行，停止时先注销
CUDA 资源再关闭 D3D11 设备。该路径当前以 INI/Benchmark 显式开关启用，默认路径不受影响。

DirectML 互操作复用三张同样的 BGRA8 ROI 纹理，但纹理以
`D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE` 创建。Capture 在共享
提交锁内执行 copy、D3D11 fence `Signal` 与 `Flush`；Detector 的 D3D12 队列等待该
fence，再把 BGRA8 转成固定 DML float CHW UAV。输入 OrtValue 通过 ORT DML API
包装该 D3D12 资源并使用 I/O Binding，不经过 CPU `cv::Mat` 或中间设备复制。

GPU 前处理 A/B 使用已构建的真实模型测试程序，在 clean `PATH` 下核对变化输入、Graph on/off、
失败帧、检测连续性和上传字节数：

```powershell
.\scripts\benchmark_gpu_preprocess.ps1 `
  -BuildDirectory ".\build" `
  -ModelPath "C:\path\to\model.onnx" `
  -TensorRtCachePath ".\cache\tensorrt\gpu-preprocess" `
  -VideoDirectory "C:\path\to\videos"
```

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

需要区分“目标离开中心 FOV”和“目标在 FOV 内但模型漏检”时，先从一次成功的
`center` 基准生成严格绑定视频 SHA-256、帧数和 ROI 的标注模板：

```powershell
.\scripts\new_video_visibility_annotations.ps1 `
  -VideoDirectory "C:\path\to\videos" `
  -BenchmarkReport ".\cache\benchmarks\detector-videos.csv" `
  -OutputDirectory ".\cache\visibility"
```

模板默认把全部帧设为 `ignore`，只是待人工复核的骨架，不是真值。每个文件使用
`<完整视频文件名>.visibility.json` 命名，`intervals` 必须从第 0 帧到末帧连续、无重叠、
无缺口，状态只接受 `visible`、`not_visible` 和 `ignore`。策略
`target_frame_visibility_v1` 规定：目标身体中心位于中心 ROI 内、可见身体高度不少于
32 个源像素且遮挡不超过约 50% 时标为 `visible`；中心在 ROI 外时标为
`not_visible`；边界、严重运动模糊、遮挡或无法稳定判断的帧标为 `ignore`。
人工标注完成后追加参数运行正式基准：

```powershell
.\scripts\benchmark_detector_videos.ps1 `
  -ModelPath "C:\path\to\model.onnx" `
  -VideoDirectory "C:\path\to\videos" `
  -VisibilityDirectory ".\cache\visibility" `
  -InputMode center
```

脚本会在 CSV 旁生成同名前缀的 JSON 环境清单，记录 Git 状态、模型 SHA-256、
视频逐文件 SHA-256、可执行文件与部署 DLL 哈希、GPU/驱动、SDK 精确版本，以及
FP16、CUDA Graph、线程和缓存配置。启用标注时还会登记每份标注的 SHA-256，并在
运行前后复核；视频、标注或二进制变化都会使验收失败。CSV 和 JSON 均先写同目录
临时文件，完成校验和 flush/close 后原子发布。性能报告和环境清单默认位于
`cache/benchmarks/`，均不提交 Git。

项目不迁入旧仓库的 `Xen/benchmarks/provider_benchmark.cpp`。旧实现会重复 Provider、
前处理、解码和统计逻辑，并通过编译期开关维护两套不同管线。当前基准始终复用生产
`Detector`，避免性能测试与真实运行的数据流、复制链和输出语义漂移。

CSV 中每段视频先重复第一帧预热 50 次，
正式统计覆盖全部视频帧，输出各阶段 mean/P50/P95/P99。视频没有人工标注时，
`detection_frame_rate` 只表示“存在检测结果的帧比例”，不能直接解释为 Recall；
目标在全屏范围移动而采集范围固定为中心 ROI 时，ROI 外的空检测是正确结果。
该指标同时受到目标进入 ROI 的时间占比与 ROI 内检测成功率影响；只有补充目标可见性
标注后，才能计算 `visible_frame_recall`、可见漏检帧数及最长连续可见漏检。该指标是
单目标素材的帧级 Recall：可见帧只要存在任一检测就算命中，不具备目标框匹配和类别
混淆信息，不能替代对象级 Precision/Recall 或 mAP。`not_visible` 帧出现检测只记录为
`not_visible_detected_frames` 诊断项，不能直接称为 false positive。无标注模式明确输出
`visibility_annotations=0`、`recall_available=0`，Recall 与可评价率字段留空。

Aim 的真实追踪评价不能复用 Detector 的“任一检测即命中”标注。可从同一份成功的中心 ROI
视频基准生成逐帧对象真值骨架：

```powershell
.\scripts\new_aim_ground_truth_annotations.ps1 `
  -VideoDirectory "C:\path\to\videos" `
  -BenchmarkReport ".\cache\benchmarks\detector-videos.csv" `
  -OutputDirectory ".\cache\aim-ground-truth"
```

人工标注完成后，通过同一条 TensorRT 视频基准链路执行 Aim：

```powershell
.\scripts\benchmark_detector_videos.ps1 `
  -ModelPath "C:\path\to\model.onnx" `
  -VideoDirectory "C:\path\to\videos" `
  -AimAnnotationDirectory ".\cache\aim-ground-truth" `
  -InputMode center
```

尚未完成人工框/身份标注时，可先建立不含 Recall 或身份结论的控制命令连续性基线：

```powershell
.\scripts\benchmark_detector_videos.ps1 `
  -ModelPath "C:\path\to\model.onnx" `
  -VideoDirectory "C:\path\to\videos" `
  -AimContinuity `
  -InputMode center
```

模板文件名为 `<完整视频文件名>.aim.json`，默认每帧都是 `ignore` 且没有目标框，不是真值。
人工标注时，`visible` 帧必须填写位于主机完整 FOV 坐标中的目标框和视频内稳定 `track_id`；
`not_visible/ignore` 帧不得携带目标。schema 会严格绑定视频 SHA-256、帧数、输入模式和主机
ROI。Aim 离线评价器可统计 ROI Recall、ID Switch、连续轨迹片段、碎片事件和旧目标仍可见时
发生的无必要切换，并拒绝错误帧号和错误几何。独立的控制连续性评价不需要人工真值，统计
命令/无命令/无目标/预测帧、连续段、方向反转、限幅边界，以及命令绝对值、向量幅值、一阶
变化和二阶变化的 mean/P50/P95/P99/max；无命令、目标切换、轨迹状态或预测标志变化都会切断
连续段，不能被误计为控制抖动。浮点残余量化为零时允许无命令快照保留成对的 sequence 和
captured_at 元数据，但仍要求 counts 为零；半残留元数据会被拒绝。主机 `2560x1440` 的默认 ROI 仍是
`(1120,560,320,320)`；辅机 `1920x1080` 不属于标注或评价坐标。正式视频基准通过
`-AimAnnotationDirectory` 同时执行生产 Detector 和 Aim，CSV 记录 ROI Recall、ID Switch、
轨迹碎片、无必要切换、控制连续性、逐帧状态守恒、完整 Aim 配置、评价阈值和按视频 FPS
合成的单调时间基准，schema 4 JSON 绑定标注与二进制快照。整套标注至少要有一个
`visible` 真值帧，全 `ignore` 模板会在 CSV 原子发布前被拒绝。当前已完成报告接入和合成回归；
真实素材的人工框/身份标注仍待执行，不能把合成结果写成真实基线。

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

三类真实 ONNX 可通过独立 CPU/Release 兼容矩阵重复验证。脚本会先运行全量 CTest，再在
clean `PATH` 下分别显式请求三种输出契约，核对真实 Provider、模型与二进制 SHA-256、
变化输入输出指纹，并把 JSON 原子发布到 `cache/compatibility/`：

```powershell
.\scripts\test_detector_compatibility.ps1 `
  -ModernRawModelPath "C:\path\to\modern-raw.onnx" `
  -YoloV5ObjectnessModelPath "C:\path\to\yolov5-objectness.onnx" `
  -EndToEndModelPath "C:\path\to\end-to-end-nms.onnx" `
  -ComparisonImagePath "C:\path\to\real-scene.jpg" `
  -OnnxRuntimeRoot "C:\path\to\onnxruntime" `
  -OpenCvDir "C:\path\to\opencv\build\x64\vc16\lib"
```

端到端模型的图内 NMS 可能把黑图和白图都归并为同一份空输出，因此该契约必须提供一张
能形成真实检测结果的对照图，不能用两张都无目标的合成图证明输入变化传播。模型和对照图
只作为本地验收输入，不复制进构建目录，也不纳入 Git。

当前仅支持单输入、单输出、NCHW、float32 的 detect 模型。分割、姿态、OBB、多输出 head 和真正的 batch inference 不在当前范围内。

### 运行时模型热重载

检测页显示当前活动模型、Provider、重载状态和模型代次。`Runtime::reload_detector()` 只在
`RUNNING` 接受请求，且同一时刻只允许一个候选加载。加载期间旧 Detector 继续服务；成功切换
不复制当前帧或张量，只交换 Detector 所有权，旧 Session 在加载线程释放。加载失败写入独立的
`detector_reload_error`，不会把 Runtime 置为 `FAILED`。

D3D11/CUDA 或 D3D11/DirectML 互操作启用时，三个纹理与 Detector 设备资源均与
Capture 设备同寿命；为避免旧 Session 销毁资源时 Capture 仍向空闲槽提交 copy，
这两种模式均拒绝运行期热重载。需要换模型时应先停止
Runtime，保存新配置后重新启动。

停止期间若 TensorRT 正在首次构建 Engine，Runtime 会先停止 Capture/Pipeline，再等待不可取消的
ORT/TensorRT Session 创建自然结束并丢弃候选。该等待可能持续十几秒，但不会 detach 后台线程或在
Runtime 析构后访问资源。主机/辅机分辨率与热重载相互独立：主机 `2560x1440`、辅机
`1920x1080` 时仍使用主机中心 ROI `(1120,560,320,320)`，不做辅机分辨率缩放。

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
