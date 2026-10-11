# 独立急停采样报告收尾与取消

基线 `af7c0c59e327e375acdb0b32956ac6fc1dfaa5b2`，分支
`fix/auto-stop-capture-finalize-20261011`。测试编译包含生产 CLI，factory 固定为
3×2 合成 BGR；控制台注册和等待是可控替身，断言只创建/打开/关闭该假采集器。
不打开 DXGI/NDI、不操作桌面、游戏或输入设备；等待被压缩，耗时不是采样性能结果。

有效旧代码反例：CSV 和 summary 的真实 filebuf 关闭故障均被析构忽略、CLI 返回0，
CSV 关闭失败后还发布 COMPLETED 摘要；非 BMP 目录 PNG 保存失败；等待边界收到
Ctrl+C 后仍多调用一次 grab。五个失败断言对应四类问题，不算五个独立生产缺陷。
关闭注入只改变测试进程 C++ locale 的 codecvt unshift，先证明 write/flush 成功而
close 确实失败。首次注入器错误依赖 mbstate_t 保留标记、没有命中；日志保留且不计证据。
有效版本以串行流前缀选中 CSV/summary，PNG 不计入报告关闭序号。

现在 CSV/summary 显式 close，沿用既有 exceptions 设置向 CLI 传播失败，CSV 失败
不继续写摘要。PNG 保持压缩1，用内存编码和原生 filesystem 路径流写入/关闭；
Unicode 成功并逐像素检查。等待返回后重新检查取消，已返回的一帧仍完成保存并标注
INTERRUPTED。零帧取消状态追加反例证明原无帧失败条件会盖过取消，现明确保留
INTERRUPTED；自然结束无帧和真实帧错误同时取消仍是 FAILED。

包括健康、两关闭故障、三次健康新目录恢复、Unicode、等待中取消、返回帧后取消、
自然无帧及真实错误同时取消共十一轮 CLI 调用，另有真实 filebuf 注入控制。
固定提交还运行实际工具六种 --help/--dry-run 参数前置例，均不进入采集或创建输出目录。
Release 应用/工具/测试、专项 CTest、产物身份、远端与实际CI元数据见本地
`docs/evidence/XEN-ROUND1-CLOSEOUT-20261011-001/AUTO_CAPTURE_RESULT.md`。

保留失败时已有证据文件，不声称跨PNG/CSV/summary事务或断电持久性。
summary关闭失败可能留有部分内容，CLI失败不能被文件存在或其中状态覆盖。
真实SDK grab/close阻塞上界与实际系统控制台终止不在本次证明内。
