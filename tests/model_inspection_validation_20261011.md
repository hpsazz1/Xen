# D25模型检查同字节身份

基线4ae606dbfe4490b6f2c8977dded0415e61ac071f；分支fix/model-inspection-bytes-20261011。

真实ORT1.22.1 CPU Session加载本任务生成的ONNX。测试仅在构造返回后替换路径内容，保留构造与推理实现：原实现Session输出1且metadata=A，model_sha256却属于替换后输出2/metadata=B文件。另两反例为真实外部initializer和Constant属性Tensor权重，原inspect均成功调用ORT加载，却只给出不覆盖外部权重的模型文件摘要。

修复一次读取模型bytes，现有递归TensorProto检查处理同一份protobuf，拒绝外部数据后将原bytes传给InferenceSession并计算SHA256。严格显式CPUExecutionProvider不变。带模型的inspect现在需要onnx和onnxruntime，二者均在既有固定训练环境依赖中；缺少onnx时明确DEPENDENCY_MISSING，不自动安装。模型检查不产生部署或激活权限。

十场景通过：健康/加载后替换、两类外部权重、四类names字符串解析、缺少onnx、无效protobuf作业FAILED。修复后外部权重两场景的ORT调用数均0，健康/替换均实际CPU推理输出1且摘要保持A。缺少依赖/无效protobuf在ORT前失败；非法names只显示警告，不执行表达式。names既有单元测试仍隔离字符串解析；本新增脚本提供真实protobuf/ORT覆盖，不混称替身为实际模型验收。

四CTest通过4.15秒：model_data_pipeline_tests（32用例）、model_workspace_tests、model_training_environment_tests、model_catalog_tests。文档19文件195链接0错误。专项以既有训练环境执行tests/model_inspection_snapshot_tests.py --output-root <任务临时目录>，夹具保留；没有安装或升级环境、下载模型、启动Xen/UI或访问真实采集/设备输入。

正式Release身份、完整SHA、远端与CI见本地docs/evidence/XEN-MODEL-INSPECTION-BYTES-20261011-001/RESULT.md。本批修复Python模型元数据入口，不声称C++Detector模型装载做了同类调整。

依据：[ORT Python API](https://onnxruntime.ai/docs/api/python/api_summary.html)的path_or_bytes参数、[ONNX序列化](https://onnx.ai/onnx/api/serialization.html)、[ONNX外部数据](https://onnx.ai/onnx/repo-docs/ExternalData.html)。本地验证ONNX1.19.0/ORT1.22.1，未按网页当前版本更换依赖。
