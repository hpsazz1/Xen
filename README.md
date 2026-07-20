# Xen

Xen 是一个基于 C++17 的 Windows 实时视觉辅助工具，提供目标检测、目标跟踪、鼠标控制、桌面设置界面和可选的局域网 Web 控制台。项目支持 DirectML 与 CUDA + TensorRT 两条编译后端，适用于需要在不同 GPU 环境中部署的场景。

> 请确认使用场景、软件和输入设备符合所在平台的服务条款及当地法律。默认配置不会启用自动射击或局域网控制台。

![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?style=for-the-badge&logo=cplusplus&logoColor=white)
![DirectML](https://img.shields.io/badge/DirectML-Windows-0078D4?style=for-the-badge&logo=windows&logoColor=white)
![CUDA 13.2](https://img.shields.io/badge/CUDA-13.2-76B900?style=for-the-badge&logo=nvidia&logoColor=white)
![License: MIT](https://img.shields.io/badge/License-MIT-yellow?style=for-the-badge)

相关入口：[构建指南](docs/build.md) · [配置说明](docs/config.md) · [使用指南](docs/guides.md) · [开发规范](docs/002项目开发铁律20260713.md)

## 当前能力

- **双推理后端**：DirectML 使用 ONNX Runtime，CUDA 使用 TensorRT engine；后端在构建时选择。
- **实时检测与跟踪**：检测缓冲、目标关联、头部/身体枢轴、连续观测预测、速度与轨迹控制。
- **多种画面采集**：DXGI Desktop Duplication、WinRT、虚拟摄像头、UDP MJPEG、NDI。
- **多种鼠标输出**：Win32、G HUB、Razer、Kmbox Net、Kmbox A、Makcu。
- **桌面设置界面**：ImGui 设置页可调整模型、采集、目标、预测、输入、覆盖层、深度和数据采集参数。
- **模型类别选择**：可在 UI 或 Web 控制台选择目标身体/头部类别。DML 会读取 ONNX 的 `names` 元数据；没有元数据时显示类别 ID。
- **游戏覆盖层与深度**：可选检测框、FOV、轨迹、图标、延迟补偿和 Depth Anything v2 深度遮罩（CUDA 路径）。
- **数据采集**：运行时截图和 YOLO 格式自动标注，可按类别、置信度和帧间隔筛选。
- **局域网 Web 控制台**：手机或同一局域网浏览器可查看状态、暂停/恢复、重载配置和修改白名单参数。

## 选择后端

| 后端 | 模型格式 | 适用场景 | 主要要求 |
| --- | --- | --- | --- |
| DirectML（DML） | `.onnx` | NVIDIA、AMD、Intel、集成显卡和笔记本 | Windows 10/11 x64、支持 DirectML 的驱动 |
| CUDA + TensorRT | `.engine` | 支持的 NVIDIA 显卡，追求更高推理性能 | NVIDIA 驱动、CUDA 13.1+；源码构建需 TensorRT 10 |

CUDA 构建默认使用 CUDA 13.2 + TensorRT 10.16 依赖解析。Pascal/GTX 10xx 等旧显卡如果无法运行当前 TensorRT 路径，可改用 DML 后端。后端依赖和版本以 [构建指南](docs/build.md) 为准。

## 快速开始

1. 将 `.onnx` 模型放入 `Xen.exe` 同级的 `models` 文件夹。DML 直接加载 ONNX；CUDA 可在首次选择 ONNX 时自动生成并切换到匹配的 `.engine`。
2. 运行与后端匹配的 `Xen.exe`。标准产物位于 `build\dml\Release` 或 `build\cuda\Release`。
3. 首次启动时等待模型加载或 TensorRT engine 导出完成，耗时取决于模型、磁盘和 GPU。
4. 按 `Home` 打开设置界面，在“AI/目标”页面选择模型和目标类别，在“采集/输入”页面选择来源与鼠标输出。
5. 确认画面、目标类别和输入设备均正常后，再启用需要的控制功能。

### 模型类别

目标类别通过 `class_player`（身体）和 `class_head`（头部）保存。常见四类别模型可以使用：

| 类别 ID | 语义 |
| ---: | --- |
| 0 | 警身 |
| 1 | 警头 |
| 2 | 匪身 |
| 3 | 匪头 |

DML 会优先读取 ONNX 元数据中的类别名称，并在 UI/局域网控制台展示；名称缺失时不会猜测，仍以 ID 为准。CUDA engine 当前没有可用类别名称时同样回退为 ID。若模型类别顺序不同，请在设置页选择“自定义类别 ID”。

## 快捷键

| 按键 | 功能 |
| --- | --- |
| 鼠标右键 | 目标检测/瞄准 |
| `F2` | 退出程序 |
| `F3` | 暂停或恢复瞄准 |
| `F4` | 重新加载 `config.ini` |
| `Home` | 打开或关闭 ImGui 设置界面 |
| 方向键 | 运行时调整目标偏移（需启用 `enable_arrows_settings`） |

## 局域网 Web 控制台

控制台默认关闭。需要在 `config.ini` 中显式启用：

```ini
lan_console_enabled = true
lan_console_bind_address = 0.0.0.0
lan_console_port = 17888
```

启动后，程序控制台会打印访问地址和六位配对码。手机连接同一局域网后访问 `http://主机局域网IP:17888`，输入配对码即可。控制台支持：

- 查看运行/暂停状态、采集和检测 FPS、后端、模型及输入方式；
- 远程暂停/恢复瞄准；
- 远程重载配置；
- 修改白名单参数，包括采集帧率、置信度、目标偏移、预测开关、自动射击开关和输入方式；
- 选择警方、匪方或自定义目标类别，并显示模型类别名称。

配对码和会话令牌只在当前进程有效。不要把端口暴露到公网；建议仅在可信局域网使用，必要时将监听地址改为 `127.0.0.1` 并通过本机端口转发访问。完整接口和安全说明见 [局域网 Web 控制台](docs/198局域网Web控制台20260720.md)。

## 输入设备

通过 `config.ini` 的 `input_method` 选择输出方式：

| 值 | 说明 |
| --- | --- |
| `WIN32` | Windows 标准鼠标事件 |
| `GHUB` | Logitech G HUB DLL |
| `RAZER` | Razer `rzctl.dll` |
| `KMBOX_NET` | Kmbox Net UDP |
| `KMBOX_A` | Kmbox A HID |
| `MAKCU` | Makcu 串口桥接 |

硬件设备还需要填写对应的 IP、端口、UUID、PID/VID 或串口参数。详细字段和故障排查见 [输入方式指南](docs/guides/input-methods.md)。

## 从源码构建

推荐使用项目包装脚本，它会配置 Visual Studio、CMake/Ninja、后端依赖并生成 Release 产物：

```powershell
.\BUILDER.bat
```

也可以直接选择后端：

```powershell
.\build_dml.bat
.\build_cuda.bat
```

完成首次配置后，使用增量入口重新构建：

```powershell
.\build_no-options.bat -Backend DML
.\build_no-options.bat -Backend CUDA
```

构建、依赖布局、测试和发布身份核对请按 [docs/build.md](docs/build.md) 执行。不要使用历史 `x64\DML` 或 `x64\CUDA` 目录中的旧可执行文件进行验证。

## 配置与文档

常用配置入口：

- [完整配置说明](docs/config.md)：默认值、范围、后端差异和运行时行为。
- [使用指南总览](docs/guides.md)：首次启动、采集、覆盖层、输入设备和排错入口。
- [后端说明](docs/guides/backends.md)
- [NDI 采集](docs/guides/ndi-capture.md)、[UDP 采集](docs/guides/udp-capture.md)、[采集诊断](docs/guides/capture-diagnostics.md)
- [数据采集](docs/guides/data-collection.md)、[覆盖层](docs/guides/overlay.md)
- [构建工作流](docs/guides/build-workflow.md)、[首次启动](docs/guides/first-launch.md)
- [常见问题与排错](docs/guides/troubleshooting.md)
- [近期功能记录](docs/198局域网Web控制台20260720.md)、[界面导航重构](docs/197界面导航重构20260720.md)

## 项目结构

```text
Xen/
├── capture/       画面采集与帧分发
├── detector/      DML/CUDA 推理、YOLO 后处理
├── depth/         深度估计与遮罩
├── debug/         流水线诊断与运行数据记录
├── mouse/         目标跟踪、控制器和输入设备
├── overlay/       ImGui 设置界面与游戏覆盖层
├── runtime/       鼠标、覆盖层、Web 控制台等运行时线程
├── config/        INI 配置读写和默认值
├── modules/       OpenCV、ImGui、TensorRT 等第三方依赖
└── third_party/   运动控制等内部共享库
```

根目录中的 `BUILDER.bat`、`build_dml.bat`、`build_cuda.bat` 和 `build_no-options.bat` 是标准构建入口；`docs/` 保存配置、构建、排错和阶段记录。

## 许可证

本项目以 [MIT License](LICENSE) 发布。第三方依赖遵循各自许可证。
