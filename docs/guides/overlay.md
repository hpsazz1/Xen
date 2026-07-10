# Overlay 和 GUI 行为

有两个独立的显示概念：

- GUI/调试预览窗口，主要由 `show_window` 控制。
- 游戏 overlay，由 `game_overlay_enabled` 和相关 overlay 键控制。

GUI 预览可能需要 CPU 副本，因为它必须显示采集到的像素。游戏 overlay 可以绘制框、未来位置、帧轮廓、圆形 FOV 和可选图标。

有用的 overlay 配置键：

```ini
overlay_exclude_from_capture = true
game_overlay_enabled = false
game_overlay_compensate_latency = true
game_overlay_draw_circle_fov = true
```

如果你在测量性能，请在预览关闭和打开时分别测试，以便观察 CPU 副本需求的变化。

## 延迟补偿

应用会补偿帧采集、推理和显示之间的延迟。这使瞄准和 overlay 绘制更接近当前屏幕上的内容，而不是目标在帧采集时的位置。

仅在游戏 overlay 框显示过度校正或校正不足时使用此键：

```ini
game_overlay_compensate_latency = false
```

此设置会影响视觉 overlay 校正。它不会切换输入方法，也不会创建后备控制路径。

相关文档：

- [采集诊断](capture-diagnostics.md)
- [圆形 FOV](circle-fov.md)
