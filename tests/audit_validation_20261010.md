# 2026-10-10 审计修复验证快照

本报告记录固定源码范围，不表示未来提交、全量测试或真实设备验收。

- 审计基线：`e692e58767d8cc5f8de4a294104ba0a37c320625`。
- 修复终点：`2d66fd40ad0f2a1d4fdaa9010b2a402e5c5850bb`。
- 四批修复：`18a9f632c1c745e262379541dda3d144290d2d9e`、`00b5a1f0f71867c3e8c0a0b4d30d5c0e2fdc339e`、`bc2c75fa5cc75e4264272d7913262b8fd58b1556`、`2d66fd40ad0f2a1d4fdaa9010b2a402e5c5850bb`。
- 环境：Windows、VS2026/MSVC19.51、Release、现有 NVIDIA 构建配置；Python3.12.10、PowerShell5.1/7。
- 验证层：生产接口专项、确定性合成夹具、假设备故障注入、实际链接；未使用真实游戏或设备输入。

## 逐项结论

下表每项均已修复并通过相关回归；测试名是仓库正式入口或独立脚本。

| 发现 | 修复 | 验证入口 |
|---|---|---|
| F01 | Collector 故障切模型统一收尾 | runtime_model_reload_test |
| F02/F03 | 审核图像复制身份与冻结真值 | model_data_pipeline_tests、model_data_review_tests、集成夹具 |
| F04 | 五类发布入口逐 EXE 身份和复制冻结哈希 | publish_worker_update_tests、release_bundle_tests、reference_compare_package_tests、mouse_effect_probe_b_publish_tests |
| F05 | 无帧 FAILED 归档及重复终态判重 | session_archive_tests |
| F06 | 显式冻结数据根，启动环境恢复 | release_contract_tests、live_game_data_root_tests |
| F07 | 独立校准 L1 滚动窗口 | recoil_calibration_tests、recoil_worker_tests |
| F08 | HID 错误报告恢复不制造边沿 | mouse_tests、input_safety_tests |
| F09 | 极值类别和掩码坐标转换 | detector_tests |
| F10 | 参考输出和阈值有限性 | model_reference_compare_tests |
| F11 | 双文件句柄发布和归属回滚 | evidence_publication_tests、composite producer |
| F12 | Unicode 日志目录 | log_tests |
| F13 | 空 gap 的轨迹与丢失统计 | input_training_tests |
| F14 | RTV 失败锁存及 Resize 解绑 | overlay_tests、实际应用构建 |
| F15 | 共享双端更新事务与并行修改保护 | publication_script_tests、test_dual_machine_scripts.ps1 |
| F16 | 最终报告保存失败不伪成功 | auto_stop_probe_tests |
| F17/F18 | 官方 CRT、配对基准发布与安全回滚 | publication_script_tests |
| F19 | OBS 启用或未知滤镜拒绝 ROI 声明 | obs_capture_binding_tests |
| F20 | 旧 F9 占用迁移 | config_tests |
| F21 | 完整控制中心配对 | aim_production_red_evaluator_tests |
| F22/F23 | 发布失败恢复及逐帧路径哈希 | test_release_transfer_and_aim_manual.ps1 |
| F24 | ROI 中心向下取整 | annotation_roi_tests |
| F25 | 未知污染观察拒绝 | mouse_effect_probe_b_analysis_tests |
| F26 | 所选源最新完整采集配置 | mouse_effect_probe_b_composite_phase_prepare_integration_tests |
| F27 | 签名之外严格事件 schema | mouse_effect_probe_tests |
| F28 | CSV 摘要与同一次读取解析 | mouse_effect_probe_a2_calibration_tests、mouse_effect_probe_physical_analysis_tests |

## 收尾证据及限制

最后五类发布专项 CTest 全部通过，总计56.80秒；Worker专项471个断言通过，全量包17个新增身份拒绝用例、参考包6个拒绝用例及合法包通过。独立复核确认 Launcher 复制替换缺口和双文件回滚竞态闭合。

实际应用、校准及受影响证据工具 Release 构建通过。最终重新链接的应用、Sequence、CompositeSeal侧身份绑定修复终点、`git_dirty=false`，且大小与 SHA256 与实际文件一致。

修复终点的[公开文档 CI 成功](https://github.com/hpsazz1/Xen/actions/runs/38040700332)。该 CI 不编译 C++；上述构建及运行证据来自本机专项。

未执行全量 CTest、全量 Provider 矩阵、真实设备或游戏长时验收、SSH/SMB部署；没有合并、发布或部署。完整原始本地任务报告属于忽略文档，未随源码推送；本文件为可引用的精简历史验证快照。
