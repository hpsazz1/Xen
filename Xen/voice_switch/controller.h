#ifndef VOICE_SWITCH_CONTROLLER_H
#define VOICE_SWITCH_CONTROLLER_H
#include <memory>
namespace debug_session { class Session; struct Context; }
struct KeyboardPollResult;
struct KeyboardConfig;
namespace voice_switch {
class Server;
class Controller final {
public:
    Controller(Server&, debug_session::Session&);
    ~Controller();
    Controller(const Controller&) = delete;
    Controller& operator=(const Controller&) = delete;
    void tick(const debug_session::Context&, const KeyboardPollResult&, const KeyboardConfig&,
              bool blocked, bool capture) noexcept;
    void cancel() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
#endif
