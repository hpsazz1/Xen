#ifndef MODEL_WORKSPACE_H
#define MODEL_WORKSPACE_H

#include "data_collection/data_collection.h"

#include <filesystem>
#include <memory>
#include <string>

namespace model_workspace {

enum class Action {
    NONE, START_COLLECTION, PAUSE_COLLECTION, RESUME_COLLECTION,
    STOP_COLLECTION, MARK_SAMPLE, INSPECT_DATA, PRELABEL,
    EXPORT_REVIEW, IMPORT_LABELS, EXPORT_DATASET, TRAIN,
    CANCEL_JOB, EVALUATE, IMPORT_CANDIDATE, OPEN_DATA_DIRECTORY,
    SAVE_SETTINGS, OPEN_JOB_DIRECTORY
};

struct Settings {
    std::string root_directory;
    std::string python_executable;
    std::string script_path;
    // 按 class_id 顺序使用英文逗号分隔；必须由用户核对原模型语义。
    std::string class_names;
    std::string schema_model_path;
    std::string schema_model_sha256;
    bool class_schema_confirmed = false;
    bool resume_training = false;
    std::string weights_path;
    std::string dataset_path;
    std::string model_path;
    std::string review_manifest;
    std::string prelabels_path;
    std::string device = "0";
    int max_samples = 300;
    int max_disk_mib = 1024;
    int interval_ms = 1000;
    int exploration_interval_ms = 10000;
    int epochs = 30;
    int image_size = 320;
    int batch_size = 8;
};

struct Snapshot {
    data_collection::Snapshot collection;
    bool job_running = false;
    std::string job_operation;
    std::string job_state;
    std::string job_directory;
    std::string job_message;
    std::string message;
    std::string candidate_path;
};

// App 持有此模块，Overlay 只渲染快照和产生动作，不访问磁盘或创建进程。
class Workspace {
public:
    explicit Workspace(std::shared_ptr<data_collection::Collector> collector);
    ~Workspace();
    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;

    bool initialize(const std::filesystem::path& data_root,
                    Settings& settings, std::string& error) noexcept;
    // 仅App主线程调用；运行中仅按低频读取小状态文件。
    Snapshot poll(Settings* settings = nullptr) noexcept;
    bool execute(Action action, const Settings& settings,
                 bool runtime_running, bool gpu_frames,
                 const std::string& active_model_path) noexcept;
    void shutdown() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace model_workspace
#endif // MODEL_WORKSPACE_H
