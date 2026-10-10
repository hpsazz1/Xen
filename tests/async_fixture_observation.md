# 异步夹具的观察边界

`kmbox_button_tests` 仅使用 `127.0.0.1` 临时端口；`movement_tests` 仅使用内存 Fake。
二者执行真实生产控制器/Worker，不连接真实设备，不启动原生 UI。

## 按钮关闭

ACK 超时不表示夹具收包线程已经记录结果。UNKNOWN 场景先等前序两包被记录，随后调用
生产 `close()`，再等关闭释放包和接收线程本轮观察结束、join，最后检查原来的精确次数、
释放内容与连续序号。生产 25 ms ACK 预算不变；夹具的每次观察等待独立限定为 500 ms，
沿用本文件的观察预算。接收线程沿用 20 ms socket 超时，关闭生产端后完成一次新的空队列
观察即结束。该有限窗口不证明 UDP 今后永无迟到或重复；monitor-disable 也不是排空屏障。

夹具保留全部收包记录，不去重、不在收到首个正确包时停止。单向 monitor-disable 已无
ACK 消费者，夹具仅记录它；向关闭端口回复会引出 `WSAECONNRESET`。其他接收错误仍失败。

```powershell
ctest --test-dir build/nvidia -C Release -R '^kmbox_(button|delayed_close_observer|duplicate_close_observer)_tests$' --output-on-failure -j 1
```

- `delayed_close_observer` 在收到关闭释放包后、记录前暂停接收线程。生产关闭仍按原预算返回；
  老的立即计数断言确定性失败，完成同步后保持精确断言通过。
- `duplicate_close_observer` 通过另一真实回环 socket 添加重复释放，确认同一精确判据拒绝它。
  这项负对照检查“拒绝重复包”本身，不能当作生产重复发送的证据。
- 失败输出包含包类型、序号、实际接收和记录时刻、close 时线、receipt、fault、cleanup、错误。

## 身法超时

原场景、原谓词、3 秒等待与 1 ms 轮询保持不变。超时时输出调用位置、循环参数、谓词文本、
Worker 快照、最后 16 批输入交付及其年龄、最后 8 条输入与 Ctrl 事件历史。普通专项不武装屏障。

```powershell
ctest --test-dir build/nvidia -C Release -R '^movement_tests$' --output-on-failure -j 1
.\build\nvidia\Release\movement_tests.exe --diagnostic-observation fresh
```

独立诊断模式 `fresh`、`baseline` 应通过；`stale`、`miss-pending`、`miss-held` **预期退出 1**，
不作为普通 CTest 通过项登记。它们分别控制报告读取时机和观察者开始等待的时机，验证日志能
区别过期未启动与已完成后错过瞬态状态。生产 100 ms 新鲜度、80 ms 延迟和 15 ms 持有窗口不变。
异常退出先释放测试门，再回收 Worker。

历史 b212b2d 的通用超时没有场景/谓词记录，无法由这些人为对照唯一归因。保留该问题，
后续实际复现时按新增现场定位；不以原命令偶尔通过、增加等待预算或人为红例冒充已修复。

同步语义参考：[Microsoft sendto](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-sendto)、
[条件变量](https://learn.microsoft.com/en-us/cpp/standard-library/condition-variable-class?view=msvc-170)、
[RFC 8085 §3.3](https://www.rfc-editor.org/rfc/rfc8085.html#section-3.3)。
