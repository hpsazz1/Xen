# 数据收集

数据收集对模型工作很有用，但可能会影响性能：

```ini
collect_data_while_playing = false
collect_only_when_targets_present = true
collect_save_every_n_frames = 15
collect_jpeg_quality = 95
auto_label_data = true
```

如果采集诊断显示 `need_cpu_copy=true`，数据收集可能是原因之一。

相关文档：

- [采集诊断](capture-diagnostics.md)
- [配置指南](../config.md)

## 移动目标流水线分析

五类移动目标CSV按 `<backend>/<transport>/<scenario>.csv` 放置后，使用正式脚本分析：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/analyze_moving_target.ps1 `
  -DataRoot C:\Users\16143\Desktop\Xen `
  -OutputCsv C:\Users\16143\Desktop\Xen\moving_summary.csv
```

实际跟踪验收优先查看 `ObservedP95AbsAxisErrorPx`、`ObservedP95DistancePx` 和
`ObservedInsideTargetPct`。`P95AbsAxisErrorPx` 使用预测后的控制目标，包含有意的前瞻距离，
只用于解释控制器请求。`OutputSideFlipCount` 统计最终整数设备命令换向，
`PredictionSideFlipCount` 统计预测侧换向，两者不能互相替代。旧CSV缺少原始支点或预测坐标时，
`ObservedTrackingAvailable=0`，只能使用旧控制诊断，不能据此宣称实际跟踪误差。
