# Xen — Windows AI 辅助瞄准工具

Xen 是基于 C++20 原生实现的 Windows AI 辅助瞄准工具。核心管线为：截图采集 → YOLO 目标检测推理 → 瞄准控制。

当前仓库已实现 Log 与 Detector；Capture、Aim Control、Mouse、Keyboard 和 Runtime 仍处于设计或待实现阶段。

## 目录结构

```text
Xen/                           # 仓库根目录
├── CMakeLists.txt             # 顶层构建文件
├── AGENTS.md                  # 项目开发规范
├── README.md                  # 项目入口
├── Xen/                       # C++ 源码根目录
│   ├── detector/              # Detector 模块（.h + .cpp 平铺）
│   └── log/                   # Log 模块（.h + .cpp 平铺）
└── docs/                      # 设计文档与待办清单
```

文档和构建文件中的源码路径均以仓库根目录为基准，例如 `Xen/detector/detector.cpp`。源码内部的 `#include "detector/detector.h"` 以 `Xen/` 为包含目录基准。

## 构建状态

源码迁移到 `Xen/` 后，顶层 `CMakeLists.txt` 仍引用迁移前的 `detector/` 与 `log/` 路径，因此当前不能按原命令完成配置和构建。该问题已记录在 [待办清单](docs/todolist.md) 的 P0；修改构建文件前需按项目规范单独确认。

路径同步完成后的标准命令为：

```bat
set ONNXRUNTIME_ROOT=C:\path\to\onnxruntime-win-x64-gpu-1.27.1
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
```

## 文档索引

建议按以下顺序阅读：

1. [整体项目设计](docs/001_整体项目设计_20260730.md)
2. [Log 模块设计](docs/002_Log模块设计_20260730.md)
3. [Detector 原理与实现](docs/003_Detector原理与实现_20260730.md)
4. [Capture 模块方案](docs/004_Capture模块方案_20260730.md)
5. [待办清单](docs/todolist.md)

[AGENTS.md](AGENTS.md) 是开发与交付规范，不属于设计文档阅读序列。

## 技术栈

C++20 / ONNX Runtime / OpenCV / spdlog / CMake
