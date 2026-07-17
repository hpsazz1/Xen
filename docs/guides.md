# 使用与排错指南

本文档是实用设置和诊断的中央索引。详细说明保存在 `docs/guides/` 下的独立文件中，以保持本索引简洁。

## 从这里开始

| 指南 | 适用场景 |
|---|---|
| [首次启动检查清单](guides/first-launch.md) | 您正在启动新的构建或新解压的发布包。 |
| [后端选择与检查](guides/backends.md) | 需要选择 DML 还是 CUDA，或诊断基本的后端行为。 |
| [常用配置](guides/recipes.md) | 需要可直接复制的配置片段和基准测试命令。 |
| [排错步骤](guides/troubleshooting.md) | 遇到未知问题时的简短检查清单。 |

## 采集与性能

| 指南 | 适用场景 |
|---|---|
| [局域网 UDP 采集](guides/udp-capture.md) | 希望从另一台电脑或进程通过 UDP 接收 MJPEG 帧。 |
| [NDI 网络采集](guides/ndi-capture.md) | 希望通过网络接收 NDI 视频流。 |
| [采集诊断](guides/capture-diagnostics.md) | 正在阅读 `[CaptureDiag]` 输出或检查 CPU/GPU 采集路径。 |
| [圆形视野](guides/circle-fov.md) | 正在配置圆形视野限制器和界面预览。 |
| [数据收集](guides/data-collection.md) | 正在收集训练数据或检查其性能影响。 |

## 控制与界面

| 指南 | 适用场景 |
|---|---|
| [输入方式](guides/input-methods.md) | 正在设置 Win32、雷蛇、KMBOX 或其他控制路径。 |
| [界面和 GUI 行为](guides/overlay.md) | 正在调整预览、游戏覆盖或延迟补偿。 |

## 开发

