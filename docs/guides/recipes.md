# 常见方案

## 最低设置负担

```ini
backend = DML
ai_model = your_model.onnx
input_method = WIN32
circle_fov_enabled = true
```

## CUDA/TensorRT 性能测试

```ini
backend = TRT
ai_model = your_model.engine
capture_method = duplication_api
capture_use_cuda = true
show_window = false
collect_data_while_playing = false
```

然后检查 `[CaptureDiag]` 输出。

## Provider 基准测试

当你需要可重复的 provider 计时，而不启动采集、overlay、输入设备或文件日志时，请使用此功能：

```powershell
.\Xen.exe --benchmark-providers
.\Xen.exe --benchmark-providers cpu,dml-gpu --bench-runs 200 --bench-warmup 20
.\Xen.exe --benchmark-providers dml-gpu,dml-cpu --bench-model models\your_model.onnx
.\Xen.exe --benchmark-providers cuda --bench-cuda-model models\your_model.engine
```

基准测试会以秒为单位打印一行最终的 CSV 风格摘要。DML 构建支持 `cpu`、`dml-gpu` 和 `dml-cpu`；CUDA 构建通过 TensorRT 支持 `cuda`。

DML 基准运行将行追加到 `benchmark_results\provider_benchmark.csv`；CUDA 基准运行将行追加到 `benchmark_results\provider_benchmark_cuda.csv`。使用 `--bench-no-save` 进行一次性运行。

## Razer 控制测试

```ini
input_method = RAZER
```

确保 `rzctl.dll` 在 `Xen.exe` 旁边或位于：

```text
Xen
```

从 `AUTO` 开始广泛搜索，确认设备后缩小过滤器范围。

相关文档：

- [采集诊断](capture-diagnostics.md)
- [输入方法](input-methods.md)
- [构建工作流](build-workflow.md)
