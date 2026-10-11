# 实际 Xen.exe 文件选择期间的启动与退出验证

任务 `XEN-RECOIL-DIALOG-001` R4，2026-10-11，独立分支 `codex/recoil-dialog-cancel-20261011`。
实际启动当前分支构建的 `Xen.exe`，选择器和原生错误提示保持打开时分别请求主窗口退出，两种场景通过。
本轮未发现新的生产缺陷；新增可复用的外部验证程序，生产入口及取消实现无改动，未操作主分支。

## 验证入口与判据

[xen_exe_shutdown_probe.cpp](xen_exe_shutdown_probe.cpp) 通过 `CreateProcessW` 启动传入的实际 EXE，
保留正常 CRT 和 GUI 入口，不调用或链接生产 `wWinMain`，不注入 ImGui hook，也不增加生产测试开关。
先创建挂起进程，加入探针独占 Job 后恢复主线程；恢复前挂起计数必须为 1。
每次使用全新绝对输出目录，生成合成曲线和独立 `XEN_DATA_ROOT` 配置，禁用物理输出、辅助功能和快捷键，不启动 Runtime。

从真实界面依次打开“调试 → 弹道工具 → 导入已有曲线 JSON → 选择曲线文件”。
`error` 场景在文件名框提交本次目录下不存在的 `missing-profile.json`，保留真实“找不到文件”提示。
探针按启动 PID 固定主窗、picker 和提示的 HWND、owner、线程关系，并在 picker 活跃时持有其线程同步句柄。
外部操作者确认场景就绪后创建 `close.request`；探针重新核对窗口身份，成功路径仅向主窗投递两次 `WM_CLOSE`。
不提前关闭 picker 或点击提示确认，也不向应用注入输入队列。

准备预算为 300 秒，完整关闭预算为 3 秒。关闭期间以 1 ms 间隔采样，要求在一个紧凑样本的前后都观察到主进程未结束、
主窗仍存在，同时 picker 线程已 signaled，picker 和提示均消失。随后等待进程结束、读取退出码 0、成功枚举且无所属顶层窗口。
释放进程和线程句柄后，在关闭 Job 句柄之前确认 `ActiveProcesses=0`。见证记录到 `state.json`、`witness.json`。
任何超时、未捕获退出前样本、救援、强制终止或回收未知都不算通过；成功不得依赖 `KILL_ON_JOB_CLOSE` 兜底。

该入口需要外部真实桌面操作，明确作为桌面专项，不登记为无人值守 CTest。既有自动化入口见
[R3 源码集成验证](app_file_dialog_shutdown_validation_20261011.md)。

## 本地结果

Windows、MSVC 19.51、Release、既有 NVIDIA 配置，复用隔离构建目录。
构建 `xen_app`、`xen_exe_shutdown_probe` 成功，两个实际 EXE 场景均返回 0。

| 场景 | 主窗出现 | 请求关闭后观察到线程和子窗回收，主窗仍在 | 进程退出 | 最终检查 |
|---|---|---|---|---|
| picker，PID 20756 | 启动观察计时 162 ms | 142 ms | 173 ms，退出码 0 | 无所属顶层窗口，Job 活动进程 0，无救援/强制终止 |
| error，PID 7884 | 启动观察计时 167 ms | 141 ms | 172 ms，退出码 0 | 无所属顶层窗口，Job 活动进程 0，无救援/强制终止 |

耗时来自本次单次观察，不是性能承诺；取整后的事件时间差可能与独立关闭计时差 1 ms。
两个场景结束后再次检查，`Xen` 与 `xen_exe_shutdown_probe` 进程数均为 0，桌面窗口列表无该应用窗口。
错误提示的直接 owner 是 picker、同属 picker STA 且 picker 被禁用；真实画面确认“missing-profile.json 找不到文件”。

本轮真实界面输入使用 Computer Use。该工具的窗口截图连续两次超时，重新选择并激活窗口后仍失败；
改用 Pillow 对已核验 HWND 只读取图、查看画面，点击和键盘输入仍由 Computer Use 执行。
该截图工具限制没有作为应用失败，也没有代替两份外部生命周期见证。

