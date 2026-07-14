# 配置指南

运行时配置文件为 `config.ini`。应用首次启动时，若该文件不存在，则会自动创建。

大多数设置以简单的根级 `key = value` 行形式保存。目前应用唯一写入的真正 INI 段落是 `[Games]`，用于存储游戏灵敏度配置文件。

对于大多数用户而言，GUI 是编辑设置最安全的方式。此页面旨在帮助你理解和直接编辑配置文件。

## 基本规则

- 布尔值使用 `true` 或 `false`。
- 小数值使用点号，例如 `0.35`。
- 按键列表以逗号分隔，例如 `RightMouseButton,LeftShift`。
- 路径可以是相对于可执行文件夹的路径。
- 如果某个值超出其可接受范围，应用会将其限制在范围内或回退到有效的默认值。
- Razer 控制方式在被选中时不会回退到其他方式。

以下默认值是针对全新配置生成的默认值。源代码中有少量旧版缺失键的回退值因向后兼容而不同；这些差异已在相关位置标注。

## 快速后端设置

### DML 构建

使用 ONNX 模型：

```ini
backend = DML
ai_model = sunxds_0.5.6.onnx
```

如果 DML 运行但未检测到任何内容，请检查模型是否为 `.onnx`，暂时降低 `confidence_threshold`，并确认你的类别 ID。

### CUDA + TensorRT 构建

使用 TensorRT 引擎模型：

```ini
backend = TRT
ai_model = sunxds_0.5.6.engine
capture_use_cuda = true
```

圆形 FOV 可保持启用状态，用于圆形目标过滤。

## 画面捕获

| 键 | 默认值 | 说明 |
|---|---:|---|
| `capture_method` | `duplication_api` | 捕获源。常用值有 `duplication_api`、`winrt`、`virtual_camera`、`udp_capture` 和 `ndi`。 |
| `capture_target` | `monitor` | 捕获目标类型。通常为 `monitor`。 |
| `capture_window_title` | 空 | 选择窗口捕获目标时使用的窗口标题。 |
| `ndi_source_name` | `HPSAZZ (main)` | NDI 完整源名称；也可设为 `Auto` 自动连接第一个发现的源。 |
| `ndi_source_width` | `2560` | NDI 预裁剪 ROI 对应的完整游戏 FOV 宽度。`0` 表示使用 Xen 帧元数据或视频帧宽度。 |
| `ndi_source_height` | `1440` | NDI 预裁剪 ROI 对应的完整游戏 FOV 高度。必须与宽度成对设置。 |
| `udp_ip` | `192.168.3.10` | UDP 捕获的发送端 IP 过滤；诊断时可使用 `0.0.0.0` 接受任意发送端。 |
| `udp_port` | `2333` | UDP 捕获端口。 |
| `udp_source_width` | `2560` | UDP 预裁剪 ROI 对应的完整游戏 FOV 宽度；发送完整画面或未知时设为 `0`。 |
| `udp_source_height` | `1440` | UDP 预裁剪 ROI 对应的完整游戏 FOV 高度；必须与宽度成对设置。 |
| `detection_resolution` | `320` | 方形推理/捕获处理尺寸。有效值为 `160`、`320` 和 `640`。值越高可提升细节，但会消耗更多性能。 |
| `capture_fps` | `60` | 请求的捕获帧率。 |
| `monitor_idx` | `0` | 显示器捕获的显示器索引。 |
| `circle_fov_enabled` | `true` | 启用当前圆形 FOV 限制器。 |
| `circle_fov_radius_percent` | `100` | 圆形 FOV 半径，以处理后的捕获区域的百分比表示。取值范围限定在 `1..100`。 |
| `circle_fov_show_preview` | `true` | 在 GUI 预览中显示圆形 FOV（如果可用）。 |
| `capture_borders` | `true` | 在适用时包含窗口边框。 |
| `capture_cursor` | `true` | 在适用时包含光标。 |
| `virtual_camera_name` | `None` | 虚拟摄像头捕获的摄像头名称。 |
| `virtual_camera_width` | `1920` | 请求的虚拟摄像头宽度。 |
| `virtual_camera_heigth` | `1080` | 请求的虚拟摄像头高度。该键在配置中目前拼写为 `heigth` 以保证兼容性。 |

### UDP 捕获

当另一台 PC 或进程通过网络将屏幕/摄像头流发送到此应用时，使用 UDP 捕获。

```ini
capture_method = udp_capture
udp_ip = 192.168.3.10
udp_port = 2333
udp_source_width = 2560
udp_source_height = 1440
detection_resolution = 320
capture_fps = 60
```

