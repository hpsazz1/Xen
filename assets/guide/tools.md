# 输入评估、命令与离线工具

[返回 README](../../README.md) · [采集与训练](training.md) · [控制与弹道](controls.md) · [工具与输入评估](tools.md) · [开发与诊断](development.md)

本文中的命令均在仓库根目录执行；正式包路径另有说明。界面参数以当前配置校验为准。

## 目录

- [使用边界](#使用边界)
- [单 Run 离线复盘](#单-run-离线复盘)
- [原生调试工作区](#原生调试工作区)
- [人工输入记录与回看](#人工输入记录与回看)
- [急停与射击命令行](#急停与射击命令行)
- [录制、HUD与离线重评](#录制hud与离线重评)
- [独立参考比较](#独立参考比较)
- [有界设备实验与画面取证](#有界设备实验与画面取证)
- [长稳与资源采样](#长稳与资源采样)
- [无输出调度诊断](#无输出调度诊断)

## 使用边界

离线评价、协议ACK和模型估计不能替代真实设备与游戏效果。设备动作按用户明确授权手动或自动执行，Prepare不等于已运行；传入入口要求的物理确认参数，回收停止与释放结果。

## 单 Run 离线复盘

`scripts/review_run.py` 只读已有 Runtime/Aim 报告、配置身份和截图清单，生成可重复查看的离线汇总。使用 Python 3.11+，在仓库根执行：

```powershell
python -B -X utf8 scripts/review_run.py --run "C:\path\to\existing-run" --output "C:\path\to\new-review"
```

将路径替换为已有 Run 和全新的输出目录；输出目录必须位于原归档之外。工具生成 `index.html`、`REVIEW.md` 和 `review.json`，保留原文件不变。完整发布包中的入口为 `tools/review/review_run.py`。

报告汇总已记录的性能分布、异常区间和配置哈希；缺失值保持未知。画面关联要求有效的截图哈希与同一来源时钟身份，不用文件时间或相似文件名猜测逐帧对应。离线结果不等于真实设备与游戏效果验证。

## 原生调试工作区

“调试”位于“设置”上方，集中急停测试、人工录制与回看、独立射击节奏、弹道工具及运行诊断。
按目的选择入口：日常急停/扳机/压枪在辅助页运行；调试页用于独立实验、已有曲线验证、采集优化和报告回看。
“急停测试”仍执行固定反向时长的独立动作计划，不运行生产HUD动态制动；“按武器间隔动态移动”只分配实验移动时长。
生产新配置及缺键旧配置默认采用HUD；显式 `experimental_hud_model=false` 保留H40对照。
调试页固定时序实验与生产策略分别配置，以本次入口和实际配置为准。
已有弹道先选武器和测试曲线，关闭“记录优化数据”即可准备验证，不要求重新标定或重采。
需要优化时才记录数据；新采集按所选采集方式准备标定，追加阶段复用满意的短基线，五组齐备后生成候选。
辅助页保留急停、扳机和压枪运行设置；共享武器点射资料在“调试 → 弹道工具 → 高级：射击节奏”编辑，源端焦点与GSI连接统一在设置页。
新界面直接调用与 `auto_stop_counterpulse` CLI 共用的 C++ 核心，不启动外部 PowerShell。
部署、凭据准备和旧人工脚本入口保留兼容。

计划先校验并准备，参数冻结在独立目录；GUI真实测试由用户勾选本次物理输出并点击启动，
无需手输令牌。既有CLI仍要求 `-AllowPhysicalOutput` 与确认令牌。
测试与生产Runtime互斥，复用已有设备连接；GUI不按ACK最大等待或历史连接超时配置拒绝已有连接。
300ms表示最大ACK等待，不是固定动作时长；丢失ACK时取消可能等到实际设备超时，不能承诺100ms退出。
运行、录制或清理未结束时不能切换设备；结束不自动恢复Runtime。执行仍校验实际ACK、迟到与释放。
准备结果和启动按钮旁显示具体阻断；就绪阶段显示源端聚焦、松键及15秒等待进度。

调试页可独立启用“调试测试快捷键”，沿用现有按键捕获方式，绑定后保存配置。开关默认关闭；
开启表示允许用户每次按键执行最近一次已准备的物理测试。可绑定无冲突的KMBOX侧键，不能用左键、
WASD或已被其他功能占用的键。每次按下只执行一组，长按不循环，忙碌不排队，每组独立报告目录。
离开调试页不清模板；修改实验草稿、切换实验类型/设备、启动生产Runtime或急停后需重新准备。
输入健康初次建立或恢复时，已按住测试键不会触发，须释放后重新按下。快捷键来自既有设备监听，
不能把本机按键检测当作KMBOX输入证据。
离线重评、默认基准及动作候选派生不连接设备。旧Run只读，每次重评和复测产生新目录。

独立射击每组固定15次左键按住，仅有按住时长和DOWN提交间隔两个参数，次数不是子弹数。
急停与射击页共用武器选择，选择或点击“带入所选武器参数”仅复制按住时长和DOWN间隔到当前页；两页动作草稿独立，切页、目录刷新和载入失败不覆盖已有参数。高级“从文件载入射击设置”读取独立文件，与所选武器无关，成功后才替换射击页两字段。测试范围与生产表范围分别校验，不自动回写生产资料。
射击节奏页点击旁边的“保存武器参数”，将当前两字段写回所选共享武器，保留其他武器及资料字段并递增版本。按住须为1～500ms，间隔须大于按住且不超过2000ms；失败保留原文件和当前草稿。保存后可重新带入新值，生产运行下次启动读取新版本。
急停首发为基准射击。GUI新建默认20次、反向40ms、释放后18ms、按住5ms、DOWN间隔300ms，启用“按武器间隔动态移动”：移动上限默认500ms，实际移动时长按上一DOWN目标扣除实际UP、反向和释放等待后分配，剩余过长时先等待；没有正移动预算则停止，不发下一枪。DOWN间隔不是固定移动时长，也不保证游戏内精确射速。
导入旧计划缺少`overlap_fire_interval`时沿用固定移动语义；普通表单编辑只更新所改字段，保留`fire_delay_ms`等隐藏参数。明确开启动态移动会清除额外等待与旧并行等待选项，原地模式关闭动态移动。射击测试仍独立原地执行；提交间隔、DOWN ACK到UP提交、ACK到ACK分别显示，缺测保持未知。
原地射击不启用模型HUD，不生成后座恢复或真实停稳判断。

调试页仅保留“显示 HUD”一个开关，空闲即可打开并跟随 Xen 浅色/深色主题；任务结束或失败后继续保留。切页和最小化不结束任务，取消勾选仅隐藏，运行中关闭 HUD 窗口会请求停止当前任务。关闭主窗会先取消并等待后台清理；
释放或记录退出未确认时锁定新设备任务。分析与报告生成在后台，界面只读快照；HUD复用原生Win32窗口，
保留折线、分色柱和默认/实际参数对照。默认阈线没有重跑默认参数模型，模型结论不是实际停稳证据。

## 人工输入记录与回看

调试页的“人工录制与回看”提供按需输入记录和离线回看。选择记录根目录后点击“开始输入记录”，
每次创建独立 `debug-*/input` 子目录；结束时保存原始事件分块、清单和近期换键摘要。它只订阅已经连接的
KMBOX，Runtime停止时也可使用，不会启动检测或武装输出。回看填写具体Run目录后点击“读取离线记录”。

换键分级使用接收时差：绝对值不超过2ms为完美、不超过10ms为优秀，其余按重叠/空隙标偏早/偏晚；
超过120ms、同包先后不明或误差跨界的记录保留为未分类。它衡量输入节奏，不证明游戏停稳。
左键按住片段保存收到的原始报告，不因压枪曲线耗尽结束，也不以128点重采样覆盖档案。
当前KMBOX坐标增量语义尚未实机核实，在线记录明确显示轨迹不可用；只有声明相对counts的离线数据
可绘路径、方向和拐点。缺口、缺少按下/松开和限额均显示不完整，不把网络丢包未知称为全链无损。
游戏实际开枪稳定和空中加速尚未实现，界面不生成替代分数。

## 急停与射击命令行

自动移动、反向轻点和开枪测试使用正式 `auto_stop_counterpulse` 目标及
[`scripts/invoke_auto_stop_counterpulse.ps1`](../../scripts/invoke_auto_stop_counterpulse.ps1)。
Prepare 直接绑定已构建程序和配置路径，不复制程序、模型或 DLL；加 `-Repeatable` 后，日常只编辑
同目录 `plan.json`，再按用户明确授权手动或自动执行 TASK.md 中的 Launch 命令，无需重新打包。
Prepare 同时生成 `start-test.bat`、`edit-config.bat` 和 `PARAMETERS.md`；参数文件带中文说明，双击编辑、保存后双击启动即可。
程序或脚本更新后重新 Prepare 绑定身份；旧 Run 不能直接用新入口 Launch。

新计划 schema 2 使用显式的释放后时序，删除 `ShotIntervalMs`、`BrakeWindowMs`、`NoCapture`：
默认不采图，记录命令以及原始 KMBOX 输入报告。保留必要的动作参数：

| 参数 | 默认值 | 含义 |
|---|---:|---|
| `Shots` | 20 | 一组开火按住次数，1～30；不等于实际子弹数 |
| `MoveMs` | 300 | 正向键最短保持时间，ms |
| `FireDelayMs` | 300 | 上一枪左键释放 ACK 后的等待，ms |
| `MoveDuringFireDelay` | true | 等待时同时移动；false 表示等完再移动 |
| `CounterDelayMs` | 50 | 原方向释放 ACK 至反向按下的等待，ms |
| `CounterHoldMs` | 5 | 反向键保持时间，ms |
| `ShotAfterReleaseMs` | 0 | 最后方向键释放 ACK 至开枪的等待，ms |
| `ShotHoldMs` | 5 | 左键按下 ACK 至松开的保持时间，1～2000ms，可设1000支持蓄力输入 |
| `FireIntervalMs` | 0 | 相邻开火按下命令提交的最小间隔，0～5000ms；0不额外限制 |
| `LateToleranceMs` | 5 | 调度迟到容差，ms，不是停稳阈值 |
| `Baseline` / `Direction` | counter / A | 静止、无反向或反向对照，以及正向键 |

开火间隔等待放在下一轮移动前；实际周期还受动作、ACK与调度耗时影响，不承诺游戏中的精确射速。
长按开火计入整组40秒预算，超限配置拒绝；按住期间取消仍须松开左键。全零等待不会退回旧固定窗；静止对照仍需正的射后等待。JSONC 支持中文注释，未知或过时字段拒绝。
每组启动冻结 `execution-plan.json`；运行中改配置只影响下一组。Repeatable 延续同目录覆盖上一组
`result` 的约定，需要保留对比证据时先另存结果或使用新 Run 目录。
源焦点、全松和设备独占不足时拒绝，End/Ctrl+C或人工方向/鼠标按钮输入取消，不自动重试。
真实运行按用户明确授权手动或自动执行，并传入 `-AllowPhysicalOutput -Confirm AUTO_STOP_COUNTERPULSE`。

结束后 `result/training-evaluation.json` 分别记录命令 ACK 域和 KMBOX monitor 域的换键评价、
按住段与数据完整性，复用生产 `input_training::Session`。原始档案位于 `command-training` 和
`monitor-training`，可在调试页离线回看。参考换键分类只评价输入间隙/重叠，不代表实际停稳或子弹稳定。
monitor 未回显软件动作时不会用命令记录冒充监听证据；未知 X/Y 语义仍不积分为真实轨迹。

## 录制、HUD与离线重评

人工练习使用 Prepare 生成的 `start-recording.bat`：仅录制 KMBOX 监听到的键鼠，软件输入固定禁用。
每次在 `manual-recordings` 创建独立目录，保存原始 `raw` 档案、模型参数快照、分析和空白 `labels.json`。
默认录制最多120秒，也可点击 HUD 的“停止录制”；停止后关闭设备连接，双图和最近32次结果继续置顶保留，
直到关闭窗口。`show-hud.bat` 可重开最近的已有结果，不连接设备。普通桌面置顶已测试，游戏内可见性需实际验证。
HUD急停面板显示最近偏差、平均快慢、优秀率、波动、最快/最慢和整体习惯，折线按完美蓝、优秀绿、偏早黄、偏晚红分类。
开枪面板显示模型比例、误差、稳定占比及分段柱图；当前阈值与默认阈比参考分别标注，默认参数和本轮参数并列。
图表显示最近32次；统计取最近300次保留记录的有效采样，并显示有效分母。无效数据不当作稳定，参考颜色不改变控制逻辑。
绘制使用离屏缓冲，静止结果不反复刷新；统计和默认基准同时保存到分析报告的 `feedback` 字段。
自动测试的急停换键间隔由正式分析器统一生成，实时HUD、保存报告和重开HUD使用同一份timings；HTML报告显示汇总及最近32条间隔折线。自动记录来源是ACK间隔，人工记录来源是KMBOX接收间隔，均不等同物理停稳。开火周期之间、无效回执和不支持的混合方向不拼接；零间隔保留为有效值。
`analyze-recording.bat` 复用最近人工录制，按当前模型参数离线重评，提取方向键保持、换向、左键保持、
松键至开火和相邻操作间隔的均值/波动/极值及原始来源。分析另存，不覆盖录制。
入口可选择按方向、动作模式分组的平均间隔提案，编辑保存后由正式解析器检查，再生成独立复测目录。
复测目录沿用 `edit-config.bat` / `start-test.bat`，不复制程序或依赖，也不自动启动动作。
重叠换向的负等待等不兼容值会原样保留并提示；只有修改到正式参数范围并验证通过才生成可执行计划。
候选采用接收时间均值，而自动动作按ACK计时，且现流程先开第1枪再进入移动周期，不能称为人工操作精确重放。
`edit-sampling.bat` 调整移速模型、阈值比例及采样时机，下一次录制或显式离线重评生效。

`prepare-default-test.bat` 从公式生成独立默认基线和复测目录，不使用人工素材、不自动启动动作。
模型初值为待校准假设：归一化最大速度1、加速5.5、自然减速2.5、反向制动14、稳定阈比0.34，
首采样18ms、短按界限90ms、持续采样间隔100ms；这些是参考模型初值，并非游戏实测常数。
在此模型下推导：移动182ms达到上限、无间隙反向72ms、松键后2ms消除整数过冲、射后中性等待13ms覆盖首采样。
8次开火、按住5ms和调度容差5ms是可编辑的工具选择。默认计划先开1次，再执行7个移动周期。
`default-baseline.json` 分别记录公式、积分结果和假设；模型预测回零不代表游戏实际停稳。
修改模型后可用正式程序 `--derive-defaults --output '<新目录>' --sampling-settings '<修改后的模型文件>'`
重新推导；超出现有动作范围会明确拒绝，不裁剪参数。

人工反馈用录制ID和按住编号范围对应数据；按住编号不等于实际子弹编号。`labels.json` 的
`qualified_shot_ranges` / `rejected_shot_ranges` 填写如 `[[1,5],[8,10]]`，未标记不作为合格数据。
确认整份录制不可靠时设置 `recording_usable=false`；快捷入口跳过它，正式人工重评也拒绝使用它。
分析保留最近300次按住，原始档案独立保存；越界、重复或冲突标签拒绝。仅有合格样本不能唯一校准模型，
正反例齐全时先给固定模型下的阈值约束，不自动修改参数。使用下列入口重评同一录制，显式覆盖参数时
追加 `--sampling-settings '<新参数文件>'`，报告同时保留原参数：

```powershell
& '<正式程序目录>\auto_stop_counterpulse.exe' --evaluate-manual '<人工录制目录>' --output '<不存在的重评目录>'
```

已有 `result.json` 可用同一正式程序离线重评，不接设备，不需要配置或物理授权：

```powershell
& '<正式程序目录>\auto_stop_counterpulse.exe' --evaluate-result '<已完成Run>\result\result.json' --output '<不存在的重评目录>'
```

[`scripts/analyze_auto_stop_impacts.py`](../../scripts/analyze_auto_stop_impacts.py) 仍用于独立的已有图像证据，
不把输入评价分数当作游戏弹着或停稳测量。
两组已完成Run可用 scripts/analyze_auto_stop_runs.py 离线比较；它调用正式程序分析，并仅改变反向保持时长做假设重放，保留源报告的ACK延迟形状。首发未移动，移动后汇总单独统计。候选是未执行的模型假设，不是实际停稳证据；不会连接设备或修改输入报告。
独立原地射击使用 `scripts/invoke_weapon_fire_test.ps1`：Prepare生成独立目录，
`edit-config.bat`只编辑 `fire-settings.json` 的 `shot_hold_ms`（默认80）与 `fire_interval_ms`（默认800）。
按住从DOWN ACK起计时，间隔为相邻DOWN提交的最小间隔，单位ms，间隔必须大于按住时长。
`check-config.bat`离线校验，`start-test.bat`按授权执行每组15次左键按住；原地不移动，
关闭移动模型HUD，不把按住次数当子弹数或把模型零速当后座恢复。每组保留在独立runs目录，
修改只影响下一组。80/800是观察用工程起点，不是人类反应常数或任何武器的已验证恢复值。
当前测试阶段不添加随机范围或自动等待补偿；保持固定参数便于对照。

## 独立参考比较

`xen_reference_compare` 生成 `XenReferenceCompare.exe`，只读输入训练归档或合成事件，
并列展示 cs-match-hud 固定提交的 Basic 评分与现有 Xen 输入评估；另可显示当前 H40 纯状态机的假 ACK 计划。
它不连接设备、不修改生产急停/扳机、不代表完整控制策略 A/B 或游戏实际停稳。
参考来源、MIT 许可、参数和完整用法见 [独立工具说明](../../assets/reference_assessment/GUIDE.md)。

```powershell
$buildDirectory = "build/nvidia" # 与已配置后端一致，也可为 build/directml 或 build/openvino
cmake --build $buildDirectory --config Release --target xen_reference_compare reference_assessment_tests
ctest --test-dir $buildDirectory -C Release -R '^(reference_assessment_tests|reference_compare_cli_tests)$' --output-on-failure
& "$buildDirectory/Release/XenReferenceCompare.exe" --events .\assets\reference_assessment\example.json --output .\cache\reference-report-1
```

构建目录须先按项目依赖完成 CMake 配置。独立打包使用 `scripts/package_reference_compare.ps1`，只创建新目录，保留原发布入口。

## 有界设备实验与画面取证

设备实验使用独立 `auto_stop_probe` 目标。它复用生产 KMBOX owner，但不创建 Aim/Runtime，
仅接受 WASD 单键或相邻组合；每步最多 2 秒、每计划最多 15 秒，支持 End 取消与退出清理。
计划格式为 `{"steps":[{"held_mask":1,"hold_ms":150},{"held_mask":0,"hold_ms":1500}]}`，
W/A/S/D 对应位值 1/2/4/8。`--plan <计划> --output <新报告> --dry-run` 只校验计划，不连接设备。
实际执行另需 `--config <配置>`、`--allow-physical-output --confirm AUTO_STOP_WASD_PHYSICAL`。
无需人工按 Ctrl 初始化。默认在设备连接与独占 owner 确认后执行有界计划；尚未收到物理报告时
键态仍为未知，报告 `monitor_initial_state_known=false`。首个 End 报告即可取消；已收到有效键态后
监听失效会取消。`--monitor-ready-timeout-ms` 默认 0，显式设置 1～30000 只增加有界观察时间。

连续实验可用 `--session-dir <新目录> --session-seconds <1～600>` 替代单计划参数，复用同一连接。
按序原子放入 `001.plan.json`、`002.plan.json` 等请求，工具生成同序号结果和 `session.status.json`；
目录中的 `STOP`、End 或时限结束会清理并关闭。空闲不发送运动，失败终止整次会话。
设备 ACK、实际命令间隔和释放结果均写入报告，不能据此宣称角色停稳。
`--mask-check --config <配置> --output <新报告>` 是独立实体 W 检查模式，仍要求上述物理授权参数。
它等待有效的实体 W 持有报告，150 毫秒后仅屏蔽 W，保持 1500 毫秒后解除，再观察 250 毫秒；
不会合成非零软件键，也不需要 Ctrl。屏蔽期间正常松开/重按 W 可取证新的物理报告；
开始观察的时间不冒充 W 实际按下时刻。其它方向、End、监听异常或回执未知均取消并清理。

只读画面取证使用 `xen_auto_stop_capture` 目标，输出 `XenAutoStopCapture.exe`，支持
`--output <新目录> --seconds <1～30> --fps <1～240>`；默认采样左侧 650×310 HUD 区域，
可通过 `--roi-x/--roi-y/--roi-width/--roi-height` 调整。PNG 与 `frames.csv` 保留实际帧时间，
请求 FPS 不代表实际采样率。该工具没有物理输出能力，不改变生产采集 ROI。
`scripts/read_auto_stop_hud.ps1` 使用 Windows PowerShell 5.1 的本地 WinRT OCR，保留原文和缺失值；
`scripts/analyze_auto_stop_hud.py` 分析客户端可见运动、零显示与位置稳定区间，
不将两台机器时钟直接相减，不将 HUD 括号中的 3 秒峰值当作当前速度。
这些实验工具不等于生产急停已完成；持续物理持键时的屏蔽、归还及共享调度仍需分别验证。

## 长稳与资源采样

长稳使用 `invoke_hud_stop_acceptance.ps1 -Profile Soak` 的 Prepare、Validate、Launch、Recover
四阶段入口。Prepare 冻结包身份和连续 Runtime 目标时长，默认60分钟；如需不同目标，应在 Prepare
时指定 `-RuntimeDurationSeconds`，后续阶段不能改写计划。按用户明确授权手动或自动执行该 Run 的 `TASK.md`
中唯一 Launch 命令，保留 `-AllowPhysicalOutput` 和专用确认串。确认资源采集就绪后再启动 Runtime，
完成连续运行后先停止 Runtime，再退出 Worker 界面；计时提示只按资源就绪时间计算。
Recover 收集证据，连续 Runtime 时长、全程统计、资源趋势与人工体验仍需分别复核，不自动判定长稳通过。

资源采集需要明确的 PID、完整映像路径和 UTC 启动时间，并写入全新目录。默认每5秒采样、
持续60分钟；完成时长按首末有效采样跨度计算。CPU以单逻辑核100%计，分母使用紧邻CPU读取的
单调时间，内存与线程等属性按顺序读取。只有最终 `summary.json` 的
`resource_capture_completed=true` 配合完整 `samples.csv` 表示资源采样完成；`partial` 文件表示
未完成。进程退出、身份变化或写入失败分别记录。该脚本仅观察已有进程，资源结果与Runtime成功、
时延和真实操作效果分别判断。

## 无输出调度诊断

`XenMouseEffectProbeCompositeSeal --study-scheduler <绝对新目录>` 提供独立的无鼠标输出调度诊断。
它先冻结采样协议，在 300/325/350 微秒 guard 上各记录 10 个 42-event 批次，再选择全部观测达标的
最小值，以另外 10 批新数据验证。迟到/marker 超限在表征阶段保留，硬 active、API、下一目标已错过、
停止或超时则中止；验证失败不再选替补或重试。150/100 微秒质量上限、每事件 350 微秒及每批
14.7 毫秒 active 上限保持不变，整个 campaign 最多 40 批，派生 active 上限 588 毫秒，经过时间
上限 30 秒（线程恢复执行时检查，Windows 调度不提供硬实时保证）。已有目录拒绝复用，Ctrl+C 请求
中止并保存已取得记录。产物仅为带插桩的有限经验筛查；后续数据不参与选值，但不宣称统计独立或
尾部可靠性。该工具不会发布正式 preflight/plan，也不会修改已有 sequence、Run 或系统调度设置。
