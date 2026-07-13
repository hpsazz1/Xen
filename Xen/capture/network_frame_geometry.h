#ifndef NETWORK_FRAME_GEOMETRY_H
#define NETWORK_FRAME_GEOMETRY_H

/**
 * @brief 网络预裁剪帧对应的完整游戏 FOV 尺寸。
 *
 * encodedWidth/Height 是实际传输图像尺寸；configuredSourceWidth/Height 是该 ROI 在主机完整
 * 游戏画面中的坐标空间。配置尺寸必须成对有效、不得小于编码帧且不得超过安全上限，否则回退
 * 到编码尺寸，避免错误配置把鼠标角度换算放大。
 */
struct NetworkFrameGeometry
{
    int sourceWidth = 0;
    int sourceHeight = 0;
    bool fromConfig = false;
};

inline NetworkFrameGeometry ResolveConfiguredNetworkFrameGeometry(
    int encodedWidth,
    int encodedHeight,
    int configuredSourceWidth,
    int configuredSourceHeight)
{
    NetworkFrameGeometry result{ encodedWidth, encodedHeight, false };
    if (encodedWidth <= 0 || encodedHeight <= 0)
        return result;

    constexpr int kMaxSourceDimension = 16384;
    if (configuredSourceWidth >= encodedWidth &&
        configuredSourceHeight >= encodedHeight &&
        configuredSourceWidth <= kMaxSourceDimension &&
        configuredSourceHeight <= kMaxSourceDimension)
    {
        result.sourceWidth = configuredSourceWidth;
        result.sourceHeight = configuredSourceHeight;
        result.fromConfig = true;
    }
    return result;
}

#endif // NETWORK_FRAME_GEOMETRY_H
