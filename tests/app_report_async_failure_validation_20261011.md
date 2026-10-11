# App 后台停止与封尾异常恢复

基线 e476e6d74adc17a00b34db3964c863a768012fe2。ReportLifecycle 的后台任务创建失败原从主线程动作路由逃逸，任务体异常原经 future.get 逃逸，失败回调与取消重启均被跳过。

独立回归使用真实 std::async/future、生产 ReportLifecycle 和 finish_reports_after_archive。仅当前测试线程一次 operator new 分配失败，原生 async 控制确认触发；Runtime 为无设备适配。创建 FINISH/STOP 和工作体 FINISH/STOP 四例、四个同实例健康重试；基线19条失败断言，只有launch修复时剩8条，完整修复后0。断言数不是缺陷数。FINISH工作体在归档停止之后快照边界失败，STOP是适配器故障注入，不宣称真实设备线程失败。重复点击、一次失败交付、错误可见、取消重启与恢复后清除错误同时覆盖。

创建故障以不分配内存的类别和静态消息保存，主线程回收前仍busy；不把未执行任务当成完成。工作异常回收为失败结果，共用取消重启和UI反馈。成功路径依然后台执行，未改成同步停止或deferred future。新测试在 CTest app_report_async_failure_tests，既有 app_report_lifecycle/interleave 和 collection_lifecycle 为直接消费者。

边界：不是全进程OOM安全证明，也未耗尽OS线程池；生命周期入口之外的字符串/快照分配、持续资源不足和SDK无限阻塞未验证。关闭时持续无法创建任务会继续等待/重试，不假报设备已停止；实际设备清理、原生窗口和物理E未执行。Provider、武装、保持键、End、UNKNOWN和数据审核合同保持。

设计依据：C++工作草案 [futures.async](https://eel.is/c++draft/futures.async)规定launch可抛bad_alloc/system_error，工作异常存入共享结果；[future::get](https://eel.is/c++draft/futures.unique.future)会重抛存储异常。MSVC [future functions](https://learn.microsoft.com/en-us/cpp/standard-library/future-functions?view=msvc-170)说明async在线程池执行。采用既有future所有权/屏障，未引入新线程池、依赖或复制第三方实现，无新增许可证义务。
