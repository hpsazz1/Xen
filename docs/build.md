# 从源码构建

本文档记录了当前基于包装脚本的构建系统，包含依赖管理和模型导出的实用说明。

使用项目提供的批处理包装脚本进行构建。这些脚本会自动配置 PowerShell 环境、Visual Studio 编译环境、Ninja、后端特定依赖、CMake 生成器和构建配置。

## 1. 选择后端

| 后端 | 适用场景 | 模型格式 |
|---|---|---|
| DML | 希望使用最简单的 Windows GPU 路径，或需要非 NVIDIA 支持 | `.onnx` |
| CUDA + TensorRT | 拥有支持的 NVIDIA 显卡并追求最佳性能 | `.engine`，首次构建可用 `.onnx` |

两种构建在编译时分离。CUDA 构建不会编译或链接 DirectML/ONNX Runtime 文件，DML 构建不会编译 CUDA/TensorRT 文件。

## 2. 环境要求

两种构建均需要：

- Windows 10 或 Windows 11 x64。
- Visual Studio 或 Visual Studio Build Tools，需安装"使用 C++ 的桌面开发"工作负载。
- 带有 C++/WinRT 头文件的 Windows SDK。
- CMake。
- PowerShell。
- 首次设置依赖时需要互联网连接（如果所有依赖已本地准备则不需要）。

脚本可以下载或还原多个项目依赖，但无法自动安装 Visual Studio、显卡驱动或 NVIDIA 账户限制的 SDK。

### DML 要求

- 支持 DirectML 的显卡和驱动。
- 脚本会还原的 NuGet 包：
  - `Microsoft.ML.OnnxRuntime.DirectML`
  - `Microsoft.AI.DirectML`
- OpenCV DML 布局位于：

```text
Xen\modules\opencv\build\dml
```

DML 包装脚本可以自动准备 OpenCV 布局。

### CUDA 要求

- 支持的 NVIDIA 显卡。GTX 10xx/Pascal 及更旧版本不受当前 TensorRT 路径支持。
- 运行时需要 CUDA Toolkit 13.1 或更新版本。当前依赖解析器优先使用 CUDA 13.2 + TensorRT 10.16，CUDA 13.1 + TensorRT 10.14 作为回退方案。
- TensorRT 10 Windows 二进制 SDK，解压到：

```text
Xen\modules\TensorRT-*
```

- 使用 CUDA 支持编译的 OpenCV 4.13.0，位于：

```text
Xen\modules\opencv\build\cuda
```

cuDNN 对本项目是可选的。CUDA OpenCV 助手默认禁用 OpenCV DNN 的 CUDA/cuDNN，因为推理使用的是 TensorRT 而非 OpenCV DNN。

## 3. 首次构建

在项目根目录运行交互式启动器：

```powershell
.\BUILDER.bat
```

根据提示选择 `DML` 或 `CUDA`。如果是首次构建，请允许下载/更新，除非所有依赖已准备就绪。

也可以直接使用后端包装脚本：

```powershell
.\build_dml.bat
.\build_cuda.bat
```

非交互式示例：

```powershell
.\build_dml.bat -NonInteractive -OpenCvAlreadyBuilt $false -DownloadOrUpdateNeeded $true
.\build_cuda.bat -NonInteractive -OpenCvAlreadyBuilt $false -DownloadOrUpdateNeeded $true -OpenBrowserForDownloads
```

常用 CUDA 选项：

```powershell
.\build_cuda.bat -CudaArchBin 8.6
.\build_cuda.bat -CudaArchBin all
.\build_cuda.bat -SkipOpenCvBuild -OpenCvAlreadyBuilt $true
```

CUDA Release 构建默认使用 `-CudaArchBin all`。该选项同时控制 OpenCV CUDA 内核与主程序的 `cuda_preprocess.cu`，不能只覆盖其中一处。

`-CudaArchBin all` 展开为：

