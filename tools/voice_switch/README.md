# 主机道具清单、双图与可选语音

本工具在主机运行；辅机 Xen 提供当前地图已保存的道具清单。点按右键打开菜单，点击方案后恢复此前游戏窗口；辅机确认载入后显示站位与瞄点双图，对齐完成后双图无声隐藏。默认不需要麦克风或语音模型。可选语音沿用当前明确启用页的身法或道具目录，音频只在主机内存中识别，不上传、不保存。

手机清单网页现在由辅机 Xen 内置提供，默认端口 5015，地图/本地阵营直接跟随共享 GSI；访问网页不需要启动本工具或独立 Python 服务。手机选择暂为预览。下述主机参考图采集与道具制作调试工具继续保留，制作完成后把方案与双图资源打包到辅机；桌面清单和可选语音仍属既有独立入口。

## 安装与启动

默认清单模式需要 Windows x64 与包含 Tk 的 Python 3.12（本轮验证版本），不需要安装语音依赖。以下命令在工具目录运行：

```powershell
pwsh -NoProfile -File .\start.ps1 -HostAddress <辅机地址>
# 可选语音：先安装环境，再显式以 -Voice 启动。
powershell -NoProfile -ExecutionPolicy Bypass -File .\install.ps1
pwsh -NoProfile -File .\start.ps1 -HostAddress <辅机地址> -Voice
```

本轮主入口使用 PowerShell 7（`pwsh`）。启动脚本优先使用已有 `.venv`，没有时使用系统 `py -3`；目录模式也可直接运行 `py -3 -X utf8 -B client.py --host <辅机地址>`。默认 TCP 端口 5014；`--port` 用于对齐辅机设置。双图默认右上角，使用 `--corner left`（启动脚本为 `-Corner left`）切到左上角；`--references`（启动脚本为 `-References`）指定本机资源根目录，默认程序根的 `cache/grenade/references`。

可点击清单使用标准库 Tk；双图使用 Win32 不激活、鼠标穿透窗口。GUI 在无控制台窗口的本地子进程主线程运行，通过标准输入输出交换状态和点击请求，不新增网络监听或常驻服务；网络/可选音频不会被图片读取和绘制阻塞。关闭客户端同时关闭 GUI，按 Ctrl+C 退出。

显式 `--voice`（启动脚本 `-Voice`）才加载模型和音频依赖，并需要默认麦克风/输出设备；`--model` 仅用于指定已核验模型位置。首次 `install.ps1` 需要联网安装固定依赖及约 32.7 MB 官方模型包，创建同目录 `.venv`，模型目录只保留约 5 MB 的三份 int8 权重、tokens 和说明。不要与安装同时运行语音客户端。

安装包 SHA-256：`b2f7c89690dc8ce4c6ed6afeab7cd800c36ad1421fb6b6302b4a4b194cf7f35f`。模型来源及许可证见 [LICENSES.md](LICENSES.md)。`prepare_model.py --archive <已下载压缩包>` 支持复用相同官方包，仍校验摘要。

## 使用约束

先在辅机道具页明确启用测试，并在快捷键设置启用道具助手、保存四向键。道具清单按当前地图显示全部已保存方案，包括没有语音口令的方案；按名称中明确的 T 编号、CT 来源编号分组，没有明确来源的放入未标注组。清单显示方案名、可选口令和双图状态，可滚动并单击选择，未保存草稿不会被静默覆盖。

点按右键打开清单并点选，自己拿出对应道具、站到参考点位并大致朝向瞄点；上键开始对齐，双图隐藏后再按下键投掷一次。左键取消本次道具操作并收起菜单或双图，End 保留全局急停。默认使用独立方向区；若实际需要数字区 6/8/2/4，通过辅机已有按键捕获分别绑定，两区不混用。对齐中或失败时保留双图；失败原因显示在图下。打开清单、换方案或地图会使旧对齐失效；断线同时隐藏主机显示并取消辅机旧操作。

语音需另绑非冲突按键并显式启动 `-Voice`，不占用默认右键。语音候选只来自当前明确启用的执行域，不混合两库；每套方案使用一个简短中文口令，如“跳箱”“一号窗烟”，数字写成中文，不使用拉丁缩写或标点。空口令、模型不支持及同音歧义只从识别候选排除，不从可点击清单删除。选择本身不执行动作。

按住语音键说话，松开后等待短提示音：高音表示辅机确认新方案已提交；低音表示未确认成功。低音可能是没识别、歧义、拒绝、断线或超时；超时不证明辅机没有切换。每轮只响一次，多个不同关键词拒绝，不按第一个词任选。模型就绪及目录同步前不会补启动旧按住轮次。

