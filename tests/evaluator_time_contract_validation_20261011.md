# D33 evaluator时间合同

基线 d1db1c56f5f48e597a6ebf954b04efcd9e0f763d，分支 fix/evaluator-time-contract-20261011。

实际 evaluator CLI 消费重新计算 trace/manifest 摘要的合成证据。原实现只核行内时序和 dt>0，未验证相邻 control 时间差及 age 等式。六个红例：时间固定、倒退、单独 dt 矛盾、age 错误、age 负数，以及 B0/C/C-D0 同时固定时间；前五例原本仍 BASELINE_RED_LOCKED，最后一例仍 CANDIDATE_DEVELOPMENT_GREEN。六例现均 RED_INVALID/ROW_TIME_CONTRACT_INVALID，未用哈希一致代替时间有效性。

本地 producer 在第二行起已要求 controller_dt_ns 等于 control_at_ns 的相邻差，导出 observation_age_ns 等于 control-capture；fidelity builder 同样生成原始 control 差值。Aim 内部 1–50ms 钳位不改变这两个导出字段的口径。修复 evaluator 与既有数据生产合同一致，首行没有前一个 control，只要求原有正 dt。时间规则加入 red_schema_contract 的语义摘要，不静默复用旧合同身份。

十组真实 CLI 通过：六非法和四合法控制（正常基线、合法不等间隔、独立首行 dt、正常开发候选）。所谓开发候选仅是离线评估状态，没有真实输出或采用/发布操作。所有文件均来自合成夹具，未读真实设备或采集、未启动 UI。

五项 CTest 共 3.40 秒：evaluator_time_contract_tests、aim_production_red_evaluator_tests、aim_production_red_producer_integration_tests、aim_production_red_plan_builder_tests、aim_production_red_fidelity_plan_builder_tests。公共文档 19 文件/195 链接/0 错误。正式 Release、提交、远端及 CI 证据见 docs/evidence/XEN-EVALUATOR-TIME-CONTRACT-20261011-001/RESULT.md。D34 时间戳加法和 F01 间歇 worker 测试异常仍待办。
