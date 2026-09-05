#include "mouse/mouse.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

// 本进程只使用公开 lease 接口；不创建 Mouse adapter，也不打开或发送设备输入。
int main(int argc, char* argv[]) {
    if (argc < 2 || argc > 3) {
        std::cerr << "必须指定 production 或 current-process-test\n";
        return 2;
    }
    const bool wait_after_refusal = argc == 3;
    if (wait_after_refusal && std::string(argv[2]) != "--wait-after-refusal") {
        std::cerr << "未知 lease-only 测试选项\n";
        return 2;
    }
    const std::string scope_name = argv[1];
    MouseOutputOwnerScope scope;
    if (scope_name == "production") {
        scope = MouseOutputOwnerScope::PRODUCTION;
    } else if (scope_name == "current-process-test") {
        scope = MouseOutputOwnerScope::CURRENT_PROCESS_TEST;
    } else {
        std::cerr << "未知 lease scope\n";
        return 2;
    }

    MouseOutputOwnerLease owner;
    std::string error;
    if (!owner.acquire(scope, "output-owner-public-contract-test", error)) {
        if (owner.held() || error.empty()) {
            std::cerr << "获取失败后 lease 状态或错误信息不正确\n";
            return 1;
        }
        std::cerr << error << '\n';
        std::cout << "REFUSED" << std::endl;
        if (wait_after_refusal) {
            // 保留失败对象及进程，避免把退出时的 OS 清理误认为 acquire 当场回滚。
            std::string command;
            if (!std::getline(std::cin, command) || command != "finish-refused" ||
                owner.held()) {
                std::cerr << "拒绝后的存活同步或 lease 状态不正确\n";
                return 1;
            }
            std::cout << "REFUSAL_FINISHED" << std::endl;
        }
        return 3;
    }
    if (!owner.held()) {
        std::cerr << "获取成功后必须持有 lease\n";
        return 1;
    }
    // 父进程收到此消息后才能启动竞争方，不能把启动时间当作持锁证据。
    std::cout << "HELD" << std::endl;
    std::string command;
    while (std::getline(std::cin, command)) {
        if (command == "exit-without-release") {
            // 绕过析构，检验进程终止后由操作系统关闭所有 lease 文件句柄。
            std::_Exit(0);
        } else if (command == "check-same-process-conflict") {
            MouseOutputOwnerLease contender;
            if (contender.acquire(scope, "same-process-contender", error) ||
                contender.held() || error.empty()) {
                std::cerr << "同进程第二个 lease 不应获取成功\n";
                return 1;
            }
            std::cout << "SAME_PROCESS_CONFLICT" << std::endl;
        } else if (command == "release" || command == "release-other-thread") {
            if (command == "release-other-thread") {
                // 文件句柄的 lease 生命周期不绑定获取它的线程。
                std::thread releaser([&owner] { owner.release(); });
                releaser.join();
            } else {
                owner.release();
            }
            if (owner.held()) {
                std::cerr << "release 后仍持有 lease\n";
                return 1;
            }
            std::cout << "RELEASED" << std::endl;
            return 0;
        } else {
            std::cerr << "未知父进程同步命令\n";
            return 2;
        }
    }
    std::cerr << "父进程未发送释放命令就关闭同步通道\n";
    return 1;
}
