# D16-E1 独立来源许可验证

基线366339cc7306e8e8e2df251be86af080f1b6c3e4，隔离分支fix/recoil-source-epoch-20261011。
本批补齐独立CLI、App Worker和WallRun；前一提交的Counterpulse与连续就绪窗口结论保持。

## 实际路径与红灯

先把CLI/App既有来源判断原样抽到生产内部适配器并接回调用点，再以相同适配器驱动真实
RecoilWorker和run_wall_capture。合成Foreground/Background由实际SourceContext Evidence
接受，两个非零事件40/80ms由实际Loader与Controller处理；内存设备第一次move返回ACK前
触发同session Background→Foreground，健康对照仅Foreground且序号每次加7。

旧逻辑四个Worker案例均moves=2、COMPLETED；WallRun在实际on_ready后发生同样恢复，
健康和断点两例均DOWN=1。WallRun测量关闭，capture_factory调用数必须为0，仍检查正常清理。
不运行有效授权的CLI进程，因为该入口会创建物理设备；测试覆盖其实际生产来源适配器和Worker。
未宣称执行完整App网络服务链或真实输入。

## 修复与新增发现

一次运行用短mutex临界区原子绑定(session, source epoch, GSI epoch)，optional区分未绑定与
合法代次0；发现变化后锁存撤销，旧快照晚到也不能恢复许可。Worker输入使用同一个focus快照
的control_safety_epoch。CLI保留稳定session观察，App保持原武器条件并共用绑定对象；对象由
Worker回调持有，异常清理期间仍有效。锁内不等待设备、网络或线程退出。

第一次修复后，两个Worker已从2次move降为1次，但实际批次仍误记EXHAUSTED/COMPLETED：
Controller先因trust_changed取消，随后“已执行且不再活动”分支覆盖了原因。保留实际终态后，
断点案例必须是1次move、CONTEXT预算与CONTEXT批次；健康案例保持2次move、COMPLETED。
WallRun断点必须DOWN=0且不成功，健康仍DOWN=1并成功。回归还检查代次0、GSI代次变化、
恢复后的旧快照与两个并发完整组合只能首次绑定一个，冲突不能复活。

夹具最初遗漏WallRun必需的trigger key，运行红灯前已纠正；诊断代码初次编译缺少事件类型
头文件，已补齐。失败构建后的旧二进制输出不计新增证据。没有放宽ACK、相位或预算。

十项相关CTest通过28.66秒：recoil、recoil_worker、recoil_calibration、recoil_calibration_cli、
source_context、debug_source_epoch、debug_runner、debug_session、recoil_source_epoch、wall_capture_run。
提交后的官方Release及远端身份记录在本地
docs/evidence/XEN-DEBUG-SOURCE-EPOCH-20261011-001/RECOIL_RESULT.md。
原始日志保留红灯、只阻断命令但终态错误的中间结果及最终回归；不以专项测试代替多Provider、
新UI或物理闭环验收。候选参数保存/采用的受限入口没有修改。无新增常驻规则。
