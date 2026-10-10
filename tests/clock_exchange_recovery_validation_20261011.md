# D03 单次UTC异常恢复验证

基线824e3e6e0a01117e536ab23058db1d98887d96e1，分支fix/clock-exchange-recovery-20261011。

原Server::serve_once在UTC回拨/无效时将响应序列化拒绝升级为false；正式clock_source/main在false后退出。本次内部接缝仅允许在尚未打开的Server实例替换UTC读取函数，生产默认system_clock不变；没有全局生产钩子、CLI选项或系统时间修改。

实际本机UDP收发使用生产Server和协议编解码；故障前健康请求取得session，第二次注入两次UTC读数，第三次发送健康请求。测试按正式CLI语义在serve_once=false后停止服务，不绕过该失败再调用。

| 单次交换 | 修前响应/存活/恢复 | 修后响应/存活/恢复 |
|---|---|---|
| UTC回拨 | 0/0/0 | 0/1/1 |
| 接收UTC为0 | 0/0/0 | 0/1/1 |
| 发送UTC为0 | 0/0/0 | 0/1/1 |
| 正向UTC | 1/1/1 | 1/1/1 |
| 相等UTC | 1/1/1 | 1/1/1 |

坏交换被丢弃，后续响应保留同一session；没有钳位或伪造时间戳，合法响应逐字段核对。尚未打开的服务仍返回失败，打开后不能修改测试时钟。原协议、AffineMapper过期与真实Client/Server健康环回保持。

clock_sync_tests与clock_quality_measurement_numeric_oracle共2项CTest通过（1.52秒），相应Release测试与XenClockSource构建通过；文档19文件195链接0错误。干净提交后的正式应用Release、产物身份与远端核对另记录在本地docs/evidence/XEN-CLOCK-EXCHANGE-RECOVERY-20261011-001/RESULT.md。只验证生产Server而非注入整个正式CLI进程，没有NDI图像、采集、设备/系统输入、UI或跨机实验。
