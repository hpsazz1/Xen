# D29 训练验证阶段取消

基线 53a992b3486506601251050c4b792bbb36115f2c，分支 fix/training-validation-cancel-20261011。

固定 Ultralytics 8.3.203 的实际 CPU Trainer/Validator 使用本地生成的三层 Conv/Detect 小模型与已审核合成图片，epochs=3、imgsz=64、batch=2、workers=0。测试仅在实际 callback 写入取消文件，未替换训练、验证或检查点保存实现。原代码在首轮验证结束或保存后的回调收到取消，仍训练第二轮；在末轮这两个位置取消则被 final_eval 清除 optimizer，last.pt 摘要也与 checkpoint.json 不符。四个真实入口红例均已转绿，健康三轮训练和 ONNX 导出对照通过。

修复在模型保存后重新检查取消，并在 on_fit_epoch_end 与下一轮 on_train_epoch_start 检查，保留训练中收到取消后完成本轮并保存检查点的策略。首轮取消只产生 epoch 0 的训练 batch；末轮取消保持 epoch 2 的检查点，四个取消例均保留 optimizer，元数据摘要与实际文件一致，状态 CANCELLED/退出码 2，未发布 candidate.json。末轮已完成计划 epoch，不表示可继续原计划；此次验证检查点内容与摘要，未把它表述为实际续训验收。

脚本：tests/training_validation_cancel_tests.py --output-root <独立临时目录>，需现有固定训练环境（torch 2.8.0+cu128、Ultralytics 8.3.203、ONNX 1.19.0），全程明确 CPU。源数据、随机权重、输出均在本任务目录，无下载、依赖安装、全局配置写入、设备输入或 UI 启动。

六个 CTest 通过（6.69 秒）：model_data_pipeline_tests（32 个单元例）、dataset_snapshot_tests、model_workspace_tests、model_training_environment_tests、model_data_review_tests、model_data_integration_tests。公共文档检查 19 文件/195 链接/0 错误。正式 Release、提交、远端及 CI 记录见 docs/evidence/XEN-TRAINING-VALIDATION-CANCEL-20261011-001/RESULT.md。

回调顺序依据：[Ultralytics v8.3.203 trainer](https://raw.githubusercontent.com/ultralytics/ultralytics/v8.3.203/ultralytics/engine/trainer.py)，已与本机固定版本核对。未对模型精度或实际设备性能作结论。
