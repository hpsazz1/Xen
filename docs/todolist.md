# 待办清单

## P0 — 阻塞核心功能

- [ ] 同步源码迁移后的 CMake 路径：`log/`、`detector/` → `Xen/log/`、`Xen/detector/`
- [ ] 建立 Log 与 Detector 单元测试目标，覆盖初始化、解码、坐标还原和 NMS
- [ ] 用真实 ONNX 输出验证 YOLOv5/v8/v10 的张量布局和坐标语义
- [ ] 校验 Detector 空图、非三通道图和动态输入尺寸，失败时安全返回
- [ ] EP 加载失败时自动降级（TRT → CUDA → CPU）
- [ ] Detector 动态高宽与非法输入形状处理（静态输入尺寸已从 ONNX 读取）
- [ ] Desktop Duplication 本地采集实现（先打通无新增依赖的最小闭环）
- [ ] UDP Capture 接收端实现（OpenCV MJPEG 解码 + 最新帧队列）
- [ ] NDI Capture 接收端实现（NDI SDK 集成 + mDNS 发现）
- [ ] IMouseController 抽象基类 + MouseDeviceFactory 工厂
- [ ] KMBOX NET 实现（UDP + kmNet 协议）
- [ ] IntentManager 意图管理器（轨迹拟人化）
- [ ] KeyboardListener 全局热键监听

## P1 — 重要但不阻塞

- [ ] 使用 RAII 对象替换 `Session` 中的裸 `new/delete`
- [ ] 隔离 `log.h` 对 spdlog 公有头文件的直接依赖
- [ ] 评估将多模块轮转日志文件从 `detector.log` 改为通用名称
- [ ] 在 Log 与 Session 边界统一捕获文件系统、格式化和 ORT Provider 配置异常
- [ ] Detector 模型热重载（运行中替换 ONNX 文件）
- [ ] 运行时通过配置 INI 切换日志级别
- [ ] 按模块设置不同日志级别
- [ ] Overlay 热键显示最近日志（环形缓冲区可视化）
- [ ] 崩溃处理入口绑定 → 触发 `Log::dump_ring_buffer()`
- [ ] Aim Control 模块实现
- [ ] GPU 前处理（CUDA kernel 或 OpenCV CUDA）
- [ ] 异步推理 pipeline（截图线程 ↔ 推理线程分离）
- [ ] KMBOX A 实现（USB HID）
- [ ] 飞易来实现（msdk.dll）
- [ ] Win32 API 实现（系统鼠标兜底）

## P2 — 长期优化

- [ ] 实例分割支持（YOLOv8-seg）
- [ ] 姿态估计支持（YOLOv8-pose）
- [ ] 旋转目标检测 (OBB) 支持
- [ ] OpenVINO EP 支持（Intel GPU）
- [ ] 去 OpenCV 依赖（纯 C++ LetterBox 替代）
- [ ] 多 stream 并发推理（批量多帧）
- [ ] GHub / RzCtrl / Makcu 实现
- [ ] Arduino 串口实现
- [ ] LCD 图片推送（KMBOX NET 屏幕显示检测框）

## 文档待办

- [x] Capture 模块设计文档
- [ ] Mouse + Keyboard 模块设计文档
- [ ] Aim Control 模块设计文档
- [ ] 单元测试架构文档