接收端期望通过 UDP 接收 MJPEG 字节流。每个帧必须是标准的 JPEG 图像；应用通过 JPEG 起始/结束标记查找帧，并使用 OpenCV 解码。这不是 RTP、RTSP 或自定义数据包头协议。

`udp_ip = 0.0.0.0` 是推荐的诊断设置，因为它接受任何发送端。仅当你希望忽略来自其他机器的数据包时，才将 `udp_ip` 设置为特定的发送端 IPv4 地址。应用在 `udp_port` 上监听；请确保该 UDP 端口已在接收端 PC 的 Windows 防火墙中放行。

有关 FFmpeg 发送端示例，请参阅 [UDP 局域网捕获](guides/udp-capture.md)。

### 圆形 FOV

使用 `circle_fov_enabled` 进行常规的圆形瞄准限制和 overlay 可视化。

## 目标锁定

| 键 | 默认值 | 说明 |
|---|---:|---|
| `disable_headshot` | `false` | 禁用针对头部的特定瞄准行为。 |
| `body_y_offset` | `0.15` | 身体瞄准的垂直目标偏移量。 |
| `head_y_offset` | `0.05` | 头部瞄准的垂直目标偏移量。 |
| `auto_aim` | `false` | 在当前控件和按钮支持时启用自动瞄准行为。 |
| `tracker_enabled` | `true` | 启用简单的持久目标跟踪器。禁用时，瞄准会在每个检测帧回退到最近目标选择。 |
| `tracker_overlay_table_enabled` | `true` | 在 Tracker overlay 标签页中显示目标跟踪信息表。 |

## 鼠标移动与跟踪

| 键 | 默认值 | 说明 |
|---|---:|---|
| `fovX` | `106` | 用于移动转换的水平游戏 FOV。 |
| `fovY` | `74` | 用于移动转换的垂直游戏 FOV。 |
| `minSpeedMultiplier` | `0.1` | 最小移动倍率。 |
| `maxSpeedMultiplier` | `0.1` | 最大移动倍率。 |
| `move_response_ms` | `80` | 基础控制响应时间，单位毫秒；决定未限速阶段的收敛形态。 |
| `move_max_speed_cps` | `1440` | 设备最大移动速度，单位 counts/s，范围30~4000；程序按相邻有效观测的真实间隔换算单帧预算。默认保持静止基线，320裁剪高速jump复测使用3200。 |
| `move_integral_time_ms` | `0` | 移动目标PI积分时间，单位毫秒；0为关闭，非零最小50 ms。用于消除匀速目标的比例稳态误差；有效积分输出在进入中心区及轻微越心时继续保持，反向误差达到稳定半径后清除对应轴旧积分。源码默认关闭供未标定部署使用；当前已验证现场基线冻结为320 ms。 |
| `auto_derive_tracker_params` | `true` | 按检测分辨率和实际捕获 FPS 自动推导目标跟踪参数；不会覆盖 `move_response_ms` 或 `move_max_speed_cps`。 |
| `prediction_enabled` | `true` | 启用连续真实观测预测。关闭时预测器完全旁路，基础滤波位置直接进入控制器。 |
| `prediction_lead_ms` | `50` | 在自动补偿观测年龄之外增加的常速度前瞻时间，范围0~100 ms。 |
| `prediction_velocity_tau_ms` | `50` | 兼容旧配置键名，实际表示稳健速度回归窗口，范围40~120 ms；更小值提高变向响应，更大值增强抗检测抖动。 |
| `prediction_strength` | `1.0` | 对“稳健速度×前瞻时间”的自动提前距离做统一缩放，范围0~4。 |
| `profile_calibration_enabled` | `false` | 启用被动Profile标定。只读取设备实际成功发送的counts与后续raw pivot，展示实测比例、角度响应、延迟和可信度；不会主动移动鼠标、覆盖`[Games]`或保存机器缓存。 |
| `snapRadius` | `1.5` | 近距离目标吸附半径。 |
| `nearRadius` | `25.0` | 近距离目标行为开始的半径。 |
| `speedCurveExponent` | `3.0` | 速度缩放的曲线形状。 |
| `snapBoostFactor` | `1.15` | 接近吸附半径时的额外速度。 |
| `easynorecoil` | `false` | 启用简单的后坐力补偿。 |
| `easynorecoilstrength` | `0.0` | 后坐力补偿强度。 |
| `input_method` | `WIN32` | 输出/控制方式。见下文。 |

