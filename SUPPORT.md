# 获取帮助

先查看 [README](README.md) 中的启动与环境说明，再根据问题查阅指南：

- [采集、审核与训练](assets/guide/training.md)
- [Aim、辅助、扳机与弹道](assets/guide/controls.md)
- [输入评估与离线工具](assets/guide/tools.md)
- [构建、发布与日志诊断](assets/guide/development.md)

## 选择报告入口

| 情况 | 入口与内容 |
| :-- | :-- |
| 故障、崩溃或文档错误 | 先搜索 [已有 Issues](https://github.com/hpsazz1/Xen/issues)，再 [创建 Issue](https://github.com/hpsazz1/Xen/issues/new/choose)，提供可复现步骤。 |
| 功能建议 | [创建 Issue](https://github.com/hpsazz1/Xen/issues/new/choose)，说明要解决的实际问题、现有做法和期望结果。 |
| 已有修复或文档改进 | 阅读 [贡献指南](CONTRIBUTING.md)，提交 [Pull Request](https://github.com/hpsazz1/Xen/pulls)。 |
| 安全漏洞、凭据泄露或可被利用的缺陷 | 遵循 [安全政策](SECURITY.md) 中的报告方式；不要先在公开 Issue 或 PR 披露细节。 |
| 骚扰、垃圾信息或其他社区违规 | 按 [社区行为准则](CODE_OF_CONDUCT.md) 报告。 |

## 故障报告应包含什么

- 使用的提交或包版本、Windows 版本，以及相关硬件和推理 Provider。
- 出现问题的入口、采集方式和必要配置项；不要上传整份私人配置。
- 尽可能短的复现步骤、预期行为、实际行为，以及是否稳定重现。
- 与问题直接相关的错误文本、脱敏日志或截图；构建问题附执行命令和依赖信息。
- 已尝试的处理方式，以及结果。

对于控制或设备问题，请区分界面状态、软件命令与 ACK、离线结果及真实观察。说明是否实际启动、武装和产生过设备输出；不要仅凭自动报告把实际体验标记为通过。报告问题不要求你执行额外的真实设备操作。

公开材料中不要包含密码、令牌、设备认证信息、私人网络地址、个人路径或未获授权的数据。使用占位值保留复现结构；模型与数据问题可优先提供合成样本和最小配置。若涉及安全风险，请转到安全报告流程。

回复和修复取决于维护者可用时间与复现信息；本页不承诺响应时限或特定版本的长期维护。
