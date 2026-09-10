#ifndef SOURCE_CONTEXT_INTERNAL_H
#define SOURCE_CONTEXT_INTERNAL_H

#include "source_context/source_context.h"
#include <array>
#include <chrono>
#include <span>
#include <string_view>

namespace source_context::detail {
using Clock = std::chrono::steady_clock;
using Nonce = std::array<std::uint8_t, 16>;
inline constexpr std::size_t kPacketBytes = 136;
using Packet = std::array<std::uint8_t, kPacketBytes>;
enum class Focus : std::uint8_t { Unknown = 0, Background = 1, Foreground = 2 };
struct Message {
    bool response = false;
    Focus focus = Focus::Unknown;
    Nonce nonce{};
    std::uint64_t session = 0;
    std::uint64_t sequence = 0;
    std::string process_name;
};

bool valid_config(const SourceContextConfig& config) noexcept;
// 仅接受 ASCII exe 文件名，精确匹配（Windows 文件名不区分大小写），不匹配标题或子串。
bool exact_process_match(std::string_view expected, std::string_view actual) noexcept;
bool encode(const Message& message, std::string_view token, Packet& packet) noexcept;
bool decode(std::span<const std::uint8_t> packet, std::string_view token, Message& message) noexcept;

// 独立于网络和 Windows 前台查询的证据接受器，测试使用可控单调时间。
class Evidence final {
public:
    void begin(const Nonce& nonce, Clock::time_point sent_at) noexcept;
    bool accept(const Message& response, std::string_view process,
                Clock::time_point now, int ttl_ms) noexcept;
    void invalidate() noexcept;
    SourceContextSnapshot snapshot(Clock::time_point now) const noexcept;
private:
    Nonce nonce_{};
    bool pending_ = false;
    Clock::time_point sent_at_{};
    Clock::time_point evidence_at_{};
    Clock::time_point expires_at_{};
    SourceContextSnapshot snapshot_{};
};
} // namespace source_context::detail
#endif
