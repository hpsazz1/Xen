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

不同轮次起点不一致时，使用`ObservedSteadyP95AbsAxisErrorPx`比较末尾稳态，不要用全段P95
直接归因控制器。输出换向还需同时查看`OutputSideFlipMeanAbsCounts`和
`OutputSideFlipMaxAbsCounts`；只有换向率、幅度和发生阶段跨至少三轮一致，才允许修改中心门控。

r41起CSV包含`ErrorMotionX/Y`、`MovingInsideSettleX/Y`和`SettledX/Y`。分析Y轴震荡时必须使用
`-Axis Y`并同时查看`AxisSettledPct`：水平移动期间Y轴应长期独立锁存，可靠垂直或斜向运动时
`AxisMovingInsideSettlePct`应出现释放证据。只有二维`Settled`不能证明静止轴已停止输出。

r42起分析移动不平滑时同时查看`P95PredictionLeadDeltaPx`、`P95PredictionLeadJerkPx`、
`PredictionLeadCappedPct`和`PredictionInterruptionCount`。前两项只度量连续激活段内的幅值连续性；
中断代表停止、反向或门控撤销，不能通过保留陈旧提前量来换取更低的表面跳变量。

r43起CSV增加`PredictionStationarySuppressed`，分析器汇总为
`PredictionStationarySuppressedPct`。单向持续运动中该值应接近0；明确停止段达到确认时间后应为1且
提前量归零。中断发生而自运动、高频往返、高速和停止抑制字段均为0时，才继续检查方向反转或
回归窗口失效，不得把所有`PredictionOffset=0`笼统归因于停止。
