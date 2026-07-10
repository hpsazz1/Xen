# 采集诊断

当诊断日志输出类似以下行时：

```text
[CaptureDiag] backend=TRT method=duplication_api capture_fps=60 use_cuda=true show_window=true prefer_gpu=false need_cpu_copy=true ...
```

使用以下字段进行解读：

| 字段 | 含义 |
|---|---|
| `backend` | 当前推理 backend，如 `TRT` 或 `DML`。 |
| `method` | 采集方法，通常为 `duplication_api`。 |
| `use_cuda` | 配置/构建是否允许 CUDA 采集。 |
| `show_window` | 预览/调试窗口是否打开。 |
| `prefer_gpu` | 采集路径是否尝试保持在 GPU 端。 |
| `need_cpu_copy` | 当前某个功能是否需要 CPU 可读像素。 |
| `gpu_attempts` | GPU 采集尝试次数。如果为 `0`，则表示未尝试 GPU 路径。 |
| `gpu_ok` | 成功的 GPU 采集帧数。 |
| `gpu_timeout`、`gpu_not_ready`、`gpu_lost` | GPU 采集失败类别。 |
| `cuda_map_failed`、`cuda_array_failed`、`cuda_copy_failed` | CUDA 互操作/复制失败类别。 |
| `cpu_path_frames` | 通过 CPU 路径采集的帧数。 |
| `trt_cpu_submitted` | 从 CPU 准备的输入提交给 TensorRT 的帧数。 |
| `trt_gpu_submitted` | 从 GPU 路径提交给 TensorRT 的帧数。 |

例如，以下模式表示 TensorRT 正在运行，但采集/预处理通过 CPU 帧进行：

```text
prefer_gpu=false need_cpu_copy=true gpu_attempts=0 cpu_path_frames=6000 trt_cpu_submitted=6000
```

这并不一定是错误的。它表示当前某些设置或功能正在选择 CPU 可读路径。

相关文档：

- [后端选择与检查](backends.md)
- [Overlay 和 GUI 行为](overlay.md)
- [数据收集](data-collection.md)
