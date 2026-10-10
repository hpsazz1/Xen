# D31 GSI accept 暂态错误恢复

基线 89e5553d17548be2f0e4508069bd3e6b0c6c7f12，分支 fix/gsi-accept-recovery-20261011。

测试编译原始 weapon.cpp，仅将 accept 调用指向窄包装：真实回环连接到达后消费并关闭该夹具连接，返回一次指定错误；WOULDBLOCK 返回保留排队连接。实际 Receiver 线程、HTTP 收发、解析和 GsiState 均不替换。全部地址为 127.0.0.1，自造 JSON，不读取游戏或使用设备、UI。

原代码在一次 WSAECONNRESET 或 WSAEWOULDBLOCK 后结束整个接收线程，状态被 reset，后续正常请求无法处理。两条红例已转绿；修复仅对这两个返回重新进入带停止检查的循环，其余错误继续退出。五组控制含正常、RESET、WOULDBLOCK、NOTSOCK、NETDOWN。恢复后四类 epoch 保持各自值、同秒期限不刷新、重复与倒序包不能推进状态、过期后仍保留时间戳高水位。未合并 epoch 或放宽 payload/peer 边界。

依据：[Microsoft accept 合同](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-accept)。RESET 描述的是尚未接受的单连接结束，不能据此认定监听服务失败。错误为确定性注入的真实 API 返回接缝，不宣称操作系统自然触发了这些错误。

验证：gsi_accept_recovery_tests 3.08 秒、weapon_tests 0.01 秒通过；weapon_stop_integration_tests 初次运行在原地按住用例抬键原因断言失败，保留原日志。该用例不调用 GsiReceiver。新增具体失败原因输出与 --stationary-owner 单项入口后，完整组合通过 17.43 秒，单项有界诊断 30/30 通过，但未重现并解释最初失败，列为 F01 待查，不把重跑通过当根因解决。原断言与安全门禁未弱化。

首个测试夹具遗漏 Content-Type，被原协议校验拒绝；修正后才取得上述正式红例，夹具失败日志不计缺陷证据。公共文档 19 文件/195 链接/0 错误。正式 Release、产物身份、提交、远端和 CI 见 docs/evidence/XEN-GSI-ACCEPT-RECOVERY-20261011-001/RESULT.md。
