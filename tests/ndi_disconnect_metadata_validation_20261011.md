# D21已观测NDI断线后的坐标缓存

基线aab891dd9e425ca344e6c80e0f295d725ae22178，分支fix/ndi-disconnect-metadata-20261011。使用既有INdiCaptureSystem注入SDK连接计数和合成像素，运行真实接收线程、XML解析、几何校验、帧池与公开grab；不调用真实NDI SDK、mDNS、时钟网络或真实采集。

原实现两反例均失败：实际250ms生产轮询依次观察1→0→1后，无新metadata的视频仍继承断线前ROI(100,200)，required错误接收，optional错误标记source_mapping_verified=true。新metadata控制正常。

修复仅在已连接到零连接的观测边界清除last_metadata。两反例修复为required拒绝并INVALID_CONFIG、optional保留合法配置fallback(1200,640)且verified=false；两种模式的新metadata(300,400)均接受并可由后续同连接帧复用。同连接原缓存控制保持。没有重建receiver，不修改receiver_generation既有实例代次语义。

三项CTest（ndi_capture_contract_tests、capture_evidence_tests、runtime_aim_frame_tests）通过5.71秒，含既有内部重连/代次、坏视频期限、时间映射、缓存槽复用及源映射证据。文档19文件195链接0错误。正式Release、完整提交、产物身份及远端/CI见本地docs/evidence/XEN-NDI-DISCONNECT-METADATA-20261011-001/RESULT.md。

范围限于实际观测的断线；不声称识别250ms轮询间未观测的断线或远端session身份。Provider、输出安全、UNKNOWN与数据审核合同不变，没有UI、设备输入或受限候选入口操作。

依据：[NDI接收器官方说明](https://docs.ndi.video/all/developing-with-ndi/sdk/ndi-recv)、[metadata来源说明](https://docs.ndi.video/all/developing-with-ndi/ndi-metadata/metadata-sources)。SDK提供连接和帧信息；清除Xen坐标缓存是本项目根据反例实施的应用层边界，并非假设SDK代为清理。
