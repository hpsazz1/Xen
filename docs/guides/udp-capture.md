# 局域网 UDP 采集（FFmpeg）

UDP 采集从另一台 PC 或进程接收 MJPEG 字节流。发送端通过 UDP 流式传输 JPEG 帧；本应用在接收端 PC 上解码这些帧。

## 接收端 PC 设置

1. 设置采集配置：

```ini
capture_method = udp_capture
udp_ip = 0.0.0.0
udp_port = 1234
detection_resolution = 640
capture_fps = 60
```

2. 在 Windows 防火墙中打开 UDP 端口。以管理员身份运行 PowerShell：

```powershell
New-NetFirewallRule -DisplayName "Ashfiexe UDP Capture 1234" -Direction Inbound -Protocol UDP -LocalPort 1234 -Action Allow
```

3. 找到接收端 PC 的本地 IP 地址：

```powershell
ipconfig
```

在下面的发送端命令中，使用接收端 PC 的 `IPv4 Address` 作为 `RECEIVER_IP`。

## 发送端 PC 示例

桌面采集：

```bash
ffmpeg -f gdigrab -framerate 60 -i desktop -vcodec mjpeg -q:v 5 -f mjpeg udp://RECEIVER_IP:1234
```

接收端会从完整网络帧中心按 1:1 像素裁出 `detection_resolution`，不会再把 16:9 画面拉伸为正方形。裸 MJPEG 没有源画面元数据，因此不要在发送端预先只发送中心 ROI；否则接收端无法知道该 ROI 原本属于多大的完整 FOV。

OBS 虚拟摄像头：

```bash
ffmpeg -f dshow -i video="OBS Virtual Camera" -vf scale=640:640 -vcodec mjpeg -q:v 5 -f mjpeg udp://RECEIVER_IP:1234
```

有效的检测分辨率为 `160`、`320` 和 `640`；不支持的值将回退为 `320`。为降低带宽而缩小完整帧时必须保持原宽高比，例如 2560×1440 可缩放为 1280×720，禁止缩放为 320×320。

## UDP 采集收不到帧

首先检查以下项目：

1. `capture_method = udp_capture`
2. `udp_port` 与发送端命令匹配。
3. 接收端防火墙允许该端口的入站 UDP。
4. 发送端向接收端 PC IP 流式传输 MJPEG，而不是向 `0.0.0.0`。
5. 诊断时将 `udp_ip = 0.0.0.0`，以便接受来自任何发送端的数据包。

仅在流正常工作后，才将 `udp_ip` 设置为特定的发送端 IPv4 地址。在当前实现中，此值用于过滤发送端地址，而不是本地绑定地址。

相关文档：

- [采集配置](../config.md#capture)
- [采集诊断](capture-diagnostics.md)
