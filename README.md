<div align="center">

# Xen

基于 C++ 原生实现的 Windows 视觉辅助工具，支持 DirectML 和 CUDA + TensorRT 双运行后端。

[![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?style=for-the-badge&logo=cplusplus&logoColor=white)](https://github.com/hpsazz1/Xen)
[![CUDA 13.2](https://img.shields.io/badge/CUDA-13.2-76B900?style=for-the-badge&logo=nvidia&logoColor=white)](https://developer.nvidia.com/cuda-downloads)
[![TensorRT 10](https://img.shields.io/badge/TensorRT-10.16-76B900?style=for-the-badge&logo=nvidia&logoColor=white)](https://docs.nvidia.com/deeplearning/tensorrt/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow?style=for-the-badge)](LICENSE)

<p>
  <a href="docs/build.md">构建指南</a>
  &nbsp;|&nbsp;
  <a href="docs/config.md">配置说明</a>
  &nbsp;|&nbsp;
  <a href="docs/guides.md">使用指南</a>
</p>

</div>

---

## 概述

Xen 是一个实时视觉辅助系统，集成了 AI 目标检测、多目标追踪、卡尔曼滤波预测和仿人鼠标控制。核心特性：

- **双后端推理** — DirectML（通用 GPU）和 CUDA + TensorRT（NVIDIA 加速），编译时分离
- **多目标追踪** — 匈牙利匹配 + 7 状态卡尔曼滤波（MOT），单目标状态机（SOT：锁定/惯性/丢失）
- **仿人移动** — Pure Pursuit 执行控制器 + WindMouse/贝塞尔轨迹模拟 + 三区间速度曲线
- **射击人性化** — 对数正态延迟/保持分布、稳定帧确认、过冲模拟
- **6 种画面采集** — DXGI Desktop Duplication、WinRT、虚拟摄像头、UDP 网络流、NDI 专业视频源
- **7 种输入设备** — Win32、G HUB、Razer、Kmbox Net、Kmbox A、Makcu
- **游戏内覆盖层** — DirectX 延迟补偿检测框、深度可视化、未来轨迹、图标叠加
- **深度感知** — Depth Anything v2 深度估计，支持检测抑制遮罩
- **数据采集** — 运行时自动截图与 YOLO 格式自动标注

| 运行时 | 适用场景 | 配置说明 |
| --- | --- | --- |
| **DirectML（DML）** | NVIDIA、AMD、Intel、集成显卡、笔记本和旧系统 | 通用 Windows 10/11 x64 构建，无需 CUDA |
| **CUDA + TensorRT** | 追求最佳性能的 NVIDIA 显卡 | 需要最新 NVIDIA 驱动和 CUDA 13.2 |

## 运行时支持

### DirectML

在兼容性优先于 NVIDIA 专属加速时，选择 DML 构建。

| 要求 | 说明 |
| --- | --- |
| 系统 | Windows 10/11 x64 |
| 显卡 | 主流 NVIDIA、AMD Radeon、Intel Iris/Xe 或集成显卡 |
| 额外运行时 | 无 |
| 推荐用于 | GTX 10xx/9xx/7xx、AMD 显卡、Intel 显卡、笔记本和办公电脑 |

### CUDA + TensorRT

选择 CUDA 构建以获得最快的 NVIDIA 加速路径。

| 要求 | 说明 |
| --- | --- |
| 系统 | Windows 10/11 x64 |
| 显卡 | GTX 1660 或 RTX 2000/3000/4000/5000 |
| 驱动 | 最新 NVIDIA 显卡驱动 |
| CUDA | CUDA 13.2（最低 13.1） |
| 不支持 | GTX 10xx/Pascal 及更旧（因 TensorRT 限制） |
| 源码构建额外 | TensorRT 10.16.1.11（仅源码构建时需要单独准备） |

预编译的 CUDA 版本已包含所需 CUDA + TensorRT 运行时文件。在排查 CUDA DLL 缺失或启动闪退问题前，请先更新 NVIDIA 驱动并安装 [CUDA 13.2](https://developer.nvidia.com/cuda-downloads)。

## 快速开始

1. 将 `.onnx` 模型放入 `models` 文件夹。
2. 运行 `Xen.exe`。
3. 等待首次模型导出。首次启动最长需 5 分钟。
4. 按 `Home` 键，在界面上选择模型并调整设置。

## 快捷键

| 按键 | 功能 |
| --- | --- |
| 鼠标右键 | 瞄准检测到的目标 |
| F2 | 退出程序 |
| F3 | 暂停瞄准 |
| F4 | 重新加载配置 |
| Home | 打开或关闭设置界面 |
| ↑ / ↓（Shift+↑/↓） | 运行时调整目标偏移（需启用 `enable_arrows_settings`） |

## 输入设备

支持 7 种鼠标输出方式，通过 `config.ini` 的 `input_method` 切换：

| 方式 | 说明 | 接口 |
| --- | --- | --- |
| `WIN32` | 标准 Windows 鼠标事件 | 系统 API |
| `GHUB` | 罗技 G HUB 驱动注入 | `ghub_mouse.dll` |
| `RAZER` | 雷蛇 rzctl 驱动注入 | `rzctl.dll` |
| `KMBOX_NET` | Kmbox Net UDP 协议 | 网络 |
| `KMBOX_A` | Kmbox A HID 协议 | HID |
| `MAKCU` | Makcu 串口桥接（CH343） | UART |

## 从源码构建

| 资源 | 链接 |
| --- | --- |
| 构建流程 | [docs/build.md](docs/build.md) |
| CUDA Toolkit | [developer.nvidia.com/cuda-downloads](https://developer.nvidia.com/cuda-downloads) |
| TensorRT 文档 | [docs.nvidia.com/deeplearning/tensorrt](https://docs.nvidia.com/deeplearning/tensorrt/) |

## 文档

| 主题 | 链接 |
| --- | --- |
| 配置说明 | [docs/config.md](docs/config.md) |
| 安装、常见问题与排错 | [docs/guides.md](docs/guides.md) |
| 后端说明 | [docs/guides/backends.md](docs/guides/backends.md) |
| 输入方式 | [docs/guides/input-methods.md](docs/guides/input-methods.md) |
| NDI 采集 | [docs/guides/ndi-capture.md](docs/guides/ndi-capture.md) |
| UDP 局域网采集 | [docs/guides/udp-capture.md](docs/guides/udp-capture.md) |
| 数据收集 | [docs/guides/data-collection.md](docs/guides/data-collection.md) |
| 游戏覆盖层 | [docs/guides/overlay.md](docs/guides/overlay.md) |
| 采集诊断 | [docs/guides/capture-diagnostics.md](docs/guides/capture-diagnostics.md) |
| 构建工作流 | [docs/guides/build-workflow.md](docs/guides/build-workflow.md) |
| 首次启动 | [docs/guides/first-launch.md](docs/guides/first-launch.md) |
| 配方示例 | [docs/guides/recipes.md](docs/guides/recipes.md) |
| 排错 | [docs/guides/troubleshooting.md](docs/guides/troubleshooting.md) |
| 源码配置文件 | [Xen/config/config.cpp](Xen/config/config.cpp)、[Xen/config/config.h](Xen/config/config.h) |

## 项目结构

### 文件树

```
├── Xen.sln                         # VS 解决方案
├── CMakeLists.txt                  # CMake 构建配置
├── BUILDER.bat / BUILDER.ps1       # 一键构建脚本（交互式选择 DML/CUDA）
├── build_dml.bat / build_cuda.bat  # DML / CUDA 后端构建入口
├── build_no-options.bat / .ps1     # 快速增量编译（跳过依赖检查）
├── LICENSE                         # MIT 许可证
├── docs\                           # 文档
├── tools\                          # 构建工具脚本
├── packages\                       # NuGet 包（构建时自动还原）
│
├── Xen\                            # 源代码
│   ├── Xen.cpp / Xen.h             # 主入口 ── 全局变量、线程启动、设备初始化
│   │
│   ├── capture\                    # 画面采集 ── 6 种采集源
│   │   ├── capture.*               #   ├─ 采集调度：后端选择、帧率控制、帧分发
│   │   ├── duplication_api_*       #   ├─ DXGI Desktop Duplication API
│   │   ├── winrt_capture.*         #   ├─ WinRT 窗口/显示器采集
│   │   ├── virtual_camera.*        #   ├─ 虚拟摄像头输入
│   │   ├── udp_capture.*           #   ├─ UDP 网络流采集（MJPEG）
│   │   ├── ndi_capture.*           #   ├─ NDI 网络视频源采集
│   │   └── circle_fov.h            #   └─ 圆形 FOV 裁剪
│   │
│   ├── detector\                   # AI 目标检测 ── 推理 + 后处理
│   │   ├── detection_buffer.*      #   ├─ 跨线程检测帧缓冲
│   │   ├── dml_detector.*          #   ├─ DirectML 推理（ONNX Runtime）
│   │   ├── trt_detector.*          #   ├─ TensorRT 推理（CUDA）
│   │   ├── postProcess.*           #   ├─ YOLO 解码、NMS、阈值过滤
│   │   └── cuda_preprocess.*       #   └─ CUDA 核函数预处理
│   │
│   ├── depth\                      # 深度估计 ── Depth Anything v2（仅 CUDA）
│   │   ├── depth_anything_trt.*    #   ├─ TensorRT 引擎加载与推理
│   │   ├── depth_mask.*            #   ├─ 深度遮罩生成（直方图阈值 + 膨胀）
│   │   └── depth_utils.*           #   └─ 深度图工具函数
│   │
│   ├── tensorrt\                   # TensorRT 封装 ── 引擎构建、序列化、日志
│   │   ├── nvinf.*                 #   ├─ Inference 封装
│   │   └── trt_monitor.h           #   └─ 导出进度监控 UI
│   │
│   ├── mouse\                      # 鼠标控制 ── 算法 + 10 种设备驱动
│   │   ├── mouse.*                 #   ├─ 核心：预测、速度曲线、轨迹模拟、
│   │   │                            #      Pure Pursuit 控制器、射击人性化
│   │   ├── MouseInput.*            #   ├─ 设备抽象层（IMouseInput 工厂，10 种实现）
│   │   ├── AimbotTarget.*          #   ├─ 多目标追踪（MOT 匈牙利 + SOT 状态机）
│   │   ├── ghub.* / rzctl.*        #   ├─ 罗技 G HUB / 雷蛇 rzctl 驱动注入
│   │   ├── kmbox_net\              #   ├─ Kmbox Net（UDP 协议）
│   │   ├── kmboxA.* / KmboxA*      #   ├─ Kmbox A（HID 协议）
│   │   ├── Makcu.*                 #   └─ Makcu 串口桥接（CH343）
│   │
│   ├── keyboard\                   # 键盘监听 ── 快捷键、状态检测
│   │   ├── keyboard_listener.*     #   └─ 键盘线程：瞄准/射击/缩放/热键
│   │   └── keycodes.*              #      虚拟键码映射表
│   │
│   ├── overlay\                    # 设置界面 ── ImGui + D3D11 渲染
│   │   ├── overlay.*               #   ├─ 窗口创建、D3D11/DComp、主题、字体
│   │   ├── Game_overlay.*          #   ├─ 游戏叠加层配置管理
│   │   ├── export_progress_panel.h #   ├─ TensorRT 导出进度面板
│   │   ├── config_dirty.*          #   ├─ 配置变更追踪
│   │   ├── draw_settings.h          #   ├─ 绘制设置接口声明
│   │   ├── ui_sections.h           #   ├─ UI 辅助组件（行标签、动画过渡）
│   │   ├── draw_ai.*               #   ├─ AI 模型/检测参数设置
│   │   ├── draw_mouse.*            #   ├─ 鼠标设置（FOV/预测/轨迹/设备/射击）
│   │   ├── draw_capture.*          #   ├─ 捕获设置（来源/分辨率/FPS）
│   │   ├── draw_target.*           #   ├─ 目标偏移 + 追踪器设置
│   │   ├── draw_buttons.*          #   ├─ 热键绑定
│   │   ├── draw_depth.*            #   ├─ 深度估计设置
│   │   ├── draw_game_overlay.*     #   ├─ 游戏覆盖层样式设置
│   │   ├── draw_overlay.*          #   ├─ 叠加层外观设置
│   │   ├── draw_stats.*            #   ├─ 性能统计图表（推理耗时/采集帧率/诊断）
│   │   └── draw_debug.*            #   └─ 截图/数据收集/调试工具
│   │
│   ├── runtime\                    # 运行时线程 ── 主循环调度
│   │   ├── mouse_thread_loop.*     #   ├─ 鼠标线程：检测→追踪→预测→控制→移动→射击
│   │   ├── game_overlay_loop.*     #   ├─ 游戏覆盖层 DirectX 渲染线程
│   │   ├── thread_loops_shared.*   #   ├─ 线程间共享状态
│   │   ├── thread_loops.h          #   ├─ 线程循环声明
│   │   └── speed_curve.h           #   └─ 统一三区间速度曲线函数
│   │
│   ├── config\                     # 配置管理 ── INI 文件读写
│   │   ├── config.*               #   └─ 全部配置项默认值、读写逻辑、结构体定义
│   │
│   ├── scr\                        # 杂项工具
│   │   ├── other_tools.*           #   ├─ 系统信息、窗口枚举、模型管理、图像加载
│   │   └── data_collector.*        #   └─ 数据收集/自动标注
│   │
│   ├── mem\                        # 资源管理
│   │   ├── cpu_affinity_manager.*  #   ├─ CPU 核心预留 / 系统内存预留
│   │   └── gpu_resource_manager.*  #   └─ GPU 显存预留 + 独占模式（CUDA）
│   │
│   ├── benchmarks\                 # 性能基准测试
│   │   └── provider_benchmark.*    #   └─ DML/CUDA 后端推理速度对比
│   │
│   ├── third_party\                # 第三方库（自有）
│   │   └── motion_control\         #   └─ Header-only 运动控制库
│   │       ├── estimation.h         #     ├─ 7 状态卡尔曼、匈牙利匹配、SOT 状态机
│   │       ├── execution.h          #     ├─ Pure Pursuit 执行控制器
│   │       ├── filters.h            #     ├─ EMA/DEMA/滑动中值/运动突变检测
│   │       └── prediction.h         #     └─ 延迟补偿预测
│   │
│   ├── include\                    # 内部共享头文件
│   │   ├── other_tools.h           #   ├─ 工具函数、窗口枚举、模型管理
│   │   └── memory_images.h         #   └─ Base64 编码内存图像
│   │
│   │
│   └── modules\                    # 第三方模块
│       ├── opencv\build\           #   ├─ DML / CUDA 双 OpenCV 构建
│       ├── TensorRT-*\             #   ├─ TensorRT SDK
│       ├── makcu\                  #   ├─ Makcu 串口通信库（CH343）
│       ├── serial\                 #   ├─ 跨平台串口库
│       ├── imgui-1.92.8\           #   ├─ Dear ImGui 界面库
│       ├── stb\                    #   ├─ stb_image 图像加载
│       └── SimpleIni.h             #   └─ INI 文件解析库
│
└── .gitignore
```

## 模块关系图

```
                          ┌──────────────────────────┐
                          │          main()           │
                          │        Xen.cpp            │
                          │ 线程创建 / 设备初始化 / 主循环│
                          └──────┬──────────┬─────────┘
                                 │          │
                    ┌────────────┘          └────────────┐
                    ▼                                    ▼
        ┌─────────────────────┐              ┌─────────────────────┐
        │     capture\        │              │     keyboard\        │
        │   6 种画面采集源      │              │  键盘监听 + 状态检测   │
        └────────┬────────────┘              └────────┬────────────┘
                 │ cv::Mat                            │ aiming / shooting
                 ▼                                    ▼
        ┌─────────────────────┐              ┌─────────────────────┐
        │     detector\       │──DetectBuf──▶│     AimbotTarget\    │
        │   AI 目标检测        │              │    多目标追踪          │
        │  dml / trt / nms    │              │  MOT 匈牙利 + 7态卡尔曼 │
        └────────┬────────────┘              │  SOT 锁定→惯性→丢失    │
                 │                           └────────┬────────────┘
                 │ 深度推理 + 遮罩                      │ 目标坐标 + 速度
                 ▼                                    ▼
        ┌─────────────────────┐              ┌─────────────────────┐
        │     depth\          │              │      mouse\          │
        │  Depth Anything v2   │              │    鼠标控制核心        │
        │  深度估计 + 检测抑制   │              │                     │
        └────────┬────────────┘              │ ┌──────────────────┐ │
                 │                            │ │  motion_control\ │ │
                 ▼                            │ │  运动控制算法库    │ │
        ┌─────────────────────┐              │ │ Pure Pursuit 执行 │ │
        │     tensorrt\       │              │ │ Wind/Bezier 轨迹  │ │
        │  引擎构建/加载/推理   │              │ │ EMA/突变检测 过滤  │ │
        └─────────────────────┘              │ │ 延迟补偿预测       │ │
                                              │ └──────────────────┘ │
                                              │                     │
                                              │  ┌──────────────┐   │
                                              │  │ MouseInput\  │   │
                                              │  │ 10 种设备驱动  │   │
                                              │  └──────┬───────┘   │
                                              └─────────┼───────────┘
                                                        │ 物理鼠标移动
                                                        ▼
                                                   ┌───────────┐
                                                   │   游戏     │
                                                   └───────────┘

  ┌───────────────────────┐      读写       ┌───────────────────────┐
  │    overlay\            │◀──────────────▶│       config\           │
  │  设置界面 (ImGui)       │               │     config.ini          │
  └───────────────────────┘               └───────────┬───────────────┘
                                                    │ 配置广播
  ┌───────────────────────┐               ┌─────────┴───────────────┐
  │  game_overlay_loop\    │◀──配置/数据──▶│       runtime\           │
  │  游戏覆盖层 (DirectX)   │               │  mouse_thread_loop      │
  │  延迟补偿框 / 深度 /    │               │  game_overlay_loop     │
  │  未来轨迹 / 图标叠加     │               │  thread_loops_shared   │
  └───────────────────────┘               └─────────────────────────┘

  数据流: capture → detector → AimbotTarget → mouse → MouseInput → 游戏
  追踪流: detector → AimbotTarget(MOT/SOT) → mouse(速度注入 + Pure Pursuit)
  控制流: keyboard / overlay ↔ config ──广播──▶ 各模块
```

## 技术架构

### 目标追踪与预测

```
检测帧 ──▶ MOT 多目标追踪 ──▶ SOT 单目标锁定
              │                    │
              │  匈牙利匹配          │  状态机：锁定→惯性→丢失
              │  7状态卡尔曼          │
              │  [cls,cx,cy,w,h,    │  外部速度注入
              │   vx,vy]            │
              └────────┬─────────────┘
                       ▼
              mouse.cpp 预测管线
              ├─ SOT 卡尔曼速度（优先）
              ├─ 帧差速度估计（备选）
              ├─ 时间校正 EMA 平滑
              ├─ 恒速外推 + 自适应钳位
              └─ 自身运动补偿（环形缓冲区）
                       │
                       ▼
              Pure Pursuit 控制器
              ├─ 像素空间 2D 追踪
              ├─ 近距减速 + 死区
              ├─ IIR 输出平滑
              ├─ 速度前馈 + 运动突变保护
              └─ 子像素累积
                       │
                       ▼
              轨迹模拟（可选）
              ├─ WindMouse（黄金比例双正弦振荡）
              ├─ 贝塞尔曲线（Perlin 噪声扰动）
              └─ 直通（无轨迹模拟）
```

### 速度曲线（三区间）

```
速度
  ▲
  │  maxSpeed ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─
  │
  │              ╱  近区间（指数插值）
  │            ╱
  │  snap ─ ─╱  ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─
  │
  │         ╱│
  │       ╱  │  远区间（线性映射）
  │     ╱    │
  │───╱──────│──────────────────────────▶ 距离
  │  snapR   nearR
  │  吸附区   │
  │  min × boost
```

## 许可说明

本项目采用 [MIT License](LICENSE)。

| 依赖项 | 许可证 |
| --- | --- |
| OpenCV | [Apache License 2.0](https://opencv.org/license.html) |
| ImGui | [MIT License](https://github.com/ocornut/imgui/blob/master/LICENSE) |
| TensorRT | [NVIDIA Software License](https://docs.nvidia.com/deeplearning/tensorrt/sla/) |
| ONNX Runtime | [MIT License](https://github.com/microsoft/onnxruntime/blob/main/LICENSE) |

## 外部链接

- [TensorRT 文档](https://docs.nvidia.com/deeplearning/tensorrt/)
- [OpenCV 文档](https://docs.opencv.org/4.x/d1/dfb/intro.html)
- [ImGui](https://github.com/ocornut/imgui)
- [ONNX Runtime](https://onnxruntime.ai/)
- [NDI SDK](https://ndi.video/)