四个预测项位于UI侧栏“瞄准 → 预测参数”。关闭“启用预测”后，三个数值项会置灰但仍保持可见。程序先补偿自身视角运动，再对短窗连续观测做直线回归；只有净位移、速度和轨迹一致性同时成立才使用常速度提前。已建立预测可跨过最多两帧窗口退化；连续两帧低速才撤销，反向必须由可靠回归方向确认。目标框尺寸和诊断加速度均不参与提前量计算。

r11额外使用最近三帧屏幕目标收敛、自身视角移动和预测方向联合识别静止收敛伪迹。该内部安全门控没有新增调参项；命中时仅撤销本帧提前，并通过流水线CSV字段 `PredictionSelfMotionSuppressed` 审计。

首轮实机数据证明15 ms单帧速度与显式加速度会放大检测抖动，因此预测基线为 `prediction_lead_ms=50`、`prediction_velocity_tau_ms=50`、`prediction_strength=1.0`。r10使用透视投影统一像素、视角和设备计数；320裁剪jump速度复测将 `move_max_speed_cps` 提高到3200。该候选只补偿可靠的可观测运动，不估算目标距离或武器弹道。

被动Profile标定位于UI“瞄准 → 游戏配置”。标定时保持目标与角色静止，目标初始位置应与准星有足够偏差以产生连续设备命令，且期间不要手动移动鼠标。X/Y独立判断有效；某轴缺少足够控制激励时，该轴会保持无效。详细设计、CSV字段和复测步骤见`docs/067被动Profile自动标定实现20260714.md`。

## 输入方式

有效值：

```text
```

| 方式 | 说明 |
|---|---|
| `WIN32` | 标准 Windows 鼠标事件。 |
| `GHUB` | 使用 GHub DLL 输出（如果可用）。 |
| `RAZER` | 通过 `rzctl.dll` 使用 Razer 控制 DLL 输出。 |
| `KMBOX_NET` | 网络 kmbox 控制。 |
| `KMBOX_A` | kmbox A 串行/HID 风格控制。 |
| `MAKCU` | MAKCU 串行控制。 |


当选择硬件方式时，应用期望该方式正常工作。Razer 是明确指定且不会静默回退到 Win32 或其他备用方式的。

## 轨迹模拟

| 键 | 默认值 | 说明 |
|---|---:|---|
| `wind_mouse_enabled` | `false` | 启用轨迹模拟，使鼠标移动更接近人手操作的曲线路径。 |
| `wind_G` | `18.0` | 鼠标移速系数，越大移向目标越快越直。 |
| `wind_W` | `15.0` | 轨迹摆动幅度，越小移动路径越直。 |
| `wind_M` | `10.0` | 单步最大移动像素数，限制每帧位移上限。 |
| `wind_D` | `8.0` | 微调距离阈值，靠近目标后切换为精细微调模式。 |

## 设备控制章节

### Kmbox Net

| 键 | 默认值 | 说明 |
|---|---:|---|
| `kmbox_net_ip` | `192.168.2.188` | 设备 IP 地址。 |
| `kmbox_net_port` | `13384` | 设备端口。 |
| `kmbox_net_uuid` | `7679E04E` | 设备 UUID/令牌。 |

### Kmbox A

| 键 | 默认值 | 说明 |
|---|---:|---|
| `kmbox_a_pidvid` | 空 | 组合的 PID/VID 字符串，格式为 `PPPPVVVV`。 |

### MAKCU

| 键 | 默认值 | 说明 |
|---|---:|---|
| `makcu_baudrate` | `115200` | 串行波特率。 |
| `makcu_port` | `COM0` | 串行端口。 |

## 鼠标射击

| 键 | 默认值 | 说明 |
|---|---:|---|
| `auto_shoot` | `false` | 启用自动射击行为。 |
| `bScope_multiplier` | `1.0` | 瞄准镜倍率。旧版配置的缺失键回退值为 `1.2`。 |

## AI

| 键 | 默认值 | 说明 |
|---|---:|---|
| `backend` | CUDA 中为 `TRT`，DML 中为 `DML` | 推理后端。 |
| `dml_device_id` | `0` | DirectML 设备索引。 |
| `ai_model` | CUDA: `sunxds_0.5.6.engine`，DML: `sunxds_0.5.6.onnx` | 模型文件。旧版配置的缺失键回退目前使用 `sunxds_0.8.0.*`。 |
| `confidence_threshold` | `0.10` | 最低检测置信度。缺失键回退值为 `0.15`。 |
| `nms_threshold` | `0.50` | 非极大值抑制阈值。 |
| `max_detections` | `100` | 每帧保留的最大检测数。缺失键回退值为 `20`。 |
| `export_enable_fp8` | 生成的 CUDA 配置中为 `false` | TensorRT 导出选项，仅限 CUDA 构建。缺失键回退值为 `true`。 |
| `export_enable_fp16` | CUDA 中为 `true` | TensorRT 导出选项，仅限 CUDA 构建。 |

