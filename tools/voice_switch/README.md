# 主机道具制作与可选语音

日常入口已改为辅机内置手机网页，端口5015；使用辅机连接路由器的网卡地址。手机显示地图/阵营、站位分组、瞄点卡和实际操作结果；点选真正切换方案。主机不再显示清单或双图，右键旧入口已退役。Runtime保持运行，上键找点、下键执行原动作、左键取消。

## F10 自动制作与发送

制作时在游戏主机启动一次（以实际辅机专线地址为准）：

```powershell
pwsh -NoProfile -File .\capture_references.ps1 -HostAddress 192.168.3.20
```

辅机运行新版Xen，手机或道具测试页选中并启用本次方案。人工试投成功后，恢复原站位与正确瞄角，关闭控制台、切回CS2，按F10。Runtime可保持运行；不要同时执行找点或投掷。F10本身就是人工试投确认，不再弹二次确认、不自动投掷。

工具读取辅机实际选择、截当前主屏、绑定原方案、生成双图与短名ZIP，再发送并回读。忙时重复按键忽略，Ctrl+C结束制作监听。Mirage T1～T5共用`standing/mirage-t-spawns.png`；其他站位将PNG放到程序根`cache/grenade/standing/<方案文件stem>.png`。两图可不同尺寸；原图不改，手机覆盖绘制站位、中央原生320×320 ROI和瞄点。

本地成品位于`cache/grenade/references/<方案stem>/`，资源包位于`cache/grenade/packages/<地图>/<简短名称>.zip`。默认发送到`\\192.168.3.20\XenLab$\releases\Xen-current\cache\grenade`。`-DestinationRoot`指定已有共享目标，`-WebPort`默认5015。只传本次资源，不覆盖辅机原动作方案；重采保留备份，失败保留本地成品和待发送记录。

成功发送后，手机自动发现辅机资源版本、刷新图片与采集状态；辅机撤销旧对齐结果。手机使用路由器网段地址（本次WLAN为192.168.1.3），主机制作仍可走192.168.3.20专线。手机不需要主机client.py运行。

## 安装与启动

只有需要可选语音时才启动`start.ps1 -Voice`。需要Windows x64、Python3.12、麦克风及语音依赖；本机音频不上传、不保存。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\install.ps1
pwsh -NoProfile -File .\start.ps1 -HostAddress 192.168.3.20 -Voice
```

语音使用辅机单独绑定的语音键，按住说话、松开选择；不占用右键，不执行投掷。空口令仅从语音候选排除，不影响手机选择。默认TCP5014是可选语音通道，HTTP5015是手机页面和制作上下文。没有语音环境时不应为手机流程安装模型。

模型来源及许可证见[LICENSES.md](LICENSES.md)。`prepare_model.py --archive <已下载压缩包>`可复用已核验模型包。语音断线不重放旧请求，也不取消手机发起的道具操作。

## 命令行分步采集入口

下列旧分步入口供制作排查，日常直接F10。原始PNG不裁剪、不缩放、不往原图画框；站位图可以独立尺寸。

```powershell
py -3 -X utf8 -B tools/voice_switch/reference_tool.py list --plans cache/grenade/plans
py -3 -X utf8 -B tools/voice_switch/reference_tool.py begin --plan cache/grenade/plans/mirage-t1-vip-r2.json --verified --standing-note "站在对应出生点，位置保持不动"
py -3 -X utf8 -B tools/voice_switch/reference_tool.py standing --id mirage-t1-vip-r2 --capture --delay 5
py -3 -X utf8 -B tools/voice_switch/reference_tool.py aim --id mirage-t1-vip-r2 --capture --delay 5
py -3 -X utf8 -B tools/voice_switch/reference_tool.py validate --id mirage-t1-vip-r2
```

已有原始PNG可用`--image <路径>`代替`--capture`。默认ROI为全屏中央原生320×320，瞄点为ROI坐标(160,160)。来源有裁剪或缩放时，按实际映射提供`--roi-x/--roi-y/--scale-x/--scale-y`；与实时采集几何不一致不能用于找点。

## 验证范围

资源、制作和客户端专项分别为`tests/grenade_reference_tests.py`、`tests/grenade_capture_workflow_tests.py`、`tests/voice_switch_client_tests.py`；HTTP和实际Session选择由C++专项覆盖。历史`guidance.py`仅保留显式制作测试，日常client不再创建GUI。合成图、协议回执和文件同步不代表真实NDI/KMBOX找点与投掷落点验收。
