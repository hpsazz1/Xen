# Sender采集返回期间取消回归

生产run_sender在grab返回有效帧后重新检查取消；已观察到Ctrl+C/Break时，不编码或发送这张新帧，已完成样本保留signal停止原因。采集错误仍exit4，零帧报告仍拒绝发布并exit6。

sender_cancel_tests仅替换ICapture factory并直接调用生产handler，不运行wmain、不注册系统控制事件，不启动DXGI/NDI、游戏、设备或GPU。生产XudpSender真实JPEG与数据报仅发127.0.0.1，接收端保持绑定并核对报告数据报数。十轮覆盖两个控制事件的首帧/第二帧取消及恢复，无帧、错误同时取消、零帧报告和最终恢复。原生产实现出现九个失败断言，修改后必须全部通过。

Microsoft说明AcquireNextFrame等待不能取消，应使用有限超时并检查终止条件：[AcquireNextFrame](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgioutputduplication-acquirenextframe)。保持已有4ms参数，未引入线程或第三方代码。系统关闭事件可能在handler返回后终止进程：[HandlerRoutine](https://learn.microsoft.com/en-us/windows/console/handlerroutine)，因此直接handler调用不证明OS关闭报告必达，也不证明实际采集停止时延。

本测试不覆盖检查之后才到达的信号、已进入JPEG/分片发送的撤回、真实Provider故障恢复、永久SDK阻塞或断电耐久性。既有原子报告拒绝覆盖/显式close检查及UNKNOWN边界保持，不新增常驻规则。
