#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <shellapi.h>
#include <bcrypt.h>
#ifdef ERROR
#undef ERROR
#endif

#include "model_workspace/model_workspace.h"
#include "log/log.h"

#include <nlohmann/json.hpp>
#include <array>
#include <chrono>
#include <fstream>
#include <set>
#include <sstream>
#include <string_view>
#include <stdexcept>
#include <vector>

namespace model_workspace {
namespace {
using Json = nlohmann::json;
namespace fs = std::filesystem;

std::string utf8(const fs::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
fs::path path_from(const std::string& value) { return fs::u8path(value); }

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value_; }
    bool valid() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
    void reset(HANDLE value = nullptr) {
        if (valid()) CloseHandle(value_);
        value_ = value;
    }
private:
    HANDLE value_ = nullptr;
};

void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

fs::path checked_path(const fs::path& path) {
    check(!path.empty(), "路径不能为空");
    const auto absolute = fs::absolute(path).lexically_normal();
    for (auto current = absolute; !current.empty();) {
        const DWORD attributes = GetFileAttributesW(current.c_str());
        check(attributes == INVALID_FILE_ATTRIBUTES ||
              !(attributes & FILE_ATTRIBUTE_REPARSE_POINT),
              "数据工作区不接受链接或重解析路径");
        const auto parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }
    return absolute;
}

Json read_json(const fs::path& path, std::uintmax_t max_bytes = 1024 * 1024) {
    checked_path(path);
    check(fs::is_regular_file(path) && fs::file_size(path) <= max_bytes,
          "JSON文件缺失或超过读取上限");
    std::ifstream stream(path, std::ios::binary);
    check(stream.good(), "无法读取状态文件");
    return Json::parse(stream);
}

void write_json(const fs::path& path, const Json& value) {
    checked_path(path);
    const auto pending = fs::path(path.wstring() + L".pending");
    checked_path(pending);
    {
        std::ofstream stream(pending, std::ios::binary | std::ios::trunc);
        check(stream.good(), "无法创建工作区记录");
        stream << value.dump(2) << '\n';
        stream.close();
        check(!stream.fail(), "工作区记录写入失败");
    }
    check(MoveFileExW(pending.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0,
          "工作区记录原子提交失败");
}

std::vector<std::string> names_from(const Settings& settings) {
    check(settings.class_schema_confirmed, "请先核对并确认类别顺序与原模型一致");
    std::istringstream input(settings.class_names);
    std::vector<std::string> names;
    std::set<std::string> unique;
    for (std::string name; std::getline(input, name, ',');) {
        const auto begin = name.find_first_not_of(" \t\r\n");
        const auto end = name.find_last_not_of(" \t\r\n");
        check(begin != std::string::npos, "类别名称不能为空");
        name = name.substr(begin, end - begin + 1);
        check(unique.insert(name).second, "类别名称不能重复");
        names.push_back(std::move(name));
    }
    check(!names.empty() && names.size() <= 256 &&
              !settings.class_names.ends_with(','), "类别清单无效");
    return names;
}

// 使用Windows参数规则逐项引用；不经过cmd或PowerShell解释路径。
std::wstring quote(const std::wstring& argument) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const wchar_t value : argument) {
        if (value == L'\\') { ++slashes; continue; }
        result.append(slashes * (value == L'"' ? 2 : 1), L'\\');
        slashes = 0;
        if (value == L'"') result += L'\\';
        result += value;
    }
    result.append(slashes * 2, L'\\');
    result += L'"';
    return result;
}

