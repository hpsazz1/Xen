# 采集诊断

## 统一帧率判断

“采集详情”和流水线 CSV 对全部采集方式使用同一组分层指标：

| UI / CSV | 含义 |
|---|---|
| 输入源声明 FPS / `SourceDeclaredFPS` | 协议帧头或设备驱动声明值；Desktop Duplication、WinRT、UDP 不提供时显示 `N/A`。 |
| 输入源实际到达 FPS / `SourceReceiveFPS` | 后端真实收到或成功取得的源帧率。 |
| 捕获处理 FPS / `FPS` | Xen 完成取帧并提交给检测器的速率。 |
| 检测发布 FPS / `InferenceFPS` | 检测器完成推理并发布结果的速率。 |
| 累计输入 / `SourceReceivedFrames` | 当前采集会话真实取得或收到的源帧数。 |
| 源帧淘汰 / `SourceDroppedFrames` | 尚未消费便被新帧替换，或采集 API 明确报告遗漏的帧数；不是网络丢包的同义词。 |

实时瞄准始终优先保留最新帧。输入速度高于消费速度时，NDI 和 UDP 会覆盖旧帧并增加“源帧淘汰”，不会排队处理已经过期的画面。声明 FPS 只用于对照，控制器仍使用实际观测时间间隔和捕获窗统计，不绑定固定 240 FPS。

移动诊断另提供 `FrameCountLimit` 和 `SpeedLimited`。前者是按本帧真实时间间隔换算出的设备计数预算，后者为 `1` 时表示请求被最大设备速率截断。连续大量出现 `SpeedLimited=1` 且稳定后无越界，才构成提高 `move_max_speed_cps` 的数据依据。

四链路九宫格数据可使用正式脚本统一分析，目录结构应为 `<CUDA|DML>/<ndi|udp>/<左|上|右>.csv`：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/analyze_nine_grid.ps1 -DataRoot C:\Users\User\Desktop\Xen
```

脚本按相邻记录超过 100 ms 自动分段，左右文件依次映射为上、中、下，上方文件依次映射为左、中、右，并汇总首次稳定时间、稳定后最大误差、稳定态退出、主轴越心、限速比例、帧率、观测年龄和输出队列深度。需要留存机器可读结果时传入 `-OutputCsv <路径>`。

四链路基线建立后，日常静止目标调参可只执行 CUDA 环境下的 NDI、UDP 完整九宫格；CUDA 的较高推理吞吐更容易暴露传输差异和高频近中心边界。发布前仍需 DML UDP 的上中、左下、右下三点冒烟以及 DML/CUDA 双后端 Release 构建。修改推理、捕获、坐标、控制器或输入队列时必须恢复四链路完整九宫格，详细判定见 `docs/015四链路复测与最小测试矩阵20260713.md`。

移动目标CSV额外提供 `ObservedVelocityX/ObservedVelocityY` 有符号相对观测速度。CUDA环境下的NDI/UDP移动数据使用以下命令统一分析：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/analyze_moving_target.ps1 -DataRoot C:\Users\User\Desktop\XenMoving -Axis X
```

首轮只采集水平向左、水平向右和左右反转三个固定脚本场景，保持80 ms与1440 counts/s静止基线不变。指标定义、目录结构和决策顺序见 `docs/016移动目标测试方案20260713.md`。

分析器使用 `FinalMx/My` 计算实际输出counts/s，并从请求像素与请求计数恢复counts/px，输出 `ApproxClosedLoopLagMs`。只有文件名包含 `reverse` 或 `reversal` 的往返场景才统计持续误差侧翻转和±8 px中心恢复，单向场景固定不报告反转。默认剔除不足500 ms或预热后少于30个有效样本的碎片，可用 `MinTrialDurationMs/MinTrialSamples` 调整。首轮数据结论与PI候选见 `docs/017移动目标首轮分析与PI候选20260713.md`，两轮中心停发与DML积分解卷绕见 `docs/018移动PI稳定态冲突修复20260714.md`、`docs/019DML移动复测与积分解卷绕20260714.md`。

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
