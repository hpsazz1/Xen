# D22停止超时与清单提交一致性

基线7cba49e63948317b637ee9673a325c450efd44c2，分支fix/input-training-timeout-commit-20261011。真实Session、publish_file与visit_archive，唯一临时目录中的空档案/两合成事件；内部钩子仅暂停最终提交边界，不替换写盘、重命名或读档。

原实现红例：达到LIMIT后，worker通过最后timed_out检查但尚未发布manifest；停止等待20ms返回STOP_TIMEOUT，worker恢复后仍发布LIMIT清单，visit_archive接受。健康LIMIT控制通过。这个有效格式反例不表示LIMIT具备完整人工验收资格。

修复使用原子状态裁决超时与最终提交。manifest.pending实际写入、flush和close后才争取提交；超时先发生则保留未完成.pending，禁止正式manifest。提交先准入但超出停止等待预算时，公开FINALIZING（归档提交中），继续在后台完成，不冒充STOPPED或不可再提交的STOP_TIMEOUT。真正IO错误仍FAILED；状态快照在准入前准备好，轮询不重复分配它。没有将磁盘IO放进停止等待的completion_mutex。

FINALIZING追加到枚举末尾，既有状态数值和归档格式不变；正式清单仍只允许STOPPED/LIMIT。界面/Runtime将提交中视为busy；调试与人工收尾按未确认处理，不能开始新设备任务或自动清除UNKNOWN。

七个专项场景：健康LIMIT、超时胜出、提交胜出、提交准入后目标冲突失败，以及普通STOPPED的健康/超时胜出/提交胜出。后者由串行生命周期owner发起实际stop，主线程仅观测和释放写盘边界。20或100ms预算下，阻塞的worker不阻塞stop超过500ms测试上限；未完成时拒绝新会话和读档，最终状态/样本数符合真实结果，冲突目标保留。

最终九项CTest通过3.70秒：input_training_commit_tests、input_training_tests、input_training_start_failure_tests、runtime_input_training_tests、input_training_panel_tests、debug_session_tests、debug_runner_tests、auto_stop_manual_finalize_tests、auto_stop_training_evaluation_tests。随后仅扩展普通STOPPED控制并重跑专项通过。UI检查仅为无窗口ImGui绘制数据与状态边界，不是桌面点击或截图验收。文档19文件195链接0错误。

正式Release、完整SHA、产物身份与远端/CI见本地docs/evidence/XEN-INPUT-TIMEOUT-COMMIT-20261011-001/RESULT.md。无真实采集、设备/系统输入、UI操作或受限候选入口，既有Provider/禁输出/保持键/End/UNKNOWN/审核边界保留。仍不承诺断电恢复或任意OS调度下的硬实时截止。

依据：[C++原子比较交换](https://eel.is/c++draft/atomics.types.operations)、[条件变量等待](https://eel.is/c++draft/thread.condition.condvar)。本实现只用原子状态决定哪个操作先获准，成功归档仍取决于实际文件发布；没有将准入当成IO成功。
