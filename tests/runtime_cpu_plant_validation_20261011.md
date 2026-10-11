# 完整Runtime CPU合成闭环回归

使用实际Runtime启动、队列、CPU Detector预处理/ONNX推理/后处理、Aim、最终派发与反馈；仅采集和鼠标由现有接口替换，不新增生产hook。自有350字节模型把32×32灰度映射到检测框中心；内存鼠标按每count 0.15源像素改变下一帧灰度，并独立核对实际Detector框。不是预先填写Detection，也不是总是成功的零输出鼠标。

覆盖未武装、12帧非零反馈、失焦后的第一帧/持续停发、恢复焦点但保持旧按键、健康后松按恢复、两帧之间失焦又恢复的持久epoch、急停第一帧、同实例停止/重启不自动武装、显式武装后的后端失败锁存急停。每帧核对像素独立oracle，协议ACK和物理E字段始终不可用。新接管允许平滑/整数化导致首帧无命令，连续四个新帧必须恢复派发。

源证据使用真实SourceContextClient和真实认证编码，只收发127.0.0.1合成报文，不查询真实焦点。应用、屏幕采集、SendInput、KMBOX/MAKCU、游戏、原生UI及GPU均不运行；构建部署的DLL不等于执行对应Provider。明确关闭所有辅助动作与GSI，提供已READY的MemoryMouse使Runtime不创建设备后端。

模型可由Python标准库重建：`python -B tests/fixtures/generate_runtime_cpu_plant_fixture.py`。
按[ONNX ReduceMean-13](https://onnx.ai/onnx/operators/onnx__ReduceMean.html#reducemean-13)全维均值和[Mul-13](https://onnx.ai/onnx/operators/onnx__Mul.html#mul-13)广播契约构造自有权重，沿用仓库wire编码helper，不复制上游实现或下载模型。模型SHA-256为119deb0d42fc8777f1e890f288145d8cd2d72ac540d52ea56d4030e065d9aaa8。

本批没有生产缺陷修复。首次失败来自测试要求重新接管第一帧必须出命令，诊断对照显示初次接管同样无命令；另有新增诊断引用不存在的trusted字段造成一次编译失败，已改为available。原日志保留，不计作生产反例。

Release目标runtime_cpu_plant_tests，CTest同名，超时30秒，无外部模型要求。相关消费者验证为runtime_aim_frame_tests与runtime_start_failure_tests。该证据不证明模型精度、实际游戏效果、性能改进、所有在途交错、最终检查之后的撤销或真实物理E；控制参数和Provider选择没有改动。
