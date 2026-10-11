# 文件选择取消与后台回收复核

任务 `XEN-RECOIL-DIALOG-001` 的 R2，沿用独立分支 `codex/recoil-dialog-cancel-20261011`。
复核固定差异 `bd7fee98757c3d443ad19fedbf4da4e8d444c10d...ffadd38244c82241da075167412297218415a510`；R2 改动从后者继续，main 保持不变。
验证环境为 Windows / MSVC 19.51 / Release / 既有 NVIDIA 配置，复用同一隔离构建目录。

## Standards 轴

未发现已证实的标准违规或值得提出的代码异味。原正常路径的 TimerScope、COM 与 Job 释放顺序正确；
UI 观察 FINISHED 后才回收编辑状态，并重建下一任务的取消标记，没有发现普通重开串扰。
本轮新关闭目标限定为当前 COM 对话框及其直接拥有、同 STA 的可用提示窗口；保持原有回收屏障。

## Spec 轴

发现 1 个 P2 行为缺陷并修复：文件选择器弹出“文件不存在”模态提示时，根窗口被禁用，
向根窗口投递一次 WM_CLOSE 未结束 Show，后台任务一直 busy，无法再次打开文件选择器。

原实现的新增反例返回 1，触发原有 2 秒关闭断言。仅去掉 `close_posted` 并反复关闭根窗口仍返回 1。
临时探针证实 COM 与测试持有同一根 HWND，根窗口 enabled=0，存在其拥有的提示窗口；排除错误 HWND 和仅一次投递时机的问题。

修复在每个 timer tick 重新取当前窗口：根窗口禁用时，查询 `GW_ENABLEDPOPUP`，核验直接 owner 和 STA 线程后正常发送 WM_CLOSE；
提示退出后在后续 tick 关闭根窗口。移除“一次投递成功即不再处理取消”的锁存，仍以 Show 返回和 Job 完成为回收依据。
未使用强制销毁、终止线程、点击肯定按钮、全局标题匹配或新的公有接口。

两项证据缺口已补：此前只看 busy/窗口，现持有具体原生线程句柄检查退出；此前原生 owner 为空，现增加真实 Win32 宿主的禁用/恢复检查。
Job 模板补充 weak_ptr 回归，分别验证任务、value 和回调捕获对象的所有权最终释放。未发现未解决实现问题或超范围改动。

## 回归与夹具

| 场景 | 结果 |
|---|---|
| 原生取消 | 无 owner / 有原生 owner 各 1 次通过 |
| 排队尚未 poll 取消 | 1 次通过，无选择窗口 |
| 首次发现窗口即取消 | 连续重开 3 次通过 |
| 窗口可见后取消 | 连续重开 3 次通过 |
| 文件不存在提示期间取消 | 无 owner 1 次、有原生 owner 3 次通过 |
| 重复取消 | 每次全局取消均重复调用，保持幂等 |
| 取消状态 | 13 次均无 Prepare、busy 回收、原选择与目录文件名集合不变、无遗留选择窗口 |
| 原生线程与宿主 | 14 次实际窗口操作均在关闭前持有线程句柄，之后确认线程退出；有 owner 时检查宿主恢复，可见取消还检查曾实际禁用 |
| 页面回收后重新打开 | 取消回收后销毁并重建 RecoilPanel，目录内选择及外部导入仍成功 |
| Job 所有权回收 | 成功/异常 × 完成后释放/运行中提前释放，4 个组合通过 |
| 完整流程 | `--recoil-flow` 通过，原候选、优化、追加阶段及历史回归保留 |
| Release / CTest | UI、Job 测试及 `xen_app` 构建通过；`recoil_editor_job_tests`、`overlay_tests` 2/2 通过 |

原生系统错误提示可能与选择器使用相同标题。最初实验的夹具误认提示，未能发出预期取消；该轮只记夹具失败，不计生产红灯。
夹具改为固定首次捕获的 picker HWND，才得到上述实际生产反例。超时救援限定本次窗口，救援后始终失败，不借救援得到绿灯。
重复的文件名控件查找复用同一助手，操作模式用具名枚举；临时 DEBUG 探针全部移除。

实际命令形式（每次使用新输出目录）：

```powershell
cmake --build <build> --config Release --target debug_workspace_ui_preview recoil_editor_job_tests --parallel 6
& <build>/Release/debug_workspace_ui_preview.exe <独立输出目录> --recoil-flow
ctest --test-dir <build> -C Release -R '^(recoil_editor_job_tests|overlay_tests)$' --output-on-failure
cmake --build <build> --config Release --target xen_app --parallel 6
```

## 平台依据与边界

[GetWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getwindow) 提供 owned popup/owner 关系；
[WM_CLOSE](https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-close) 由窗口自行处理，投递不能替代完成。
不以 [GetLastActivePopup](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getlastactivepopup) 作为生产唯一查找：输入窗口已有 owner 时它可能返回自身。
线程回归使用 [OpenThread](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-openthread) 的 SYNCHRONIZE 权限，
通过 [WaitForSingleObject](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitforsingleobject) 有界零等待轮询；等待期间继续处理宿主消息。
采用 API 契约独立实现，无外部源码复制或新增依赖，许可证边界沿用首轮报告。

V3 直接消费者及 V4 STA/消息循环专项通过；本轮检查的是实际组件、原生宿主及同类 cancel→poll 回收协议，
没有运行完整 Overlay/App 关闭集成测试，其退出屏障仅做源码复核。
当前错误提示反例通过不保证任意 Shell 扩展、任意深层或不可关闭提示都能退出；没有强制接受确认、放宽超时或提前清 busy。
未执行全量 CTest、多 Provider、真实设备/游戏/KMBOX、远程部署或主分支合并。
