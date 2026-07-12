# 输入方法

## 控制方式指南

使用以下方式设置控制方式：

```ini
input_method = WIN32
```

有效值：

```text
WIN32, GHUB, RAZER, KMBOX_NET, KMBOX_A, MAKCU
```

硬件方法是明确指定的。如果你选择了 `RAZER`，但匹配的运行环境不可用，应用不会自动切换到其他方法。

## 当 WIN32 在游戏中无法移动时

`WIN32` 通过标准的 Win32 输入路径发送标准的 Windows 鼠标事件。这对于快速桌面测试很有用，但某些游戏会忽略或阻止此类合成输入。在这种情况下，检测可以正常工作，预览可以显示框，GUI 可以通过 `Home` 键打开，但瞄准移动或自动射击仍然无法到达游戏。

请将此视为输入链问题，而非模型或采集问题：

1. 确认框或预览检测可见。
2. 确认控制台打印了预期的行，例如 `[Mouse] Using WIN32 input.`
3. 测试移动在 Windows 桌面或非屏蔽应用中是否工作。
4. 如果桌面移动正常但游戏无反应，请切换离开 `WIN32`。
5. 使用与你实际拥有的运行环境或设备匹配的方法：`GHUB`、`RAZER`、`KMBOX_NET`、`KMBOX_A` 或 `MAKCU`。

对于屏蔽标准 Win32 路径的游戏，通常需要一个单独支持的输入设备。硬件类方法如 KMBOX 或 MAKCU 通过外部桥接器发送移动，而不是依赖标准的 Windows 合成鼠标事件。应用不会为你创建此设备；它必须已连接、配置好，并且对所选 `input_method` 可见。

## Razer 设置

使用：

```ini
input_method = RAZER
```

运行环境包装器加载：

```text
rzctl.dll
```

预期项目路径：

```text
rzctl.dll
```

该 DLL 也可以放在 `Xen.exe` 旁边。如果 DLL 缺失或导出函数错误，Razer 移动将无法工作，且不使用后备控制方法。

快速检查：

- 确认 DLL 文件名确切为 `rzctl.dll`。
- 确认其架构与应用相同，通常为 x64。
- 确认 `input_method = RAZER`。
- 查看控制台日志中的 `[Razer]` 消息。


相关文档：

- [输入方法配置](../config.md#input-method)
- [常见方案](recipes.md)