std::string sha256(const fs::path& path) {
    checked_path(path);
    std::ifstream stream(path, std::ios::binary);
    check(stream.good(), "无法读取模型文件");
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    check(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
                                     nullptr, 0) >= 0, "无法初始化SHA256");
    try {
        check(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0,
              "无法创建SHA256");
        std::array<unsigned char, 64 * 1024> buffer{};
        while (stream) {
            stream.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
            const auto size = stream.gcount();
            if (size > 0)
                check(BCryptHashData(hash, buffer.data(), static_cast<ULONG>(size), 0) >= 0,
                      "模型SHA256读取失败");
        }
        check(stream.eof(), "模型文件读取中断");
        std::array<unsigned char, 32> digest{};
        check(BCryptFinishHash(hash, digest.data(), digest.size(), 0) >= 0,
              "模型SHA256结束失败");
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        constexpr char hex[] = "0123456789abcdef";
        std::string result;
        for (const auto value : digest) {
            result += hex[value >> 4]; result += hex[value & 15];
        }
        return result;
    } catch (...) {
        if (hash) BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        throw;
    }
}

const char* operation_for(Action action) {
    switch (action) {
        case Action::INSPECT_DATA: return "inspect";
        case Action::PRELABEL: return "prelabel";
        case Action::EXPORT_REVIEW: return "review_export";
        case Action::IMPORT_LABELS: return "import_labels";
        case Action::EXPORT_DATASET: return "export";
        case Action::TRAIN: return "train";
        case Action::EVALUATE: return "evaluate";
        default: return nullptr;
    }
}

Json settings_json(const Settings& value) {
    return {{"schema_version", 1}, {"root_directory", value.root_directory},
        {"python_executable", value.python_executable}, {"script_path", value.script_path},
        {"base_python_executable", value.base_python_executable},
        {"environment_root", value.environment_root},
        {"class_names", value.class_names}, {"class_schema_confirmed", value.class_schema_confirmed},
        {"schema_model_path", value.schema_model_path}, {"schema_model_sha256", value.schema_model_sha256},
        {"resume_training", value.resume_training},
        {"weights_path", value.weights_path}, {"dataset_path", value.dataset_path},
        {"model_path", value.model_path}, {"review_manifest", value.review_manifest},
        {"prelabels_path", value.prelabels_path},
        {"device", value.device}, {"max_samples", value.max_samples},
        {"max_disk_mib", value.max_disk_mib}, {"interval_ms", value.interval_ms},
        {"exploration_interval_ms", value.exploration_interval_ms},
        {"epochs", value.epochs}, {"image_size", value.image_size},
        {"batch_size", value.batch_size}};
}

} // namespace

struct Workspace::Impl {
    explicit Impl(std::shared_ptr<data_collection::Collector> value)
        : collector(std::move(value)) {}
    std::shared_ptr<data_collection::Collector> collector;
    fs::path data_root;
    fs::path workspace_root;
    fs::path job_directory;
    Snapshot view;
    Handle process;
    Handle job;
    Json last_result;
    std::chrono::steady_clock::time_point last_poll{};
    std::uint64_t sequence = 0;
    bool cancellation_requested = false;
    bool result_applied = false;
    std::string checked_python;
    std::string checked_weights;
    std::string checked_environment_device;
    std::string checked_weights_device;
    int checked_image_size = 0;

    void cancel() {
        if (!process.valid()) return;
        write_json(job_directory / "cancel.flag", {{"cancel", true}});
        cancellation_requested = true;
        view.job_state = "CANCELLING";
        view.message = "已请求取消，等待当前训练步骤保存并退出；退出应用将终止剩余子进程。";
    }

