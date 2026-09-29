# README 界面图片

这些图片于 2026-09-29 使用当前生产 Overlay 重新生成，统一白色主题，窗口1000×820；不是历史截图或界面设计稿。

- UI 源码基线：ad7aa2615a66b424c27dacc192e4e050a3d262ec。
- 生成入口：tests/model_workspace_ui_preview.cpp，对应已构建的 model_workspace_ui_preview 目标。
- 不传 --dark 或 --minimum，保持统一主题及窗口尺寸。
- collection.png：采集与批量审核。
- aim.png：瞄准与阵营筛选。
- training.png：训练环境及数据设置。

预览入口渲染生产UI，使用合成状态和示例路径，不创建Runtime或输入设备、不执行训练。截图中的示例值不是推荐参数或实战结果。原图直接使用，未进行图像编辑。
