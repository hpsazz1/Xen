# Runtime 运行中的应用退出验证

2026-10-11，任务 `XEN-RECOIL-DIALOG-001` R5，沿用 `codex/recoil-dialog-cancel-20261011`。
实际 Xen.exe 的无设备 Runtime 退出、Runtime 与选择器/错误提示的两个组件组合专项均通过。
本轮没有发现新的生产退出缺陷，改动仅涉及测试、构建登记及说明。

## 明确区分两个验证层

当前产品的 `Overlay::editable` 在 Runtime RUNNING/STARTING/STOPPING 时禁用弹道编辑，
应用启动入口也会拒绝在选择器后台任务未结束时启动 Runtime。模型审核文件选择同样要求停止状态。
因此 Runtime 与选择器同时打开不是当前实际 EXE 可自然进入的界面流程。
用户已明确选择保留限制：实际程序验证 Runtime 退出，测试宿主构造组合验证生产组件回收。
没有放开运行时编辑，也没有把测试宿主结果表述为实际 EXE 的自然界面路径。

- **实际 EXE**：[外部探针](xen_exe_shutdown_probe.cpp) 新增 `runtime` 模式，通过真实界面的“启动”按钮运行当前构建的 Xen.exe。
  不替换 wWinMain/CRT、不注入应用。读取生产诊断归档的连续 RUNNING 分段，确认成功帧和水位增长。
  收到 `close.request` 后先记当前水位，等待另一个新发布 RUNNING 分段且拒绝已存在最终报告的会话，之后才向主窗投递 WM_CLOSE。
  最终核对退出码、所属顶层窗口、独占 Job 活动进程，以及生产正式报告和归档封尾。
- **组合集成**：[Runtime 与文件选择器测试](runtime_file_dialog_shutdown_tests.cpp) 使用真实 Overlay，在 STOPPED 状态通过实际 ImGui 控件打开原生 picker 或文件不存在提示，
  再由测试宿主直接调用其持有的真实 Runtime::start。这有意绕过 App 的启动准入，不修改该准入。
  Runtime 持续处理且提示仍打开时向主窗投递两次 WM_CLOSE，调用生产 Overlay cancel/poll 与 ReportLifecycle stop/poll 接缝。
  在 ImGui Shutdown、主 HWND 尚存在时核对 picker 线程 signaled、弹窗消失、Runtime STOPPED/CLOSED、停止 future 已收集；
  最后核对发送器 join、主窗/context/所属顶层窗口消失。宿主没有创建生产报告对象；报告归档由实际 EXE 层覆盖。

## 无设备输入与观察判据

[共享输入夹具](xen_exe_runtime_fixture.h) 复用仓库自有 [极小 ONNX 模型](fixtures/detector_typeinfo_segmentation.onnx)，
从已有 ORT 构建明确选择 CPUExecutionProvider。每 20 ms 向 `127.0.0.1` 临时端口发送一张 320×320 合成 JPEG，
生产 UDP_MJPEG Capture、Detector、Aim 照常执行；不捕获实际桌面，不读取私人模型或用户配置。
使用 `WIN32_SEND_INPUT + allow_send_input=false`，关闭辅助输出功能、GSI、源状态与控制快捷键，不武装输出。
输入夹具在启动前检查这组配置；生产 Mouse 在 SendInput 调用之前检查输出开关。

处理帧数包含失败帧，因此同时要求 detection_status=SUCCESS、成功样本>0、failed_frames/failed_samples=0、mouse_commands=0、
output_allowed_by_config/output_armed=false。正式最终报告还须 STOPPED、Capture/Mouse CLOSED，
归档 manifest 须 closed/complete=true、accepted_samples=written_samples、各类丢失为0、错误为空。
Runtime::stop 在 join Capture/Pipeline/重载线程、释放模块后发布 STOPPED；应用停止作业再封存报告。
使用这些业务证据与外部进程/窗口见证交叉验证，不凭进程消失推断全部业务线程已协作退出。

实际探针准备上限300秒，组合宿主准备上限45秒；发出主窗 WM_CLOSE 后均以3秒为关闭预算。
等待下一个归档段属于验证准备时间（通常约5秒），不算应用响应 WM_CLOSE 的退出耗时。
任何救援永久判失败；实际探针成功证据在关闭 KILL_ON_JOB_CLOSE 句柄之前确认 Job 归零，组合宿主不使用强杀取得绿灯。

## 本地结果

Windows、MSVC 19.51、Release，复用已有 NVIDIA 构建目录的依赖，仅执行现有 CPU Provider 路径。
受影响目标构建通过，组合专项 CTest 2/2 通过，总计3.75秒。

| 验证层与场景 | Runtime 处理证据 | 关闭后回收见证 | 最终结果 |
|---|---|---|---|
| 实际 Xen.exe，Runtime 运行 | 关闭验证信号后478→639帧，最终644个成功帧；失败/输出0 | WM_CLOSE后200 ms进程返回0，正式报告STOPPED/CLOSED，644/644归档完整 | 无所属窗口、Job活动进程0，发送器joined，无救援/强杀 |
| 组件宿主，picker打开 | 3→4帧且4个成功样本，停止后11帧；失败/输出0 | 283 ms主窗仍在时picker线程结束、窗口消失、Runtime已停止；302 ms宿主窗口销毁完毕 | 停止future与sender已回收，无所属窗口，无救援 |
| 组件宿主，文件不存在提示打开 | 3→4帧且4个成功样本，停止后12帧；失败/输出0 | 300 ms主窗仍在时提示/picker消失、线程结束、Runtime已停止；318 ms宿主窗口销毁完毕 | 停止future与sender已回收，无所属窗口，无救援 |

