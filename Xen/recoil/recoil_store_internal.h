#ifndef RECOIL_STORE_INTERNAL_H
#define RECOIL_STORE_INTERNAL_H

#include <cstdint>
#include <istream>
#include <limits>
#include <stdexcept>
#include <string>

namespace recoil::detail {

// 文件大小是分配上界而非可截断许可；读完还须确认没有增长或短读。
inline std::string read_bounded_stream(std::istream& stream, std::uintmax_t checked_size,
                                       std::uintmax_t maximum) {
    if (checked_size > maximum || checked_size > std::numeric_limits<std::size_t>::max() ||
        checked_size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max()))
        throw std::runtime_error("曲线文件大小超过读取上限");
    std::string text(static_cast<std::size_t>(checked_size), '\0');
    stream.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (stream.bad() || static_cast<std::uintmax_t>(stream.gcount()) != checked_size)
        throw std::runtime_error("读取曲线文件失败或文件已缩短");
    char extra = 0;
    if (stream.get(extra) || !stream.eof() || stream.bad())
        throw std::runtime_error("曲线文件读取期间增长或读取失败");
    return text;
}

} // namespace recoil::detail

#endif // RECOIL_STORE_INTERNAL_H
