# 采集会话与检测帧身份回归

基线 `b8af2e58ce9b37bafdf604d87271cae44df7f053`，任务 D27-E1。
原 D27 已修复 Runtime 停止、重启和重载与采集启动的准入交错；本项处理已完成检测但尚未
offer 的旧帧跨过重载，进入新 Collector 会话的独立窗口。

## 真实失败与修复

通过每个 Runtime 实例独立的 `before_collection_offer` 屏障暂停旧帧，此时真实 CPU Detector
已返回 SUCCESS 且释放 Detector 锁。执行真实 reload，使用新 RuntimeSnapshot 启动 Collector，
请求手动采样后放行旧帧，再交付新帧。生产默认不安装该测试屏障。

旧版真实日志：`old_generation=1 new_generation=2 old_saved=1 saved=1 paused=1 record_generation=1`。
旧帧被保存且消耗人工请求，随后正确新帧因首次错误绑定的模型代次而暂停。

Runtime 现在在生命周期锁内校验准入后，将 Runtime 与 Detector 代次一并交给 Collector。
Collector 在会话 active 发布前固定身份；offer 在既有 mutex 下先检查身份，迟到的旧模型帧
或错误 Runtime 帧只记为 filtered，不消耗人工请求、去重状态或采样间隔。旧会话遇到更高
模型代次仍明确暂停并要求重开。独立 Collector 的全零身份保留既有首次绑定合同；半身份拒绝。

会话和样本 JSON 新增 runtime_generation，会话另记录绑定的 detector_generation；样本通过
session_id 关联模型路径、摘要与类别。既有 schema_version、预标注和审核含义不变。

## 验证与边界

- 同模型 reload 和真实 `14w_v11.onnx`→`14w_v5.onnx` 两条路径均为
  `old_saved=0 saved=1 paused=0 record_generation=2`。
- 检查实际 samples/1.json、session.json 和 images/1.png；新图片像素为第二帧特征，
  不能只替换旧图片的记录代次。旧帧不靠 try_lock 丢帧偶然通过，明确断言 dropped=0。
- `data_collection_tests` 覆盖部分身份拒绝、runtime=0/旧/未来而模型相同的拒绝、正确身份恢复、
  更高模型代次暂停和原独立 Collector 合同。
- 16 个直接消费者 CTest 在全部重新链接后通过：10 项 collection_lifecycle、data_collection、
  model_workspace、runtime_model_reload、runtime_start_failure、runtime、input_safety，耗时16.91秒。
- 新增身份字段的3项数据消费者（model_data_pipeline/review/integration）通过，耗时5.26秒；
  合计19个不同CTest。公开文档19文件195链接0错误，git diff --check通过。
- Release 构建 xen_app 和全部直接消费者通过；干净提交后的正式构建与远端核对保存在本地
  `docs/evidence/XEN-COLLECTION-FRAME-IDENTITY-20261011-001/RESULT.md`。

复现入口（已配置 XEN_TEST_MODEL）：

```text
ctest --test-dir build/nvidia -C Release -R "^collection_lifecycle_frame-identity$" --output-on-failure
collection_lifecycle_tests.exe <first.onnx> --frame-identity <second.onnx>
```

全部像素合成、CPU 模型只读，使用内存禁输出设备，确认 moves=0。不是设备采集、游戏输入、
物理闭环或多 Provider 验收；本项没有新 UI 修改，未重跑画面截图。
此修复不解决 D25 的模型文件被外部替换后 Session 与路径摘要不一致问题，不以重复 hash 冒充
同字节冻结，也不扩大 class_names 自动审核权限。后续按 D04/D07→D06/D11 推进。
