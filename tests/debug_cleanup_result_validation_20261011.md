# D30 索引保存失败保留清理事实

基线 0e8c58836ffdc3ab012a24927d3a193c5db8fa94，分支 fix/debug-cleanup-result-20261011。

实际 Session→run_debug→execute_counterpulse→write_document 路径使用记录型内存设备和本地回环合成焦点响应。测试链接替换 CounterpulseHud 构造为立即失败，使用原有显示不可用回退；没有窗口、桌面操作、真实采集或硬件/系统输入。结果索引的实际 ofstream/rename 通过本夹具拥有的目录冲突失败，不替换 writer 或 Session 状态机。

原实现三条红例：按钮和键盘均 ACK 清理后，result-index.json.partial 无法创建、result-index.json 无法发布、以及用户取消后索引无法创建，均被无条件覆盖为 CLEANUP_UNKNOWN 并阻止新准备。修改给索引保存异常携带已验证的清理事实，外层只在清理未确认时锁存 UNKNOWN；不清除任何已有未知状态。索引失败仍 FAILED 或 CANCELED，撤销重复执行许可，不能显示 COMPLETED。未确认 ACK 对照仍 CLEANUP_UNKNOWN 且拒绝新准备。

六组实际入口全部通过：正常完成、partial 冲突、正式索引冲突、取消+partial 冲突、未知 ACK+partial 冲突、未知 ACK 无冲突。只在已确认清理组接受显式重新准备。首轮夹具正常对照遇 Windows 默认睡眠粒度导致 RELEASE_DEADLINE_MISSED，日志保留且未计通过；正式矩阵使用原合同内 10ms 保持/10ms 容差，无生产时序或系统计时设置修改。

五个 CTest 共 4.15 秒通过：debug_cleanup_result_tests、debug_runner_tests（含实际 Reader 未退出的停止超时）、debug_source_epoch_tests（焦点断点与健康对照）、auto_stop_counterpulse_tests、input_training_commit_tests。未运行会创建原生窗口的 debug_session_tests。文档检查 19 文件/195 链接/0 错误。正式 Release、产物身份、提交、远端及 CI 记录在 docs/evidence/XEN-DEBUG-CLEANUP-RESULT-20261011-001/RESULT.md。

仅关闭 AutoStopProbe 执行返回后结果索引保存异常这一入口；不据此宣称所有异常或其他诊断模块的设备清理状态已完成验收。
