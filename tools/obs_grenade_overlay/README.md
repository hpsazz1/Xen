# OBS 道具菜单、参考图与 F10 采集

主机道具操作统一由本插件承载。OBS Browser Source 和 libobs 负责菜单、图片与标记的绘制；桌面窗口属于 OBS 进程，不需要旧 Python 双图窗口、手机选择页面或语音客户端。

OBS 原有中央 320×320 画布继续专门通过 NDI 传给辅机。插件私有源不加入主场景，不改变原有裁剪、NDI 输出或已测试的投掷动作。

## 操作

1. 保持辅机 Xen 的 Runtime 运行。在 OBS“工具 → Xen 道具提示”启用道具菜单、参考图和 F10 采集，填写辅机接口地址；本机交付默认 `http://192.168.3.20:5015`。
2. 游戏中按小键盘右键呼出或隐藏菜单：NumLock 关闭时用方向右，开启时用小键盘 6。菜单居中，约占屏幕宽度 56%，深色背景约 92% 不透明。地图和阵营跟随 GSI，按站位或落点分类，鼠标点选即可切换方案。
3. 辅机载入所选方案后，菜单自动消失并将焦点交还游戏，右上角显示站位和瞄点参考。参考卡采用 Codex 暗色背景 #181818、白字和蓝色标记，约 90% 不透明，鼠标穿透。
4. 拿出道具、站到位置、把视角转到瞄点附近，按上键找点、下键投掷；左键取消，End 全局急停。对齐完成或开始投掷后参考卡隐藏，不播放成功提示音。

再次查看参考时重新点选方案，可以选择同一方案。启动、重连、重新开启插件及 F10 资源刷新都不会重播已经隐藏的旧选择。菜单可再次按右键或按 Esc 关闭；切到其他应用时也会隐藏。

菜单和参考卡均通过 `SetWindowDisplayAffinity` 请求 Windows 系统截图排除。F10 采集另主动隐藏插件窗口，待桌面更新后再截完整原图，避免卡片混入参考照片。这不改变原中心 ROI 的 NDI 输出。

## F10 制作

人工试投成功后，恢复原站位和正确瞄角，在游戏中按 F10。插件自动读取当前方案、采全屏、命名、打包并同步辅机，完成后显示短通知。不需要额外打开 PowerShell 或采集监听器，F10 也不自动投掷。

安装时自动填写主机现有 Python 3 路径；仅使用标准库，没有语音模型依赖。Python 只在这一次内部作业期间运行。OBS 设置中可修改“采集Python”“采集本地目录”“辅机资源目录”；设置保存于 `plugin_config/xen-grenade-overlay/settings.json`，已有值保留。

Mirage T1～T5 共用随插件安装的出生点图；其他站位图片放在采集本地目录 `standing/<方案文件stem>.png`。本地原图、短名资源包和发送记录沿用原目录；重复采集保留备份，发送失败保留本地文件。内部文件与状态约定随包保存在 `capture/README.md`，源码位于 `tools/grenade_capture/README.md`。

手机首页不再提供选择入口，HTTP 根路径返回 404；5015 保留为插件读取 GSI、方案、参考图和提交选择的内部接口。语音服务、快捷键和客户端已退役；历史方案中的语音字段只作文件兼容。

## 构建和安装

```powershell
pwsh -NoProfile -File scripts/build_obs_grenade_overlay.ps1 -ObsRoot 'D:\SteamLibrary\steamapps\common\OBS Studio Beta' -SdkRoot 'E:\Xen-deps\obs-studio-32.1.0-rc4-sdk'
```

安装时关闭目标 OBS，再给同一命令追加 `-Install`。可用 `-CapturePython`、`-CaptureRoot`、`-CaptureDestination` 提供首次安装的采集路径；已有设置不覆盖，后续在 OBS 设置修改。脚本不停止进程，也不修改原场景或 NDI 配置；替换插件旧文件时保留 `.previous` 副本。

插件安装在目标 OBS 的 `obs-plugins/64bit` 和 `data/obs-plugins/xen-grenade-overlay`，内部采集文件位于后者的 `capture/`。直接启动 obs64.exe 时，工作目录使用其所在 `bin/64bit`，避免 OBS 找不到语言资源。OBS 相对模块资源路径先解析为实际绝对路径，再生成浏览器文件 URL。

SDK 头文件固定为 OBS 官方 `32.1.0-rc4`（提交 `b7bc979a659a40775904a489eeb0ed9e871e066b`）。导入库由实际目标 OBS DLL 的公开导出生成，只用于此本地 ABI，不冒称官方 SDK 发布包。复用现有 nlohmann/json，不引入 Qt SDK。

此窗口适用于桌面合成的游戏呈现模式，未注入游戏。新菜单的游戏焦点返回、鼠标点选、系统截图排除与 F10 实际采集需要实机验证；编译、离线测试和接口回执不能代替这些观察。

## 许可

本目录插件以 GPL-2.0-or-later 分发，全文见 `LICENSE.GPL-2.0`，对应源码位于本目录。OBS 接口头来自 [obsproject/obs-studio](https://github.com/obsproject/obs-studio/tree/32.1.0-rc4)，运行时复用用户安装的 OBS/libobs/obs-browser，不另分发 CEF 或 Qt。nlohmann/json 沿用其 MIT 许可。本目录许可不改变 Xen 其他文件的原许可。
