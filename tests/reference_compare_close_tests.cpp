#include "close_error_codecvt_test_support.h"
#include <algorithm>
#include <codecvt>
#include <cstring>
#include <locale>

// 运行同一离线CLI实现；不链接设备、Runtime或原生UI入口。
#define wmain reference_compare_wmain
#include "../Xen/reference_assessment/main.cpp"
#undef wmain

namespace {
using xen::test::CloseState;
using xen::test::ScopedCloseError;
int failures = 0;
void check(bool condition, const std::string& name) {
    if (!condition) { ++failures; std::cerr << "FAIL " << name << '\n'; }
}
int compare(const fs::path& input, const fs::path& output) {
    std::vector<std::string> args{"fixture", "--events", path_text(input), "--output", path_text(output)};
    std::vector<char*> pointers;
    for (auto& value : args) pointers.push_back(value.data());
    return run_main(static_cast<int>(pointers.size()), pointers.data());
}
} // namespace

int main() {
    const auto root = fs::temp_directory_path() / ("xen-reference-close-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if (!fs::create_directory(root)) return 2;
    const auto input = root / "events.json";
    write(input, R"({"schema":1,"events":[{"at_ms":0,"held_mask":0},{"at_ms":100,"held_mask":2},{"at_ms":500,"held_mask":0},{"at_ms":505,"held_mask":8},{"at_ms":510,"held_mask":0}]})");
    {
        CloseState state{1};
        ScopedCloseError scope(state);
        std::ofstream output(root / "control.txt", std::ios::binary);
        output << "fixture bytes";
        output.flush();
        check(output.good() && state.calls == 0, "write/flush succeed before selected close failure");
        output.close();
        check(output.fail() && !output.is_open() && state.calls == 1, "actual filebuf close reports failure");
    }
    // 单块输入的四个写出位置：chunk、manifest、report.json、REPORT.md。
    for (int fail_at : {0, 1, 2, 3, 4}) {
        CloseState state{fail_at};
        const auto output = root / ("case-" + std::to_string(fail_at));
        int result;
        {
            ScopedCloseError scope(state);
            result = compare(input, output);
        }
        check(fail_at == 0 ? result == 0 : result != 0,
              "CLI close ordinal " + std::to_string(fail_at));
        check(state.calls >= (fail_at ? fail_at : 4), "selected close must execute");
        if (fail_at == 1) check(!fs::exists(output / "input-copy" / "manifest.txt"), "failed chunk prevents manifest");
        if (fail_at == 2) check(!fs::exists(output / "report.json"), "failed manifest prevents comparison report");
        if (fail_at == 3) check(!fs::exists(output / "REPORT.md"), "failed JSON prevents final Markdown");
        std::cout << "close=" << fail_at << " reached=" << state.calls << " exit=" << result << '\n';
        const auto recovery = root / ("recovery-" + std::to_string(fail_at));
        check(compare(input, recovery) == 0, "new output directory recovers after failure");
        std::ifstream report_file(recovery / "report.json");
        Json report; report_file >> report;
        check(report.at("event_count") == 5 && report.at("physical_output") == false &&
                  report.at("game_stop_verified") == false &&
                  report.at("reference").at("assessments").size() == 1,
              "healthy recovery preserves real comparison and no-output boundary");
    }
    std::cout << "Fixture directory: " << path_text(root) << '\n';
    return failures ? 1 : 0;
}