    void launch(const Json& specification, const Settings& settings) {
        const auto python = checked_path(path_from(settings.python_executable));
        const auto script = checked_path(path_from(settings.script_path));
        check(fs::is_regular_file(python) && python.extension() == L".exe",
              "请填写已安装Python解释器的完整.exe路径");
        check(fs::is_regular_file(script) && script.extension() == L".py",
              "训练工具脚本不存在，请选择model_data_pipeline.py");
        const auto jobs = checked_path(workspace_root / "jobs");
        fs::create_directories(jobs);
        const auto ticks = std::chrono::system_clock::now().time_since_epoch().count();
        job_directory = jobs / (std::to_string(ticks) + "-" +
                         std::to_string(GetCurrentProcessId()) + "-" + std::to_string(++sequence));
        check(fs::create_directory(job_directory), "无法创建独立训练作业目录");
        Json spec = specification;
        spec["output"] = utf8(job_directory / "result");
        write_json(job_directory / "job.json", spec);

        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        Handle output(CreateFileW((job_directory / "process.log").c_str(),
            GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        check(output.valid() && input.valid(), "无法创建训练日志句柄");
        job.reset(CreateJobObjectW(nullptr, nullptr));
        check(job.valid(), "无法创建训练进程所有权");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        check(SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
              &limits, sizeof(limits)) != 0, "无法配置训练进程退出清理");

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = input.get();
        startup.StartupInfo.hStdOutput = output.get();
        startup.StartupInfo.hStdError = output.get();
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        std::vector<unsigned char> storage(bytes);
        startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        check(InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes) != 0,
              "无法初始化训练进程句柄清单");
        HANDLE handles[]{input.get(), output.get()};
        const bool attributes_ok = UpdateProcThreadAttribute(startup.lpAttributeList, 0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof(handles), nullptr, nullptr) != 0;
        if (!attributes_ok) {
            DeleteProcThreadAttributeList(startup.lpAttributeList);
            throw std::runtime_error("无法限定训练进程继承句柄");
        }
        std::wstring command = quote(python.wstring()) + L" -B -X utf8 " +
            quote(script.wstring()) + L" --job " + quote((job_directory / "job.json").wstring()) +
            L" --status " + quote((job_directory / "status.json").wstring()) +
            L" --cancel " + quote((job_directory / "cancel.flag").wstring());
        PROCESS_INFORMATION information{};
        const bool created = CreateProcessW(python.c_str(), command.data(), nullptr, nullptr,
            TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
            nullptr, job_directory.c_str(), &startup.StartupInfo, &information) != 0;
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        check(created, "无法启动Python训练进程");
        process.reset(information.hProcess);
        Handle thread(information.hThread);
        if (!AssignProcessToJobObject(job.get(), process.get()) ||
            ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
            TerminateProcess(process.get(), 1);
            process.reset(); job.reset();
            throw std::runtime_error("训练进程所有权建立失败，已终止候选进程");
        }
        cancellation_requested = false;
        last_result = Json();
        result_applied = false;
        view.job_running = true;
        view.job_operation = specification.at("operation").get<std::string>();
        view.job_state = "RUNNING";
        view.job_directory = utf8(job_directory);
        view.job_message = "后台作业已启动，详细输出保存在process.log。";
        view.message = "";
        last_poll = {};
    }

    void refresh_job() {
        if (!process.valid()) return;
        const bool finished = WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0;
        const auto now = std::chrono::steady_clock::now();
        if (!finished && now - last_poll < std::chrono::milliseconds(500)) return;
        last_poll = now;
        const auto status_path = job_directory / "status.json";
        if (fs::exists(status_path)) {
            try {
                const Json status = read_json(status_path);
                check(status.value("schema_version", 0) == 1, "训练状态版本不兼容");
                check(status.value("operation", "") == view.job_operation,
                      "训练状态与作业不匹配");
                view.job_state = status.value("state", "RUNNING");
                view.job_message = status.value("message", "");
                last_result = status.value("result", Json::object());
            } catch (...) {
                // 原子替换瞬间或中断时保留上一份有效状态；进程退出后不允许冒充成功。
                if (finished) view.job_state = "FAILED";
            }
        }
        if (!finished) {
            if (cancellation_requested) view.job_state = "CANCELLING";
            return;
        }
        DWORD code = 1;
        GetExitCodeProcess(process.get(), &code);
        process.reset(); job.reset();
        view.job_running = false;
        if (cancellation_requested) {
            view.job_state = "CANCELLED";
            view.job_message = "作业已取消；已完成记录保留，未自动导入模型。";
        } else if (code != 0 || view.job_state != "SUCCEEDED") {
            view.job_state = "FAILED";
            if (view.job_message.empty())
                view.job_message = "后台作业未成功完成，请查看process.log。";
        } else if (last_result.is_object()) {
            view.candidate_path = last_result.value("candidate", view.candidate_path);
        }
        if (view.job_state != "SUCCEEDED") {
            if (view.job_operation == "env_install" || view.job_operation == "env_check")
                view.environment_message = "环境未通过检查；请查看作业状态和日志。";
            if (view.job_operation == "pt_check")
                view.weights_message = "PT未通过检查；请查看作业状态和日志。";
        }
    }
};