断线取消当前轮次，三秒后恢复连接，只同步新目录，不自动重试选择。单次收音上限 10 秒，等待回执上限 8 秒，辅机完整轮次上限 20 秒；目录变化或取消立即结束本轮。麦克风仅在有效按下期间打开，松键关闭，退出释放。两种固定短音使用系统默认输出设备，可能被录屏的系统声音轨道录入。日志记录轮次和原因，保存在工具目录 `logs/voice.log`，单文件最多 2 MiB、保留两份轮转，不保留原始音频。

## 协议和验证

UTF-8 JSON 行、每行最多 64 KiB、单 TCP 连接。辅机发送 `catalog/catalog_show/catalog_result/begin/end/cancel/result/guidance`，主机发送 `ready/catalog_select/select/fail`。目录携带 `domain` 和条目的 `id/name/map/team/command`。轮次、地图 epoch 和候选 revision 必须匹配，结果同时对应实际 plan_id。文件身份只用作目录键，客户端不接收或提交方案路径。

菜单显示、菜单选择和语音轮次分别处理。`catalog_show` 使用当前地图、epoch、revision；点击发送独立递增 ID 的 `catalog_select`，辅机复核后异步返回同 ID 的 `catalog_result`。旧目录、旧连接、重复请求及忙碌状态不能排队执行；菜单结果不播放语音提示音。GUI 点选先恢复原前台窗口，只有辅机成功提交后的 `guidance selected` 才显示双图。

`guidance` 独立于收音轮次，首连同步当前状态；同连接只接受更大的 revision，断线隐藏并重置。状态为 `hidden/selected/aligning/aligned/failed`，其中 `hidden/aligned` 隐藏窗口；图片不经过 TCP。

```json
{"type":"guidance","revision":12,"state":"selected","plan_id":"mirage-t1-vip-r2.json","reference_id":"mirage-t1-vip-r2","message":"已选择道具方案"}
```

每次新状态和显示期间的文件变更都会重新读取清单并核对图片 SHA-256；资源不一致时显示未就绪，不能继续显示旧图片。

## F10 自动制作与发送

制作时在游戏主机启动一次：

```powershell
pwsh -NoProfile -File .\capture_references.ps1 -HostAddress 192.168.3.20
```

辅机先运行新版 Xen，在道具测试页选中、保存并启用本次方案，Runtime 保持停止。你人工试投成功后，恢复该方案的原站位和正确瞄角、关闭控制台并切回 CS2，按 **F10**。F10 本身就是人工试投确认；工具自动读取辅机实际选择、截当前主屏、绑定原方案、生成双图与短名 ZIP，再发送并回读校验，不弹出二次确认、不自动投掷。忙时重复按键忽略，Ctrl+C 结束制作监听；F10被其他程序或Xen控制键占用时明确拒绝。

Mirage 的 T1～T5 共用 `standing/mirage-t-spawns.png`，显示时自动圈出对应站位；其他站位将你截取的 PNG 放到程序根 `cache/grenade/standing/<方案文件stem>.png`。站位图和瞄点图可不同尺寸，原图均不改；瞄点显示层自动标记中央原生320×320 ROI和中心点。默认名称如“T1 B小烟”“T1 VIP烟”，备用瞄角等区别保留。

本地成品在 `cache/grenade/references/<方案stem>/`，资源包在 `cache/grenade/packages/<地图>/<简短名称>.zip`。默认发送到辅机 `\\192.168.3.20\XenLab$\releases\Xen-current\cache\grenade`，可用 `-DestinationRoot` 指定现有共享目标，`-WebPort` 默认5015。只写该方案参考资源与资源包，不覆盖辅机方案；发送前后核对方案JSON、源SHA和图片摘要。重采旧件保留备份，发送失败保留本地成品及待发送记录；“已发送”只表示文件回读通过，不等于真实对齐或投掷验收。不会设置系统服务、改防火墙或安装语音模型。

网页手机选择仍是预览；F10绑定的是辅机道具测试页实际启用的方案。首次使用此制作入口需要新版辅机程序提供 `/capture-context.json`。

## 命令行分步采集入口

先人工确认当前方案成功投掷，再依次采站位图、复位采瞄点图。工具不会输入坐标、移动鼠标或投掷，也不会自动添加“人工通过”。以下命令从程序根运行；`--root` 可指定与客户端相同的本机资源根。

