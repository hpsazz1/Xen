// SPDX-License-Identifier: GPL-2.0-or-later
#include "sync_log.h"
#include <windows.h>
#include <winhttp.h>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

namespace xen::recorder {
std::int64_t steady_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::int64_t system_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
Json marker_record(const EncodedMarker& m) {
    Json result{{"type","encoded_marker"},{"marker_id",m.id},{"matched",m.matched},
        {"render_begin_steady_ns",std::to_string(m.render_begin_steady_ns)},
        {"render_end_steady_ns",std::to_string(m.render_end_steady_ns)},
        {"render_begin_system_unix_ns",std::to_string(m.render_begin_system_unix_ns)},
        {"render_end_system_unix_ns",std::to_string(m.render_end_system_unix_ns)},
        {"render_frame_time_ns",std::to_string(m.render_frame_time_ns)},
        {"video_pts",m.video_pts},{"timebase_num",m.timebase_num},{"timebase_den",m.timebase_den},
        {"error",m.error},{"video_seconds",nullptr}};
    if (m.matched && m.timebase_num > 0 && m.timebase_den > 0)
        result["video_seconds"] = static_cast<double>(m.video_pts) * m.timebase_num / m.timebase_den;
    return result;
}
Json clock_mapping(std::int64_t sent, std::int64_t received,
                   std::int64_t auxiliary_received, std::int64_t auxiliary_replied) {
    if (sent <= 0 || received < sent || auxiliary_received <= 0 || auxiliary_replied < auxiliary_received)
        throw std::runtime_error("时戳无效或倒退");
    const auto elapsed = received - sent;
    const auto auxiliary_elapsed = auxiliary_replied - auxiliary_received;
    if (elapsed < auxiliary_elapsed || elapsed - auxiliary_elapsed > 2000000000LL)
        throw std::runtime_error("网络往返无效或超过2秒");
    const auto rtt = elapsed - auxiliary_elapsed;
    return {{"offset_aux_minus_source_ns",std::to_string((auxiliary_received - sent) - rtt / 2)},
        {"network_round_trip_ns",std::to_string(rtt)},
        {"network_uncertainty_ns",std::to_string(rtt / 2 + rtt % 2)},
        {"offset_rounding_ns",std::to_string(rtt % 2)},
        {"uncertainty_scope","network_exchange_only; video_sampling_and_game_response_excluded"}};
}
namespace {
std::wstring widen(const std::string& text) {
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    if (!length && !text.empty()) throw std::runtime_error("URL不是有效UTF-8");
    std::wstring result(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}
struct InternetHandle {
    HINTERNET value = nullptr;
    ~InternetHandle() { if (value) WinHttpCloseHandle(value); }
};
struct Reply { Json body; DWORD status = 0; std::int64_t received = 0; };
Reply post(const std::string& base, const Json& document) {
    auto url = widen(base + "/sync-marker.json");
    URL_COMPONENTS parts{}; parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) ||
        (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS))
        throw std::runtime_error("辅机URL无效");
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    const std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    InternetHandle session{WinHttpOpen(L"XenRecorder/1",WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0)};
    if (!session.value) throw std::runtime_error("HTTP会话创建失败");
    WinHttpSetTimeouts(session.value,500,500,500,500);
    InternetHandle connection{WinHttpConnect(session.value,host.c_str(),parts.nPort,0)};
    if (!connection.value) throw std::runtime_error("辅机连接失败");
    InternetHandle request{WinHttpOpenRequest(connection.value,L"POST",path.c_str(),nullptr,
        WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,
        parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)};
    if (!request.value) throw std::runtime_error("同步请求创建失败");
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(request.value,WINHTTP_OPTION_REDIRECT_POLICY,&redirect,sizeof(redirect));
    const auto body = document.dump();
    if (!WinHttpSendRequest(request.value,L"Content-Type: application/json\r\n",static_cast<DWORD>(-1),
        const_cast<char*>(body.data()),static_cast<DWORD>(body.size()),static_cast<DWORD>(body.size()),0) ||
        !WinHttpReceiveResponse(request.value,nullptr)) throw std::runtime_error("辅机同步请求失败");
    Reply reply;
    DWORD size = sizeof(reply.status);
    if (!WinHttpQueryHeaders(request.value,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,&reply.status,&size,WINHTTP_NO_HEADER_INDEX))
        throw std::runtime_error("同步响应状态读取失败");
    std::string response;
    char buffer[2048]; DWORD read = 0;
    do {
        if (!WinHttpReadData(request.value,buffer,sizeof(buffer),&read))
            throw std::runtime_error("同步响应读取失败");
        response.append(buffer,read);
        if (response.size() > 8192) throw std::runtime_error("同步响应过大");
    } while (read);
    reply.received = steady_ns();
    reply.body = Json::parse(response);
    return reply;
}
void accepted(const Reply& reply, const std::string& id) {
    if (reply.status != 200 || reply.body.value("state",std::string{}) != "queued" ||
        reply.body.value("marker_id",std::string{}) != id)
        throw std::runtime_error(reply.body.value("reason",std::string{"辅机未接受同步标记"}));
}
}
struct SyncLog::Impl {
    std::string session, url, message = "本地同步索引已打开";
    std::filesystem::path index_path;
    std::ofstream stream;
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<EncodedMarker> queue;
    std::thread thread;
    bool closing = false, finished = false;
    bool local_failure = false;
    std::uint64_t dropped = 0, synced = 0, failed = 0;
    void write(const Json& record) {
        stream << record.dump() << '\n'; stream.flush();
        if (!stream) throw std::runtime_error("同步索引写入失败");
    }
    void process(const EncodedMarker& marker) {
        // 本地编码证据先落盘；网络不可用也不能抹掉它。
        write(marker_record(marker));
        if (!marker.matched || url.empty()) return;
        try {
            Json paint{{"schema",1},{"stage","paint"},{"marker_id",marker.id},{"source_session",session},
                {"duration_ms",2000},
                {"source_paint_begin_steady_ns",std::to_string(marker.render_begin_steady_ns)},
                {"source_paint_end_steady_ns",std::to_string(marker.render_end_steady_ns)},
                {"source_paint_system_unix_ns",std::to_string(marker.render_begin_system_unix_ns)},
                {"source_send_steady_ns",std::to_string(steady_ns())},
                {"source_send_system_unix_ns",std::to_string(system_ns())}};
            const auto reply = post(url,paint);
            accepted(reply,marker.id);
            auto receipt = paint; receipt["stage"] = "receipt";
            receipt["source_reply_received_steady_ns"] = std::to_string(reply.received);
            receipt["aux_received_steady_ns"] = reply.body.at("aux_received_steady_ns");
            receipt["aux_replied_steady_ns"] = reply.body.at("aux_replied_steady_ns");
            const auto sent = std::stoll(paint.at("source_send_steady_ns").get<std::string>());
            const auto auxiliary_received = std::stoll(reply.body.at("aux_received_steady_ns").get<std::string>());
            const auto auxiliary_replied = std::stoll(reply.body.at("aux_replied_steady_ns").get<std::string>());
            auto mapping = clock_mapping(sent,reply.received,auxiliary_received,auxiliary_replied);
            mapping["mapped_paint_begin_aux_steady_ns"] = std::to_string(
                marker.render_begin_steady_ns + std::stoll(mapping.at("offset_aux_minus_source_ns").get<std::string>()));
            const auto receipt_reply = post(url,receipt);
            accepted(receipt_reply,marker.id);
            write({{"type","auxiliary_sync"},{"marker_id",marker.id},{"state","queued"},
                {"archive_status","pending_app_delivery"},{"paint",paint},{"receipt",receipt},{"clock_mapping",mapping}});
            std::lock_guard lock(mutex); ++synced; message = "辅机已接收同步（归档状态需回收核对）";
        } catch (const std::exception& error) {
            write({{"type","auxiliary_sync"},{"marker_id",marker.id},{"state","failed"},{"reason",error.what()}});
            std::lock_guard lock(mutex); ++failed; message = std::string("本地标记已保存；辅机同步失败：") + error.what();
        }
    }
    void run() {
        for (;;) {
            EncodedMarker marker;
            {
                std::unique_lock lock(mutex);
                condition.wait(lock,[&]{return closing || !queue.empty();});
                if (queue.empty()) return;
                marker = std::move(queue.front()); queue.pop_front();
            }
            try { process(marker); }
            catch (const std::exception& error) {
                std::lock_guard lock(mutex); local_failure = true; message = error.what();
            }
        }
    }
};
SyncLog::SyncLog(const std::filesystem::path& video, std::string session, std::string auxiliary_url)
    : impl_(std::make_unique<Impl>()) {
    auto& p = *impl_; p.session = std::move(session); p.url = std::move(auxiliary_url);
    while (!p.url.empty() && p.url.back() == '/') p.url.pop_back();
    p.index_path = video; p.index_path.replace_extension(L".sync.jsonl");
    if (std::filesystem::exists(p.index_path)) throw std::runtime_error("同步索引已存在，拒绝覆盖");
    p.stream.open(p.index_path,std::ios::out|std::ios::binary);
    if (!p.stream) throw std::runtime_error("无法创建同步索引");
    p.write({{"schema",1},{"type","session"},{"source_session",p.session},
        {"video_filename",video.filename().string()},{"auxiliary_enabled",!p.url.empty()},
        {"pts_scope","encoder_packet_pts; validate_against_decoded_container"}});
    p.thread = std::thread([&p]{p.run();});
}
SyncLog::~SyncLog() {
    if (!impl_) return;
    { std::lock_guard lock(impl_->mutex); impl_->closing = true; }
    impl_->condition.notify_one();
    if (impl_->thread.joinable()) impl_->thread.join();
}
void SyncLog::submit(EncodedMarker marker) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->closing || impl_->queue.size() >= 64) { ++impl_->dropped; return; }
    impl_->queue.push_back(std::move(marker)); impl_->condition.notify_one();
}
std::string SyncLog::status() const { std::lock_guard lock(impl_->mutex); return impl_->message; }
bool SyncLog::local_complete() const { std::lock_guard lock(impl_->mutex); return !impl_->local_failure && impl_->dropped==0; }
void SyncLog::finish(const Snapshot& s) {
    auto& p = *impl_;
    { std::lock_guard lock(p.mutex); if (p.finished) return; p.closing = true; }
    p.condition.notify_one(); if (p.thread.joinable()) p.thread.join();
    p.write({{"type","closed"},{"stop_completed",s.stop_completed},{"encoder",s.encoder_id},
        {"width",s.width},{"height",s.height},{"fps",s.fps},{"total_frames",s.total_frames},
        {"encoded_packets",s.encoded_packets},{"bytes_written",s.bytes_written},
        {"markers_requested",s.markers_requested},{"markers_matched",s.markers_matched},
        {"markers_dropped",s.markers_dropped},{"markers_pending",s.markers_pending},
        {"render_queue_gaps",s.render_queue_gaps},{"packet_queue_gaps",s.packet_queue_gaps},
        {"output_dropped_frames",s.output_dropped_frames},
        {"render_total_frames",s.render_total_frames},{"render_lagged_frames",s.render_lagged_frames},
        {"encoder_total_frames",s.encoder_total_frames},{"encoder_skipped_frames",s.encoder_skipped_frames},
        {"active_render_fps",s.active_render_fps},{"average_render_ns",s.average_render_ns},
        {"sync_queue_dropped",p.dropped},{"local_write_failure",p.local_failure},
        {"auxiliary_queued",p.synced},{"auxiliary_failed",p.failed},
        {"software_fallback",s.software_fallback},{"last_error",s.last_error}});
    p.finished = true; p.stream.close();
}
}
