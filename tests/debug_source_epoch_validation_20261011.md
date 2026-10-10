# D16-E1 Counterpulse来源断点验证

基线4cc74400b90b3acb9526199cba229f059d241bd5，隔离分支fix/debug-source-epoch-20261011。
本子批只修Counterpulse实际执行许可与连续就绪窗口；独立Recoil CLI、App Worker和WallRun的
遗漏来源epoch仍属D16-E1后续，不据本测试宣称整个扩展项关闭。

## 实际执行器红绿

debug_source_epoch_tests调用真实run_debug、SourceContextClient、鉴权解码/Evidence接受器和
Counterpulse执行器；设备接口全部在内存记录，不创建生产鼠标、不创建Capture。合成服务器
只绑定127.0.0.1临时端口，固定测试认证向量仅设置在测试子进程，不读取部署凭据或真实前台。

实际就绪后，在既有EVENT_SUBSCRIPTION进度回调停住；同session先响应Background再响应
Foreground，等客户端第三次请求才继续，证明前两次响应已走过真实客户端。执行预算尚未开始，
不放宽ACK或命令超时。健康控制始终Foreground且seq每次跳7；首个内存DOWN后测试取消。

旧行为：healthy-gap和recovered均downs=1、cleanup=1、failure=USER_STOP。
新行为：healthy-gap仍downs=1，recovered为downs=0、cleanup=1、failure=SOURCE_FOCUS；
回归要求UP、WASD清理和订阅结束全部发生，失败终态不能变成功。

## 连续就绪窗口

实际Evidence先接受Foreground，t200ms Background、t299ms Foreground；旧就绪器t300ms
仍ready=true。新就绪器在持久epoch变化时清零窗口，t300ms拒绝、t600ms才再次就绪。
健康前台序号跳跃不增epoch也不重置300ms窗口；初始epoch0照常有效。

修改只读取既有control_safety_epoch：就绪时冻结session+epoch，执行回调核对二者；
readiness诊断新增source_epoch_changed。未改来源协议、TTL、输出时序、UNKNOWN处理或
Runtime原五类消费者。构建初期夹具Windows ERROR宏和Evidence链接问题已修正，不计生产缺陷。

原始红绿、相关CTest及提交后的Release/远端身份见本地
docs/evidence/XEN-DEBUG-SOURCE-EPOCH-20261011-001/。没有新UI改动或物理闭环证据，无新增常驻规则。
六项相关CTest已通过3.80秒：source_context、Counterpulse、readiness、实际来源epoch、
debug_runner与debug_session。公开文档19文件195链接0错误，git diff --check通过。
