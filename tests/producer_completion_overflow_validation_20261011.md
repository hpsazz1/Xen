# D34 producer 完成时间戳溢出

基线 32816ffbfe025cdcdcda4af07b912f795841ae82，隔离分支 fix/producer-completion-overflow-20261011。

真实禁输出 producer CLI 的五个红例：control 分别等于 INT64_MAX-99999、INT64_MAX-50000、INT64_MAX，第二行越界和可见目标越界。旧实现均 exit 0 并发布负的 backend_completed_at_ns。首次红例使用 D32 的 producer 产物；D33 只改 evaluator 及测试，producer 源码与 D32 一致。

现在在运行时和 JSON 加法前验证 control_at_ns <= INT64_MAX-100000，计算一次完成时间供两处复用。五例均 exit 2 且不发布正式 bundle。正常时间、INT64_MAX-100000、第二行最大合法边界三个控制仍精确输出，其中最大完成时间等于 INT64_MAX。没有钳位输入或依赖编译器回绕。

一手依据：[C++ 表达式可表示范围](https://eel.is/c++draft/expr.pre)：有符号计算结果不可表示时行为未定义。实际红例证明本机 Release 曾发布无效时间戳，不将观察到的回绕推广为标准行为。

八组实际 CLI 和六项 CTest 通过，最终回归 2.41 秒：producer_completion_overflow_tests、producer_integer_bounds_tests、aim_production_red_producer_tests、aim_production_red_producer_integration_tests、aim_production_red_evaluator_tests、evaluator_time_contract_tests。C++ 单测在库修改后重新链接并重跑；公共文档 19 文件/195 链接/0 错误。

仅合成数据、禁输出 CLI；没有启动 UI、采集、游戏或设备输入。应用与 producer 干净 Release、完整 SHA、远端及 CI 查询归档在本地 docs/evidence/XEN-PRODUCER-COMPLETION-OVERFLOW-20261011-001。F01 间歇 worker 组合异常仍待诊断。
