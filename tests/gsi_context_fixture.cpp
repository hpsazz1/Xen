#include "weapon/weapon.h"
#include "weapon/context_sharing_internal.h"
#include <mutex>
#include <nlohmann/json.hpp>
#include <iostream>
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    weapon::GsiConfig config;
    config.enabled = true;
    config.port = static_cast<std::uint16_t>(std::stoi(argv[1]));
    config.ttl_ms = 1500;
    weapon::GsiReceiver receiver;
    weapon::detail::ContextPublisher publisher;
    weapon::detail::LineupLocateEvent locate;
    std::mutex mutex;
    const auto start = [&] {
        if (!receiver.start(config, false)) return false;
        return publisher.start(config, [&] { return receiver.snapshot(); },
            [&] { std::lock_guard lock(mutex); return locate; });
    };
    if (!start()) return 3;
    std::cout << "ready" << std::endl;
    std::string command;
    while (std::getline(std::cin, command)) {
        if (command == "stop") { publisher.stop(); receiver.stop(); }
        else if (command == "start") { if (!start()) return 4; }
        else if (command == "locate" || command == "locate-stale") {
            std::lock_guard lock(mutex);
            ++locate.sequence;
            locate.at = weapon::Clock::now() - (command == "locate-stale" ? std::chrono::seconds(1) : std::chrono::seconds(0));
        }
        else if (command == "quit") break;
        const auto s = receiver.snapshot();
        std::cout << nlohmann::json{{"valid", s.valid}, {"context_valid", s.context_valid},
            {"map", s.map_name}, {"team", weapon::team_name(s.local_team)},
            {"weapon", s.canonical_id}, {"locate_sequence", locate.sequence}, {"error", receiver.last_error()}}.dump() << std::endl;
    }
    publisher.stop();
    receiver.stop();
}