```text
7.5;8.0;8.6;8.7;8.8;8.9;9.0;10.0;10.3;11.0;12.0;12.1
```

OpenCV 安装完成后会在 `Xen\modules\opencv\build\cuda\opencv-cuda-build.json` 写入架构清单。后续构建只有在清单与请求架构完全一致时才复用 OpenCV；旧依赖没有清单、清单损坏或架构不一致时会自动清洁重建。`-SkipOpenCvBuild` 遇到不兼容清单会直接失败，避免生成只能在构建机显卡上运行的发布包。

CUDA 13.2 的此处 `all` 是项目支持的 Turing 及更新架构集合，并不包含 Pascal `6.1` 等旧架构。旧显卡应使用兼容的旧 CUDA 工具链，或运行 DML 后端。

对于发布版本构建，建议保留多架构 CUDA 集，但将 OpenCV 限制为应用实际使用的模块，并降低 OpenCV 构建并行度：

```powershell
.\build_cuda.bat -CudaArchBin all -OpenCvAlreadyBuilt $false -DownloadOrUpdateNeeded $true -OpenCvBuildList cudev,core,imgproc,imgcodecs,videoio,highgui,dnn,cudaarithm,cudaimgproc,cudawarping -OpenCvMaxCpuCount 2 -OpenCvCleanBuild
```

这样会为 OpenCV 和主程序生成同一套 `all` 架构，同时避免编译未使用的 contrib CUDA 模块（如 `cudafilters`）。更改 OpenCV 模块列表时请使用 `-OpenCvCleanBuild`，防止旧 CMake 缓存保留过时的模块状态。

