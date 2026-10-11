# 候选确认与编辑器采用验证

基线 `bfdc4e4ed416ae11dbd9021c1ef6c58026580663`；独立分支 `codex/candidate-adoption-20261011`。
本轮在 Windows、MSVC 19.51、Release、既有 NVIDIA 构建配置执行；不修改或合并 main，不部署或启动真实设备。

## 修复与兼容

- 已确认文件被修改后仍可采用：旧解析器见到 `verified=true` 就重新调用人工确认。现在 `confirmation_sha256` 绑定版本、武器、灵敏度及节点等实际内容；读取只校验，缺失或失配时保留为可编辑候选，拒绝采用。内存对象修改后也不能借另存递增版本沿用确认。
- 保存候选后加载被待用组拒绝，却清组、误报成功并准备测试：私有加载入口现在返回完成结果，保存文件名先保持局部，成功载入后才更新选择、清理数据并准备测试。失败时明确显示已保存的文件和未载入原因。下拉、导入及优化遵循相同提交顺序。
- 旧精简布尔确认需要在编辑器重新确认；schema1 实测格式不变。历史离散迁移先验证原确认，再对等价迁移结果绑定新内容，不启动物理输出。

## 回归证据

原生 Store 反例在旧实现退出 1，覆盖连续/离散、节点/时刻/灵敏度/身份/版本/缺摘要及内存修改。修复后通过，另保留排版变化与未改内容的正常控制。
生产面板进程内点击反例在旧实现失败：“候选载入被待用组拒绝后不得误准备另一条曲线”。修复后候选专项通过，核对旧选择、持久化选择、待用组、候选、失败反馈；明确清组后重试，验证请求路径与新选择一致，并继续覆盖五组优化、追加阶段与历史记录。

11 项相关 CTest 通过：`recoil_tests`、`recoil_store_tests`、`import_recoil_profiles_tests`、`migrate_legacy_recoil_profiles_tests`、`recoil_tuner_tests`、`recoil_wall_analysis_tests`、`recoil_cli_tests`、`recoil_editor_job_tests`、`recoil_worker_tests`、`aim_recoil_integration_tests`、`runtime_aim_frame_tests`。
迁移测试包含 17 种武器名的自制数据，34 个迁移前后文件经真实 C++ 解析器检查；来源占用和 CLI 专项沿用正式生产入口。
相关测试目标、`debug_workspace_ui_preview` 和 `xen_app` Release 构建通过。

精确 UI 入口（输出目录使用新的独立目录）：

```powershell
<build>/Release/debug_workspace_ui_preview.exe <独立输出目录> --recoil-candidate-flow
```

## 验证限制

完整 `--recoil-flow` 在进入候选场景前触发既有文件选择器取消断言：2 秒后窗口仍存在。原断言保留，未修改取消生产机制，不能宣称完整 UI 流程通过；单独跟踪为 `XEN-RECOIL-DIALOG-001`。
后续 `XEN-RECOIL-DIALOG-001` 已在独立分支修复并通过完整流程，见[取消修复验证](recoil_dialog_cancel_validation_20261011.md)；上述失败保留为本任务当时的结果。
专项中同时修正了三处过期命名断言：文件按正式武器和 revision 命名，发数仍检查曲线 id，没有删除原覆盖。
未执行全量 CTest、多 Provider、真实游戏/KMBOX、远程部署或故障注入全矩阵。软件确认和自动回归不代表视觉体验或真实闭环通过。内容摘要用于识别改动，不是防恶意重算的签名。
