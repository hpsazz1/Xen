#include "ndi_frame_geometry.h"

#include <charconv>
#include <cctype>
#include <string_view>

namespace
{
constexpr int kMaxSupportedDimension = 16384;

bool IsValidSourceGeometry(int sourceWidth, int sourceHeight, int encodedWidth, int encodedHeight)
{
    // 当前协议只支持 1:1 像素中心 ROI，因此完整 FOV 不能小于收到的 ROI。
    // 上限防止损坏或恶意元数据把坐标比例放大到不可控范围。
    return sourceWidth >= encodedWidth && sourceHeight >= encodedHeight &&
           sourceWidth <= kMaxSupportedDimension && sourceHeight <= kMaxSupportedDimension;
}

bool ParsePositiveAttribute(std::string_view element, std::string_view name, int& value)
{
    const size_t namePos = element.find(name);
    if (namePos == std::string_view::npos)
        return false;

    size_t cursor = namePos + name.size();
    while (cursor < element.size() && std::isspace(static_cast<unsigned char>(element[cursor])))
        ++cursor;
    if (cursor >= element.size() || element[cursor] != '=')
        return false;
    ++cursor;
    while (cursor < element.size() && std::isspace(static_cast<unsigned char>(element[cursor])))
        ++cursor;
    if (cursor >= element.size() || (element[cursor] != '"' && element[cursor] != '\''))
        return false;

    const char quote = element[cursor++];
    const size_t end = element.find(quote, cursor);
    if (end == std::string_view::npos || end == cursor)
        return false;

    int parsed = 0;
    const char* first = element.data() + cursor;
    const char* last = element.data() + end;
    const auto result = std::from_chars(first, last, parsed);
    if (result.ec != std::errc() || result.ptr != last || parsed <= 0)
        return false;

    value = parsed;
    return true;
}
}

NdiFrameGeometry ResolveNdiFrameGeometry(
    int encodedWidth,
    int encodedHeight,
    std::string_view metadata,
    int configuredSourceWidth,
    int configuredSourceHeight)
{
    NdiFrameGeometry result{ encodedWidth, encodedHeight, false, false };
    if (encodedWidth <= 0 || encodedHeight <= 0)
        return result;

    const size_t xenStart = metadata.find("<xen");
    if (xenStart != std::string_view::npos)
    {
        const size_t xenEnd = metadata.find('>', xenStart);
        const std::string_view xenElement = metadata.substr(
            xenStart, xenEnd == std::string_view::npos ? metadata.size() - xenStart : xenEnd - xenStart + 1);
        int metadataWidth = 0;
        int metadataHeight = 0;
        if (ParsePositiveAttribute(xenElement, "source_width", metadataWidth) &&
            ParsePositiveAttribute(xenElement, "source_height", metadataHeight) &&
            IsValidSourceGeometry(metadataWidth, metadataHeight, encodedWidth, encodedHeight))
        {
            result.sourceWidth = metadataWidth;
            result.sourceHeight = metadataHeight;
            result.fromMetadata = true;
            return result;
        }
    }

    if (IsValidSourceGeometry(
            configuredSourceWidth, configuredSourceHeight, encodedWidth, encodedHeight))
    {
        result.sourceWidth = configuredSourceWidth;
        result.sourceHeight = configuredSourceHeight;
        result.fromConfig = true;
    }
    return result;
}
