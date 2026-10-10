# D04/D07 正式证据链验证快照

基线 `67e87e9dd1660958c02d6e940515b0e96aa5ceb8`，隔离分支
`fix/evidence-validation-20261011`。全部为合成离线证据、真实校验进程与正式脚本入口。
没有捕获屏幕、网络发送、系统/游戏输入或校准输出。

## D04：共享严格schema与执行合同

原CompositeSeal只计算并比较报告的无密钥自摘要，没有调用库的严格字段校验。
真实CLI→producer→binder→evaluator中，缺safety、false、ACK早于issue、零response额外
dispatch、累计不守恒五例均 `native=0 producer=0 published=True`，下游也接受。

将生产writer的事件序列化、字段类型、执行时序和累计守恒提取为纯数据模块，writer与reader
共用同一执行合同。CLI在同一次JSON解析上完成严格校验及摘要计算，不重新打开路径。
报告解析还检查数值转换范围、终态枚举、连续source事实与内部身份。外部冻结序列/plan绑定
继续由各消费者核验，报告内的nominal重建不冒充外部序列校验。

追加零response的safety=false反例，初次共享原writer合同后仍被接受；实际executor在记录任何
物理sample前检查安全门，因此共享合同现要求所有物理事件（包括零命令）得到许可。
六例均转为 `native=2 producer=2 published=False`，合法B全链通过。派发、完成、ACK计数
由完整报告的实际事件统计，并与绑定命令数量核对，不再只写固定38。

CompositeSeal仍不链接Mouse/Capture/Runtime实现。生成的vcxproj仅引用纯校验库；实际PE导入
仅bcrypt、KERNEL32及C/C++运行库，无SendInput、WS2_32、GetAsyncKeyState、DXGI或NDI入口。
没有为了复用验证代码而扩大工具的输出能力。

## D07：同一次字节读取支撑判断、摘要与发布

producer反例A将合法B的响应RTT从0.004改为0.005，保留B的摘要。静态A被真实CLI拒绝、B
被完整链接受；在实际subprocess.run前恢复原路径B，旧实现却发布A的5000ns数据并引用B身份。
新实现一次读取报告bytes，解析、摘要和传给原生校验进程的私有临时副本均使用这份bytes。
路径替换后A仍被拒绝，两个ledger均不存在。没有mock verifier成功或只做前后hash。

benchmark_xudp_sender相邻入口也先检查A、稍后按原路径hash并移动B。正式脚本回归使用独立
C#进程只写合成sender JSON，在检查后的Git调用边界将原待发布报告改为complete=false、
frames_failed=1；旧脚本仍打印通过并发布失败B。修复后解析、摘要及私有发布副本来自冻结
bytes，两组实际脚本测试均发布相同合法A字节，环境清单的摘要与长度也一致。
新增私有副本仅在CreateNew成功取得所有权后纳入清理；原有其他发布所有权问题归D02，
writer关闭错误归D17，不在本项提前关闭。

## 已执行验证

- Release构建纯报告库、mouse_effect_probe、runner、CompositeSeal、Sequence和xen_app。
- 10项CTest通过（12.06秒）：probe、runner；composite evaluator/binder/plan/producer及
  script_contract；新增报告语义、报告路径替换及XUDP正式脚本路径替换。
- schema常量进一步归并后，重新构建并复核直接使用者；干净提交后的正式Release身份与
  远端校验落在本地 `docs/evidence/XEN-EVIDENCE-VALIDATION-20261011-001/RESULT.md`。
- 公开文档19文件195链接0错误，git diff --check通过。

已有producer合成夹具将Mouse状态改为真实枚举READY，并让零控制帧延续正确累计值；这两项
夹具纠正不计生产缺陷。PowerShell回归保留Windows PowerShell 5所需BOM与闭包状态。
原始红绿日志与PE导入表保存在本地证据目录，未强行加入ignored文档。

未执行真实设备、多Provider或完整Prepare/采集链。D06实际Recorder来源绑定、D11人工未知
事实、D17关闭失败、其余文件输入冻结各自单列后续。无新增常驻规则。
