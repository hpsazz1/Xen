#include "recoil_tuner/recoil_tuner.h"
#include "recoil/recoil.h"
#include "recoil/recoil_store.h"
#include "weapon/weapon_timing.h"
#include "weapon/weapon_catalog.h"

#include <nlohmann/json.hpp>
#include <opencv2/imgcodecs.hpp>
#include <charconv>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <set>

namespace {
std::string read_text(const std::filesystem::path& path) {
    if (std::filesystem::file_size(path) > 16 * 1024 * 1024) throw std::runtime_error("文件过大");
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("无法读取文件");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
std::uint64_t number(const char* text) {
    const std::string value(text);
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || result == 0) throw std::runtime_error("序号必须为正整数");
    return result;
}
}
int main(int argc, char** argv) {
    try {
        // 明确的人工确认迁移入口：只写全新目录，不改原件、不启用设备。
        if (argc == 5 && std::string(argv[1]) == "compact-confirmed") {
            const auto destination = std::filesystem::absolute(argv[4]);
            if (std::filesystem::exists(destination)) throw std::runtime_error("迁移目标必须是新目录");
            std::string error;
            std::vector<RecoilStoredProfile> originals;
            RecoilStore source(argv[2]);
            if (!source.list(originals,error) || originals.empty()) throw std::runtime_error(error);
            auto timing=weapon::default_timing_catalog();
            if (std::string(argv[3])!="-" && !weapon::load_timing_catalog(argv[3],timing,error))
                throw std::runtime_error(error);
            std::set<std::string> weapons;
            for (const auto& item : originals) {
                if (!weapons.insert(item.profile->weapon_id).second)
                    throw std::runtime_error("同武器有多个版本，请先明确迁移对象");
                auto checked = *item.profile;
                if (!confirm_recoil_profile(checked,error)) throw std::runtime_error(error);
            }
            std::filesystem::create_directories(destination);
            RecoilStore output(destination/"profiles");
            nlohmann::json entries = nlohmann::json::array();
            for (const auto& item : originals) {
                auto confirmed = *item.profile;
                const auto gsi_name=weapon::gsi_name(confirmed.weapon_id);
                if(gsi_name.empty())throw std::runtime_error("迁移武器没有明确GSI名称");
                confirmed.id=gsi_name;
                if (!confirm_recoil_profile(confirmed,error)) throw std::runtime_error(error);
                std::string file;
                if (!output.save_new(confirmed,file,error,true) || !output.set_active(confirmed.weapon_id,file,error))
                    throw std::runtime_error(error);
                RecoilConfig config;
                config.sensitivity = *confirmed.calibration.sensitivity;
                const auto resolved = output.resolve(config,confirmed.weapon_id,error);
                if (!resolved || resolved->points.size()!=item.profile->points.size() ||
                    resolved->calibration.sensitivity!=item.profile->calibration.sensitivity)
                    throw std::runtime_error("迁移后活动弹道或灵敏度不一致");
                for (std::size_t i=0;i<resolved->points.size();++i) {
                    const auto& a=resolved->points[i]; const auto& b=item.profile->points[i];
                    if(a.time_ms!=b.time_ms||a.x_counts!=b.x_counts||a.y_counts!=b.y_counts)
                        throw std::runtime_error("迁移改变了弹道点");
                }
                entries.push_back({{"original_file",item.file},{"file",file},{"points",resolved->points.size()},
                    {"sensitivity",config.sensitivity},{"verified",true},{"active_resolved",true}});
            }
            if (!weapon::save_timing_catalog(destination/"weapon-timing.json",timing,error))
                throw std::runtime_error(error);
            weapon::TimingCatalog reread;
            if (!weapon::load_timing_catalog(destination/"weapon-timing.json",reread,error)) throw std::runtime_error(error);
            for (const auto& row:timing.profiles) {
                const auto* other=weapon::find_timing(reread,row.canonical_id);
                if(!other||other->shot_hold_ms!=row.shot_hold_ms||other->fire_interval_ms!=row.fire_interval_ms||other->enabled!=row.enabled)
                    throw std::runtime_error("迁移改变了武器时序");
            }
            const nlohmann::json report={{"kind","user_confirmed_profile_migration"},{"physical_test_performed",false},
                {"entries",entries},{"timing_values_unchanged",true}};
            std::ofstream manifest(destination/"migration-report.json",std::ios::binary);
            manifest << report.dump(2); manifest.close();
            if(!manifest) throw std::runtime_error("迁移报告保存失败");
            std::cout << report.dump(2) << '\n'; return 0;
        }
        if (argc == 7 && std::string(argv[1]) == "optimize") {
            recoil_tuner::Dataset dataset;
            std::string error;
            if (!recoil_tuner::load_dataset(argv[2], dataset, error)) { std::cerr << error << '\n'; return 1; }
            RecoilProfile base;
            if (!load_recoil_profile(read_text(argv[3]), base, error)) { std::cerr << error << '\n'; return 1; }
            if (dataset.base_profile_revision != std::to_string(base.revision) || dataset.base_curve.size() != base.points.size()) {
                std::cerr << "数据集与生产基线版本或曲线不匹配。\n"; return 1;
            }
            for (std::size_t i = 0; i < base.points.size(); ++i) {
                if (dataset.base_curve[i].time_ms != base.points[i].time_ms || dataset.base_curve[i].x_counts != base.points[i].x_counts ||
                    dataset.base_curve[i].y_counts != base.points[i].y_counts) { std::cerr << "数据集曲线不等于执行基线。\n"; return 1; }
            }
            recoil_tuner::Request request;
            request.generation = number(argv[5]); request.candidate_revision = argv[6];
            const auto revision = number(argv[6]);
            if (revision <= base.revision) { std::cerr << "候选版本须大于基线版本。\n"; return 1; }
            const std::filesystem::path result_directory(argv[4]);
            // 曲线目录只能包含生产profile JSON；分析报告放子目录，避免
            // RecoilStore把通用candidate/report当成可执行曲线读取。
            if (!std::filesystem::create_directory(result_directory)) {
                std::cerr << "结果目录已存在；拒绝覆盖或重复消费留出。\n"; return 1;
            }
            RecoilCandidateReplayReport replay;
            bool replay_attempted = false;
            auto report = recoil_tuner::optimize_profile_recorded(dataset, request, base, [&](const std::vector<recoil_tuner::CurvePoint>& points, std::string& validation_error) {
                auto candidate = base;
                candidate.revision = revision;
                candidate.state = RecoilProfileState::SCHEMA_VALID;
                candidate.phase_tolerance_ms.reset(); candidate.recovery_ms.reset(); candidate.execution_phase_budget_ms.reset();
                candidate.calibration.evidence.clear();
                candidate.points.clear();
                for (const auto& p : points) candidate.points.push_back({p.time_ms, p.x_counts, p.y_counts});
                replay_attempted = true;
                return validate_recoil_candidate_execution(candidate, {}, replay, validation_error);
            });
            if (replay_attempted) {
                const auto checked = [&](bool value) {
                    return value ? "通过" : replay.validated && !replay.acknowledged_commands ? "不适用（正常回放无非零意图）" : "未完成";
                };
                std::ostringstream summary;
                summary << "软件回放" << (replay.validated ? "通过" : "未通过")
                    << "：固定步长=" << replay.step_ms << " ms，另以相位预算=" << replay.phase_budget_ms
                    << " ms推进边界回放；曲线时长=" << replay.duration_ms << " ms，advance=" << replay.advance_calls
                    << '/' << replay.advance_limit << "，现有单命令每轴上限=" << replay.command_axis_limit_counts
                    << " counts，回放最大单轴命令=" << replay.max_abs_command_axis_counts << " counts；正常模拟ACK="
                    << replay.acknowledged_commands << "条、L1=" << replay.acknowledged_l1_counts << " counts，尾部="
                    << checked(replay.tail_checked) << "，相位边界=" << checked(replay.phase_edge_checked)
                    << "，时限=" << checked(replay.deadline_checked) << "，取消=" << checked(replay.cancellation_checked)
                    << "，UNKNOWN=" << checked(replay.unknown_receipt_checked) << "，NOT_SENT=" << checked(replay.not_sent_checked)
                    << "。这些是模拟条件与模拟回执，未验证实测相位、Worker累计/滚动预算或物理效果；候选仍未校准。";
                report.messages.push_back(summary.str());
            }
            if (!recoil_tuner::save_result(result_directory / "analysis", report, error)) { std::cerr << error << '\n'; return 1; }
            if (report.status == recoil_tuner::Status::CANDIDATE_VALIDATED && report.candidate) {
                auto candidate = base;
                candidate.revision = revision;
                candidate.state = RecoilProfileState::SCHEMA_VALID;
                candidate.phase_tolerance_ms.reset(); candidate.recovery_ms.reset(); candidate.execution_phase_budget_ms.reset();
                candidate.calibration.evidence.clear(); candidate.points.clear();
                for (const auto& point : report.candidate->points)
                    candidate.points.push_back({point.time_ms, point.x_counts, point.y_counts});
                std::string file;
                if (!RecoilStore(result_directory).save_new(candidate, file, error)) {
                    std::cerr << "生产候选文件保存失败：" << error << '\n'; return 1;
                }
                std::cout << "编辑器可加载的未校准候选：" << (result_directory / file).string() << '\n';
            }
            for (const auto& message : report.messages) std::cout << message << '\n';
            std::cout << "报告已写入独立目录的analysis子目录，活动曲线未改变。默认优化预算：每轴5 counts、总量5 counts；未进行实机。\n";
            return report.status == recoil_tuner::Status::CANDIDATE_VALIDATED ? 0 : 2;
        }
        if (argc == 6 && std::string(argv[1]) == "measure") {
            const auto j = nlohmann::json::parse(read_text(argv[4]));
            recoil_tuner::ImageRequest request;
            const auto reference = j.at("reference").get<std::array<double, 2>>();
            request.reference = {reference[0], reference[1]}; request.reference_confirmed = j.at("reference_confirmed");
            const auto registration = j.at("registration_roi").get<std::array<int, 4>>();
            const auto measurement = j.at("measurement_roi").get<std::array<int, 4>>();
            request.registration_roi = {registration[0], registration[1], registration[2], registration[3]};
            request.measurement_roi = {measurement[0], measurement[1], measurement[2], measurement[3]};
            request.max_translation_pixels = j.value("max_translation_pixels", request.max_translation_pixels);
            request.min_registration_response = j.value("min_registration_response", request.min_registration_response);
            request.threshold = j.value("threshold", request.threshold); request.min_area = j.value("min_area", request.min_area);
            request.max_area = j.value("max_area", request.max_area); request.max_changed_fraction = j.value("max_changed_fraction", request.max_changed_fraction);
            const auto result = recoil_tuner::measure_image_pair(cv::imread(argv[2], cv::IMREAD_UNCHANGED), cv::imread(argv[3], cv::IMREAD_UNCHANGED), request);
            std::string error;
            if (!recoil_tuner::save_image_measurement(argv[5], result, error)) { std::cerr << error << '\n'; return 1; }
            std::cout << result.message << '\n';
            return result.valid ? 0 : 2;
        }
        std::cout << "离线弹道优化器（不连接设备、不改变活动曲线）\n"
            "  compact-confirmed <旧profiles目录> <weapon-timing.json或-使用内置值> <全新迁移目录>\n"
            "  optimize <数据集.json> <基线profile.json> <新结果目录> <优化代际> <候选版本>\n"
            "  measure <之前图像> <之后图像> <人工锚点与区域.json> <新测量结果.json>\n"
            "测量仅输出待人工确认的暗斑候选；优化结果需用户前台试验和人工验收。\n";
        return 1;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
      catch (...) { std::cerr << "输入参数、文件或数据格式无效；未执行设备操作。\n"; return 1; }
}
