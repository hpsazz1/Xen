#pragma once

#include <atomic>
#include <cstdint>

// 异步设备命令的取消代次。生产者入队时捕获当前代次；目标失效或瞄准释放时
// 递增代次，使已出队但尚未进入设备调用的旧命令也能被可靠识别并丢弃。
class CommandCancellationEpoch
{
public:
    using Token = std::uint64_t;

    Token capture() const noexcept
    {
        return value_.load(std::memory_order_acquire);
    }

    Token cancel() noexcept
    {
        return value_.fetch_add(1, std::memory_order_acq_rel) + 1;
    }

    bool isCurrent(Token token) const noexcept
    {
        return token == capture();
    }

private:
    std::atomic<Token> value_{0};
};
