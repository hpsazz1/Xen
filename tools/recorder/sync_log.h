// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef XEN_RECORDER_SYNC_LOG_H
#define XEN_RECORDER_SYNC_LOG_H
#include "recorder.h"
#include <nlohmann/json.hpp>
#include <memory>

namespace xen::recorder {
using Json = nlohmann::json;
std::int64_t steady_ns();
std::int64_t system_ns();
Json marker_record(const EncodedMarker& marker);
// 四戳映射只覆盖网络交换误差，不能外推为游戏响应或首发命中时间。
Json clock_mapping(std::int64_t sent, std::int64_t received,
                   std::int64_t auxiliary_received, std::int64_t auxiliary_replied);

class SyncLog final {
public:
    SyncLog(const std::filesystem::path& video, std::string session, std::string auxiliary_url);
    ~SyncLog();
    void submit(EncodedMarker marker);
    void finish(const Snapshot& snapshot);
    std::string status() const;
    bool local_complete() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
#endif
