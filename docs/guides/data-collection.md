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
