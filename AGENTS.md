# Xen 项目开发铁律

本文件适用于仓库根目录及全部子目录。参与本项目的开发者和 AI 编码代理必须遵守以下规则。

---

## 项目概述

Xen 是基于 C++20 原生实现的 Windows AI 辅助瞄准工具，核心管线为：截图采集 → YOLO 目标检测推理 → 瞄准控制。

- **技术栈**：C++20, ONNX Runtime, OpenCV, spdlog, CMake
- **推理后端**：CUDA EP, TensorRT EP, DirectML EP, CPU EP
- **代码风格**：源码集中在 `Xen/` 下，各模块采用平铺式目录结构（`.h` + `.cpp` 同目录，不做 `include/` / `src/` 分离）
- **编译器**：Visual Studio 2026，MSVC v14.51 工具链（`/std:c++20`）

### 依赖版本建议

截至 2026 年 7 月，各依赖的推荐版本及理由：

| 依赖 | 推荐版本 | 发布时间 | 理由 |
|---|---|---|---|
| **C++ 标准** | C++20 | 2020 年 | VS 2026 默认标准，完整支持，所有依赖兼容。C++23 尚在预览，C++26 仅标准库部分可用。 |
| **CUDA Toolkit** | 13.3.1 | 2026 年 6 月 | 最新稳定版。13.4.0 仅为开发者预览，不可用于生产。 |
| **TensorRT** | 11.1 | 2026 年 6 月 GA | 11.0 以下存在反序列化安全漏洞（SNYK-2026-17972627），升级到 11.1 是安全之举。默认 CUDA 升至 13.3。 |
| **ONNX Runtime** | 1.27.1 | 2026 年 7 月 11 日 | 最新稳定版，含安全补丁和 CUDA QMoE 优化。目标 ONNX 1.21。当前项目通过 `ONNXRUNTIME_ROOT` 指向本地 SDK。 |
| **OpenCV** | 4.14.0 | 2026 年 | 4.x 最后稳定线，CPU LetterBox 够用。5.0.0 的 DNN 改进与项目无关（推理走 ONNX Runtime），且不兼容 4.x。 |
| **CMake** | 4.4.0 | 2026 年 7 月 14 日 | 最新版。当前项目最低要求 3.18，实际建议 3.21+ 以利用 FetchContent 的 `FIND_PACKAGE_ARGS`。 |
| **spdlog** | 1.17.0 | 2026 年 1 月 4 日 | 内置 fmt 升至 12.1.0，修复 `%z` 格式化跨平台一致性问题。当前项目 FetchContent 锁定 v1.15.0，升级代价极低。 |

**升级优先级：**

1. **spdlog**（低成本）：仅需改 FetchContent 的 `GIT_TAG` 为 `v1.17.0`，无代码改动。
2. **ONNX Runtime**（中成本）：需要重新下载 ORT SDK 包，但 API 向后兼容，无代码改动。
3. **TensorRT / CUDA**（高成本）：需同步升级 GPU 驱动和 CUDA Toolkit，建议在新硬件或安全要求下再升级。
4. **OpenCV 5.0.0**（不推荐当前升级）：不兼容 4.x，需改代码，且项目只用 `cv::resize` + `cv::cvtColor`，升级收益为零。

---

## 构建命令

```bash
# 指定 ONNX Runtime 路径
set ONNXRUNTIME_ROOT=C:\path\to\onnxruntime-win-x64-gpu

# 配置 + 构建
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel

# 仅编译 log 模块（调试用）
cmake --build build --target log
```

---

## 目录结构

```
Xen/
├── CMakeLists.txt              ← 唯一顶层构建文件，所有源文件集中管理
├── AGENTS.md                   ← 本文
├── README.md                   ← 项目入口与文档索引
├── Xen/                        ← C++ 源码根目录
│   ├── detector/               ← 检测器模块
│   │   ├── detector.h / .cpp   ← 唯一公有头文件 + pipeline 编排
│   │   ├── session.h / .cpp    ← ONNX Runtime 会话封装
│   │   ├── preprocess.h / .cpp ← LetterBox 前处理
│   │   └── postprocess.h / .cpp ← 解码 + NMS
│   └── log/                    ← 日志基础设施模块
│       ├── log.h               ← Log 静态单例 + 宏定义
│       └── log.cpp             ← spdlog 异步线程池 + 4 种 sink 实现
└── docs/                       ← 项目文档、分析记录、踩坑笔记
    ├── 001_整体项目设计_20260730.md
    ├── 002_Log模块设计_20260730.md
    ├── 003_Detector原理与实现_20260730.md
    ├── 004_Capture模块方案_20260730.md
    └── todolist.md              ← 待办清单
```

**关键规则：**
- 模块内头文件和源文件平铺在一起，不做 `include/` / `src/` 分离
- 文档和构建文件中的源码路径以仓库根目录为基准，统一写作 `Xen/<module>/<file>`
- 源码内部 include 以源码根目录 `Xen/` 为基准，统一写作 `<module>/<file>`
- Log 是全局基础设施，不隶属于任何模块，不依赖项目中任何其他模块
- Detector 对上游截图和下游瞄准均无依赖，输入是 `cv::Mat`，输出是 `std::vector<Detection>`
- 每个模块只暴露一个公有头文件（`detector.h` / `log.h`）
- 内部实现通过 `detail/` 命名空间隔离

---

## 构建与代码完整性铁律

1. **每次代码变更必须提交到 Git 仓库并推送云端**，提交信息使用中文。提交前确认改动范围、文档和测试结果，推送后确认远端包含该提交。

2. **所有代码修改必须同步更新相关文档**；问题分析、技术选型、流程和踩坑记录统一保存在根目录 `docs/`。

