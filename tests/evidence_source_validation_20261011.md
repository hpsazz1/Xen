# D06/D11 来源绑定与人工观察验证

基线90803abc6d579acf8e0bc04b63f9e85a7aa2db58；分支fix/evidence-source-binding-20261011。
所有检验仅使用合成像素、JSON、CSV和本地校验进程，无真实捕获或输入输出。

## D06 来源副本必须参与身份验证

新增fixture直接调用生产CaptureEvidenceRecorder::start/record/finish记录252帧，发布实际PNG、
manifest与source-binding副本。正常样本通过真实native verifier→producer→binder→evaluator。
原producer忽略manifest.source_binding，十二个反例均exit0、发布账本，下游也通过：缺字段、
错hash、错scope/run/epoch/sequence/source、上级与绝对路径、report绑定hash不符、安全标志为真、
epoch为bool。身份负例重新计算binding hash并真实原生封签report，排除借用其他hash错误拒绝。

修复后，来源副本路径限定在manifest目录内；一次读取的bytes同时用于SHA与JSON；比对manifest、
report及冻结plan身份，核验现有schema/profile/禁输出约束。十二例均exit2且两份账本不发布，
正常链路继续通过。首次wrong-sequence夹具误用了相同值，已改为不同值并断言差异；修改后的
完整十二例再次在原始90803ab生产脚本上复现，不将无效夹具计为证据。

该修复不验证副本内外部config等引用文件的当前内容，也不宣称防御恶意并发文件系统重解析点
替换。Recorder不创建ICapture，使用测试构造的CapturedFrame。

## D11 不确定文字不能成为绿色结论

真实candidate和holdout CLI各有正常控制；六类错误观察原先都发布成功：无法确认无人工输入、
互相矛盾的输入陈述、不确定witness、不确定急停、未看到变化、重复字段后写覆盖。
另有明确使用鼠标/WASD控制原先即正确拒绝。修复后两个入口共十四例全部拒绝发布。

人工结论按分号分为完整陈述，保留既有明确措辞；witness/遮挡/异常也只做完整匹配，
大小写与末尾句号归一化。CLEAN、CONTAMINATED、UNKNOWN分别记录，任一非CLEAN阻止发布。
重复字段直接UNKNOWN；有效candidate/holdout保存本次完整原文、字段和状态，holdout不借用
calibration人工事实。Prepare观察模板同步说明填写方式，原“用户原话”字段保持。

回归入口：composite_sidecar_binding_tests、mouse_effect_probe_a2_calibration_tests；相关Recorder、
composite producer/binder/evaluator/plan与脚本合同一起验证。最终Release身份、远端SHA、准确
CTest结果及原始红绿日志保存于本地docs/evidence/XEN-EVIDENCE-SOURCE-20261011-001/RESULT.md。
13个不同CTest已通过：首轮10项核心检查通过；补建Prepare所需工具后A2合同及集成2项通过；
composite Prepare的深层路径由273字符缩短至236后，正式脚本集成检查通过3.01秒。
最初缺工具与超长测试路径失败均保留日志，未计为业务缺陷；没有改系统长路径策略。
未执行真实设备、全Provider矩阵或人工物理闭环，无新增常驻规则。
