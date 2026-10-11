# GSI 实际传输与生命周期补证

基线8bdd91c4934c2228427f7b8d1c73bb4b35872bf9。本批仅补回归和登记，未发现需要改变生产行为的新缺陷，不把新断言通过计成修复。

独立EXE编译原weapon.cpp，只以观察包装调用原Winsock recv并累计其实际返回长度；不伪造字节、错误或替换Receiver、HTTP解析、GsiState。7个TCP切分位置在发送剩余字节之前等待Receiver实际读取前缀，确认未完整请求不更新revision，完成后只发布一次。四种拒绝：重复Content-Length、chunked、过量body声明、超过8192B头；每次后续合法请求恢复，拒绝不延长旧TTL。头/body两种半包由服务端100ms请求期限关闭（检查EOF/RESET，不接受客户端TIMEOUT冒充），后续合法请求恢复。

两种已读取半包触发stop，再做无效配置重启与健康重启，旧快照失效且代际推进。只设宽松1s测试上限，证明本机本例没等满2s请求期限；不证明恰好在内核select阻塞中取消、全负载P95或跨机时间界限。新实例端口来自自有回环临时端口，同Receiver重启使用新临时端口，不声称同端口TIME_WAIT重绑覆盖。

实际GsiState接受正常→单调时间回退拒绝→恢复时间接受，确认control/recoil断点持续且同秒TTL不刷新；未来墙钟包拒绝后健康包仍可接受，不污染timestamp高水位。没有修改机器时钟。既有D31 accept API错误注入回归和weapon状态/时序测试作为相关消费者。

官方依据：[Winsock recv](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-recv)明确TCP返回当前可用字节、EOF为0，不能把send调用当读取边界；[select](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-select)说明readable与超时契约。包装仅用于读屏障，不改变API返回值或生产调度，不复制第三方实现。

所有地址仅127.0.0.1，自造身份/武器字段；无游戏、物理设备、原生UI、采集/GPU。GSI本身仍不证明前台、停稳、开火许可或源端逐包顺序。真实网络故障、任意fragment排列、SDK/驱动停机、峰值与物理E未覆盖。无新增常驻规则。