Workspace::Workspace(std::shared_ptr<data_collection::Collector> collector)
    : impl_(std::make_unique<Impl>(std::move(collector))) {
    Log::register_module("model-data", LogLevel::INFO);
}
Workspace::~Workspace() { shutdown(); }

bool Workspace::initialize(const fs::path& data_root, Settings& settings,
                           std::string& error) noexcept {
    try {
        impl_->data_root = checked_path(data_root);
        impl_->workspace_root = checked_path(impl_->data_root / "cache" / "model-workspace");
        fs::create_directories(impl_->workspace_root);
        settings.root_directory = utf8(impl_->data_root / "cache" / "datasets");
        settings.environment_root = utf8(impl_->data_root / "cache" / "training-environments");
        std::array<wchar_t, 32768> executable{};
        const auto size = GetModuleFileNameW(nullptr, executable.data(), executable.size());
        check(size > 0 && size < executable.size(), "无法定位训练工具目录");
        settings.script_path = utf8(fs::path(executable.data()).parent_path() /
                                    "scripts" / "model_data_pipeline.py");
        std::array<wchar_t, 32768> python{};
        const auto found = SearchPathW(nullptr, L"python.exe", nullptr,
                                      python.size(), python.data(), nullptr);
        if (found > 0 && found < python.size()) settings.python_executable = utf8(python.data());
        settings.base_python_executable = settings.python_executable;
        const auto file = impl_->workspace_root / "settings.json";
        if (fs::exists(file)) {
            const auto saved = read_json(file);
            check(saved.value("schema_version", 0) == 1, "工作区设置版本不兼容");
#define XEN_DATA_SETTING(field) settings.field = saved.value(#field, settings.field)
            XEN_DATA_SETTING(root_directory); XEN_DATA_SETTING(python_executable);
            XEN_DATA_SETTING(base_python_executable); XEN_DATA_SETTING(environment_root);
            XEN_DATA_SETTING(script_path); XEN_DATA_SETTING(class_names);
            XEN_DATA_SETTING(schema_model_path); XEN_DATA_SETTING(schema_model_sha256);
            XEN_DATA_SETTING(class_schema_confirmed); XEN_DATA_SETTING(weights_path);
            XEN_DATA_SETTING(resume_training);
            XEN_DATA_SETTING(dataset_path); XEN_DATA_SETTING(model_path);
            XEN_DATA_SETTING(review_manifest); XEN_DATA_SETTING(device);
            XEN_DATA_SETTING(prelabels_path);
            XEN_DATA_SETTING(max_samples); XEN_DATA_SETTING(max_disk_mib);
            XEN_DATA_SETTING(interval_ms); XEN_DATA_SETTING(exploration_interval_ms);
            XEN_DATA_SETTING(epochs); XEN_DATA_SETTING(image_size); XEN_DATA_SETTING(batch_size);
#undef XEN_DATA_SETTING
        }
        error.clear();
        return true;
    } catch (const std::exception& exception) { error = exception.what(); }
      catch (...) { error = "模型工作区初始化失败"; }
    return false;
}

