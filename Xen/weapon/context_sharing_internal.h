#ifndef WEAPON_CONTEXT_SHARING_INTERNAL_H
#define WEAPON_CONTEXT_SHARING_INTERNAL_H
#include "weapon/weapon.h"
#include <functional>
namespace weapon::detail {
struct LineupLocateEvent {
    std::uint64_t sequence = 0;
    Clock::time_point at{};
};
class ContextPublisher final {
public:
    ContextPublisher();
    ~ContextPublisher();
    bool start(const GsiConfig&, std::function<WeaponSnapshot()> sample,
               std::function<LineupLocateEvent()> locate = {}) noexcept;
    void stop() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
#endif
