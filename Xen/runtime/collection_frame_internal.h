#ifndef COLLECTION_FRAME_INTERNAL_H
#define COLLECTION_FRAME_INTERNAL_H

#include "capture/capture.h"
#include "source_context/source_context.h"
#include "weapon/weapon.h"
#include <cmath>

namespace runtime::detail {

// 只限制自动训练采样；普通切枪/换弹和非枪械不影响场景价值。
inline bool collection_game_context_allowed(bool enabled,
        const weapon::WeaponSnapshot& snapshot,
        std::chrono::steady_clock::time_point now) noexcept {
    if (!enabled) return true;
    return snapshot.identity_match && snapshot.player_playing &&
        snapshot.player_health.value_or(0) > 0 &&
        snapshot.game_phase != weapon::GamePhase::UNKNOWN &&
        snapshot.received_at <= now && now < snapshot.valid_until;
}

// 仅限制采集域；不依赖物理武装、按键或检测框，空场景仍可采集。
class CollectionFrameGate {
public:
    bool accept(bool context_enabled,
                const source_context::SourceContextSnapshot& source,
                const FrameTiming& timing,
                std::chrono::steady_clock::time_point now,
                bool require_source_time = true) noexcept {
        if (!context_enabled) {
            reset();
            return true;
        }
        if (!source.available || !source.focused || source.session_id == 0) {
            reset();
            return false;
        }
        if (!trusted_ || session_ != source.session_id) {
            trusted_ = true;
            session_ = source.session_id;
            focused_since_ = now;
        }
        // 配置了源时钟则不能在失效时降级到接收时刻。
        if (require_source_time && !timing.source_time_timing_valid) return false;
        // 无源映射后端只能筛掉本机积压，不能据此证明网络源画面生成时的焦点。
        const auto frame_at = timing.source_time_timing_valid
            ? timing.source_time_at : timing.captured_at;
        if (frame_at < focused_since_ || frame_at > now) return false;
        if (timing.source_time_timing_valid) {
            const double uncertainty = timing.source_clock_uncertainty_ms;
            return std::isfinite(uncertainty) && uncertainty >= 0.0 &&
                std::chrono::duration<double, std::milli>(frame_at - focused_since_).count()
                    >= uncertainty;
        }
        return true;
    }

private:
    void reset() noexcept {
        trusted_ = false;
        session_ = 0;
        focused_since_ = {};
    }
    bool trusted_ = false;
    std::uint64_t session_ = 0;
    std::chrono::steady_clock::time_point focused_since_{};
};

} // namespace runtime::detail

#endif
