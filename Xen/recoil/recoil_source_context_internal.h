#ifndef RECOIL_SOURCE_CONTEXT_INTERNAL_H
#define RECOIL_SOURCE_CONTEXT_INTERNAL_H

#include "recoil/recoil.h"
#include "source_context/source_context.h"
#include <mutex>
#include <optional>

namespace recoil_detail {
// 一次准备只绑定一个完整来源组合；恢复前台或较晚抵达的旧快照不能恢复旧许可。
class SourceRunBinding {
public:
    bool permits(const source_context::SourceContextSnapshot& focus, std::uint64_t weapon_epoch) {
        std::lock_guard lock(mutex_);
        const Identity current{focus.session_id, focus.control_safety_epoch, weapon_epoch};
        if (bound_ && (*bound_ != current || !focus.available || !focus.focused)) revoked_ = true;
        if (revoked_ || !focus.available || !focus.focused) return false;
        if (!bound_) bound_ = current;
        return true;
    }
    bool changed(const source_context::SourceContextSnapshot& focus, std::uint64_t weapon_epoch) const {
        std::lock_guard lock(mutex_);
        return revoked_ || (bound_ && *bound_ != Identity{focus.session_id, focus.control_safety_epoch, weapon_epoch});
    }
private:
    struct Identity {
        std::uint64_t session, source_epoch, weapon_epoch;
        bool operator==(const Identity&) const = default;
    };
    mutable std::mutex mutex_;
    std::optional<Identity> bound_; // 代次0是合法值，不能用作“尚未绑定”。
    bool revoked_ = false;
};

// 独立CLI的来源适配；只有其Worker线程推进。
class CalibrationSourceContext {
public:
    void apply(RecoilInput& input, const source_context::SourceContextSnapshot& focus, std::uint64_t weapon_epoch) {
        input.source_context_epoch = focus.control_safety_epoch;
        const bool stable = focus.available && focus.focused && focus.session_id == previous_session_;
        previous_session_ = focus.available && focus.focused ? focus.session_id : 0;
        input.focused = stable && input.profile_conditions_match && binding_.permits(focus, weapon_epoch);
    }
private:
    std::uint64_t previous_session_ = 0;
    SourceRunBinding binding_;
};

// App调试的来源/GSI绑定同时由采集runner与Worker读取。
class DebugSourceContext {
public:
    bool permits(const source_context::SourceContextSnapshot& focus, std::uint64_t weapon_epoch) {
        return binding_.permits(focus, weapon_epoch);
    }
    bool changed(const source_context::SourceContextSnapshot& focus, std::uint64_t weapon_epoch) const {
        return binding_.changed(focus, weapon_epoch);
    }
    void apply(RecoilInput& input, const source_context::SourceContextSnapshot& focus, bool context_valid) const {
        input.source_context_epoch = focus.control_safety_epoch;
        input.profile_conditions_match = context_valid;
        input.focused = input.profile_conditions_match;
    }
private:
    SourceRunBinding binding_;
};
}
#endif
