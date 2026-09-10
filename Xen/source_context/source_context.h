#ifndef SOURCE_CONTEXT_H
#define SOURCE_CONTEXT_H

#include <cstdint>
#include <memory>
#include <string>

namespace source_context {

struct SourceContextConfig {
    bool enabled = false;
    // 空配置默认关闭；host 为 IPv4 地址，服务端可绑定 0.0.0.0。
    std::string host;
    std::uint16_t port = 0;
    std::string token;
    std::string process_name;
    int ttl_ms = 200;
};

struct SourceContextSnapshot {
    bool available = false;
    bool focused = false;
    std::uint64_t session_id = 0;
    std::uint64_t sequence = 0;
    int age_ms = -1;
};

class SourceContextClient final {
public:
    SourceContextClient();
    ~SourceContextClient();
    SourceContextClient(const SourceContextClient&) = delete;
    SourceContextClient& operator=(const SourceContextClient&) = delete;
    // start/stop 由单一生命周期 owner 串行调用；snapshot 可跨线程读取。
    bool start(const SourceContextConfig& config) noexcept;
    void stop() noexcept;
    SourceContextSnapshot snapshot() const noexcept;
    std::string last_error() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class SourceContextServer final {
public:
    SourceContextServer();
    ~SourceContextServer();
    bool start(const SourceContextConfig& config) noexcept;
    // 调用线程推进服务；超时属正常，返回 false 表示服务错误。
    bool serve_once(int timeout_ms = 20) noexcept;
    void stop() noexcept;
    std::string last_error() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace source_context
#endif
