# U-ENV1 固定训练版本声明去重

基线d3cdb31e3fd4e24b610f2433aa880d1df91cf676，分支refactor/training-version-contract-20261011。

现有model_training_requirements.txt成为唯一版本声明。环境模块以标准库读取六个完整固定版本，训练pipeline复用该模块的trainer版本；环境目录名称的torch部分也由声明生成。可信CUDA wheel完整URL与SHA保留，另核对其版本与声明一致。所有实际固定版本保持原值，没有安装、下载或升级环境。

真实脚本的隔离副本共九场景：健康控制通过，八红例转绿，包括trainer/onnx两处声明漂移、重复/范围/缺失/额外包/wheel不一致五种合同错误，以及安装中原requirements被替换。安装编排使用Context.run替身，pip与子worker从未执行；冻结的原字节供版本、记录摘要和pip路径共同使用，副本被替换后仍一致。该替身结果不代表真实安装或GPU验收。

解析器只支持项目当前六个name==major.minor.patch及整行注释，拒绝其他语法，不冒充通用pip解析器。[pip官方requirements文档](https://pip.pypa.io/en/stable/reference/requirements-file-format/)说明其完整语法包含pip专用选项；本次有意保持项目固定声明合同。

五项CTest全部通过，共4.93秒：model_data_pipeline_tests、dataset_snapshot_tests、model_training_environment_tests、training_version_contract_tests、model_data_integration_tests。公共文档19文件195链接0错误。

只读现有固定环境metadata确认：ultralytics8.3.203、torch2.8.0+cu128、torchvision0.23.0+cu128、onnx1.19.0、onnxruntime1.22.1、Pillow11.3.0，trainer8.3.203；读取两个模块未导入torch/ultralytics。未训练、未加载用户权重、未运行UI/采集或设备输入。Release、SHA、远端和CI原始证据见本地docs/evidence/XEN-TRAINING-VERSION-CONTRACT-20261011-001。
