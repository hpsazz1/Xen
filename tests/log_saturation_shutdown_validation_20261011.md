# 日志双队列饱和、停机和格式化重入补证

基线1c6bb60799a280bb3bc63332bc84fd9e86fa6196。本批仅新增测试和登记，不改变生产实现。独立EXE编译原log.cpp，将实际rotating_file_sink包一层可主动放行的等待门；真实文件sink、spdlog 1.17.0 thread_pool/async_logger/queue、Log AccessLease/AccessPause/init/shutdown均未替换。无生产测试钩子或公有接口扩展。

普通worker进入慢sink后发送5000条INFO，优先worker也进入慢sink后填满256槽WARN队列，第258条WARN调用确实阻塞。启动shutdown，观察新访问被暂停但关闭尚未完成；暂停后的新WARN不进入旧文件。主动放行后，生产者/关闭都完成，未依靠5秒保护超时放行。实际文件核对普通尾部4096条、904条覆盖且最旧不存在，优先258条逐条唯一保存；同目录重新初始化追加新记录，旧文件为精确前缀。

真实std::formatter从LOG_INFO调用中嵌套写WARN并Log::shutdown，确认内层消息排空，外层格式化完成后不访问已销毁logger；同目录再初始化恢复。该回调发生在write_view的AccessLease之前，不模拟不存在的生产自定义sink回调，也不声称任意sink/error-handler重入安全。

官方机制：[spdlog异步文档](https://github.com/gabime/spdlog/wiki/Asynchronous-logging)定义阻塞/覆盖队列及weak线程池所有权；[v1.17.0 thread_pool](https://github.com/gabime/spdlog/blob/v1.17.0/include/spdlog/details/thread_pool-inl.h)按队尾terminate和join清理。当前本地依赖头文件确认rotating sink为final，因此采用组合包装，不继承修改依赖。仅调用既有MIT依赖，无源码复制或版本变化。

此夹具证明有界阻塞解除后的顺序和存量记录，不是实际磁盘长期卡死、断电持久性或全负载P95证明。WARN生产者仍按既有契约可能因满队列阻塞；shutdown会等待在途访问与sink。完整log.cpp静态复核；公开dump_ring_buffer无生产调用者，为best-effort void转储，fwrite/fclose错误反馈未新增，本批不将其当可靠归档。既有崩溃紧急尾部是独立原子缓冲路径。

仅自有TEMP文件和线程，无设备、网络、游戏、Capture、UI、GPU。初步饱和用例.141秒，加入formatter后的反馈环.062秒，均是本机功能测试耗时，不能当性能改进数据。无新增常驻规则。