构建脚本单元测试：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tests\build_script_tests.ps1
```

## 4. 快速本地重新构建

在完整构建脚本完成选定后端的首次配置后，可以使用：

```powershell
.\build_no-options.bat -Backend DML
.\build_no-options.bat -Backend CUDA
```

此命令仅运行现有的 CMake 构建树：

```text
cmake --build build\<backend> --config Release --target ai --parallel
```

它不会还原包、下载依赖、重新构建 OpenCV 或刷新 CMake 缓存路径。如果依赖路径发生变化或构建树已过期，请重新运行 `BUILDER.bat`、`build_dml.bat` 或 `build_cuda.bat`。

## 5. 包装脚本的准备工作

完整的构建包装脚本会自动完成以前需要手动操作的设置工作：

- 通过 `VsDevCmd.bat` 导入 Visual Studio 编译环境。
- 查找或缓存 Ninja。
- 为 DML 构建还原 NuGet 包。
- 下载或准备 `SimpleIni.h` 和内嵌的 `serial` 模块（如缺失）。
- 准备 DML OpenCV 或编译 CUDA OpenCV。
- 仅解析选定后端的依赖。
- 写入 `build\dependency-resolution.json` 用于调试。
- 使用 `Ninja Multi-Config` 配置 CMake。
- 编译 `Xen.exe`。

顶层的 `CMakeLists.txt` 仅接受 Ninja 生成器作为自动化构建路径。请勿使用旧版 Visual Studio CMake 生成器命令作为常规构建方式。

## 6. 依赖布局

包装脚本可以准备大部分依赖布局，但了解项目期望的结构仍然有用：

| 依赖项 | 预期位置 |
|---|---|
| SimpleIni | `Xen\modules\SimpleIni.h` |
| serial | `Xen\modules\serial\` |
| TensorRT | `Xen\modules\TensorRT-*\` |
| DML OpenCV | `Xen\modules\opencv\build\dml\` |
| CUDA OpenCV | `Xen\modules\opencv\build\cuda\` |
| DML NuGet 包 | `packages\Microsoft.ML.OnnxRuntime.DirectML.*` 和 `packages\Microsoft.AI.DirectML.*` |

Visual Studio 项目文件不是依赖路径的真实来源。请使用包装脚本生成的 CMake 配置。

## 7. 输出和运行时文件

默认输出路径：

```text
build\dml\Release\Xen.exe
build\cuda\Release\Xen.exe
```

这两个 `build` 路径是唯一规范的测试与发布产物。历史 `x64\DML\Xen.exe`、`x64\CUDA\Xen.exe` 不会由当前构建脚本更新，不得用于复测。构建脚本发现历史副本时会输出警告；流水线CSV中的 `BuildBackend/BuildRevision/BuildTimestampUtc/ControllerRevision` 用于确认实际运行版本。

主界面侧边栏标题同步显示相同身份，格式为 `Xen  <DML|CUDA> <7位提交> r<控制器修订号>`；未提交构建会在提交号后显示 `*`。现场复测可同时核对界面标题与CSV首行。

启动时，`Xen.exe` 会将工作目录切换到自身所在文件夹，并创建运行时目录，例如：

```text
models
depth_models
```

将检测模型放入 `Xen.exe` 旁边的 `models` 文件夹。将深度模型放入 `Xen.exe` 旁边的 `depth_models` 文件夹。`screenshots` 目录仅在保存截图时创建。

当 `AIMBOT_COPY_RUNTIME_DLLS` 启用时，CMake 会将可用的运行时 DLL 复制到 `Xen.exe` 旁边，包括：

- OpenCV 运行时 DLL。
- `ghub_mouse.dll`（如存在）。
- `rzctl.dll`（如存在）。
- DML 构建的 ONNX Runtime 和 DirectML DLL。
- CUDA 构建的 TensorRT 和可选的 cuDNN DLL。

## 8. 高级 CMake 覆盖

优先使用包装脚本参数而非手动 CMake 命令。需要时可以通过批处理包装脚本传递额外的 CMake 缓存变量：

```powershell
.\build_dml.bat -DAIMBOT_OPENCV_DML_ROOT=C:/path/to/opencv/dml
.\build_cuda.bat -DAIMBOT_TENSORRT_ROOT=C:/path/to/TensorRT-10.x -DAIMBOT_CUDNN_ROOT=C:/path/to/cudnn
```

常用缓存变量：

| 变量 | 说明 |
|---|---|
| `AIMBOT_USE_CUDA` | `ON` 为 CUDA + TensorRT，`OFF` 为 DML。由包装脚本设置。 |
| `AIMBOT_COPY_RUNTIME_DLLS` | 将运行时 DLL 复制到 `Xen.exe` 旁边。 |
| `AIMBOT_OPENCV_DML_ROOT` | DML OpenCV 构建根目录。 |
| `AIMBOT_OPENCV_CUDA_ROOT` | CUDA OpenCV 安装根目录。 |
| `AIMBOT_TENSORRT_ROOT` | TensorRT SDK 根目录。 |
| `AIMBOT_CUDNN_ROOT` | 可选的 cuDNN 根目录。 |
| `AIMBOT_RZCTL_DLL` | `rzctl.dll` 的源路径。 |
| `AIMBOT_CPPWINRT_INCLUDE_DIR` | C++/WinRT 包含目录（自动检测失败时使用）。 |

## 9. 导出 AI 模型

使用 Ultralytics 将 PyTorch `.pt` YOLO 模型转换为 ONNX：

```bash
pip install ultralytics -U

# TensorRT/CUDA 源 ONNX。应用可以从此生成匹配的 .engine。
yolo export model=your_model.pt format=onnx dynamic=true simplify=true

