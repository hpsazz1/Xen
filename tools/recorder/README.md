# Xen 独立录制

复用本机 OBS 32.1 的 libobs、桌面采集及 NVENC H.264 编码，独立进程录制主显示器原生分辨率、默认 240 FPS / 60 Mbps，不采集系统声音。原 OBS 可以继续提供 320×320 NDI。录制程序不连接输入设备，不修改急停参数。

双击桌面“Xen 独立录制”快捷方式，先在辅机开启“详细运行记录”并启动 Runtime，再点“开始录制”并返回游戏。结束时点“停止并保存”，等界面提示“已保存录像和同步索引”后退出，通常需要两秒多完成尾标记和封尾。“补一个SYNC”可在长录像中追加标记；不需要再按 Ctrl+F7。SYNC 直接画入录制帧，程序窗口不预览游戏；应在成片中查看蓝色编号。

录像默认保存于 Windows“视频”目录下的 `XenRecorder` 文件夹。每次生成 `.mkv`、同名 `.sync.jsonl` 以及诊断 `.log`。MKV 在意外退出后更易恢复；保留原始文件用于分析。索引保存实际编码包 PTS/timebase、对应渲染 CTS、主机稳态/系统时钟和辅机四戳映射。辅机 HTTP 接收仅代表入队，需结合辅机归档核对是否实际写入；网络失败会保留本地视频/索引并显示原因。

首尾标记由程序自动生成，每个持续两秒。前后标记可以校正时钟偏移及漂移；网络不确定度与视频帧间隔分别保留，不能当作游戏停稳或首发命中的证明。GPU/编码掉帧、未匹配标记及队列缺口会在索引封尾记录。

本版采用 `obs_nvenc_h264_soft`，仍使用 NVIDIA 硬件编码。OBS 32.1 texture 编码路径的内容与回调 CTS 在本次实测存在一帧错位，raw 帧入口经过逐帧解码及容器时间核对，未用固定帧偏移修补。代价是一次 GPU→CPU→NVENC 的帧传递；游戏负载下应检查界面和索引的“渲染/编码掉帧”。

分析时以索引的 PTS/timebase 和 MKV 原始视频时间为准。部分播放器或 OpenCV 5 会因静音 AAC 的 CodecDelay 整体平移显示时间；不要直接把播放器毫秒读数当作原始视频 PTS。随附源码中的验证器会分别报告容器时间和解码器归一化偏移。

程序依赖当前安装的 OBS 32.1；不需要手工配置 PATH。附带的 `obs-ffmpeg-mux.exe` 和 `obs-nvenc-test.exe` 来自同版本 OBS，其他运行库从 OBS 安装目录加载。安装目录变化时用 `--obs-root` 指定新路径；升级 OBS ABI 前重新核对和构建。

## 构建与有界验证

在仓库根执行 `scripts/build_xen_recorder.ps1 -Install -DesktopShortcut`。只构建独立工具和两项专项，安装至 `cache/tools/recorder/XenRecorder.exe`，并创建桌面“Xen 独立录制”快捷方式。该入口显式使用240FPS、不采集系统声音。

```powershell
# 合成画面：不打开屏幕或声音设备，仍通过真实NVENC/封装链。
& .\build\nvidia\obs-recorder\package\XenRecorder.exe --synthetic --no-audio --width 1280 --height 720 --fps 120 --duration 7 --aux-url none --output E:\Xen\temp\XEN-RECORDER-20261029-001\synthetic

# 默认主显示器，无系统声音；录制8秒，真实设备输入不参与。
& .\build\nvidia\obs-recorder\package\XenRecorder.exe --duration 8
```

默认不打开 WASAPI，文件仍包含封装器要求的静音 AAC 音轨；`--audio` 才采集系统声音。自动模式时长限制为 6～43200 秒。额外选项：`--output`、`--fps`、`--bitrate`（kbps）、`--aux-url`。默认必须有 NVENC，只有显式传入 `--allow-software-encoder` 才允许软件编码回退，并在索引记录实际编码器。

```powershell
# 顺序解码并读取实际MKV视频Block时间，核对首尾蓝条与同名索引。
py -3 -X utf8 -B tools/recorder/verify_video.py --video <录像.mkv> --report <核查.json> --contact-sheet <标记裁图.png>

# 回环服务测试严格四戳消息；不连接辅机或输入设备。
py -3 -X utf8 -B tools/recorder/integration_tests.py --recorder E:\Xen\cache\tools\recorder\XenRecorder.exe --output <新建测试目录>
```

视频验证器使用现有 Python/OpenCV/NumPy，只支持本工具单路 AVC、无视频 lacing 的 MKV。它核对实际容器 PTS，允许毫秒封装量化产生的0.6ms误差；缺标记、PTS错位、未封尾或掉帧/队列缺口时返回失败。编号文字需结合仅含蓝条的裁图人工核对。

本工具独立采用 GPL-2.0-or-later，见 `LICENSE.GPL-2.0`；OBS 原始代码和二进制受其各自许可证约束。接口研究与交付证据保存在仓库本地 `docs/evidence/XEN-RECORDER-20261029-001/`。
