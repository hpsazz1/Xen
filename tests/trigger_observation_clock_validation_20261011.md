# F01 后续：观测发布与决策时间交错

基线 333b36684c02dc18c4f9f86f539ca2571ffeecca，分支 fix/trigger-observation-clock-20261011。

真实 TriggerWorker 原先在循环开始取 now，随后才读取并发发布的 latest。发布线程可在两者之间采样并发布健康观测；真实控制器因此看到 observed_at > now，以 TIMING_UNAVAILABLE 提前执行 UP。

专项直接编译生产 trigger_worker.cpp，仅替换该翻译单元的时钟调用：真实 now 值不变，返回前通过屏障暂停，在窗口内发布真实时间的新合成观测。接缝从当前源码定位唯一决策取样语句并通过 source_location 匹配，没有修改系统时钟、伪造控制器结果或放宽校验。纯内存设备通过实际 worker、controller、回执和事件环。旧代码出现实际 TIMING_UNAVAILABLE/UP；修复后同一交错正常 RELEASED，真实未来时间仍 TIMING_UNAVAILABLE，陈旧时间仍 STALE，两个非法控制均无 DOWN。

决策时间现在在读取观测及许可后取得；取消清理也使用当次时间，不复用取消 ACK 之前的旧时间。Provider、武装、保持键、End、来源、UNKNOWN 和数据审核边界不变。

依据：[steady_clock 合同](https://eel.is/c++draft/time.clock.steady)、[原子顺序合同](https://eel.is/c++draft/atomics.order)。获取原子发布值不会让此前已经采样的时间自动变新；无需容忍未来时间或钳位。

五项 CTest 全部通过，共 133.59 秒：weapon_stop_integration_tests 17.40 秒、trigger_tests 0.01 秒、trigger_worker_tests 55.69 秒、trigger_observation_clock_tests 0.21 秒、auto_stop_worker_tests 60.28 秒。公共文档 19 文件/195 链接/0 错误。首次诊断夹具配置保持150ms但复用间隔120ms导致启动拒绝，修正为合法100ms后才取得上述红例；该夹具错误保留，不算生产缺陷。

F01 原 D31 间歇失败没有完整事件原因，不能断言就是本窗口造成。原断言保留，新增失败时完整事件输出；已关闭的是本次确定性窗口，历史归因仍未证实。不靠反复重跑绿关闭历史问题。没有 UI、真实设备或游戏输入。Release、提交与远端证据见本地 docs/evidence/XEN-STATIONARY-UP-INTERMITTENT-20261011-001。
