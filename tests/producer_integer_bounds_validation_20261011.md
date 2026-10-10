# D32 producer整数域校验

基线 ab0434d2b0cf1a7c7c51961b957ba83d56617d84，分支 fix/producer-integer-bounds-20261011。

实际禁输出 producer CLI 使用八条合成样本。原实现先将 score_begin/end、plant delay、ROI 收窄到 int，再验证；4294967298/4294967304 评分被折成 2/8，并发布正式 bundle。负数及 uint64 大值也能在收窄后变回合法值。十个错误接受入口已复现并转绿：五个评分区间、两个延迟、三个 ROI。

修复在原整数域验证：通用 int64 读取明确拒绝超范围 uint64，ROI 在转换前检查 int 范围，延迟先检查既有合同上限，评分区间在已验证样本数量内检查再转换。ScoreInterval 只解析一次，trace 与 manifest 共用该值；不增加新尺寸限制，int32 最大正 ROI 仍有效。

22 个实际 CLI 例全部通过：20 个非法输入退出 2 且没有正式 bundle，正常和 int32 最大 ROI 两个正例成功；正例还检查 manifest 与全部八行 trace 的评分区间相同。脚本 tests/producer_integer_bounds_tests.py --producer <禁输出程序> --output-root <独立夹具目录> 保留输入与结果，未连接设备或采集，没有原生 UI。

四项 CTest 共 1.63 秒通过：producer_integer_bounds_tests、aim_production_red_producer_tests、aim_production_red_producer_integration_tests、aim_production_red_evaluator_tests。文档 19 文件/195 链接/0 错误。首轮修复构建遇 Windows min/max 宏展开冲突，采用局部括号调用修复；失败日志保留，未改变全局编译设置。正式应用与 producer Release 身份、提交、远端、CI 记录见 docs/evidence/XEN-PRODUCER-INTEGER-BOUNDS-20261011-001/RESULT.md。

依据：[C++ 整数转换规则](https://eel.is/c++draft/conv.integral)。D33 时间字段一致性、D34 时间戳加法溢出及 F01 间歇测试异常仍是独立待办，本修复不声称关闭它们。
