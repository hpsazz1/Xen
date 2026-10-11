# Capture evidence Unicode PNG 路径回归

基线 `39317ac24871f72226c4eebafdb387fc53d5c4bc`，隔离分支
`fix/capture-unicode-png-20261011`。真实 Recorder fixture 只读取自生成的一帧
3×2 PNG，不创建 Capture、NDI 源、输入设备或窗口。旧产物身份为干净 `b212b2d`，
Recorder 与 fixture 源码相对此基线无差异；产物 SHA 与 sidecar 一致。

ASCII 与中文输出目录成功，含非 BMP 字符 `🚀` 的目录失败且没有正式 manifest：
`path.string()` 无法映射到本机代码页。随后在当前源码新增回归并重新编译，
同一反例仍失败；写入冲突后重试控制原先通过。不能把中文目录也计为已复现失败。

现在保持原 PNG 压缩参数，用 `imencode` 编码当前一帧，再通过 filesystem 原生路径
文件流写入并检查关闭状态。采集阶段仍只缓冲 BGR；编码缓冲仅存在于 finish 当前帧。
768 MiB 是原 BGR 缓冲上限，不能声称包含编码器与临时 PNG 的总进程内存峰值。
[OpenCV 官方接口文档](https://docs.opencv.org/4.x/d4/da8/group__imgcodecs.html)
说明 imencode 使用指定格式及参数生成内存编码；实际兼容性由本机 OpenCV 4.14.0 构建验证。

新增三目录控制验证无损像素、PNG 字节一致、manifest/binding 同时发布及拒绝覆盖。
故意创建本轮 incoming 内的同名空目录使 PNG 写入失败：不得发布 manifest，
只移除此测试自建空目录后，同一 Recorder 重试成功。不是磁盘关闭故障注入。
既有完整录制、时间缺失/恢复、原子发布竞争和缓冲限制回归保持。

既有 252 帧真实 Recorder→原生 verifier→producer→binder→evaluator 测试现使用
中文及非 BMP 输出目录，继续核对十二个重封摘要来源绑定负例，避免只验证孤立 PNG。
固定提交 Release、两项相关 CTest、产物与源码身份、远端及 CI 的最终记录见本地
`docs/evidence/XEN-ROUND1-CLOSEOUT-20261011-001/CAPTURE_UNICODE_RESULT.md`。

本修复不扩展任意长路径支持，不改变输出许可、来源时钟或数据审核边界。