Snapshot Workspace::poll(Settings* settings) noexcept {
    try {
        impl_->refresh_job();
        if (settings && !impl_->result_applied && !impl_->view.job_running &&
            impl_->view.job_state == "SUCCEEDED" && impl_->last_result.is_object()) {
            const auto& result = impl_->last_result;
            if (impl_->view.job_operation == "env_install" || impl_->view.job_operation == "env_check") {
                check(result.value("ready", false), "环境作业未通过GPU自检");
                const auto python = checked_path(path_from(result.at("python_executable").get<std::string>()));
                check(fs::is_regular_file(python), "自检解释器已不存在");
                settings->python_executable = utf8(python);
                impl_->checked_python = settings->python_executable;
                impl_->checked_environment_device = result.value("device_requested", settings->device);
                impl_->view.environment_ready = true;
                impl_->view.environment_message = "GPU环境自检通过；结果与依赖记录见当前作业目录。";
                impl_->view.weights_ready = false;
                impl_->view.weights_message = "环境已检查，请重新检查PT兼容性。";
                write_json(impl_->workspace_root / "settings.json", settings_json(*settings));
            }
            if (impl_->view.job_operation == "pt_check") {
                check(result.value("ready", false) && result.value("synthetic_compatibility_only", false),
                      "PT作业未完成合成输入兼容检查");
                check(fs::equivalent(path_from(result.value("weights", settings->weights_path)),
                                     path_from(settings->weights_path)), "权重路径已变化，请重新检查");
                impl_->checked_weights = settings->weights_path;
                impl_->checked_python = settings->python_executable;
                impl_->checked_weights_device = result.value("device_requested", settings->device);
                impl_->checked_image_size = result.value("input_size", settings->image_size);
                impl_->view.weights_ready = true;
                impl_->view.weights_message = "PT加载与合成输入反向传播通过；仍需真实审核数据训练和评价。";
            }
            settings->dataset_path = result.value("dataset", settings->dataset_path);
            settings->review_manifest = result.value("review_manifest", settings->review_manifest);
            settings->prelabels_path = result.value("prelabels", settings->prelabels_path);
            settings->model_path = result.value("candidate", settings->model_path);
            if (impl_->view.job_operation == "inspect" && result.contains("model_sha256")) {
                const auto inspected_hash = result.at("model_sha256").get<std::string>();
                if (inspected_hash != settings->schema_model_sha256)
                    settings->class_schema_confirmed = false;
                settings->schema_model_path = result.at("model").get<std::string>();
                settings->schema_model_sha256 = inspected_hash;
            }
            if ((impl_->view.job_operation == "inspect" || impl_->view.job_operation == "pt_check") &&
                result.contains("model_class_names") && result["model_class_names"].is_array() &&
                !result["model_class_names"].empty()) {
                std::string names;
                for (const auto& name : result["model_class_names"]) {
                    if (!names.empty()) names += ',';
                    names += name.get<std::string>();
                }
                if (names != settings->class_names) {
                    settings->class_names = std::move(names);
                    settings->class_schema_confirmed = false;
                }
                impl_->view.message = "已读取模型类别，请到采集页核对名称与ID顺序并确认。";
            }
            impl_->result_applied = true;
        }
        if (settings) {
            if (settings->python_executable != impl_->checked_python) {
                impl_->view.environment_ready = false;
                impl_->view.environment_message.clear();
                impl_->view.weights_ready = false;
                impl_->view.weights_message.clear();
            }
            if (settings->weights_path != impl_->checked_weights) {
                impl_->view.weights_ready = false;
                impl_->view.weights_message.clear();
            }
            if (settings->device != impl_->checked_environment_device) {
                impl_->view.environment_ready = false;
                impl_->view.environment_message.clear();
            }
            if (settings->device != impl_->checked_weights_device ||
                settings->image_size != impl_->checked_image_size) {
                impl_->view.weights_ready = false;
                impl_->view.weights_message.clear();
            }
        }
        if (impl_->collector) impl_->view.collection = impl_->collector->snapshot();
        return impl_->view;
    } catch (...) {
        // 状态文件不可读不等于作业结束，保留进程所有权门禁。
        Snapshot fallback;
        fallback.job_running = impl_->process.valid();
        fallback.collection.active = impl_->view.collection.active;
        return fallback;
    }
}

