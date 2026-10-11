# 弹道文件选择器取消验证

任务 `XEN-RECOIL-DIALOG-001`，基线 `bd7fee98757c3d443ad19fedbf4da4e8d444c10d`，独立分支 `codex/recoil-dialog-cancel-20261011`。
在 Windows、MSVC 19.51、Release、既有 NVIDIA 配置执行；源码与构建复用隔离目录，未修改或合并 main。

本文保留首轮结果；后续复核发现并修复内嵌提示期间取消失败，新增真实宿主与线程/对象回收证据，见 [R2 生命周期复核](recoil_dialog_lifecycle_validation_20261011.md)。首轮的一次投递锁存已被 R2 移除。

## 原始失败与归因

基线完整 `--recoil-flow` 返回 1：全局取消后 2 秒窗口仍存在，测试必须再次发送原生取消才能退出。
原断言和失败救援均保留；救援成功仍将该轮判为失败。

临时探针确认取消标记到达正在执行的任务，TimerProc 在创建 COM 对象的同一 STA 持续执行；
`IFileDialog::Close` 反复返回 `S_OK`，但 `Show` 直到测试救援才返回 `ERROR_CANCELLED`。
`IOleWindow::GetWindow` 与测试观察到的 HWND 一致，取消回调时窗口可见且启用。
因此排除取消标记丢失、定时器未执行、过期窗口句柄，以及“仅仅没有等到窗口显示”的解释。
这些探针均已移除，不成为生产日志。

## 最小修复及依据

保留原 STA、后台任务及定时器生命周期；取消时从当前 COM 对象取得 `IOleWindow` 的窗口，向该窗口投递 `WM_CLOSE`。
获取窗口或投递失败时下一次定时器重试，投递成功后不重复投递。投递成功不表示任务完成，仍等 `Show` 返回和后台任务结束后回收 busy。
不枚举标题寻找生产窗口，不跨线程调用 COM，不新增线程、依赖或公有接口。

微软 [Close 文档](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ifiledialog-close)仅泛称对话框打开期间的回调，不能把成功 HRESULT 当作本轮已退出的证据。
[Chromium 固定版本的原作者实现](https://chromium.googlesource.com/chromium/src.git/+/refs/tags/136.0.7103.95/ui/shell_dialogs/auto_close_dialog_event_handler_win.cc)指出 Close 预期位于 IFileDialogEvents 回调，其他同线程回调采用窗口关闭消息；此限制属于作者工程说明，未冒充微软文档的明确保证。
窗口取得与正常关闭分别参照 [IOleWindow::GetWindow](https://learn.microsoft.com/en-us/windows/win32/api/oleidl/nf-oleidl-iolewindow-getwindow) 和 [WM_CLOSE](https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-close)。
本轮独立实现这组 API 的最小组合，没有复制 Chromium 类、注释、钩子代码或依赖；其代码使用 BSD-style 许可证。

## 本地验证

| 检查 | 结果 |
|---|---|
| 原始完整 UI 反例 | 修复前返回 1；修复后完整 `--recoil-flow` 返回 0 |
| 原生窗口取消 | 1 次通过 |
| 点击已排队、尚未 poll 就取消 | 1 次通过，未打开原生窗口 |
| 首次发现窗口即全局取消 | 连续重开 3 次通过 |
| 明确等待窗口可见后全局取消 | 与上一场景交替 3 次通过 |
| 每轮取消的状态 | 无 Prepare、busy 回收、已选曲线与目录文件名集合不变、无遗留选择窗口 |
| 取消后正常选择 | 目录内已有曲线与外部文件导入通过 |
| 其余完整 UI 流程 | 原有候选、优化、追加阶段、历史记录断言全部通过 |
| Release 目标 | `debug_workspace_ui_preview`、`xen_app`、`recoil_editor_job_tests`、`overlay_tests` 构建通过 |
| CTest | `recoil_editor_job_tests`、`overlay_tests`，2/2 通过 |

复现形式（每次使用新的输出目录）：

```powershell
cmake --build <build> --config Release --target debug_workspace_ui_preview xen_app recoil_editor_job_tests overlay_tests --parallel 6
& <build>/Release/debug_workspace_ui_preview.exe <独立输出目录> --recoil-flow
ctest --test-dir <build> -C Release -R '^(recoil_editor_job_tests|overlay_tests)$' --output-on-failure
```

本轮首次重新链接时，构建身份检查发现缓存中的旧源码提交，明确拒绝写入身份；按原缓存重新配置同一构建目录后通过，未绕过检查或升级依赖。

## 验证边界

验证级别为 V3 直接消费者与 V4 STA/消息循环专项。首次捕获窗口不保证命中 GetWindow 暂不可用分支；
该分支重试目前由代码契约复核支持，没有宣称 API 失败故障注入通过。
2 秒是此机器的回归上限，不是所有 Shell 扩展或任意负载下的时延保证。
未运行全量 CTest、多 Provider、远程部署、真实游戏/KMBOX 或物理输出；生产面板回归仅生成 Prepare 意图，未启动设备。
