# 测试入口与验证报告索引

2026-10-12 核对。测试目标和条件注册以根目录 [CMakeLists.txt](../CMakeLists.txt) 为准，构建命令与验证层级见[开发指南](../assets/guide/development.md#源码与验证)。本文提供导航，不把不同时点的专项拼成当前提交的全量通过。

## 当前入口

- [10 月 12 日 main 构建与交付记录](../assets/guide/releases/2026-10-12.md)：列出受测提交、三个后端的实际覆盖、分阶段重测及未执行范围。
- 先对受影响目标使用 Release 构建，再运行匹配的 CTest；完整矩阵只用于正式发布、共享 ABI/运行库等扩大影响面的变化。文档变更只做文档检查。
- 可用 `ctest --test-dir build/nvidia -C Release -N` 查看该目录已经配置的集合；DirectML/OpenVINO 换用对应固定目录。测试集合依赖本机模型与 SDK 配置，未注册不表示已通过。
- [实际 EXE 退出](xen_exe_shutdown_validation_20261011.md)与[Runtime/弹窗组件组合](runtime_file_dialog_shutdown_validation_20261011.md)分别证明各自入口；桌面探针需要真实桌面操作，不是无人值守 CTest。
- 测试中的 CPU、合成画面、回环 socket、假设备和 ACK 都不替代真实设备/游戏体验。
- OBS菜单的分组和站位图来源可运行 `node tests/grenade_menu_tests.cjs`：直接执行 `Xen/grenade_web/page.html` 的真实JS函数，DOM仅作替身，不启动HTTP或游戏输入；覆盖自定义首token站位、完整落点、无空格独立分组、当前站位优先和既有出生点共图。文字换行与首屏布局另用实际浏览器检查。

## 夹具说明

- [弹道扩展夹具](fixtures/recoil_extension/README.md)
- [弹道登记夹具](fixtures/recoil_registration/README.md)

## 按主题查历史报告

以下文件保留原任务、日期、基线、命令和失败记录。原文的“当前分支”“未合并”“下一步”指写作时点；相关源码现已汇入 main，旧分支名只用于追溯。重跑时必须核对当前 CMake/CLI，使用自己的独立输出目录，不直接照搬历史机器路径。修复已被后续提交覆盖时，以对应后续记录为准，不回填旧测试身份。

### 应用、退出与报告

- [文件选择期间的完整应用关闭集成验证](app_file_dialog_shutdown_validation_20261011.md)
- [App 后台停止与封尾异常恢复](app_report_async_failure_validation_20261011.md)
- [D20待持久化故障快照](archive_pending_failure_validation_20261011.md)
- [U01 ImGui 1.92.9b 热修复](imgui_hotfix_validation_20261011.md)
- [日志双队列饱和、停机和格式化重入补证](log_saturation_shutdown_validation_20261011.md)
- [Overlay部分初始化清理](overlay_partial_init_validation_20261011.md)
- [D17 真实报告关闭故障验证](report_close_validation_20261011.md)
- [Runtime 运行中的应用退出验证](runtime_file_dialog_shutdown_validation_20261011.md)
- [实际 Xen.exe 文件选择期间的启动与退出验证](xen_exe_shutdown_validation_20261011.md)

### 采集、传输与时钟

- [Capture evidence Unicode PNG 路径回归](capture_unicode_png_validation_20261011.md)
- [D03 单次UTC异常恢复验证](clock_exchange_recovery_validation_20261011.md)
- [D31 GSI accept 暂态错误恢复](gsi_accept_recovery_validation_20261011.md)
- [GSI 实际传输与生命周期补证](gsi_transport_lifecycle_validation_20261011.md)
- [D21已观测NDI断线后的坐标缓存](ndi_disconnect_metadata_validation_20261011.md)
- [D07 network环境/产物绑定验证](network_evidence_binding_validation_20261011.md)
- [D02 接收运行ID验证](receiver_run_id_validation_20261011.md)
- [Sender采集返回期间取消回归](sender_cancel_validation_20261011.md)
- [D02 传输临时目录所有权验证](transfer_directory_ownership_validation_20261011.md)
- [D19 XUDP解码后提交流切换](xudp_stream_validation_20261011.md)

### 模型、数据与训练

- [采集会话与检测帧身份回归](collection_frame_identity_validation_20261011.md)
- [加载模型与采集会话的字节身份](collection_model_identity_validation_20261011.md)
- [D24数据集字节与摘要一致性](dataset_snapshot_validation_20261011.md)
- [D25模型检查同字节身份](model_inspection_validation_20261011.md)
- [Native Workspace CPU evaluation and file-path boundary](model_workspace_cpu_validation_20261011.md)
- [Actual CPU resume after cancellation](training_resume_cpu_validation_20261011.md)
- [D29 训练验证阶段取消](training_validation_cancel_validation_20261011.md)
- [U-ENV1 固定训练版本声明去重](training_version_contract_validation_20261011.md)

### 控制、输入与弹道

- [D28诊断跨代与缺帧分段](aim_diagnostic_epochs_validation_20261011.md)
- [X反向缩放候选的有界回放](aim_reverse_scale_validation_20261011.md)
- [候选确认与编辑器采用验证](candidate_adoption_validation_20261011.md)
- [Composite producer/binder 输入字节快照](composite_input_snapshot_validation_20261011.md)
- [D22停止超时与清单提交一致性](input_training_commit_validation_20261011.md)
- [D23输入训练线程启动失败](input_training_start_failure_validation_20261011.md)
- [Movement热配置、取消及ACK交错](movement_config_cancel_validation_20261011.md)
- [弹道文件选择器取消验证](recoil_dialog_cancel_validation_20261011.md)
- [文件选择取消与后台回收复核](recoil_dialog_lifecycle_validation_20261011.md)
- [D16-E1 独立来源许可验证](recoil_source_epoch_validation_20261011.md)
- [在途Recoil位移遇到源换代](recoil_source_receipt_validation_20261011.md)
- [完整Runtime CPU合成闭环回归](runtime_cpu_plant_validation_20261011.md)
- [来源信任断点回归快照](source_trust_validation_20261011.md)
- [源变化与LEFT UP回执组合](source_up_receipt_validation_20261011.md)
- [F01 后续：观测发布与决策时间交错](trigger_observation_clock_validation_20261011.md)

### 工具、构建与审计

- [异步夹具的观察边界](async_fixture_observation.md)
- [2026-10-10 审计修复验证快照](audit_validation_20261010.md)
- [独立急停采样报告收尾与取消](auto_stop_capture_finalize_validation_20261011.md)
- [U-ID1 构建身份补齐与实际依赖核对](benchmark_artifact_identity_validation_20261011.md)
- [D07 Runtime报告字节绑定验证](benchmark_evidence_snapshot_validation_20261011.md)
- [D02 基准报告所有权子范围](benchmark_output_ownership_validation_20261011.md)
- [D18 正式CLI停止轮次验证](console_stop_epoch_validation_20261011.md)
- [D30 索引保存失败保留清理事实](debug_cleanup_result_validation_20261011.md)
- [D16-E1 Counterpulse来源断点验证](debug_source_epoch_validation_20261011.md)
- [D33 evaluator时间合同](evaluator_time_contract_validation_20261011.md)
- [D06/D11 来源绑定与人工观察验证](evidence_source_validation_20261011.md)
- [D04/D07 正式证据链验证快照](evidence_validation_20261011.md)
- [2026-10-10 有界热点性能验证](hotspot_performance_20261010.md)
- [鼠标基准末对取消与报告关闭](mouse_benchmark_finalize_validation_20261011.md)
- [D09 远端Prepare实际入口配对验证](prepare_activation_pair_validation_20261011.md)
- [D08 Prepare提交状态与恢复材料验证](prepare_commit_accounting_validation_20261011.md)
- [Probe计划字节快照回归](probe_plan_snapshot_validation_20261011.md)
- [D34 producer 完成时间戳溢出](producer_completion_overflow_validation_20261011.md)
- [D32 producer整数域校验](producer_integer_bounds_validation_20261011.md)
- [D26序列容量与D17-E2关闭检查](sequence_file_budget_validation_20261011.md)
- [Windows 长路径离线回归](windows_long_path_validation_20261011.md)
