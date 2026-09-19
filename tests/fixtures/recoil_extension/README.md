# 真实追加段离线回归数据

任务：`RECOIL-WORKFLOW-001`。本目录只用于重放“3 发基线追加到 8 发时被全局 ±5 counts
微调限幅截断”的缺陷。五组数据已用于原优化，尤其两组 holdout 已消费；这里的再次运行是诊断和
软件回归，不是新的独立验证、更不是实际压枪质量验收。代理未触发任何真实输出。

## 数据契约

- `base.json`：从第一组 `run/plan.json` 的 `/profile` 提取，供 `load_recoil_profile` 读取。
  已逐份核对五组执行计划的完整 profile 相同，并核对五组 `run/batches/batch-1-1.json`
  中实际记录的 profile ID、revision 和所有节点与计划一致。
- `fit-1.json`、`fit-2.json`、`fit-3.json`：前三组拟合数据。
- `holdout-1.json`、`holdout-2.json`：后两组已消费的预测比较数据。
  五份均是原始 `run/measurement.json` 的逐字节副本，直接用 `load_wall_report` 读取；
  保留原始时间、counts、像素响应、候选、来源 SHA 和来源 Run，不复制计划配置、帧图或设备配置。
- `provenance.json`：源文件绝对路径、原文件 SHA-256、测量内容 SHA、计划绑定的 profile SHA、
  基线 fixture SHA 与来源对应关系。五个测量内容 SHA 已和原 `optimization-analysis.json`
  的 `calibration_evidence` 集合精确核对。

文件 SHA 是原始文件字节的哈希；测量 `candidate.source.sha256` 与计划 `source_sha256`
是生产链已有的内容身份，两者不可互换。`base.json` 仅改变了 JSON 的排版，数值和字段保持不变。
测试不应访问 provenance 中的本机或 SMB 路径；这些路径只用于溯源。所有原始证据保持只读。

## 来源与数值边界

| 文件 | 原始 Run | 锁定边界 Y 残差 | 共同末端 Y 残差 |
|---|---|---:|---:|
| fit-1.json | debug-17898341699818747-15 | 18.68733353 | 93.46430988 |
| fit-2.json | debug-17898342599427201-16 | 16.40018092 | 94.17719968 |
| fit-3.json | debug-17898343581934046-18 | 17.75341892 | 91.62619003 |
| holdout-1.json | debug-17898360570615792-2 | 14.00426995 | 93.87339049 |
| holdout-2.json | debug-17898360799733384-3 | 16.35583697 | 93.09429011 |

表中数值通过原始候选节点线性插值得到，单位是 counts；锁定边界 `242.4281 ms`，共同末端
`684.7013 ms`。精确回归应从 JSON 读取，不使用表中截断值。

实际基线 ID 为 `ak47-3shots-6ffc35b94d8b460493e5246c8a9a323c`，revision 为 2，包含 24 个
累计节点，末节点为 `[242.4281, -2.246824391913159, 59.65416887251631]`。
前三组共同末端平均 Y 残差约 `93.08923320`，减去锁定边界平均残差后约 `75.47558874`；
这使仅追加净 `5 counts` 的旧行为能够成为有实际数据依据的红灯，而不只是检查时域变长。

这些是图像测量及离线预测。`shot_timing_available=false` 和
`requires_manual_confirmation=true` 的原始声明保留；不能把候选时间、设备 ACK 或离线误差改善
解释为新的物理效果证明。