独立只读复核发现并修正探针失败路径的状态判断：子进程尚未加入 Job 时，空 Job 不能掩盖未确认的进程退出；
未确认退出保留句柄供失败兜底。修正后重新构建，再完成上述两个场景。
额外执行“传入已有输出目录”负例：退出码 1，既有 `state.json`/`witness.json` 哈希不变，未启动新 Xen。
该负例验证证据保护，不是本轮对生产取消逻辑的故障注入；取消失效的红绿证据属于 R3。

实际受测 EXE 的基线提交为 `b30a8fc26cca4877c5d4a16d5fa7c4b2b7e4fe08`，构建身份 `dirty=true`，
差异为本轮测试/CMake 工作，生产 `main.cpp` 与 `recoil_panel.cpp` 无差异。
受测文件大小为 4305408 字节，SHA-256：
`fb8cd352f23d9ff13d754c6f4f5d5540bd6fea1181989a17f19d038901ee8947`。
探针 SHA-256：`cfdf9863e73a77fdf2a922fa1aee439940fe809409899dda65a9be39af170422`。
这描述实际测试时的文件，不将后续提交哈希倒填为该二进制身份。

## 重复验证

在已有本地桌面和依赖配置的构建目录执行；尖括号参数替换为实际绝对路径。

```powershell
cmake -S <source> -B <build>
cmake --build <build> --config Release --target xen_exe_shutdown_probe --parallel 6
# 终端一：输出目录必须尚不存在，每个场景各用一个新目录
& <build>/Release/xen_exe_shutdown_probe.exe <build>/Release/Xen.exe <全新绝对输出目录> picker
# error 场景另起一轮，将末尾参数改为 error，并使用另一个新目录
```

按上面的真实界面路径打开 picker；`error` 还要提交本轮目录下不存在的文件并保留提示。
不要启动 Runtime。终端一出现 `ready` 后，在终端二核对并发起退出：

```powershell
$runDirectory = '<本轮全新绝对输出目录>'
$state = Get-Content -LiteralPath (Join-Path $runDirectory 'state.json') -Raw | ConvertFrom-Json
if ($state.status -ne 'ready') { throw '场景未就绪' }
New-Item -ItemType File -Path (Join-Path $runDirectory 'close.request') -ErrorAction Stop
```

等待终端一结束，检查退出码、`witness.json` 的 `status=passed`、退出前观察事件、空窗口列表、
Job 归零及 `rescue_used=false`、`forced=false`。失败报告必须保留，不能靠补关窗口把同一次失败改成通过。

## 来源和验证范围

[CreateProcessW](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessw)、
[ResumeThread](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-resumethread) 约束实际启动与句柄归属；
[Job Objects](https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects) 和
[Job 计数](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_basic_accounting_information) 约束失败兜底及释放引用后的归零观察。
[WaitForSingleObject](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitforsingleobject) 提供终止见证，
[ExitProcess](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-exitprocess) 会先结束其他线程再标记进程结束，
因此“线程 signaled、进程尚未 signaled”单独不足以证明协作清理。本轮还观察主窗存活，结合当前生产退出顺序及 R3 内部关闭屏障证据判断；
不将外部瞬时样本提升为所有业务对象已析构的证明。未观察到时判验证失败，不倒推出应用一定泄漏。

Job 覆盖本轮纳入的进程树，不涵盖独立 Shell broker；顶层窗口枚举不等于全部隐藏子窗枚举。
本轮仅验证当前构建实际 EXE、普通选择器/文件不存在提示和主窗 `WM_CLOSE`，独立组合平台 API，未复制第三方实现或新增依赖。
没有执行 Launcher 发布入口、系统关机、Runtime 运行中退出、其他 Shell 扩展、全量 CTest、多 Provider、
模型/离线回放指标、真实设备/KMBOX、远程部署或主分支合并。
