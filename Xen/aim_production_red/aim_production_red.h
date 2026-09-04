#ifndef AIM_PRODUCTION_RED_H
#define AIM_PRODUCTION_RED_H

#include <cstddef>
#include <filesystem>
#include <string>

namespace aim_production_red {

struct ProduceOptions {
    std::filesystem::path plan_path;
    std::filesystem::path config_path;
    std::filesystem::path measured_reference_source_path;
    std::filesystem::path producer_binary_path;
    std::filesystem::path output_directory;
};

struct ProduceResult {
    std::filesystem::path manifest_path;
    std::size_t trace_count = 0;
    std::size_t sample_count = 0;
};

// 运行离线 Aim 与确定性模拟 backend/plant，并原子发布 evaluator 可消费的
// hash-bound bundle。该模块不链接 Mouse，也不具备任何物理输出能力。
bool produce_output_off_bundle(
    const ProduceOptions& options,
    ProduceResult& result,
    std::string& error) noexcept;

} // namespace aim_production_red

#endif // AIM_PRODUCTION_RED_H
