# Probe计划字节快照回归

计划文件原先先hash，再重新打开解析。仅收拢原步骤到私有验证器后的基线，在实际哈希消费完成处替换自有文件：合法A换非法B被误拒；非法A换合法B被误接纳。两条反例均实际失败，原始日志保留；没有启动runner、sidecar、采集、调度或Mouse。

现在读取一次有界快照，文件hash、JSON解析与语义校验共用字节，仅成功时返回hash。沿用1MiB上限，多读一字节检测长度变化；这不是整个解析进程的峰值内存承诺，也不提供多个文件之间的原子快照。文件变化后的内容不替换已经接纳的内存计划。

独立测试直接编译同一生产私有验证器，不增加公有接口或生产故障开关。12例包含两个哈希后替换、读取前hash漂移、Unicode、空/缺失/截断文件、语义与sequence绑定漂移、恰好及超过1MiB。拒绝必须清空旧身份并给出原因；既有物理授权、严格来源、保持键、End和UNKNOWN边界未改变。实际NDI runner与物理E不在本证据中。

采用微软文档中[read/gcount/EOF语义](https://learn.microsoft.com/en-us/cpp/standard-library/basic-istream-class?view=msvc-170#read)实现有界读长核验；按[BCryptHashData输入缓冲区合同](https://learn.microsoft.com/en-us/windows/win32/api/bcrypt/nf-bcrypt-bcrypthashdata)对解析所用字节计算hash。沿用仓库已有BCrypt封装，无第三方代码复制、依赖或权限变化。