# DML 源 ONNX。
yolo export model=your_model.pt format=onnx simplify=true
```

对于 DML，将导出的 `.onnx` 放入 `Xen.exe` 旁边的 `models` 文件夹，然后在界面中选择它。

对于 CUDA，将导出的 `.onnx` 放入 `Xen.exe` 旁边的 `models` 文件夹。当 CUDA 后端加载 `.onnx` 模型且缺少对应的 `.engine` 文件时，它会自动构建并将 `.engine` 保存在 `.onnx` 旁边，然后更新 `config.ini` 以使用生成的引擎。

深度模型是独立的。将深度 `.onnx` 文件放入 `depth_models` 文件夹，使用界面中的 Depth 部分在需要时导出 TensorRT 深度引擎。

## 10. 验证

在源码或构建系统更改后，通过对应的包装脚本重新构建您更改的后端，并从输出文件夹运行 `Xen.exe`。优先使用真实的 DML 或 CUDA 冒烟测试，而非静态源码检查，因为架构经常变化。

如需可重复的提供者计时，可从输出文件夹运行提供者基准测试：

```powershell
.\Xen.exe --benchmark-providers
.\Xen.exe --benchmark-providers cpu,dml-gpu --bench-runs 200 --bench-warmup 20
.\Xen.exe --benchmark-providers cuda --bench-cuda-model models\your_model.engine
```

基准测试会打印最终的 CSV 格式摘要（以秒为单位），不会写入日志文件。DML 构建测试 ONNX Runtime 提供者（`cpu`、`dml-gpu`、`dml-cpu`）并追加到 `benchmark_results\provider_benchmark.csv`。CUDA 构建仅测试 TensorRT/CUDA 并追加到 `benchmark_results\provider_benchmark_cuda.csv`。使用 `--bench-no-save` 进行仅控制台运行，或使用 `--bench-results <path>` 指定不同的 CSV 路径。

## 11. 排错

| 问题 | 检查内容 |
|---|---|
| `build_no-options` 提示 `Build tree not found` | 先为对应后端运行完整包装脚本。 |
| CMake 拒绝生成器 | 使用包装脚本。自动化路径要求 Ninja 或 Ninja Multi-Config。 |
| DML OpenCV 缺失 | 以允许下载的方式运行 `.\build_dml.bat`，或设置 `AIMBOT_OPENCV_DML_ROOT`。 |
| CUDA 依赖缺失 | 安装 CUDA Toolkit 并将 TensorRT Windows 二进制 SDK 解压到 `Xen\modules`，然后重新运行 `.\build_cuda.bat`。 |
| TensorRT 压缩包已解压但未检测到 | 确保下载的是 Windows 二进制 SDK 压缩包，而非 TensorRT 源代码。目录结构必须包含 `include\NvInfer.h`、`lib\nvinfer_10.lib` 和 `bin\nvinfer_10.dll`。 |
| OpenCV CUDA 构建失败 | 检查 CUDA、MSVC、OpenCV 4.13.0、contrib 源码和 CUDA 架构。通过 `.\build_cuda.bat` 重试，确保助手使用相同设置。 |
| `rzctl.dll` 缺失 | 保持 `Xen\rzctl.dll` 在仓库中，或将其复制到 `Xen.exe` 旁边。 |
| DML 运行但无法检测 | 确认所选模型为 `.onnx`，降低 `confidence_threshold`，并验证类别 ID。 |
| CUDA 引擎加载失败 | 删除旧的 `.engine`，选择 `.onnx`，让 CUDA 后端为当前 TensorRT/CUDA 环境重新构建引擎。 |

## 12. Release 运行时依赖清理

Release 输出采用后端独立的运行时清单。构建会从当前 CUDA Toolkit、TensorRT、Visual Studio Redistributable 或 DML/ONNX Runtime 包复制必需 DLL，并删除旧后端 DLL、未使用的 CUDA/cuDNN/TensorRT 插件以及链接和测试产物。不要把两个后端目录中的 DLL 混合复制。

CUDA 目录中的 `nvinfer_builder_resource_*.dll` 用于在目标电脑上从 ONNX 构建 TensorRT engine，属于当前完整功能所需文件；目标电脑的 `nvcuda.dll` 则由 NVIDIA 驱动提供，不应随程序复制。完整依赖依据、保留清单和验证流程见 `docs/013CUDA发布依赖清理20260713.md`。
