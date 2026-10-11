#ifndef XEN_TEST_CLOSE_ERROR_CODECVT_H
#define XEN_TEST_CLOSE_ERROR_CODECVT_H
#include <algorithm>
#include <codecvt>
#include <cstring>
#include <locale>

namespace xen::test {
struct CloseState { int fail_at = 1; int calls = 0; int write_failures = 0; };

// 不替换ofstream：真实filebuf::close调用unshift，转换终止失败才置failbit。
// 只在单线程测试进程中临时改变C++ locale；退出作用域恢复。
class CloseErrorCodecvt final : public std::codecvt<char, char, std::mbstate_t> {
public:
    explicit CloseErrorCodecvt(CloseState& state) : state_(state) {}
protected:
    result do_out(state_type&, const char* begin, const char* end,
                  const char*& next, char* to, char* limit, char*& to_next) const override {
        if (state_.fail_at == -1) {
            next = begin; to_next = to; ++state_.write_failures;
            return error;
        }
        const auto size = std::min(end - begin, limit - to);
        std::memcpy(to, begin, static_cast<std::size_t>(size));
        next = begin + size; to_next = to + size;
        return next == end ? ok : partial;
    }
    result do_in(state_type&, const char* begin, const char* end,
                 const char*& next, char* to, char* limit, char*& to_next) const override {
        const auto size = std::min(end - begin, limit - to);
        std::memcpy(to, begin, static_cast<std::size_t>(size));
        next = begin + size; to_next = to + size;
        return next == end ? ok : partial;
    }
    result do_unshift(state_type&, char* to, char*, char*& next) const override {
        next = to;
        return ++state_.calls == state_.fail_at ? error : ok;
    }
    bool do_always_noconv() const noexcept override { return false; }
    int do_encoding() const noexcept override { return 1; }
    int do_max_length() const noexcept override { return 1; }
    int do_length(state_type&, const char* from, const char* end, std::size_t max) const override {
        return static_cast<int>(std::min(static_cast<std::size_t>(end - from), max));
    }
private:
    CloseState& state_;
};
struct ScopedCloseError {
    explicit ScopedCloseError(CloseState& state)
        : previous_(std::locale::global(std::locale(std::locale(), new CloseErrorCodecvt(state)))) {}
    ~ScopedCloseError() { std::locale::global(previous_); }
    std::locale previous_;
};
} // namespace xen::test
#endif
