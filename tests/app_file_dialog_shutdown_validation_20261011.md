# 文件选择期间的完整应用关闭集成验证

任务 `XEN-RECOIL-DIALOG-001` R3，沿用独立分支 `codex/recoil-dialog-cancel-20261011`，从 `641add0` 继续。
补齐 [R2](recoil_dialog_lifecycle_validation_20261011.md) 明确保留的完整 App 关闭验证缺口。本轮没有发现新的生产关闭缺陷；
新增可重复运行的生产入口集成测试及 CTest 登记，生产 `main.cpp` 和取消实现最终无改动。

## 真实入口与观察点

`app_file_dialog_shutdown_tests` 的 console `wmain` 调用直接编译、未宏替换的生产 `wWinMain`，链接与应用相同的业务库和资源。
它实际运行 Overlay 初始化、主循环、关闭请求、后台取消与 poll 屏障、设备/日志清理和窗口销毁。
同线程 Win32 timer 通过真实消息泵安装现有 ImGui context hooks；随后保留为失败观察器，在入口返回后移除。
通过 ImGui 事件队列和已提交控件矩形依次点击“调试 → 弹道工具 → 导入已有曲线 JSON → 选择曲线文件”。
没有复制退出循环、替换取消函数、读取 RecoilPanel 私有 busy，或添加生产测试开关。

每次使用全新数据目录和 `XEN_DATA_ROOT`。配置禁用物理输出、辅助功能和快捷键，不启动 Runtime、模型推理或真实设备。
测试需要可创建 Win32/D3D11 窗口的本地桌面；不因无桌面而静默跳过。

两种场景分别保持根选择器、其真实“文件不存在”提示打开，固定 picker HWND、owner/线程身份，并在关闭前持有实际线程的 SYNCHRONIZE 句柄。
成功路径仅向真实主 HWND 连续发送两次 `WM_CLOSE`，不直接关闭选择器或提示。
ImGui Shutdown hook 发生于主 HWND 销毁前：当场确认主窗仍在、选择器和提示消失、原 picker 线程零等待返回 `WAIT_OBJECT_0`，不通过额外等待补救。
`wWinMain` 返回后检查返回值为 0、主窗和 ImGui context 消失、`EnumWindows` 成功且本进程顶层窗口为空。

关闭预算为 3 秒，在 timer、Shutdown hook 和入口返回后分别核对，避免同步阻塞期间没派发 timer 而漏报。
整个场景预算 30 秒，CTest 硬上限 45 秒。超时救援仅关闭已识别的自身窗口，永久记失败；不使用强杀线程或进程取得通过。
每次输出唯一子目录中的 `shutdown-witness.json`，记录场景准备、主窗关闭、线程/窗口状态、失败和救援事件；重复执行不覆盖历史。

## 本地结果

Windows、MSVC 19.51、Release、既有 NVIDIA 配置，同一隔离构建目录。

| 场景 | 关闭前状态 | 主窗销毁前回收见证 | 入口返回 | 最终结果 |
|---|---|---|---|---|
| `app_file_dialog_shutdown_picker` | 选择器可见，主窗禁用，线程仍运行 | 124 ms：picker 消失、线程 signaled、主窗仍在 | 138 ms，返回 0，无遗留窗口/context | 通过，无救援 |
| `app_file_dialog_shutdown_error` | 不存在文件已提交，提示可见，picker 禁用，线程仍运行 | 141 ms：提示及 picker 消失、线程 signaled、主窗仍在 | 155 ms，返回 0，无遗留窗口/context | 通过，无救援 |

以上耗时为本次单次观测，不是性能承诺。两项 CTest 共 1.58 秒，2/2 通过，Release 集成目标构建通过。
首轮两个直接运行场景同样通过；最终结果来自补齐失败判据之后的 CTest。

反证阶段临时停用生产主循环关闭分支及最终退出屏障中的两处 `overlay.cancel_background()`，保留其他逻辑：
picker 和 error 均在关闭后 3 秒判失败，救援完成后仍返回 1，见证 `failed=true`、`rescue_used=true`，无 `passed` 事件。
恢复两处调用并确认 `main.cpp` 无差异后重新构建，得到上述绿灯。该故障注入证明测试敏感性，不计为新生产缺陷。

独立只读复核发现并补齐两项测试缺口：检查 `EnumWindows` API 返回值；在非 timer 观察点检查实际关闭耗时。最终复核确认闭合。

## 可重复命令

```powershell
cmake -S . -B <原构建目录>
cmake --build <原构建目录> --config Release --target app_file_dialog_shutdown_tests --parallel 6
ctest --test-dir <原构建目录> -C Release -R '^app_file_dialog_shutdown_(picker|error)$' --output-on-failure
# 显式选择证据根目录；程序在其下创建唯一子目录
& <原构建目录>/Release/app_file_dialog_shutdown_tests.exe <独立证据根目录> picker
& <原构建目录>/Release/app_file_dialog_shutdown_tests.exe <独立证据根目录> error
```

## 来源与验证边界

[SetTimer](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-settimer) 规定回调经安装线程的消息分派执行；
[WM_CLOSE](https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-close) 是请求，不能代表已经回收。
[OpenThread](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-openthread) 与
[WaitForSingleObject](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitforsingleobject) 提供实际线程终止观察。
[EnumWindows](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-enumwindows) 的失败返回也必须纳入判据。
观察时点按 [ImGui v1.92.9b 原作者源码](https://github.com/ocornut/imgui/blob/v1.92.9b/imgui.cpp) 和当前 Overlay 销毁顺序确定；
内部 hook 不承诺跨版本兼容，升级时需复核。
[CMake add_executable](https://cmake.org/cmake/help/latest/command/add_executable.html) 与微软
[/ENTRY](https://learn.microsoft.com/en-us/cpp/build/reference/entry-entry-point-symbol?view=msvc-170) 支持保留默认 CRT、console 入口调用生产函数的方案。
本轮独立实现测试，仅使用已有 ImGui MIT 依赖及平台 API，没有复制外部实现或新增依赖。

这是生产 `wWinMain` 的完整应用源码集成验证，不是直接启动发行 `Xen.exe` 的 CRT/subsystem 或 Launcher 发布验收。
覆盖本机 Shell 的普通选择器和文件不存在提示、主窗 WM_CLOSE；不外推任意 Shell 扩展/深层提示、系统关机、渲染失败或 Runtime 正在运行时的退出。
没有执行全量 CTest、多 Provider、模型/离线指标、真实设备/KMBOX、远程部署或主分支合并。

后续 [R4 实际 EXE 验证](xen_exe_shutdown_validation_20261011.md) 已直接启动构建的 `Xen.exe`，
以进程外见证覆盖上述两类关闭场景，补齐实际 CRT/GUI 入口验证；R3 本文的历史执行范围保持不变。