3. **开发前先理解完整调用链、数据流、配置、线程和构建影响**，从顶层设计推进，禁止连续堆叠局部补丁。

4. **项目定位是"小而美的瑞士军刀"**：围绕核心功能设计最小闭环，避免大而全、过度设计和过度开发。

5. **每项代码改动必须形成闭环**：分析 → 设计 → 实现 → 文档 → 单元测试 → 构建验证 → 提交推送。核心逻辑必须有详细单元测试。

6. **踩坑记录格式**：背景 → 原因 → 排查过程 → 解决方案 → 验证结果 → 后续建议。

7. **所有方案必须基于真实使用场景、可验证数据和落地约束**，禁止不切实际、假大空的设计。

8. **文档命名规范**：`编号_中文标题_日期.md`，不含空格。例如 `003_输入延迟排查_20260713.md`。`docs/todolist.md` 是待办清单的唯一固定命名例外。

9. **核心功能优先**：非阻塞细节记录到 `docs/todolist.md`，不得长期占用主线。

10. **技术和算法选型优先采用行业最佳实践与稳定依赖**；除商业机密或现有方案不适用外，不重复造轮子。选型结论必须记录依据和权衡。

11. **使用Skills 提高效率**；工具不可用时采用可靠替代方案，不得虚构使用结果。

12. **构建和测试流程必须沉淀为正式可复用脚本**；禁止反复创建并删除一次性脚本。临时诊断命令有复用价值时应整理进正式脚本或文档。

13. **核心代码必须补充准确详细的中文注释**：解释意图、单位、边界和原因，不复述语法。

14. **Lang 统一为中文**：所有注释、文档、提交信息、讨论均为中文。变量名、函数名、类名使用英文。

---

## C++ 代码约定

- **C++20**，CMake ≥ 3.18
- **头文件使用 `#ifndef` / `#define` / `#endif` 传统守卫**，不使用 `#pragma once`
- **头文件扩展名 `.h`**，源文件 `.cpp`
- **RAII + 智能指针** 管理资源，禁止裸 `new`/`delete` 泄漏
- **不抛异常**：错误通过返回 `false` 或空 `vector` 表示，内部用 `try/catch` 兜底
- **命名规范**：类名 `PascalCase`，函数名 `snake_case`，成员变量 `snake_case_`，常量 `kPascalCase`

---

## Log 模块特定约定

Log 模块使用 spdlog 异步模式（`spdlog::init_thread_pool(4096, 1)`），设计原则：

| 约定 | 详情 |
|---|---|
| 全局单例 | `Log::init()` / `Log::shutdown()` 在应用启动和退出各调用一次 |
| 模块注册 | 每个模块通过 `Log::register_module("名称", LogLevel::INFO)` 注册 |
| 日志宏 | 始终使用 `LOG_INFO/WARN/ERROR(module, fmt, ...)` |
| TRACE/DEBUG | 仅在 `LOG_ENABLE_DEBUG` 或 Debug 构建下编译，Release 下 `((void)0)` |
| 模块名 | 使用简短字符串如 `"detector"`, `"capture"`, `"aim"` |
| Sink 过滤 | 控制台仅 INFO+、文件仅 WARN+、调试文件和环形缓冲区全部级别 |
| 环形缓冲区 | 保留最后 1024 条，崩溃时通过 `Log::dump_ring_buffer()` 写入文件 |
| 频率控制 | 高频日志（每帧）使用 TRACE；WARN 级别异常按时间间隔限流（≥5秒） |

**严禁行为：**
- 禁止在各模块中直接 `#include <spdlog/...>` 或创建 spdlog logger
- 禁止在推理热路径（`detect()`）打印 INFO 及以上级别日志
- 禁止在 Release 模式打印 debug/trace 日志

---

## Agent 行为规则

- **修改任何文件前先读取它**；修改函数前先 grep 所有调用点
- **修改 CMakeLists.txt 或添加依赖时必须停止并请求确认**
- **不确定 ONNX Runtime API 时，先查 `onnxruntime_cxx_api.h`**，不猜测
- **任务完成后总结**：改了什么文件、为什么、还有哪些 TODO
- **测试失败时修复代码**，不要删除或跳过测试

---

## 提交前最低检查清单

### 代码变更提交
- [ ] `git diff --check`
- [ ] 相关单元测试通过
- [ ] Release 构建通过
- [ ] 文档同步更新
- [ ] 中文 Git commit
- [ ] `git push` 推送当前分支到云端并核对远端提交

### 纯非代码变更（仅 docs、AGENTS.md、注释）
- [ ] 改动范围与内容一致性检查
- [ ] `git diff --check`
- [ ] 中文 Git commit

---

## 边界与禁区

| 级别 | 规则 |
|---|---|
| ✅ 始终执行 | 写中文注释、同步文档、提交前检查、使用 Log 宏 |
| ⚠️ 先确认 | 修改 CMakeLists.txt、新增依赖、改公有接口、改 ONNX Runtime EP 配置 |
| 🚫 禁止 | 提交密钥或 `.env`、跳过测试、删除已有测试、修改 `third_party/`、在热路径打印日志、裸 `new`/`delete` 不配对 |

---

## 常见陷阱

1. **ONNX Runtime 的 `GetInputTypeInfo()` 在 session 创建后才可用**，不能在 `setup_options()` 中调用
2. **spdlog 的 `init_thread_pool()` 必须在任何 logger 创建之前调用**
3. **OpenCV `cv::Mat` 默认 BGR 顺序**，传给 ONNX 前需转为 RGB
4. **CMake FetchContent 的 `FetchContent_MakeAvailable` 必须在 `add_library` 之前**
5. **环形缓冲区 `last_formatted()` 返回的是 `fmt::memory_buffer`**，需转为 `std::string`
