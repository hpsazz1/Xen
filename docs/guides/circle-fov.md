# 圆形 FOV

## 推荐配置

```ini
circle_fov_enabled = true
circle_fov_radius_percent = 100
circle_fov_show_preview = true
game_overlay_draw_circle_fov = true
```

使用 GUI 或 overlay 来可视化圆形。降低 `circle_fov_radius_percent` 可使活动区域变小。

## 绘制圆形会增加很多开销吗？

在 GUI 预览或游戏 overlay 中绘制圆形相比采集和推理的开销很小。overlay 绘制仅在 GUI 或 overlay 实际打开并配置为显示时才会产生影响。

相关文档：

- [Overlay 和 GUI 行为](overlay.md)
- [配置指南](../config.md)
