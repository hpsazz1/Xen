# 独立参考评估工具

任务 AUTO-STOP-REFERENCE-001。用户已选择只做参考评估和离线比较，保留生产控制限制。
`XenReferenceCompare.exe` 不连接 KMBOX、GSI 或游戏，不链接 Mouse/Worker/Runtime，不替换原入口。

## 使用

在本目录打开 PowerShell，运行合成样例（报告目录必须不存在）：

```powershell
.\XenReferenceCompare.exe --events .\example.json --output .\reports\example-1
```

比较现有调试页“输入训练”导出的完整 `XEN_INPUT_TRAINING_V1` 归档目录：

```powershell
.\XenReferenceCompare.exe --archive 'D:\你的归档目录' --output .\reports\recorded-1
```

打开生成的 `REPORT.md` 查看并列表格；`report.json` 包含详细时序与假设标志。
工具不接收普通Runtime日志或视频；缺原始输入时不要编造边沿。

## 输入与范围

- `example.json` 为合成样例，并非用户真实Run。`schema=1`，每个event是完整快照；省略的布尔值均为false，**不是继承上条**。
- `at_ms` 是相对时间，可有小数；`held_mask`：W=1、A=2、S=4、D=8；`left_down`、`crouch`、`shift`、`space` 为布尔值。
- 可选 `epoch`、`sequence`、`gap`、`state_valid`、`timing_uncertainty_ns` 保留接收证据；默认时间误差未知。
- 最多200000事件、600秒；参考输出最多20000条，超限明确失败。Markdown最多展示每类100条，JSON保留本轮参考全部输出和Xen最近300条时差。
- 归档若标记LIMIT、dropped或trailing_gap则拒绝整体比较。中间显式断点仍保留，参考侧等待全释放再重同步。
- 归档没有蹲/走/跳/目标/功能键/GSI元数据；Basic输出是在这些条件未知时的输入模型结果，不能验证切枪、目标消失或功能键生命周期。

## 比较对象

同一输入同时进入参考Basic评估器与**现有Xen输入评估器**。后者不是急停控制算法。
差别包括参考50ms全局记录间隔、Xen同包换向标记未分类、时间误差跨分类边界等。
参考同包快照先UP后DOWN拆边沿；`packet_order_assumed`持续标记本段后续样本，直到重同步，不能据此接受perfect标签。

可选 `h40_plans` 是另一个独立视图：直接调用当前 `AutoStopController`，以明确的假ACK延迟演示40ms反向、释放后18ms等待与取消。
例如假ACK每次1ms时，完整路径请求至估计完成为61ms。它不是同一现场输入的两套完整控制策略A/B，也不执行开火。

## 原始来源与移植边界

来源：[cs-match-hud 固定提交](https://github.com/qianjiachun/cs-match-hud/tree/e632605f8b6c20ac5ab8ac3284e4fdc33735d431)，MIT，Copyright (c) 2026 小淳。完整许可见 `LICENSE.cs-match-hud.txt`；文件哈希见 `UPSTREAM.json`。

移植 `assessment_engine.rs`、`engine.rs` 的Basic默认逻辑；`types.rs`提供默认参数。
上游没有自动急停或自动开火控制器，`FireScheduler`只调度评分采样。
原Rust核心59测试通过，8组原实现黄金轨迹用于核对C++数值与状态；这不是整个Tauri应用的验证。

默认模型：最大归一速度1，加速5.5/s、自然减速2.5/s、反向减速14/s，稳定阈值比例0.34；换向阈值2/10/120ms，去重50ms；首次采样DOWN+18ms，短按窗90ms，后续100ms采样。
这些固定参数只存在于离线模型，不用于生产控制。短tap在UP早于首次due时仍按原模型向未来DOWN+18ms结算；不代表该时刻真实发火。

不含自定义参数/键位、GSI增强、OS漏键补偿。Shift/Space只记录，不推断慢走或腾空。
边沿归一、断点后全释放重同步、单调时间及资源校验、Xen并排评估和H40假ACK视图均是本次适配。

不宣称参考算法更稳定，也不宣称已修复用户原急停问题。构建、离线结果与游戏实际效果须分别判断。