| 指南 | 适用场景 |
|---|---|
| [构建工作流](guides/build-workflow.md) | 正在重新构建本地代码或依赖项。 |
| [项目开发铁律](002项目开发铁律20260713.md) | 开始任何代码、算法、流程或文档变更前。 |
| [基础跟踪算法重构](001基础跟踪算法重构20260713.md) | 了解当前无预测、无轨迹整形的基础控制链路。 |
| [标准视频闭环模拟与预测候选](030标准视频闭环模拟与预测候选20260714.md) | 使用固定视角全屏视频统一提取轨迹、模拟动态裁剪并筛选预测参数。 |
| [快速横跳实测与联合参数优化](031快速横跳实测与联合参数优化20260714.md) | 分析20/75/1440实测瓶颈，并联合搜索预测参数与设备速度上限。 |
| [框外身位预测与防翻向设计](032框外身位预测与防翻向设计20260714.md) | 了解目标框外提前量、自身视角补偿以及停止和变向保护。 |
| [运动学自适应提前重构](033运动学自适应提前重构20260714.md) | 了解为何放弃框测距和弹速假设，以及速度、加速度自动提前模型。 |
| [P0-4A角度控制器拆分](073P0-4A角度控制器拆分20260715.md) | 了解新链路如何分离角反馈、速度前馈、角度积分与经验提前参考。 |
| [P0-4B确定性轨迹执行层](074P0-4B确定性轨迹执行层20260715.md) | 了解pass-through、固定周期调度、二维运动约束和整数余量语义。 |
| [P0-5跨域回放首轮阻断](075P0-5跨域回放首轮阻断20260715.md) | 运行同帧r30/候选跨域矩阵并根据硬门槛判断是否保持shadow。 |
| [P0-5静止角度回差复验](076P0-5静止角度回差复验20260715.md) | 了解角误差与相对LOS速率回差的选型、阈值扫描和剩余阻断。 |
| [P0-5低速反向确认复验](077P0-5低速反向确认复验20260715.md) | 了解静止回差区内80 ms二维反向确认、参数反证和跨域矩阵结果。 |
| [P0-5高速横移垂直追赶复验](078P0-5高速横移垂直追赶复验20260715.md) | 了解二维预算下的垂直饥饿、角度域追赶门控和跨域矩阵结果。 |
| [P0-5固定240Hz梯形轨迹对照](079P0-5固定240Hz梯形轨迹对照20260715.md) | 了解固定tick回放、默认/放宽运动学约束对照和保持off的依据。 |
| [P0-5相对视线速度前馈复验](080P0-5相对视线速度前馈复验20260715.md) | 了解主场景物理域矩阵、前馈单变量扫描和0.15候选依据。 |
| [P0-5前馈安全上限细化](081P0-5前馈安全上限细化20260715.md) | 了解0.15~0.20细扫、帕累托门槛和0.16安全上限。 |
| [P0-5候选响应隔离复验](131P0-5候选响应隔离复验20260716.md) | 了解冻结legacy比较器、独立扫描候选响应时间及否决60/100 ms的依据。 |
| [P0-5失败归因反事实回放](132P0-5失败归因反事实回放20260717.md) | 使用冻结检测时间线区分估计受限、执行受限与结构性失败。 |
| [P0-5机动常加速度候选复验](133P0-5机动常加速度候选复验20260717.md) | 评估全时与速率门控常加速度模型，并冻结零回退的离线影子配方。 |
| [P0-5静止真值与在途命令时轴](151P0-5静止真值与在途命令时轴20260717.md) | 复现固定真值、正式视角时轴与60 ms命令提交时域反证。 |
| [P0-5在途命令静止进入保护](152P0-5在途命令静止进入保护20260717.md) | 复现60 ms冻结目标guard、过渡hold和5个剩余损失域。 |
| [P0-5静止进入双误差守卫修正](153P0-5静止进入双误差守卫修正20260717.md) | 修正承诺终点替换当前误差的提前settle漏洞，并复现正确728/810结果。 |
| [r60 DML机动估计影子接入](134r60DML机动估计影子接入20260717.md) | 使用共享估计器采集DML/NDI模型选择、静止驻留和暂停恢复诊断。 |
| [r60静止延迟30毫秒反证](138r60静止延迟30毫秒反证20260717.md) | 对照10/20/30/60 ms并结束固定延迟扫描，确定有限响应候选边界。 |
| [r61有限相机响应影子实现](139r61有限相机响应影子实现20260717.md) | 使用20 ms响应中心和20 ms宽度复测DML九点static。 |
| [r61有限响应首轮实测](140r61有限响应首轮实测20260717.md) | 分析20/20候选的九点static失败、X轴恢复瞬态及重复采集门禁。 |
| [r61重复实测与响应核复放](141r61重复实测与响应核复放20260717.md) | 用三轮static和正式估计器复放否决共享/分轴线性响应参数搜索。 |
| [r61正样本首轮采集](142r61正样本首轮采集20260717.md) | 确认jump正样本有效，并定位首轮reverse低于12°/s门槛。 |
| [稳健常速度预测与实机振荡排查](034稳健常速度预测与实机振荡排查20260714.md) | 了解首轮实机预测振荡根因、场景边界和r8稳健速度模型。 |
| [预测连续性保持与单帧门控修复](035预测连续性保持与单帧门控修复20260714.md) | 分析r8提前量频繁中断原因，以及r9窗口级停止和反向确认。 |
| [透视坐标修正与jump视野逃逸优化](036透视坐标修正与jump视野逃逸优化20260714.md) | 分析r9静止误补偿和jump飞出320裁剪的共同坐标根因。 |
| [静止收敛自运动伪迹抑制](037静止收敛自运动伪迹抑制20260714.md) | 分析r10静止收敛残差，并验证r11三帧联合安全门控。 |

核心参考：

- [配置指南](config.md)
- [从源码构建](build.md)

## P0-5跨域视频回放

DML构建可使用同一批视频并行评价r30与新角度链。命令返回0表示全部门槛通过；返回3（部分Windows调用环境显示为非零）表示结果已完整生成但必须保持shadow：

```powershell
build\dml\Release\Xen.exe --cross-domain-replay C:\Users\16143\Desktop\Xen\Video --video-model build\dml\Release\models\yolo12n_cs2.onnx --video-output C:\Users\16143\Desktop\Xen\Video\analysis_p0_5 --crop-x 1120 --crop-y 560 --crop-width 320 --crop-height 320 --inference-fps 94 --fov-x 106 --fov-y 74 --sensitivity 1.4 --yaw 0.022 --pitch 0.022 --response-ms 80 --candidate-response-ms 80 --max-cps 1440 --settle-error-deg 0.08 --settle-rate-dps 1.2 --reverse-confirm-ms 80 --vertical-catch-up-deg 0.8 --feedforward-gain 0.16 --reversal-ff-boost 0.10 --reversal-ff-ms 80
```

