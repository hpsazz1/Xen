# 鼠标基准末对取消与报告关闭

基线 `003c67ade5153ab9649eaaeb0cb097087e14d0a1`，隔离分支
`fix/mouse-benchmark-close-20261011`。本次只有两个生产缺陷。

报告旧 writer 在 flush 后缓存 good，close 后直接返回缓存；真实 filebuf unshift
关闭故障仍发布最终报告，且占用同路径使下一轮不能恢复。三个失败断言是一处缺陷。
现在检查 close 后 failbit，沿用既有自有 pending 清理及禁止覆盖规则；关闭故障后
同路径成功重试，已有正式报告在拒绝覆盖后逐字节一致。测试只调用报告 writer，
MAKCU/COM8 是报告元数据，不调用 Mouse factory 或串口；证据保持 fake、ACK、
physical_effect_observed=false。关闭注入辅助代码从现有报告测试提取，共享测试
同时回归，不替换生产文件流。未证明断电持久性或所有OS存储故障。

末对取消在实际生产 run 上复现：127.0.0.1 UDP peer 在最后一对的正向或反向
有效 ACK 返回前请求取消，旧代码两次都返回成功、标记 complete 并发布报告，
共六个失败断言。现在完成已开始的正反命令对及 close 后重新检查取消，保留
已确认计数和样本、READY 最终状态，返回人工中止且不发布报告。两种取消各有
同路径健康新轮次恢复，严格检查六个数据包以及正反命令 x/y 净和为零。

只用进程内输出所有权和回环假设备；未运行实际工具输出命令、游戏输入、真实
KMBOX/MAKCU，也不把协议补偿等同真实物理效果。最后检查之后才到达的取消、
实际OS控制台信号、硬件close阻塞及真实物理归零不在本次证明内。

固定SHA Release应用、MouseBenchmark工具及四个专项测试、JUnit、二进制身份、
远端及实际CI元数据在本地
`docs/evidence/XEN-ROUND1-CLOSEOUT-20261011-001/MOUSE_BENCHMARK_RESULT.md`。
