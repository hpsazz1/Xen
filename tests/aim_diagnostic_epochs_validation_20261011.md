# D28诊断跨代与缺帧分段

基线416a0c656646c6007378052d0fbfc1c61083818e；分支fix/aim-diagnostic-epochs-20261011。

新增夹具使用真实BoundedSampleRing<RuntimePipelineSample,4096>：先排出epoch A的正向样本，再推入epoch B单帧reset和4096个负向样本，恰好覆盖reset。真实DebugReport保存4097样本及runtime_samples_dropped=1，后续每帧仍有B的持久epoch。实际PowerShell汇总旧实现跨两代错误统计一次方向反转；连同epoch变化、同epoch缺帧、序号重启、字段缺席变化及大整数epoch的目标/方向控制，共十个断言失败。

共享连续性键按无损uint64解析序号和持久epoch；提供但无效的身份拒绝。epoch改变、序号不是连续+1、身份字段可用性改变时，两消费者清空跨帧计数状态。epoch使用规范整数字符串比较，序号差用Decimal避免uint64末端溢出或浮点舍入。全程无epoch的旧报告仍可读，按已有序号作保守分段；旧版无序号的Aim报告保留原格式行为。

PowerShell5与7各42断言通过：实际ring/报告、九种连续性场景、十二种非法身份、跨代目标状态/预测状态；同代连续方向反转/目标切换、旧格式、epoch0、uint64最大相邻序号控制保持。三CTest共6.32秒：aim_diagnostic_epoch_tests、debug_tests、runtime_tests；既有test_aim_report.ps1也通过，文档19文件195链接0错误。

只是诊断统计分段；逐帧几何/输出合同判断、错误及丢帧计数不隐藏。人工验收drop=0门槛所在invoke_aim_manual_acceptance.ps1未修改。夹具仍明确drop=1，不具备人工验收资格。新夹具是合成RuntimePipelineSample经过实际ring/DebugReport/PS，不是完整Runtime/Detector/真实设备运行。

正式Release、完整SHA、远端/CI及夹具日志见本地docs/evidence/XEN-AIM-DIAGNOSTIC-EPOCHS-20261011-001/RESULT.md。无Xen/UI启动、截图、真实采集、游戏/系统输入或受限候选入口。未修改Aim控制算法和Provider/禁输出/武装/保持键/End/UNKNOWN边界。
