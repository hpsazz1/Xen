#include "model_workspace/model_workspace.h"
#include "platform/filesystem_path_internal.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>

namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
using Action = model_workspace::Action;

std::string utf8(const fs::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::string bytes(const fs::path& path) {
    std::ifstream stream(xen::filesystem_detail::windows_io_path(path), std::ios::binary);
    if (!stream) throw std::runtime_error("cannot read fixture");
    return {std::istreambuf_iterator<char>(stream), {}};
}

void write(const fs::path& path, const std::string& value) {
    std::ofstream stream(xen::filesystem_detail::windows_io_path(path), std::ios::binary | std::ios::trunc);
    stream.write(value.data(), static_cast<std::streamsize>(value.size()));
    stream.close();
    if (!stream) throw std::runtime_error("cannot write fixture");
}

model_workspace::Snapshot wait(model_workspace::Workspace& workspace) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
    while (std::chrono::steady_clock::now() < deadline) {
        auto view = workspace.poll();
        if (!view.job_running) return view;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    throw std::runtime_error("owned CPU worker timed out");
}
} // namespace

// Explicit-only integration host. It never constructs Runtime, Capture or a device.
int wmain(int argc, wchar_t* argv[]) try {
    if (argc != 6) return 2;
    const fs::path python(argv[1]), script(argv[2]), dataset(argv[3]), model(argv[4]), root(argv[5]);
    if (fs::exists(root)) throw std::runtime_error("fixture workspace must be new");
    fs::create_directories(root);
    Json checks = Json::array();
    int failures = 0;
    const auto expect = [&](bool passed, const char* name) {
        checks.push_back({{"case", name}, {"passed", passed}});
        if (!passed) { ++failures; std::cerr << "FAIL: " << name << '\n'; }
    };
    model_workspace::Workspace workspace(nullptr);
    model_workspace::Settings settings;
    std::string error;
    if (!workspace.initialize(root, settings, error)) throw std::runtime_error(error);
    settings.python_executable = utf8(python);
    settings.script_path = utf8(script);
    settings.dataset_path = utf8(dataset);
    settings.model_path = utf8(model);
    settings.class_names = "person";
    settings.class_schema_confirmed = true;
    settings.image_size = 64;
    settings.batch_size = 1;
    settings.loader_workers = 0;
    settings.device = "cpu";
    const auto execute = [&](Action action) {
        return workspace.execute(action, settings, false, false, "");
    };
    expect(!execute(Action::IMPORT_CANDIDATE), "no evaluation cannot import");
    if (!execute(Action::EVALUATE)) throw std::runtime_error(workspace.poll().message);
    expect(!execute(Action::EVALUATE), "running evaluation rejects duplicate action");
    expect(execute(Action::CANCEL_JOB), "request actual worker cancellation");
    const auto cancelled = wait(workspace);
    expect(cancelled.job_state == "CANCELLED" && !execute(Action::IMPORT_CANDIDATE) &&
           fs::is_regular_file(fs::u8path(cancelled.job_directory) / "cancel.flag"),
           "cancelled worker cannot import");
    if (!execute(Action::EVALUATE)) throw std::runtime_error(workspace.poll().message);
    const auto evaluated = wait(workspace);
    expect(evaluated.job_state == "SUCCEEDED", "new actual CPU evaluation succeeds after cancel");
    if (evaluated.job_state != "SUCCEEDED") throw std::runtime_error(evaluated.job_message);
    const auto report = Json::parse(bytes(fs::u8path(evaluated.job_directory) / "result/evaluation.json"));
    const auto counts = report.at("fixed_threshold_per_class").at(0);
    expect(counts.at("tp") == 0 && counts.at("fp") == 0 && counts.at("fn") == 1 &&
           report.at("negative_frames") == 1 && report.at("passed_compatibility") == true &&
           report.at("physical_acceptance") == "NOT_EXECUTED",
           "real CPU evaluator preserves known truth and no physical claim");
    expect(workspace.poll().candidate_path.empty() && !fs::exists(root / "models"),
           "evaluation success does not import automatically");

    const auto manifest = Json::parse(bytes(dataset / "dataset.json"));
    const auto sample = manifest.at("samples").at(0);
    const fs::path inputs[]{model, dataset / "dataset.json",
        dataset / fs::u8path(sample.at("image").get<std::string>()),
        dataset / fs::u8path(sample.at("label").get<std::string>())};
    const char* names[]{"changed model rejected", "changed manifest rejected",
                        "changed image rejected", "changed label rejected"};
    for (std::size_t i = 0; i < std::size(inputs); ++i) {
        const auto original = bytes(inputs[i]);
        write(inputs[i], original + "\n");
        const bool rejected = !execute(Action::IMPORT_CANDIDATE);
        write(inputs[i], original);
        expect(rejected && workspace.poll().candidate_path.empty() && !fs::exists(root / "models"), names[i]);
    }
    expect(execute(Action::IMPORT_CANDIDATE), "restored evaluated bytes can import");
    const auto imported = fs::u8path(workspace.poll().candidate_path);
    if (imported.empty()) throw std::runtime_error(workspace.poll().message);
    const auto sidecar_path = fs::path(imported.wstring() + L".json");
    const auto sidecar = Json::parse(bytes(sidecar_path));
    expect(bytes(imported) == bytes(model) && sidecar.at("model_sha256") == report.at("model_sha256") &&
           sidecar.at("dataset_sha256") == report.at("dataset_sha256") &&
           sidecar.at("metrics") == report.at("metrics") && settings.model_path == utf8(model),
           "imported bytes and evaluation sidecar agree without switching model");
    const auto imported_bytes = bytes(imported), sidecar_bytes = bytes(sidecar_path);
    expect(!execute(Action::IMPORT_CANDIDATE) && bytes(imported) == imported_bytes &&
           bytes(sidecar_path) == sidecar_bytes, "duplicate import preserves existing files");
    workspace.shutdown();
    write(root / "integration-result.json", Json{{"checks", checks}, {"failures", failures},
        {"cancelled_job", cancelled.job_directory}, {"evaluation_job", evaluated.job_directory},
        {"imported_model", utf8(imported)}, {"physical_output", false},
        {"runtime_started", false}, {"native_ui", false}}.dump(2));
    std::cout << checks.dump(2) << '\n';
    return failures ? 1 : 0;
} catch (const std::exception& exception) {
    std::cerr << exception.what() << '\n';
    return 1;
}
