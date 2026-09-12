#ifndef RECOIL_ARCHIVE_H
#define RECOIL_ARCHIVE_H
#include "recoil/recoil_execution_events.h"
#include <filesystem>

struct RecoilArchiveConfig {
    std::string acquisition_run_id;
    std::filesystem::path directory;
    RecoilConfig recoil;
    std::size_t max_records = 32768, max_batch_bytes = 16 * 1024 * 1024;
    std::uint64_t max_total_bytes = 256 * 1024 * 1024, max_files = 4096;
};
struct RecoilArchiveStatus {
    bool running = false, available = false;
    std::string acquisition_run_id, directory, error;
    std::uint64_t last_sequence = 0, files_written = 0, complete_batches = 0, incomplete_batches = 0, total_bytes = 0;
};
class RecoilBatchArchive {
public:
    using Reader = std::function<RecoilEventSlice(std::uint64_t, std::size_t)>;
    // 测试可注入慢/失败sink；生产省略时以临时文件原子发布，不覆盖既有文件。
    using Sink = std::function<bool(const std::filesystem::path&, const std::string&, std::string&)>;
    RecoilBatchArchive();
    ~RecoilBatchArchive();
    bool start(const RecoilArchiveConfig& config, Reader reader, Sink sink = {}) noexcept;
    // 调用方必须先停止producer；此处读尽最后high-water再收尾。
    void stop() noexcept;
    RecoilArchiveStatus snapshot() const;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
#endif
