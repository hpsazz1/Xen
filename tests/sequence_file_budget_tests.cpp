#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "mouse_effect_probe/mouse_effect_probe.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {
int failures = 0;
constexpr std::uintmax_t capacity = 128U * 1024U * 1024U;
void expect(bool condition, const std::string& message) {
    std::cout << (condition ? "PASS " : "FAIL ") << message << std::endl;
    if (!condition) ++failures;
}
}

int main() {
    using namespace mouse_effect_probe;
    const auto root = std::filesystem::temp_directory_path() /
        ("xen-sequence-budget-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    if (!std::filesystem::create_directory(root)) return 2;
    std::cout << "Retained fixture: " << root << std::endl;
    MouseEffectProbeSequence sequence, loaded;
    std::string error;
    expect(make_dependency_calibration_sequence({64, 7810, 1, 64}, sequence, error) && sequence.samples.size() == 1000000,
           "public generator accepts one million samples across 64 blocks: " + error);
    const auto large = root / "million.json";
    const bool written = write_mouse_effect_probe_sequence(large, sequence, error);
    expect(written, "public writer publishes valid maximum sequence: " + error);
    if (written) {
        const auto bytes = std::filesystem::file_size(large);
        std::cout << "million_file_bytes=" << bytes << std::endl;
        expect(bytes <= capacity, "published sequence fits reader byte budget");
        const bool read = read_mouse_effect_probe_sequence(large, loaded, error);
        expect(read && loaded.sequence_sha256 == sequence.sequence_sha256 && loaded.samples.size() == sequence.samples.size() && loaded.net_x_counts == sequence.net_x_counts,
               "actual reader accepts maximum sequence with unchanged semantic identity: " + error);
        expect(!write_mouse_effect_probe_sequence(large, sequence, error) && std::filesystem::file_size(large) == bytes,
               "existing maximum sequence cannot be overwritten");
    }
    sequence = {}; loaded = {};
    expect(!make_dependency_calibration_sequence({65, 7810, 1, 64}, sequence, error), "one million plus one samples remains rejected");
    expect(make_sparse_pulse_sequence({2, 3, 2}, sequence, error), "healthy small sequence generation");
    const auto small = root / "small.json";
    expect(write_mouse_effect_probe_sequence(small, sequence, error), "small sequence publication");
    {
        std::ifstream input(small, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(input)), {});
        expect(text.find("\n  \"schema\"") != std::string::npos, "small files retain readable indentation");
    }
    const auto boundary = root / "exact-capacity.json";
    std::filesystem::copy_file(small, boundary);
    {
        auto remaining = capacity - std::filesystem::file_size(boundary);
        const std::string spaces(65536, ' ');
        std::ofstream output(boundary, std::ios::binary | std::ios::app);
        while (remaining) {
            const auto amount = std::min<std::uintmax_t>(remaining, spaces.size());
            output.write(spaces.data(), static_cast<std::streamsize>(amount));
            remaining -= amount;
        }
        output.close();
        expect(output.good(), "exact-capacity whitespace fixture closed successfully");
    }
    expect(std::filesystem::file_size(boundary) == capacity && read_mouse_effect_probe_sequence(boundary, loaded, error) && loaded.sequence_sha256 == sequence.sequence_sha256,
           "reader accepts valid JSON at exact 128 MiB boundary: " + error);
    const auto oversized = root / "over-capacity.json";
    std::filesystem::copy_file(small, oversized);
    std::filesystem::resize_file(oversized, capacity + 1);
    expect(!read_mouse_effect_probe_sequence(oversized, loaded, error) && loaded.samples.empty(), "reader rejects 128 MiB plus one and clears result");
    return failures ? 1 : 0;
}
