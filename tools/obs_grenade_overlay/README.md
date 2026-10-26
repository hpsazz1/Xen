# OBS 道具桌面提示

由 OBS 插件在主机右上角显示站位与瞄点参考。卡片使用 OBS Browser Source 和 libobs 绘制，采用 Codex 暗色背景 #181818、白字和蓝色标记，窗口约 90% 不透明；窗口属于 OBS 进程，不需要旧 Python 双图窗口。

OBS 原有中央 320×320 画布继续专门通过 NDI 传给辅机；提示源由插件私有持有，不加入主场景，也不改变现有裁剪、NDI 输出或投掷动作。

打开 OBS 的“工具 → Xen 道具提示”，设置“显示游戏右上角参考图”和辅机网址。配置保存到 OBS 的 `plugin_config/xen-grenade-overlay/settings.json`。只有手机人工点选方案后才显示；对齐完成、开始投掷、取消、断线或关闭显示时隐藏。再次查看时重新点选方案（可以是同一方案）。启动、重连、重新打开显示开关和 F10 资源刷新都不会弹出旧选择；F10 只刷新当前仍显示的卡片。首次使用需新版辅机 Xen 已运行，默认本机交付使用 `http://192.168.3.20:5015`。

此窗口适用于可由桌面合成的游戏呈现模式。真正独占全屏的覆盖效果需单独观察；未注入游戏，也不发送游戏输入。显示和鼠标穿透需在实际 OBS/游戏中验证，构建通过不代表实际体验通过。

## 构建和安装

```powershell
pwsh -NoProfile -File scripts/build_obs_grenade_overlay.ps1 -ObsRoot 'D:\SteamLibrary\steamapps\common\OBS Studio Beta' -SdkRoot 'E:\Xen-deps\obs-studio-32.1.0-rc4-sdk'
```

安装时关闭目标 OBS，然后给同一命令追加 `-Install`。脚本不停止进程，不修改现有场景、NDI 配置或已有插件设置；替换本插件旧文件时保留 `.previous` 副本。插件放在目标 OBS 的 `obs-plugins/64bit` 和 `data/obs-plugins/xen-grenade-overlay`。直接启动 obs64.exe 时，工作目录使用其所在的 `bin/64bit`，避免 OBS 找不到语言资源。

OBS 模块资源路径可能是相对路径；插件先解析实际文件路径，再生成浏览器文件 URL，避免桌面窗口只有底色。

SDK 头文件固定为 OBS 官方 `32.1.0-rc4`（提交 `b7bc979a659a40775904a489eeb0ed9e871e066b`）。导入库由实际目标 OBS DLL 的公开导出生成，只用于此本地 ABI，不冒称官方 SDK 发布包。复用现有 nlohmann/json 头文件；不引入 Qt SDK，不重新构建 Xen Worker。

## 许可

本目录插件以 GPL-2.0-or-later 分发，全文见 `LICENSE.GPL-2.0`，对应源码位于本目录。OBS 接口头来自 [obsproject/obs-studio](https://github.com/obsproject/obs-studio/tree/32.1.0-rc4)，运行时复用用户安装的 OBS/libobs/obs-browser，不另分发 CEF 或 Qt。nlohmann/json 沿用其 MIT 许可。本目录许可不改变 Xen 其他文件的原许可。
