# 来源信任断点回归快照

本轮基线：`3a38a8f5fb138c8e98b564f337e77cc28dd6ff4b`。D14说明更正在
`6da1beaa66a5a1230a9120a1272933568ae8a23c` 单独提交；本文对应其后的D15/D16实现。
这是本地NVIDIA配置Release专项证据，不代表全部Provider、完整设备链或实战验收。

## D15：共享回调与完成资格不在同一状态所有者

旧Runtime焦点回调更新共享`previous_session`后，尚未向AutoStop提交撤销，另一个Trigger调用
可以读到“同会话”并取得旧完成编号。测试提取了原生产适配入口，使用两个真实worker和内存记录
设备，在适配返回和owner提交之间加有界屏障。旧版日志：`old_id=1, concurrent_id=1`。

Runtime现在只提供不可变来源快照。AutoStop在既有mutex下更新来源身份、取消代际、待处理和
完成编号、目标期限及恢复状态；不再在Runtime共享可变闭包中消费会话断点。旧的无身份bool
调用入口仍兼容，生产Runtime使用带来源身份的重载。读取不在owner锁下调用外部回调。

新回归包含会话S1→S2、同会话持久断点两种交错；两者均为
`old_id=1, concurrent_id=0, new_id=2, button_down=0`。屏障超时不能算通过；
必须先证明真实Trigger在健康前置下取得旧编号，再验证断点撤销和健康恢复的新编号。

## D16：已接受断点被最新健康快照覆盖

真实`Evidence::begin/accept/snapshot`产生合法nonce、递增序号的事实，消费者只看前后健康
快照，跳过失焦、UNKNOWN、观察到的过期、invalidate、会话往返，以及无消费者轮询的TTL缺口。
旧Aim gate六例均错误继续许可。接受器现在保留`control_safety_epoch`，新报文接受前也检查
旧TTL；无效报文和普通健康序号跳跃不增加代际，重复过期/停止不重复增加。代际溢出保持不可用。

Aim、训练采样时间边界、AutoStop、Trigger和Recoil均消费断点。Trigger/Recoil同时在控制器
和最后派发检查消费持久代际，不能依赖一次性的false回调恰好被正确层级读取。持续持键等待
健康释放，旧按钮正常清理，新健康释放可恢复；AutoStop保留原清理后完整重制动的自动核验合同。
不声称发现来源服务从未观察到的瞬时切出。

## 验证

- Release构建：`xen_app`及本次直接消费者，无新增依赖或Provider改动。
- 第一组6项：source_context、runtime_aim_frame、auto_stop_worker、weapon_stop_integration、
  trigger_worker、recoil_worker，通过137.23秒。
- 补充对照后13项：source_context、runtime_aim_frame、weapon_stop_integration、trigger、recoil、
  aim_recoil_integration、hud_stop_worker、hud_weapon_stop_integration、hud_aim_random_integration、
  movement、runtime、input_safety、config，通过66.56秒。
- Collector原生命周期9项（`^collection_lifecycle_`），真实CPU模型与假采集/禁输出设备，通过2.61秒。
- 去重25个CTest；公开文档19文件195链接0错误，`git diff --check`通过。
- 关键复现入口：`weapon_stop_integration_tests --source-session`、`runtime_aim_frame_tests`。
- 新控制器测试首次恢复断言未等待原射击冷却，同时TTL用例被原观察期限遮盖；最终明确保留
  原冷却并等待新时刻、使用足够长观察期限。该夹具调整不算生产缺陷，也没有修改原冷却合同。

原始红绿日志、测试原始输出、构建日志和产物身份保存在本地
`docs/evidence/XEN-SOURCE-TRUST-20261011-001/`，按仓库规则不强制加入ignored文档。
无真实采集、系统/游戏输入或校准输出；未合并main、发布、部署或修改安全权限。

后续独立项：D27原生命周期准入已修，但D27-E1旧检测帧跨reload进入新Collector会话尚待真实
detect→offer屏障复现；本轮9项生命周期绿灯不能关闭它。之后是D04/D07、D06/D11正式证据链。
D12/E1起点代码经独立复审关闭；固定D与实际物理延迟失配属于合同模型限制，不算未修代码bug。
