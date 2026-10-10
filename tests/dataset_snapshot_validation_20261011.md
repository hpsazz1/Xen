# D24数据集字节与摘要一致性

基线5e91911da203daf615ccc223613373a70c835169；隔离分支fix/dataset-snapshot-identity-20261011。

四个实际反例在原实现失败：dataset.json解析后替换为B并更新身份文件，旧验证仍接受解析的A；data.yaml摘要读取后替换为普通配置，旧验证接受带download字段的已认证原字节；真实CPU评估已复制A后替换源清单，evaluation.json记B；真实CPU训练已复制A后替换源清单，config/checkpoint/candidate均记B。后两者使用真实Ultralytics8.3.203、ORT1.22.1、torch2.8.0 CPU执行，模型为本地生成常量ONNX或三层小型随机网络，无模型下载或训练器替身。未执行任何download配置值。

修复将dataset.json一次读取的原始字节、解析结果和SHA256封装在DatasetSnapshot中；data.yaml解析与摘要也共用一次读取。训练、续训元数据和评估沿用已验证摘要，工作副本仍逐图片/标签校验哈希，保存dataset.snapshot.json用于复核。源目录后来变化不会重写结果所属数据版本；未改变人工审核、split白名单、PT可信来源、模型兼容性或候选未激活规则。

八专项：两读取替换反例、BOM精确字节健康控制、续训元数据旧/新版本控制、真实CPU评估健康/替换、真实CPU训练健康/替换。结果记录与保存的清单逐字节核对。续训专项只验证真实resume_source读取的元数据和文件摘要，没有声称本批完成真实优化器续训；真实训练为1epoch、64px、batch2、workers0、CPU，不是精度或性能基准。

六CTest全部通过6.33秒：dataset_snapshot_tests、model_data_pipeline_tests（32用例）、model_data_review_tests、model_data_integration_tests、model_workspace_tests、model_catalog_tests。既有审核边界、标签冻结、PT权限、工作区状态和模型目录回归保持。文档检查19文件195链接0错误。

复现命令：常规Python运行tests/dataset_snapshot_tests.py --output-root <任务临时目录>；真实模型验证在既有固定版本训练环境中追加--real --train。所有夹具保留在显式目录，YOLO/Matplotlib/Torch缓存重定向到任务夹具下，既有环境仅使用、不安装或升级。

正式Release身份、完整SHA、远端与CI见本地docs/evidence/XEN-DATASET-SNAPSHOT-20261011-001/RESULT.md。没有启动Xen/UI、真实采集、设备或系统输入。D25的ORT加载模型字节身份独立开放，本批不声称处理它。

依据：[Python JSON](https://docs.python.org/3.12/library/json.html)、[hashlib](https://docs.python.org/3.12/library/hashlib.html)。结论来自实际入口交错与产物，不把源路径后来重读的摘要视为早先已使用内容的证明。
