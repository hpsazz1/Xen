# 使用与排错指南

本文档是实用设置和诊断的中央索引。详细说明保存在 `docs/guides/` 下的独立文件中，以保持本索引简洁。

## 从这里开始

| 指南 | 适用场景 |
|---|---|
| [首次启动检查清单](guides/first-launch.md) | 您正在启动新的构建或新解压的发布包。 |
| [后端选择与检查](guides/backends.md) | 需要选择 DML 还是 CUDA，或诊断基本的后端行为。 |
| [常用配置](guides/recipes.md) | 需要可直接复制的配置片段和基准测试命令。 |
| [排错步骤](guides/troubleshooting.md) | 遇到未知问题时的简短检查清单。 |

## 采集与性能

| 指南 | 适用场景 |
|---|---|
| [局域网 UDP 采集](guides/udp-capture.md) | 希望从另一台电脑或进程通过 UDP 接收 MJPEG 帧。 |
| [NDI 网络采集](guides/ndi-capture.md) | 希望通过网络接收 NDI 视频流。 |
| [采集诊断](guides/capture-diagnostics.md) | 正在阅读 `[CaptureDiag]` 输出或检查 CPU/GPU 采集路径。 |
| [圆形视野](guides/circle-fov.md) | 正在配置圆形视野限制器和界面预览。 |
| [数据收集](guides/data-collection.md) | 正在收集训练数据或检查其性能影响。 |

## 控制与界面

| 指南 | 适用场景 |
|---|---|
| [输入方式](guides/input-methods.md) | 正在设置 Win32、雷蛇、KMBOX 或其他控制路径。 |
| [界面和 GUI 行为](guides/overlay.md) | 正在调整预览、游戏覆盖或延迟补偿。 |

## 开发

| 指南 | 适用场景 |
|---|---|
| [构建工作流](guides/build-workflow.md) | 正在重新构建本地代码或依赖项。 |

核心参考：

- [配置指南](config.md)
- [从源码构建](build.md)