bool Workspace::execute(Action action, const Settings& settings,
                        bool runtime_running, bool gpu_frames,
                        const std::string& active_model_path) noexcept {
    try {
        check(!impl_->workspace_root.empty(), "模型工作区未初始化");
        impl_->refresh_job();
        const bool collecting = impl_->collector && impl_->collector->snapshot().active;
        if (action == Action::NONE) return true;
        if (action == Action::CANCEL_JOB) { impl_->cancel(); return true; }
        if (action == Action::SAVE_SETTINGS) {
            write_json(impl_->workspace_root / "settings.json", settings_json(settings));
            impl_->view.message = "采集与训练设置已保存。"; return true;
        }
        if (action == Action::OPEN_DATA_DIRECTORY || action == Action::OPEN_JOB_DIRECTORY) {
            const auto root = checked_path(action == Action::OPEN_DATA_DIRECTORY
                ? path_from(settings.root_directory) : impl_->job_directory);
            fs::create_directories(root);
            check(reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", root.c_str(),
                nullptr, nullptr, SW_SHOWNORMAL)) > 32, "无法打开数据目录");
            return true;
        }
        if (action == Action::PAUSE_COLLECTION || action == Action::RESUME_COLLECTION ||
            action == Action::STOP_COLLECTION || action == Action::MARK_SAMPLE) {
            check(collecting, "当前没有采集会话");
            if (action == Action::STOP_COLLECTION) impl_->collector->stop();
            else if (action == Action::MARK_SAMPLE) impl_->collector->request_sample();
            else impl_->collector->set_paused(action == Action::PAUSE_COLLECTION);
            impl_->view.message = action == Action::MARK_SAMPLE
                ? "已标记下一张有效帧，保存结果以采集计数为准。" : "采集状态已更新。";
            return true;
        }
        check(!impl_->view.job_running, "请等待当前后台作业结束，或先取消作业");
        check(!collecting, "请先结束采集，再运行数据或训练作业");
        if (action == Action::START_COLLECTION) {
            check(runtime_running, "请先在概览启动Runtime，再开始图片采集");
            check(!gpu_frames, "当前GPU纹理路径不支持图片采集，请停止后选择CPU图像路径");
            check(impl_->collector != nullptr, "图片采集模块未接入");
            check(settings.max_samples > 0 && settings.max_disk_mib > 0 &&
                  settings.max_disk_mib <= 102400, "采集数量或磁盘预算无效");
            data_collection::Config config;
            config.root_directory = checked_path(path_from(settings.root_directory));
            config.class_names = names_from(settings);
            config.model_path = active_model_path;
            check(!settings.schema_model_path.empty() && !settings.schema_model_sha256.empty(),
                  "请先在训练页检查当前ONNX，再核对类别并开始采集");
            check(fs::equivalent(checked_path(path_from(settings.schema_model_path)),
                                 checked_path(path_from(active_model_path))) &&
                  sha256(path_from(active_model_path)) == settings.schema_model_sha256,
                  "当前模型与类别确认身份不同，请重新检查ONNX并确认类别");
            config.max_samples = static_cast<std::uint64_t>(settings.max_samples);
            config.max_bytes = static_cast<std::uint64_t>(settings.max_disk_mib) * 1024 * 1024;
            config.interval_ms = settings.interval_ms;
            config.exploration_interval_ms = settings.exploration_interval_ms;
            std::string error;
            check(impl_->collector->start(config, error), error.empty() ? "无法开始采集" : error.c_str());
            impl_->view.message = "智能采集已开始；自动框仍需审核，不会直接进入训练。";
            return true;
        }
        check(!runtime_running, "请先停止Runtime，再执行离线标注、训练或候选管理");
        if (action == Action::ENV_INSTALL || action == Action::ENV_CHECK || action == Action::PT_CHECK) {
            const bool install = action == Action::ENV_INSTALL;
            const bool weights = action == Action::PT_CHECK;
            if (weights) {
                impl_->view.weights_ready = false;
                impl_->view.weights_message = "PT尚未通过本次检查；状态或失败原因见作业提示。";
            } else {
                impl_->view.environment_ready = false;
                impl_->view.weights_ready = false;
                impl_->view.environment_message = "环境尚未通过本次检查；状态或失败原因见作业提示。";
                impl_->view.weights_message.clear();
            }
            if (weights) check(settings.trusted_weights, "请确认PT来自你信任的训练来源，再执行模型检查");
            Settings bootstrap = settings;
            bootstrap.python_executable = settings.base_python_executable.empty()
                ? settings.python_executable : settings.base_python_executable;
            bootstrap.script_path = utf8(path_from(settings.script_path).parent_path() /
                                          "model_training_environment.py");
            Json job{{"operation", install ? "env_install" : weights ? "pt_check" : "env_check"},
                {"environment_root", utf8(checked_path(path_from(settings.environment_root)))},
                {"python_executable", settings.python_executable}, {"weights", settings.weights_path},
                {"trusted_weights", settings.trusted_weights}, {"device", settings.device},
                {"imgsz", settings.image_size}};
            if (weights) {
                const auto path = checked_path(path_from(settings.weights_path));
                check(_wcsicmp(path.extension().c_str(), L".pt") == 0, "模型兼容检查需要本地.pt文件");
                job["expected_sha256"] = sha256(path);
            }
            impl_->launch(job, bootstrap);
            return true;
        }
        if (action == Action::IMPORT_CANDIDATE) {
            check(impl_->view.job_operation == "evaluate" && impl_->view.job_state == "SUCCEEDED" &&
                  impl_->last_result.value("passed_compatibility", false) &&
                  impl_->last_result.contains("metrics"),
                  "请先对候选ONNX完成独立评价与兼容检查");
            const auto source = checked_path(path_from(impl_->last_result.at("model").get<std::string>()));
            check(source.extension() == L".onnx", "候选必须是ONNX模型");
            const auto digest = sha256(source);
            check(digest == impl_->last_result.at("model_sha256").get<std::string>(),
                  "候选模型在评价后发生变化，请重新评价");
            const auto dataset = checked_path(path_from(
                impl_->last_result.at("dataset").get<std::string>()));
            check(sha256(dataset / "dataset.json") ==
                  impl_->last_result.at("dataset_sha256").get<std::string>(),
                  "数据集在评价后发生变化，请重新评价");
            // 累积数据快照远大于低频状态文件；独立预算支持多轮采集。
            const auto manifest = read_json(dataset / "dataset.json", 64ULL * 1024 * 1024);
            check(manifest.at("class_names") == impl_->last_result.at("class_names"),
                  "评价类别与数据集不一致");
            for (const auto& sample : manifest.at("samples")) {
                for (const auto* kind : {"image", "label"}) {
                    const auto relative = path_from(sample.at(kind).get<std::string>());
                    check(!relative.is_absolute(), "数据快照含绝对样本路径");
                    const auto file = checked_path(dataset / relative);
                    const auto within = file.lexically_relative(dataset);
                    check(!within.empty() && *within.begin() != L"..",
                          "数据快照样本路径越界");
                    check(sha256(file) == sample.at(std::string(kind) + "_sha256").get<std::string>(),
                          "图片或标签在评价后发生变化，请重新评价");
                }
            }
            const auto directory = checked_path(impl_->data_root / "models");
            fs::create_directories(directory);
            const auto destination = directory / ("candidate-" + digest.substr(0, 16) + ".onnx");
            check(!fs::exists(destination), "该候选已存在于模型目录，未覆盖旧文件");
            const auto pending = directory / ("candidate-" + digest.substr(0, 16) + "-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".pending");
            check(fs::copy_file(source, pending, fs::copy_options::none), "候选导入失败");
            check(sha256(pending) == digest, "候选复制校验失败，未发布ONNX");
            write_json(fs::path(destination.wstring() + L".json"), impl_->last_result);
            // sidecar与临时模型均完整后才出现可枚举的ONNX；绝不覆盖旧候选。
            fs::rename(pending, destination);
            impl_->view.candidate_path = utf8(destination);
            impl_->view.message = "候选已导入；请在检测页刷新模型并明确选择。当前模型未自动切换。";
            return true;
        }
        const char* operation = operation_for(action);
        check(operation != nullptr, "未知工作区操作");
        const auto dataset_input = path_from(settings.dataset_path);
        const auto dataset = dataset_input.filename() == L"data.yaml"
            ? utf8(dataset_input.parent_path()) : settings.dataset_path;
        Json job{{"operation", operation}, {"root", utf8(checked_path(path_from(settings.root_directory)))},
            {"weights", settings.weights_path},
            {"dataset", dataset}, {"review_manifest", settings.review_manifest},
            {"model", settings.model_path.empty() ? active_model_path : settings.model_path},
            {"epochs", settings.epochs}, {"imgsz", settings.image_size},
            {"batch", settings.batch_size}, {"device", settings.device}};
        if (action == Action::TRAIN) job["resume"] = settings.resume_training;
        if (action != Action::INSPECT_DATA) job["class_names"] = names_from(settings);
        else if (settings.class_schema_confirmed) job["class_names"] = names_from(settings);
        else job.erase("root");
        if (action == Action::EXPORT_REVIEW && !settings.prelabels_path.empty())
            job["prelabels"] = settings.prelabels_path;
        if (action == Action::EVALUATE && !active_model_path.empty() &&
            settings.model_path != active_model_path) job["baseline_model"] = active_model_path;
        for (const auto* field : {"weights", "model", "baseline_model"}) {
            const bool used = (action == Action::TRAIN && std::string_view(field) == "weights") ||
                ((action == Action::PRELABEL || action == Action::EVALUATE) && std::string_view(field) != "weights");
            if (!used || !job.contains(field)) continue;
            const auto model = path_from(job.at(field).get<std::string>());
            if (_wcsicmp(model.extension().c_str(), L".pt") != 0) continue;
            check(settings.trusted_weights, "加载PT前请确认可训练权重的来源可信");
            const auto trusted = checked_path(path_from(settings.weights_path));
            check(fs::equivalent(checked_path(model), trusted),
                  "预标注或评价PT需与已确认来源的可训练权重为同一文件");
            job["trusted_weights"] = true;
            job["trusted_weights_path"] = utf8(trusted);
            job["expected_weights_sha256"] = sha256(trusted);
        }
        impl_->launch(job, settings);
        return true;
    } catch (const std::exception& exception) { impl_->view.message = exception.what(); }
      catch (...) { impl_->view.message = "模型工作区操作失败"; }
    return false;
}

void Workspace::shutdown() noexcept {
    if (!impl_) return;
    try {
        if (impl_->process.valid()) {
            impl_->cancel();
            if (WaitForSingleObject(impl_->process.get(), 1000) != WAIT_OBJECT_0) {
                TerminateJobObject(impl_->job.get(), 1);
                write_json(impl_->job_directory / "interrupted.json",
                    {{"reason", "应用退出，剩余作业被终止；仅已落盘检查点可恢复"}});
            }
        }
        impl_->process.reset(); impl_->job.reset();
        if (impl_->collector) impl_->collector->stop();
    } catch (...) { impl_->process.reset(); impl_->job.reset(); }
}

} // namespace model_workspace