`fixed_input_size` 作为内部运行时配置字段存在，但当前不会写入生成的配置文件中。

## CUDA

以下键仅在 CUDA 构建中写入。

| 键 | 默认值 | 说明 |
|---|---:|---|
| `use_cuda_graph` | `false` | 在支持的位置启用 CUDA graph 路径。 |
| `use_pinned_memory` | `false` | 固定内存的生成默认值。缺失键回退值为 `true`。 |
| `gpuMemoryReserveMB` | `2048` | GPU 内存预留量。 |
| `enableGpuExclusiveMode` | `true` | 在应用支持的位置启用独占 GPU 模式。 |
| `capture_use_cuda` | `true` | 允许使用 CUDA 捕获路径。 |

当预览、调试、数据采集或其他需要 CPU 可读像素的功能需要使用画面时，CUDA 捕获仍会创建 CPU 副本。

## 系统

| 键 | 默认值 | 说明 |
|---|---:|---|
| `cpuCoreReserveCount` | `4` | 避免大量使用的 CPU 核心数。 |
| `systemMemoryReserveMB` | `2048` | 系统内存预留量。 |

## 按钮

| 键 | 默认值 | 说明 |
|---|---:|---|
| `button_targeting` | `RightMouseButton` | 瞄准/目标锁定按钮列表。 |
| `button_shoot` | `LeftMouseButton` | 射击按钮列表。 |
| `button_zoom` | `RightMouseButton` | 缩放/瞄准镜按钮列表。 |
| `button_exit` | `F2` | 退出快捷键。 |
| `button_pause` | `F3` | 暂停快捷键。 |
| `button_reload_config` | `F4` | 重新加载配置快捷键。 |
| `button_open_overlay` | `Home` | 打开 overlay 快捷键。 |
| `enable_arrows_settings` | `false` | 启用方向键设置行为。 |

在支持的位置使用 `None` 禁用按钮。

## Overlay

| 键 | 默认值 | 说明 |
|---|---:|---|
| `overlay_opacity` | `225` | Overlay 不透明度，取值范围 `0..255`。 |
| `overlay_ui_scale` | `1.0` | Overlay UI 缩放比例。 |
| `overlay_exclude_from_capture` | `true` | 尝试将 overlay 排除在捕获帧之外。 |
| `overlay_x` | `0` | Overlay 编辑器窗口 X 位置。移动后自动保存。 |
| `overlay_y` | `0` | Overlay 编辑器窗口 Y 位置。移动后自动保存。 |
| `overlay_width` | `860` | Overlay 编辑器窗口宽度。调整大小后自动保存。 |
| `overlay_height` | `526` | Overlay 编辑器窗口高度。调整大小后自动保存。 |

## 深度

| 键 | 默认值 | 说明 |
|---|---:|---|
| `depth_inference_enabled` | `true` | 启用深度推理功能。 |
| `depth_model_path` | `depth_anything_v2.engine` | 深度模型路径。 |
| `depth_fps` | `100` | 深度更新帧率。最小值为 `0`。 |
| `depth_colormap` | `18` | OpenCV 颜色映射索引。取值范围限定在 `0..21`。 |
| `depth_mask_enabled` | `false` | 启用深度遮罩。 |
| `depth_mask_fps` | `5` | 深度遮罩更新帧率。最小值为 `0`。 |
| `depth_mask_near_percent` | `20` | 近距离深度百分比。取值范围限定在 `1..100`。 |
| `depth_mask_expand` | `0` | 遮罩扩展像素数。取值范围限定在 `0..128`。 |
| `depth_mask_hold_frames` | `0` | 遮罩保持额外帧数。取值范围限定在 `0..120`。 |
| `depth_mask_alpha` | `90` | 遮罩透明度。取值范围限定在 `0..255`。 |
| `depth_mask_invert` | `false` | 反转深度遮罩。 |
| `depth_debug_overlay_enabled` | `false` | 显示深度调试 overlay。 |

## 游戏 Overlay