```powershell
# 列出 Mirage 的每套方案、原始控制台指令与双图进度。
py -3 -X utf8 -B tools/voice_switch/reference_tool.py list --plans cache/grenade/plans

# 本人已测试这一套投掷成功后，明确记录确认，并绑定原始完整方案。
py -3 -X utf8 -B tools/voice_switch/reference_tool.py begin --plan cache/grenade/plans/mirage-t1-vip-r2.json --verified --standing-note "站在对应出生点，位置保持不动"

# 自己调整到能看清站位的朝向；关闭控制台及参考窗，5秒后只截主屏。
py -3 -X utf8 -B tools/voice_switch/reference_tool.py standing --id mirage-t1-vip-r2 --capture --delay 5

# 用该套原指令复位站位和正确瞄角，落地后关闭控制台，再采瞄点图。
py -3 -X utf8 -B tools/voice_switch/reference_tool.py aim --id mirage-t1-vip-r2 --capture --delay 5
py -3 -X utf8 -B tools/voice_switch/reference_tool.py validate --id mirage-t1-vip-r2
```

已有原始 PNG 可分别使用 `standing --id <stem> --image <站位.png>` 和 `aim --id <stem> --image <瞄点.png>`，代替 `--capture`。站位图可共用另一分辨率的说明图；瞄点图须保持当前采集源的全屏原始分辨率，不裁剪、不缩放、不往原图画框。主机提示窗按各图实际尺寸显示，并绘制参考 ROI 框和瞄点。需要把两图与 `reference.json` 一起原样同步到辅机同名资源目录。

默认 ROI 是全屏中央原生 320×320，瞄点为 ROI 坐标 `(160,160)`。如果 Runtime 来源发生裁剪或缩放，按实际几何在 `aim` 步骤显式提供 `--roi-x/--roi-y/--scale-x/--scale-y`：ROI 左上角使用原图坐标，scale 表示每个 ROI 像素覆盖的原图像素数；程序不推测缩放关系。与当前 Capture 几何不一致时不能自动对齐。采图仅支持 Windows 主屏；多屏或独占全屏无法得到正确图像时，使用游戏原始 PNG 导入并人工核对，不把黑图当成功参考。

完成目录为 `cache/grenade/references/<方案stem>/`，含 `standing.png`、`aim.png`、`reference.json`；`draft.json` 保留采集起点。清单含原始完整 `plan`、`plan_id`、`manual_throw_verified=true`、两图文件名和原文件 SHA-256、源尺寸与 ROI 几何、瞄点、可选站位说明。未显式确认、缺图、瞄点图与采集源尺寸不符、内容被改或越界路径均拒绝；站位图允许独立尺寸。此分步入口不覆盖现有完整资源和已采单图；F10入口重采时自动备份旧资源。

主机实用入口不要求再次打开控制台逐次投掷；上述坐标只用于首次建立或重新建立人工参考。资源核对和自动对齐结果不代替真实投掷落点验收。

## 实现资料与验证范围

双图采用 Windows 的 [扩展窗口样式](https://learn.microsoft.com/en-us/windows/win32/winmsg/extended-window-styles) 和 [分层窗口命中规则](https://learn.microsoft.com/en-us/windows/win32/winmsg/window-features#layered-windows)：`WS_EX_NOACTIVATE` 保持焦点，分层窗口配合 `WS_EX_TRANSPARENT` 让鼠标穿过；显示时使用 `SWP_NOACTIVATE`。原图通过系统 [GDI+ Bitmap 接口](https://learn.microsoft.com/en-us/windows/win32/gdiplus/-gdiplus-bitmap-flat) 解码后复制为独立位图，及时释放源文件；Win32 绘制图片与参考框。采图复用 [GDI BitBlt/GetDIBits 的官方机制](https://learn.microsoft.com/en-us/windows/win32/gdi/capturing-an-image)，Python 标准库编码 PNG；未复制第三方实现或引入新模型。

离线资源专项为 `py -3 -X utf8 -B tests/grenade_reference_tests.py`，使用临时合成图，既不采主屏也不访问设备。菜单与引导消息纳入原客户端协议专项。显式运行 `py -3 -X utf8 -B tests/grenade_guidance_window_tests.py --run` 会短暂打开合成双图与测试菜单，核对窗口样式、焦点、隐藏、Tk 点击回执与子进程清理，不发送物理输入。真实游戏全屏显示、鼠标穿透体验、双机消息延迟和道具对齐仍需各自验收，不由本机窗口检查替代。

专项：仓库根运行 `py -3 -X utf8 -B tests/voice_switch_client_tests.py`。协议回归与官方音频推理只能证明软件契约及离线模型可用，不能代表用户口令准确率、麦克风首音、双机延迟或真实游戏效果。

使用已安装依赖的 Python，加 `--model <最小模型目录> --samples <官方模型包/test_wavs>` 运行同一测试可执行真实模型回归；两参数必须一起提供。该测试不打开麦克风。