耗时仅代表本次观测，不是性能指标。实际程序画面确认运行中弹道工具保持禁用；所有测试结束后窗口列表与 Xen/探针/组合测试进程检查均无残留。
Computer Use 截图两次超时，重新选窗后仍失败；沿用只读 Pillow HWND 截图观察画面，界面输入仍由 Computer Use 执行，没有把截图故障记为应用故障。

准备阶段第一次失败发生在夹具的完整帧尺寸配置：FULL_FRAME_1_TO_1要求源尺寸留零，由JPEG解析。
修正夹具并保留失败证据，应用当时未启动；同时修正校验函数与错误字符串的求值顺序，防止漏报校验原因。
独立复核补强旧RUNNING证据的新鲜度，以及组合失败救援的PID/原线程/窗口类和所有权核验。
实际执行“ready后先点停止”的负例，验证器因STOPPED归档段返回1，失败清理无强杀且无残留；没有复现假阳性，不将此负例描述为生产缺陷。
增加关闭前新水位及最终报告排除判据后重新执行实际 EXE，得到表中的最终644帧结果。

## 可重复执行

```powershell
cmake -S <source> -B <原构建目录>
cmake --build <原构建目录> --config Release --target xen_exe_shutdown_probe runtime_file_dialog_shutdown_tests --parallel 6
ctest --test-dir <原构建目录> -C Release -R '^runtime_file_dialog_shutdown_(picker|error)$' --output-on-failure
# 显式组件场景：证据根内自动建立唯一run子目录，可重复执行
& <原构建目录>/Release/runtime_file_dialog_shutdown_tests.exe <绝对证据根> picker <source>/tests/fixtures/detector_typeinfo_segmentation.onnx
& <原构建目录>/Release/runtime_file_dialog_shutdown_tests.exe <绝对证据根> error <source>/tests/fixtures/detector_typeinfo_segmentation.onnx
# 实际EXE：输出目录必须尚不存在
& <原构建目录>/Release/xen_exe_shutdown_probe.exe <原构建目录>/Release/Xen.exe <全新绝对输出目录> runtime <source>/tests/fixtures/detector_typeinfo_segmentation.onnx
```

实际 EXE 模式在界面点击“启动”，保持物理输出禁用；待探针打印 ready，在另一终端核对 state.json 后创建 close.request，
继续等待新RUNNING段及最终见证，不能先点“停止”再将其计为运行中退出。完整操作形式见 [R4探针说明](xen_exe_shutdown_validation_20261011.md)。
两个组合 CTest 设置 RUN_SERIAL、TIMEOUT=60，结果位于原构建目录的 runtime-file-dialog-shutdown-tests/run-*/shutdown-witness.json。
既有 R3 和 R4 场景没有删除或跳过；本轮没有重跑它们或全量 CTest。

## 受测身份和证据范围

实际 Xen.exe 基线为 `2096fc31d670e40f6390df555ded12a45094bf68`、dirty=true（本轮测试/CMake差异），大小4305408字节，
SHA-256为 `3b92bc72c1344e9cc94194aa21f5f4374bf51fc5345504118b16cd63e8c0f9eb`。
探针SHA-256为 `fba16eba2b15686a59bae75e1f48e3cbbf3823a4ff8641bf9aa32542e30c860d`，
组合测试SHA-256为 `bb3cbe7be5d570ec1adb87b64f3ba988bf0c92bcaee0e2b2bda7a3a942ff0222`。
模型SHA-256为 `223c2e3dea8835b98ccd1c4b8663fa430cf940f785d72a345d75ff3ff8ee394a`。
身份是测试时实际文件，不倒填后续源码提交哈希；未修改生产 Runtime、Overlay、App 或 Mouse。

[SendInput](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendinput) 与
[sendto](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-sendto) 约束物理输入与回环数据发送；
数据报发送成功本身不证明推理，本轮以生产成功样本为准。
[ORT执行后端](https://onnxruntime.ai/docs/execution-providers/) 与
[线程管理](https://onnxruntime.ai/docs/performance/tune-performance/threading.html) 支持现有CPU路径及线程证据边界。
[ExitProcess](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-exitprocess) 会先结束其他线程再结束进程，
所以终止/Job计数不能单独证明业务析构；不把线程数量当角色证明，不外推全部隐藏子窗或外部Shell broker。
本轮独立组合现有平台接口与自有模型，未复制第三方实现或新增依赖。

本轮只证明本机合成CPU输入下的生命周期；未执行GPU/多Provider、模型或回放质量指标、热重载中退出、Launcher发布入口、
系统关机、任意Shell扩展、真实设备/KMBOX或远程部署，也没有合并主分支。