| 键 | 默认值 | 说明 |
|---|---:|---|
| `game_overlay_enabled` | `false` | 启用游戏内 overlay 渲染。 |
| `game_overlay_max_fps` | `0` | Overlay 帧率上限。`0` 表示无上限/默认行为。 |
| `game_overlay_draw_boxes` | `true` | 绘制检测框。 |
| `game_overlay_compensate_latency` | `true` | 根据帧龄和捕获后记录的鼠标移动来偏移 overlay 框/图标。 |
| `game_overlay_draw_wind_tail` | `true` | 绘制轨迹模拟的移动轨迹。 |
| `game_overlay_draw_frame` | `true` | 绘制帧边框。 |
| `game_overlay_draw_circle_fov` | `true` | 在游戏 overlay 中绘制圆形 FOV。 |
| `game_overlay_show_target_correction` | `true` | 绘制目标校正指示器。 |
| `game_overlay_box_a/r/g/b` | `255/0/255/0` | 框颜色，格式为 alpha/红/绿/蓝。 |
| `game_overlay_frame_a/r/g/b` | `180/255/255/255` | 帧颜色，格式为 alpha/红/绿/蓝。 |
| `game_overlay_box_thickness` | `2.0` | 检测框线条粗细。 |
| `game_overlay_frame_thickness` | `1.5` | 帧线条粗细。 |
| `game_overlay_icon_enabled` | `false` | 启用绘制图标标记。 |
| `game_overlay_icon_path` | `icon.png` | 图标文件路径。 |
| `game_overlay_icon_width` | `64` | 图标宽度。 |
| `game_overlay_icon_height` | `64` | 图标高度。 |
| `game_overlay_icon_offset_x` | `0.0` | 图标 X 偏移量。 |
| `game_overlay_icon_offset_y` | `0.0` | 图标 Y 偏移量。 |
| `game_overlay_icon_anchor` | `center` | 图标锚点：`center`、`top`、`bottom` 或 `head`。 |
| `game_overlay_icon_class` | `-1` | 要绘制图标的类别。`-1` 表示所有类别。 |

## 数据采集

| 键 | 默认值 | 说明 |
|---|---:|---|
| `collect_data_while_playing` | `false` | 运行时保存数据。 |
| `collect_only_when_aimbot_running` | `false` | 仅在瞄准机器人主动运行时采集。 |
| `collect_only_when_targets_present` | `true` | 仅采集包含目标的帧。 |
| `collect_save_every_n_frames` | `15` | 保存间隔。取值范围限定在 `1..600`。 |
| `collect_jpeg_quality` | `95` | JPEG 质量。取值范围限定在 `50..100`。 |
| `collect_output_dir` | 空 | 输出文件夹。 |
| `auto_label_data` | `true` | 自动写入标签。 |
| `auto_label_min_conf` | `0.30` | 自动标注置信度。取值范围限定在 `0.01..0.99`。 |
| `auto_label_max_boxes` | `20` | 自动标注框数上限。取值范围限定在 `1..200`。 |
| `auto_label_record_classes` | 空 | 可选的类别过滤列表。 |

数据采集需要 CPU 可读的帧，因此可能会改变画面捕获的性能诊断信息。

## 类别

| 键 | 默认值 | 说明 |
|---|---:|---|
| `class_player` | `0` | 玩家/身体检测的模型类别 ID。 |
| `class_head` | `1` | 头部检测的模型类别 ID。 |

## 调试

| 键 | 默认值 | 说明 |
|---|---:|---|
| `show_window` | `true` | 显示调试/预览窗口。这可能需要 CPU 帧副本。 |
| `show_fps` | `false` | 显示帧率计数器。 |
| `screenshot_button` | `None` | 截图快捷键。 |
| `screenshot_delay` | `500` | 截图延迟，单位为毫秒。 |
| `verbose` | `false` | 启用更详细的日志输出。 |

## 游戏配置文件

激活的配置文件通过以下方式选择：

```ini
active_game = CS
```

配置文件存储在 `[Games]` 下：

```ini
[Games]
CS = 1.4,0.022,0.022
UNIFIED = 1,0.022,0.022
```

格式：

```text
name = sensitivity,yaw,pitch[,fovScaled,baseFOV]
```

示例：

```ini
[Games]
CS = 1.4,0.022,0.022
UNIFIED = 1,0.022,0.022
MY_GAME = 2.5,0.02,0.02,true,90
```

如果 `active_game` 缺失或无效，应用优先回退到内置 `CS`，再回退到 `UNIFIED`。
