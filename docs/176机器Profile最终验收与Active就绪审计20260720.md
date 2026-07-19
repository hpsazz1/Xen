# 机器Profile最终验收与Active就绪审计

## 背景

`docs/175机器Profile响应覆盖阻断与解耦20260720.md` 根据修复前 Level 3 与 Level 2 的实机 A/B，证明主动脉冲推导的 `14.096/0 ms` 不具备直接覆盖连续运行响应的资格。提交 `d39edc9f2aaa` 将证据保存与运行应用解耦：Level 3 仍可使用人工审核的 `deg/count`，但响应恢复为配置的 `20/20 ms`，并新增 `MachineProfileCalibratedResponseEnabled` 审计字段。

本轮使用同一 DML 构建、原 r64 候选、`machine_profile_aim_mode=hipfire` 和相同现场条件，在 `C:\Users\16143\Desktop\Xen\DML\ndi\5` 采集 static、horizontal_jump、horizontal_reverse 各三轮，共九个 CSV。目标是关闭响应解耦后的 Level 3 实机验收，并确认该结论是否足以解除 active 冻结。

## 数据与身份

- 构建身份统一为 `DML|d39edc9f2aaa|r64`，未出现 `dirty` 或混合提交；
- 九文件共 12712 帧，其中 9229 个运行态缓存上下文帧；
- 9229/9229 均为 Level 3、`MachineProfileCacheMatched=1`、`cache_exact_match`；
- 9229/9229 均为 `MachineProfileCalibratedResponseEnabled=0`；
- 全部文件保持 `CommandToFrameDelayMs=20.000`、`CommandResponseMs=20.000`；
- 候选 SHA-256 仍为 `794D852656522DC5726D27D26C40771FC0E567FA5478743AB82374097FBB612C`。

## 九文件正式门禁

| 场景 | 运行态帧 | 机动模型启用帧 | 启用率 |
|---|---:|---:|---:|
| static | 2321 | 0 | 0% |
| horizontal_jump | 3837 | 305 | 7.95% |
| horizontal_reverse | 3071 | 1780 | 57.96% |

三轮 static 分别为 0/693、0/745、0/883，全部满足运行态机动启用绝对为零的硬门槛。修复前 Level 3 的 static 为 121/2520（4.80%），因此本轮直接证明响应污染已被消除。jump/reverse 分别由修复前的 80.55%/99.84% 恢复到 7.95%/57.96%，与此前不应用缓存响应的 Level 2 基线 6.42%/62.29% 同量级。

执行以下适用门禁：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\analyze_shadow_pipeline.ps1 `
  -DataRoot C:\Users\16143\Desktop\Xen\DML\ndi\5 `
  -ExpectedBuildRevision d39edc9f2aaa `
  -ExpectedControllerRevision 64 `
  -RequireManeuverCandidate `
  -RequireFiniteViewResponse
```

九个文件和 Overall 均为 PASS；模式、身份、时序、Kalman 诊断、后级控制分量、暂停命令、机动模型契约、选择计数和有限响应违例均为零。`RequireStandardScenarios` 还要求历史五场景文件名包含 left/right，不适用于本轮明确冻结的 static/jump/reverse 三场景协议；附加该开关产生的 `missing scenarios: left,right` 不是数据或算法失败，不纳入本轮结论。

## Active就绪审计

机器 Profile 的 P1 闭环已经完成，但不能据此开放 active：

1. `AimPipelineRuntime::configure` 仍把所有非 legacy 请求强制设为有效 shadow；
2. reset 和逐帧 observe 均固定 `activeAvailable=false`、`commandSuppressed=true`；
3. 轨迹结果再次强制 `commandSuppressed=true`，新链没有设备队列写入口；
4. `MouseThread::moveMousePivot` 同帧运行 shadow 后，最终 `queueMove` 仍只接收 legacy `BasicAimController` 的整数命令；
5. P0-5 跨域晋级仍为 `HOLD_SHADOW`。实测物理端点交集仅 476/810，候选继续失败 jump 绝对包络、static 不变量和 left/right 质量门槛；机器 Profile 只修复本机比例与响应污染，不会改变这些跨域反例。

如果此时直接实现 active 队列接管，将绕过已经固化的质量阻断，并同时引入 legacy/新链仲裁、暂停清队列、目标切换、失效降级和紧急回退等尚未形成闭环的设备风险。因此本轮只关闭 P1，不修改源码、配置、控制器修订、active 能力或用户现场文件。

## 结论与下一主线

机器 Profile Level 3 的生成、严格失效、精确命中、响应解耦和三场景实机 shadow 验收全部完成，不需要继续采集该阶段数据。active 继续 `HOLD_SHADOW`。

下一主线回到唯一阻塞项 P0-5：不得继续围绕本机响应或局部参数扫描；必须提出能同时满足 810 域物理集合绝对安全包络、static 零新增翻转和 left/right 场景质量的上游控制方案，并先在同帧共同队列回放中通过全部晋级契约。只有 P0-5 解除 `HOLD_SHADOW` 后，才能单独设计设备接管、互斥仲裁和一键回退，并进入新的 active 灰度实机协议。
