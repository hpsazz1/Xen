# 后端选择与检查

## 选择 DML 还是 CUDA

### 何时使用 DML

- 你需要最简单的 Windows GPU 路径。
- 你在测试兼容性。
- 你不想安装 CUDA 和 TensorRT。
- 你使用的是 ONNX 模型。

### 何时使用 CUDA + TensorRT

- 你拥有 NVIDIA GPU。
- 已安装 CUDA Toolkit 和 TensorRT。
- 你需要最高性能的 backend。
- 你使用的是 TensorRT `.engine` 模型，或者你有 `.onnx` 准备首次生成 engine。

## DML 能运行但检测不到任何目标

首先检查以下配置：

```ini
backend = DML
ai_model = your_model.onnx
confidence_threshold = 0.10
class_player = 0
class_head = 1
```

常见原因：

- 选择的模型是 TensorRT `.engine` 而不是 `.onnx`。
- `confidence_threshold` 对该模型设置过高。
- 模型类别 ID 与 `class_player` 和 `class_head` 不匹配。
- `dml_device_id` 指向了错误的 GPU。
- 采集源实际上没有显示目标内容。

## CUDA 能运行但 GPU 使用率飙升

首先检查哪些功能强制使用 CPU 可读帧：

- 调试/预览窗口：`show_window = true`。
- 数据收集（data collection）。
- 屏幕截图（screenshots）。
- 任何需要像素在 CPU 上进行显示或保存的功能。

对于当前的 CUDA 路径，推荐的 FOV 限制器为：

```ini
circle_fov_enabled = true
```

如果在 GUI 或 overlay 打开时飙升消失，请比较两种状态下的采集诊断信息。GUI/预览可能会改变应用请求 CPU 副本的方式，这可能导致运行路径与 GUI 关闭时不同。

相关文档：

- [采集诊断](capture-diagnostics.md)
- [圆形 FOV](circle-fov.md)
- [常见方案](recipes.md)