逐帧明细默认只输出 `domain_fov106_2560x1440_94fps_20ms_1x`。定位汇总中的特定失败域时追加 `--cross-domain-detail-variant <精确Variant名称>`；多个域可用逗号分隔，每个名称都必须属于162项必测矩阵，否则程序直接失败。`cross_domain_decision.txt`记录实际列表，CSV用`Variant`区分来源且只写一个表头。

先查看`cross_domain_decision.txt`，再按`cross_domain_summary.csv`的`Scenario/Variant/Passed/Reason`定位单一失败模块；不要只挑通过变体调参。`cross_domain_frames.csv`只包含基准域逐帧诊断，全矩阵指标均在summary中。

`--response-ms`只定义冻结的legacy比较器，`--candidate-response-ms`只定义新控制器候选；不传后者时默认与legacy相同。每次候选响应实验必须核对decision和summary中的`LegacyResponseMs/CandidateResponseMs`，禁止移动比较器后把结果当作候选收益。

需要分析失败归因时追加`--cross-domain-attribution 1`。工具会生成`cross_domain_attribution.csv`，分别运行控制时刻真实LOS状态和100000 counts/s上限两个离线反事实；两者严格复用基线逐帧检测/重置时间线。只有`CohortStable=1`时分类才有效，出现任何`COHORT_CHANGED`必须先修评估工具，不能据此选算法。oracle与解除限速是定位上界，不是可直接发布的候选，总通过数下降时也不得选择性只统计被救回的失败项。

定位 static 在固定 1.5 倍 settle 退出边界外产生的低速反向脉冲时，可追加 `--reverse-confirm-error-multiplier 1.75` 或 `2.0`。该入口只改变离线候选的低速反向确认误差带，默认 `1.5`；不会改变 settle 退出边界，也未开放到配置、UI 或 active。每轮必须核对 decision 的 `ReverseConfirmationErrorMultiplier` 和 summary 同名列，并与默认 1.5 的完整 810 项结果比较，禁止只统计被救回的 static 变体。

`--confirm-low-speed-reverse-settle-release 1` 仅用于复现“先确认低速反向误差、再解除 settled”的离线反证。该候选把总通过从 721 降至 717，损失 19 个原通过 static 域且只救回 15 个，已正式否决；默认必须保持 `0`，不得加入配置、UI、shadow 运行参数或 active。

机动常加速度离线候选在冻结基线命令后追加`--candidate-estimator gated_ca --candidate-jerk-std-dps3 8000 --candidate-maneuver-rate-threshold-dps 12 --candidate-maneuver-hold-ms 120`。该配方只供跨域回放与下一阶段DML shadow验证；`constant_acceleration`全时模型会严重回退static，8/16°/s门槛会损失原通过项，均不得使用。summary中的`ManeuverModelPercent`必须用于检查static误驻留。

DML r61实机影子先配置`aim_shadow_command_to_frame_delay_ms=20`、`aim_shadow_command_response_ms=20`、`aim_shadow_estimator_mode=maneuver_gated_ca`及冻结的8000/12/120参数。首轮只采集九点static，并运行`tools/analyze_shadow_pipeline.ps1 -DataRoot <目录> -ExpectedControllerRevision 61 -RequireManeuverCandidate -RequireFiniteViewResponse -RequirePausedObservations`；脚本分别报告暂停态和运行态机动样本，只有运行态static为0才允许继续采集jump/reverse。CUDA强制退化为`kalman`和0 ms响应宽度，不得用于候选结论。

固定240 Hz梯形对照在同一命令后追加`--trajectory-mode trapezoid --trajectory-output-hz 240 --trajectory-max-accel-cps2 60000 --trajectory-max-jerk-cps3 4000000`，并使用独立输出目录。该模式当前仅用于反事实回放，正式默认仍为`off`。

当前主场景前馈候选在同一命令后追加`--feedforward-gain 0.16`。该参数仅改变离线候选，运行时`aim_shadow_feedforward_gain`默认仍为0；0.17会回退jump，0.18以上会回退left/right，不得只按总通过数选值。
