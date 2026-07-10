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

```
├── Xen.sln                        # VS 解决方案
├── CMakeLists.txt                 # CMake 构建配置
├── BUILDER.bat / .ps1             # 一键构建脚本
├── build_cuda.bat                 # CUDA 构建入口
├── build_dml.bat                  # DML 构建入口
│
├── Xen\                           # 源码目录
│   ├── Xen.cpp / Xen.h            # 主程序入口
│   ├── Xen.vcxproj                # VS 项目文件
│   ├── Xen.rc / xen.ico           # 图标资源
│   │
│   ├── capture\                   # 屏幕采集
│   │   ├── capture.h / .cpp       # 采集基类 + 调度
│   │   ├── duplication_api_*      # DXGI 采集
│   │   ├── winrt_capture.*        # WinRT 采集
│   │   ├── virtual_camera.*       # 虚拟摄像头采集
│   │   ├── udp_capture.*          # UDP 网络采集
│   │   └── ndi_capture.*          # NDI 网络采集
│   │
│   ├── detector\                  # 目标检测
│   │   ├── trt_detector.*         # TensorRT 检测器 (CUDA)
│   │   ├── dml_detector.*         # DirectML 检测器 (DML)
│   │   ├── postProcess.*          # 后处理
│   │   └── cuda_preprocess.cu     # CUDA 预处理核
│   │
│   ├── depth\                     # 深度估计
│   ├── tensorrt\                  # TensorRT 封装
│   ├── overlay\                   # ImGui 界面
│   ├── mouse\                     # 鼠标控制
│   ├── keyboard\                  # 键盘监听
│   ├── config\                    # 配置文件
│   ├── mem\                       # 资源管理
│   ├── runtime\                   # 线程循环
│   ├── scr\                       # 工具函数
│   ├── benchmarks\                # 性能测试
│   │
│   ├── modules\                   # 第三方模块
│   │   ├── opencv\build\dml\      # DML OpenCV
│   │   ├── opencv\build\cuda\     # CUDA OpenCV
│   │   ├── TensorRT-10.16.1.11\   # TensorRT SDK
│   │   ├── serial\                # 串口库
│   │   ├── imgui-1.92.8\          # ImGui 界面库
│   │   └── stb\                   # 图像库
│   │
│   └── x64\                       # VS 编译输出
│       ├── CUDA\Xen.exe           # CUDA 构建产物
│       └── DML\Xen.exe            # DML 构建产物
│
├── tools\                         # 构建脚本
├── packages\                      # NuGet 包
├── docs\                          # 文档
└── .gitignore
```

- [TensorRT 文档](https://docs.nvidia.com/deeplearning/tensorrt/)
- [OpenCV 文档](https://docs.opencv.org/4.x/d1/dfb/intro.html)
- [ImGui](https://github.com/ocornut/imgui)

## 许可说明

| 依赖项 | 许可证 |
| --- | --- |
| OpenCV | [Apache License 2.0](https://opencv.org/license.html) |
| ImGui | [MIT License](https://github.com/ocornut/imgui/blob/master/LICENSE) |
