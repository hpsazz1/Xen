<div align="center">

# Xen

基于 C++ 原生实现的 Windows 视觉辅助工具，支持 DirectML 和 CUDA + TensorRT 双运行后端。

[![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?style=for-the-badge&logo=cplusplus&logoColor=white)](https://github.com/hpsazz1/Xen)
[![CUDA 13.2](https://img.shields.io/badge/CUDA-13.2-76B900?style=for-the-badge&logo=nvidia&logoColor=white)](https://developer.nvidia.com/cuda-downloads)

<p>
  <a href="docs/build.md">构建指南</a>
  &nbsp;|&nbsp;
  <a href="docs/config.md">配置说明</a>
</p>

</div>

---

## 概述

Xen 提供两种 Windows 运行时方案：

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
| CUDA | CUDA 13.2 |
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
| 数据收集 | [docs/guides/data-collection.md](docs/guides/data-collection.md) |
| 源码配置文件 | [Xen/config/config.cpp](Xen/config/config.cpp)、[Xen/config/config.h](Xen/config/config.h) |

## 项目结构

### 文件树

```
├── Xen.sln                         # VS 解决方案
├── CMakeLists.txt                  # CMake 构建配置
├── BUILDER.bat / BUILDER.ps1       # 一键构建脚本（交互式选择 DML/CUDA）
├── build_dml.bat / build_cuda.bat  # DML / CUDA 后端构建入口
├── build_no-options.bat / .ps1     # 快速增量编译（跳过依赖检查）
├── docs\                           # 文档
├── tools\                          # 构建工具脚本
├── packages\                       # NuGet 包
│
├── Xen\                            # 源代码
│   ├── Xen.cpp / Xen.h             # 主入口 ── 全局变量、线程启动、设备初始化
│   │
│   ├── capture\                    # 画面采集 ── 多源获取原始帧
│   │   ├── capture.cpp             #   ├─ 采集调度中心：后端选择、帧率控制、帧分发
│   │   ├── duplication_api_*       #   ├─ DXGI Desktop Duplication API 采集
│   │   ├── winrt_capture.*         #   ├─ WinRT 窗口/显示器采集
│   │   ├── virtual_camera.*        #   ├─ 虚拟摄像头输入
│   │   ├── udp_capture.*           #   ├─ UDP 网络流采集（MJPEG）
│   │   └── ndi_capture.*           #   └─ NDI 网络视频源采集
│   │
│   ├── detector\                   # AI 目标检测 ── 推理 + 后处理
│   │   ├── dml_detector.*          #   ├─ DirectML 推理（ONNX Runtime）
│   │   ├── trt_detector.*          #   ├─ TensorRT 推理（CUDA）
│   │   ├── postProcess.*           #   ├─ YOLO 输出解码、NMS、阈值过滤
│   │   └── cuda_preprocess.*       #   └─ CUDA 核函数预处理
│   │
│   ├── depth\                      # 深度估计 ── Depth Anything 模型推理
│   │   ├── depth_anything_trt.*    #   ├─ TensorRT 引擎加载与推理
│   │   ├── depth_mask.*            #   └─ 深度遮罩生成（直方图阈值 + 膨胀）
│   │   └── depth_utils.*           #      深度图工具函数
│   │
│   ├── tensorrt\                   # TensorRT 封装 ── 引擎构建、序列化、日志
│   │   ├── nvinf.*                 #   ├─ Inference 封装
│   │   └── trt_monitor.*           #   └─ 导出进度监控 UI
│   │
│   ├── mouse\                      # 鼠标控制 ── 移动算法 + 多设备驱动
│   │   ├── mouse.cpp / .h          #   ├─ 核心：卡尔曼预测、轨迹模拟、速度曲线
│   │   ├── MouseInput.cpp / .h     #   ├─ 设备抽象层（IMouseInput 工厂）
│   │   ├── AimbotTarget.*          #   ├─ 目标选择（头部/身体优先级、多目标追踪）
│   │   ├── ghub.* / rzctl.*        #   ├─ 罗技 G HUB / 雷蛇 rzctl 驱动注入
│   │   ├── Arduino.* / RP2350.*    #   ├─ Arduino / RP2350 单片机串口
│   │   ├── Teensy41RawHid.*        #   ├─ Teensy 4.1 RawHID
│   │   ├── kmbox_net\              #   ├─ Kmbox Net（UDP 协议）
│   │   ├── kmboxA.* / KmboxA*      #   ├─ Kmbox A（HID 协议）
│   │   ├── Makcu.*                 #   └─ Makcu 串口桥接
│   │   └── hid.c                   #       Windows HID 底层 API
│   │
│   ├── keyboard\                   # 键盘监听 ── 快捷键、状态检测
│   │   ├── keyboard_listener.*     #   └─ 键盘线程：瞄准/射击/缩放/热键
│   │   └── keycodes.*              #      虚拟键码映射表
│   │
│   ├── overlay\                    # 覆盖层 UI ── ImGui + D3D11 渲染
│   │   ├── overlay.cpp / .h        #   ├─ 窗口创建、D3D11/DComp、主题、字体
│   │   ├── Game_overlay.*          #   ├─ 游戏内叠加层（检测框、深度、轨迹）
│   │   ├── draw_ai.*               #   ├─ AI 模型/检测参数设置
│   │   ├── draw_mouse.*            #   ├─ 鼠标设置（FOV/预测/轨迹模拟/输入设备）
│   │   ├── draw_capture.*          #   ├─ 捕获设置（来源/分辨率/FPS）
│   │   ├── draw_target.*           #   ├─ 目标偏移 + 追踪器
│   │   ├── draw_buttons.*          #   ├─ 热键绑定
│   │   ├── draw_depth.*            #   ├─ 深度估计设置
│   │   ├── draw_game_overlay.*     #   ├─ 游戏覆盖层样式设置
│   │   ├── draw_overlay.*          #   ├─ 叠加层外观设置
│   │   ├── draw_stats.*            #   ├─ 性能统计图表
│   │   ├── draw_debug.*            #   ├─ 截图/数据收集/调试工具
│   │   ├── config_dirty.*          #   ├─ 配置变更追踪
│   │   ├── draw_settings.*         #   └─ 绘制设置接口声明
│   │   ├── export_progress_panel.* #       TensorRT 导出进度面板
│   │   └── ui_sections.*           #       UI 辅助组件
│   │
│   ├── config\                     # 配置管理 ── INI 文件读写
│   │   ├── config.cpp              #   └─ 全部配置项的默认值、读写逻辑
│   │   └── config.h                #       Config 结构体定义
│   │
│   ├── runtime\                    # 运行时线程 ── 主循环调度
│   │   ├── mouse_thread_loop.*     #   └─ 鼠标线程：检测→预测→移动→射击
│   │   ├── game_overlay_loop.*     #      游戏覆盖层渲染线程
│   │   └── thread_loops_shared.*   #      线程间共享状态
│   │
│   ├── scr\                        # 杂项工具
│   │   ├── other_tools.*           #   └─ 系统信息、窗口枚举、模型管理、图像加载
│   │   └── data_collector.*        #      数据收集/自动标注
│   │
│   ├── mem\                        # 资源管理
│   │   ├── cpu_affinity_manager.*  #   └─ CPU 核心预留 / 系统内存预留
│   │   └── gpu_resource_manager.*  #      GPU 显存预留（CUDA）
│   │
│   ├── benchmarks\                 # 性能基准测试
│   │   └── provider_benchmark.*    #   └─ DML/CUDA 后端推理速度对比
│   │
│   ├── modules\                    # 第三方模块
│   │   ├── opencv\build\           #   ├─ DML / CUDA 双 OpenCV 构建
│   │   ├── TensorRT-*\             #   ├─ TensorRT SDK
│   │   ├── makcu\                  #   ├─ Makcu 串口通信库
│   │   ├── serial\                 #   ├─ 跨平台串口库
│   │   ├── imgui-1.92.8\           #   ├─ Dear ImGui 界面库
│   │   ├── stb\                    #   ├─ stb_image 图像加载
│   │   └── SimpleIni.h             #   └─ INI 文件解析库
│   │
│   └── x64\                        # VS 编译输出
│       ├── CUDA\Xen.exe            #   └─ CUDA 构建产物
│       └── DML\Xen.exe             #       DML 构建产物
│
└── .gitignore
```

## 模块关系图

```
                          ┌─────────────────────────────┐
                          │         main()               │
                          │       Xen.cpp                │
                          │  线程创建 / 设备初始化 / 主循环  │
                          └──────┬──────────┬───────────┘
                                 │          │
                    ┌────────────┘          └────────────┐
                    ▼                                    ▼
        ┌───────────────────┐                ┌───────────────────┐
        │   capture\        │                │   keyboard\       │
        │   画面帧采集        │                │   键盘快捷键 + 状态  │
        │   capture.cpp     │                │   keyboard_*.cpp  │
        └────────┬──────────┘                └────────┬──────────┘
                 │  cv::Mat                           │  aiming / shooting
                 ▼                                    ▼
        ┌───────────────────┐                ┌───────────────────┐
        │   detector\       │                │   mouse\          │
        │   AI 目标检测      │──── Detection ──→│   鼠标控制         │
        │   dml/trt_*.cpp   │    缓冲区        │   mouse.cpp       │
        └────────┬──────────┘                │   ├─ 卡尔曼预测     │
                 │                            │   ├─ 轨迹模拟      │
                 │ 深度推理请求                  │   ├─ 目标修正      │
                 ▼                            │   └─ 速度曲线      │
        ┌───────────────────┐                │                   │
        │   depth\          │                │   设备驱动层        │
        │   深度估计 + 遮罩   │                │   MouseInput.cpp   │
        │   depth_*_trt.cpp │                │   ┌──────┬──────┐ │
        └───────────────────┘                │   │GHUB  │Razer │ │
                                             │   ├──────┼──────┤ │
        ┌───────────────────┐                │   │KmboxA│KmboxN│ │
        │   tensorrt\       │                │   ├──────┼──────┤ │
        │   引擎构建/加载/推理 │                │   │Arduin│RP2350│ │
        │   nvinf.cpp        │                │   ├──────┼──────┤ │
        └───────────────────┘                │   │Teensy│Makcu │ │
                                             │   └──────┴──────┘ │
        ┌───────────────────┐                │   └─ 物理鼠标移动命令
        │   overlay\        │                └───────────────────┘
        │   UI 渲染 (ImGui)  │
        │   overlay.cpp      │ ◀── 所有面板读写 config
        │   ├─ draw_ai       │
        │   ├─ draw_mouse    │        ┌───────────────────┐
        │   ├─ draw_capture  │        │   config\         │
        │   ├─ draw_target   │──读写──→│   config.ini      │
        │   ├─ draw_depth    │        │   config.cpp      │
        │   ├─ draw_stats    │        └───────────────────┘
        │   ├─ draw_debug    │
        │   ├─ draw_buttons  │        ┌───────────────────┐
        │   ├─ draw_overlay  │        │   runtime\        │
        │   │                │        │   线程调度循环      │
        │   └─ Game_overlay  │        │   mouse_thread_*  │
        └───────────────────┘        │   game_overlay_*  │
                                     └───────────────────┘

        数据流: capture → detector → mouse → 设备 → 游戏
        控制流: keyboard / overlay → config → 各模块
```

### 外部链接

- [TensorRT 文档](https://docs.nvidia.com/deeplearning/tensorrt/)
- [OpenCV 文档](https://docs.opencv.org/4.x/d1/dfb/intro.html)
- [ImGui](https://github.com/ocornut/imgui)

## 许可说明

| 依赖项 | 许可证 |
| --- | --- |
| OpenCV | [Apache License 2.0](https://opencv.org/license.html) |
| ImGui | [MIT License](https://github.com/ocornut/imgui/blob/master/LICENSE) |
