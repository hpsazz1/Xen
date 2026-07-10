# 首次启动检查清单

对于新解压的构建或新的本地构建，请使用此清单。

1. 选择正确的构建版本：
   - 如果你想要最简单的设置或非 NVIDIA 兼容性，请使用 **DML**。
   - 如果你已准备好 NVIDIA CUDA/TensorRT 环境，请使用 **CUDA + TensorRT**。
2. 将检测模型放在 `Xen.exe` 旁边的 `models` 文件夹中：
   - DML 使用 `.onnx`。
   - TensorRT 通常使用 `.engine`；CUDA 构建也可以从选定的 `.onnx` 构建 `.engine`。
3. 启动应用一次，以便生成 `config.ini`。
4. 打开 GUI 或 overlay，并尽可能在那里保存设置。
5. 确认所选的 `input_method` 与你实际想要的设备/控制路径匹配。

相关文档：

- [后端选择](backends.md)
- [配置指南](../config.md)
- [从源码构建](../build.md)
