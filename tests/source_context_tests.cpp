#include "source_context/source_context_internal.h"
#include <iostream>

namespace {
int failures = 0;
void expect(bool condition, const char* message) {
    if (!condition) { ++failures; std::cerr << "[失败] " << message << '\n'; }
}
}

int main() {
    using namespace source_context;
    using namespace source_context::detail;
    using namespace std::chrono_literals;
    const std::string token(32, 'a'); // 固定测试向量，不是部署凭据。
    SourceContextConfig config;
    expect(!valid_config(config), "空配置默认关闭");
    config.enabled = true;
    config.host = "127.0.0.1";
    config.port = 5012;
    config.process_name = "game.exe";
    config.token = token;
    expect(valid_config(config), "显式启用配置有效");
    auto invalid = config;
    invalid.token.clear();
    expect(!valid_config(invalid), "缺少鉴权值不可启用");
    invalid = config;
    invalid.ttl_ms = 0;
    expect(!valid_config(invalid), "零TTL非法");
    expect(exact_process_match("game.exe", "GAME.EXE"), "Windows 文件名大小写匹配");
    expect(!exact_process_match("game.exe", "othergame.exe"), "不能子串匹配");
    expect(!exact_process_match("game.exe", "C:/game.exe"), "不能混淆全路径与文件名");

    Message request;
    request.nonce[0] = 1;
    request.process_name = "game.exe";
    Packet packet{};
    expect(encode(request, token, packet), "鉴权请求编码");
    Message parsed;
    expect(decode(packet, token, parsed) && !parsed.response && parsed.nonce == request.nonce,
           "鉴权请求往返");
    expect(!decode(packet, std::string(32, 'b'), parsed), "错误凭据不能接受");
    auto corrupt = packet;
    corrupt[8] ^= 1;
    expect(!decode(corrupt, token, parsed), "nonce篡改必须拒绝");
    corrupt = packet;
    corrupt.back() ^= 1;
    expect(!decode(corrupt, token, parsed), "MAC篡改必须拒绝");
    expect(!decode(std::span(packet).first(packet.size() - 1), token, parsed), "截断必须拒绝");
    corrupt = packet;
    corrupt[4] = 2;
    expect(!decode(corrupt, token, parsed), "未知协议版本拒绝");

    Message response = request;
    response.response = true;
    response.focus = Focus::Foreground;
    response.session = 10;
    response.sequence = 1;
    expect(encode(response, token, packet) && decode(packet, token, parsed), "前台事实编码");
    corrupt = packet;
    corrupt[6] = static_cast<std::uint8_t>(Focus::Background);
    expect(!decode(corrupt, token, parsed), "焦点位篡改必须拒绝");

    const auto start = Clock::time_point{} + 1s;
    Evidence evidence;
    expect(!evidence.snapshot(start).available, "默认没有许可");
    evidence.begin(request.nonce, start);
    expect(!evidence.accept(response, "other.exe", start + 1ms, 100), "不同源进程拒绝");
    auto wrong_nonce = response;
    wrong_nonce.nonce[1] = 2;
    expect(!evidence.accept(wrong_nonce, "game.exe", start + 1ms, 100), "非当前请求拒绝");
    expect(evidence.accept(response, "game.exe", start + 20ms, 100), "新鲜鉴权前台响应");
    expect(!evidence.snapshot(start - 1ms).available, "单调时间回退不许可");
    expect(evidence.snapshot(start + 20ms).focused && evidence.snapshot(start + 20ms).age_ms == 20,
           "年龄从请求发出计入往返延迟");
    expect(!evidence.accept(response, "game.exe", start + 21ms, 100), "同响应不可重复消费");
    expect(!evidence.snapshot(start + 100ms).available && !evidence.snapshot(start + 100ms).focused,
           "精确到期边界无许可");

    request.nonce[0] = 2;
    evidence.begin(request.nonce, start + 100ms);
    response.nonce = request.nonce;
    expect(!evidence.accept(response, "game.exe", start + 101ms, 100), "相同会话旧seq拒绝");
    response.sequence = 2;
    response.focus = Focus::Background;
    expect(evidence.accept(response, "game.exe", start + 101ms, 100), "接受新的失焦事实");
    expect(evidence.snapshot(start + 102ms).available && !evidence.snapshot(start + 102ms).focused,
           "失焦不等于不可用，仍不许可");

    request.nonce[0] = 3;
    evidence.begin(request.nonce, start + 200ms);
    response.nonce = request.nonce;
    response.sequence = 3;
    expect(!evidence.accept(response, "game.exe", start + 300ms, 100), "在途过期响应拒绝");
    evidence.begin(request.nonce, start + 400ms);
    response.session = 11;
    response.sequence = 1;
    response.focus = Focus::Unknown;
    expect(evidence.accept(response, "game.exe", start + 401ms, 100), "新会话未知前台事实");
    expect(!evidence.snapshot(start + 401ms).available, "查询失败未知不能假装前台");
    evidence.invalidate();
    expect(!evidence.snapshot(start + 402ms).available && !evidence.snapshot(start + 402ms).focused,
           "停止立即撤销");
    Evidence continuous;
    response.session = 20; response.sequence = 1; response.focus = Focus::Foreground;
    continuous.begin(response.nonce, start);
    expect(continuous.accept(response, "game.exe", start + 1ms, 100), "健康连续性前置接受");
    const auto initial_epoch = continuous.snapshot(start + 1ms).control_safety_epoch;
    response.sequence = 9; response.nonce[0] = 9;
    continuous.begin(response.nonce, start + 20ms);
    auto unauthenticated = response; unauthenticated.focus = Focus::Unknown; unauthenticated.nonce[1] ^= 1;
    expect(!continuous.accept(unauthenticated, "game.exe", start + 21ms, 100), "拒绝的报文不算已观察断点");
    expect(continuous.accept(response, "game.exe", start + 21ms, 100), "健康序号跳跃仍被接受");
    expect(continuous.snapshot(start + 21ms).control_safety_epoch == initial_epoch,
        "健康报文跳号及拒绝的坏nonce均不能制造来源信任断点");
    const auto expired_epoch = continuous.snapshot(start + 120ms).control_safety_epoch;
    expect(expired_epoch > initial_epoch && continuous.snapshot(start + 121ms).control_safety_epoch == expired_epoch,
        "同一次到期只记录一个持久断点，重复轮询不增长代际");
    continuous.invalidate(); continuous.invalidate();
    expect(continuous.snapshot(start + 122ms).control_safety_epoch == expired_epoch,
        "无健康证据时重复停止不制造新断点且不清除已有代际");
    // 只运行纯报文/证据核，不查询真实前台、不绑定端口、不控制设备。
    return failures == 0 ? 0 : 1;
}
