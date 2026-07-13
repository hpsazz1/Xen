#ifndef NDI_FRAME_GEOMETRY_H
#define NDI_FRAME_GEOMETRY_H

#include <string_view>

// NDI 预裁剪画面的坐标契约。
// encodedWidth/encodedHeight 是网络视频帧实际尺寸；sourceWidth/sourceHeight 是游戏完整 FOV
// 对应的像素跨度。二者分离后，320x320 的低带宽中心 ROI 仍可按 2560x1440 正确换算鼠标量。
struct NdiFrameGeometry
{
    int sourceWidth = 0;
    int sourceHeight = 0;
    bool fromMetadata = false;
    bool fromConfig = false;
};

// 解析形如 <xen source_width="2560" source_height="1440" .../> 的 NDI 视频帧元数据。
// 仅识别 xen 元素，避免误用 OBS/NDI 其他 XML 中含义不同的 width/height 字段。
NdiFrameGeometry ResolveNdiFrameGeometry(
    int encodedWidth,
    int encodedHeight,
    std::string_view metadata,
    int configuredSourceWidth,
    int configuredSourceHeight);

#endif // NDI_FRAME_GEOMETRY_H
