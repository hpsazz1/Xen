# D19 XUDP解码后提交流切换

基线e13250450addf44b2cec955eddfbb87f2db72d79，分支fix/xudp-validated-stream-20261011。实际生产XUDP接收器、Winsock本机127.0.0.1及OpenCV imencode/imdecode；输入仅合成色块或无效字节，不调用NDI发现、屏幕采集或任何输入设备。

三个原实现红例：先发布有效A1，再发送协议/hash完整的新流B1（非JPEG、编码尺寸与头不符、实际64x64而配置ROI越界），随后A2都无法继续发布。独立合法B控制通过。修复将完整协议帧暂存，解码和几何校验成功后由accept_frame提交接收计数与新流切换，之后才申请发布池槽；池满丢帧不否定已验证新流。

三个反例修复后A2恢复，invalid计数可见；每组随后合法B2均切换，迟到A3被拒绝，B3继续。协议层保留已有乱序、缺片、重复冲突、hash、序号和三槽总计不超过24MiB的检查。新增生命周期控制覆盖待确认状态过期、重复确认、reset、活动流无效帧重放拒绝及下一有效帧缺口计数。协议fixture的非JPEG payload显式确认仅验证协议状态，真实解码反例来自上述接收器回环。

两个相关CTest（xudp_stream_validation_tests、sender_tests）最终通过0.89秒，文档19文件195链接0错误。正式Release、完整提交、产物身份及远端/CI见本地docs/evidence/XEN-XUDP-VALIDATED-STREAM-20261011-001/RESULT.md。没有进行UI、跨机、物理设备或真实模型验证。没有改变协议字节格式、Provider选择、输入安全或数据审核边界。

source_received_frames现在只在解码方接受后增加；当前流完整但拒绝的帧保留序号高水位，后续有效帧按缺口计入transport_dropped_frames。不得将这个更准确的计数变化解释为吞吐提升。

接口依据：[OpenCV imdecode官方文档](https://docs.opencv.org/4.13.0/d4/da8/group__imgcodecs.html)明确无效缓冲可返回空Mat，带dst的重载失败时可以保留旧dst缓冲；生产仍检查本次返回值，未放松尺寸或几何验证。网页4.x当前重定向4.13；实测链接本机OpenCV4.14，以运行结果为准。
