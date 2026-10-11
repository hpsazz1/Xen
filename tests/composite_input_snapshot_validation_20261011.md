# Composite producer/binder 输入字节快照

基线 `4715084a95007dd983d91045cb72498ed94a953d`，隔离分支
`fix/composite-input-snapshots-20261011`。D07 的 command-report 原已冻结；
此次核对其余 JSON 输入的实际解析、文件摘要与最终发布关系。

新回归调用生产 main，透明包装 pathlib 的真实读取：先读并返回合法 A，
再立即把同一测试文件替换成不同的 B。没有伪造 verifier、哈希、解析或校验成功。
控制组仍启动实际原生 CompositeSeal 和 producer/binder/evaluator 进程。
四个原错误发布已复现：producer schedule 和 binder 的 plan/capture/command
都用 A 生成内容，却把 B 的文件摘要写入正式结果。另两个 producer plan/safety
原来安全拒绝；它们是相邻快照一致性控制，不计为两个错误接受漏洞。

producer 对 plan/schedule/safety 单次读取后同时解析并计算文件摘要；
binder 对三个输入同样绑定原始 bytes。语义摘要与文件摘要仍区分，既有 schema、
冻结计划、来源时钟、审核、denylist 与输出冲突校验保持。
六种替换场景现在都只读取目标一次，结果与各自无替换控制组完全相同。
发生变化的当前磁盘路径不再重新用于声称已消费的数据身份。

这是生产 Python main 的确定性 I/O 边界注入，不是依赖调度巧合的并发进程竞态；
原始读取及实际文件替换均真实发生。对输入的首次读取不保证多文件同时快照；
跨文件关联仍由原有绑定检查拒绝矛盾。执行中替换 verifier 可执行文件或自身脚本
的独立身份问题不在本次证明内，也未操作权限或凭据。

固定提交 Release、七项相关 CTest、源码/二进制身份、普通推送、远端和实际CI元数据
见本地 `docs/evidence/XEN-ROUND1-CLOSEOUT-20261011-001/COMPOSITE_SNAPSHOT_RESULT.md`。
未运行真实设备、采集、原生 UI 或任何输出执行器。

首次固定 `281e05f` 为6/7通过；新测试在默认 TEMP 的 ADMINI~1 短路径下
未命中生产 resolve 后的路径，屏障断言正确报错。修正测试先规范化同一目标，
并要求准确一次读取；保留失败日志，不把夹具问题计为生产缺陷。
