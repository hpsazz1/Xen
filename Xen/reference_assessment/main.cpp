#include "reference_assessment/reference_assessment.h"
#include "input_training/input_training.h"
#include "auto_stop/auto_stop.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace {
using Json = nlohmann::json;
namespace ref = xen::reference_assessment;
namespace fs = std::filesystem;
constexpr std::size_t kMaxEvents = 200000;
constexpr std::int64_t kMaxDurationNs = 600000000000LL;
constexpr const char* kCommit = "e632605f8b6c20ac5ab8ac3284e4fdc33735d431";
struct Row { input_training::Event event; bool crouch{}, shift{}, space{}; };
std::string path_text(const fs::path& path) {
    const auto value=path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(value.data()),value.size());
}

void write(const fs::path& path, const std::string& contents) {
    std::ofstream out(path, std::ios::binary);
    out << contents;
    if (!out) throw std::runtime_error("无法写入报告文件");
}
std::vector<Row> read_fixture(const fs::path& file, Json& document) {
    if (fs::file_size(file) > 32 * 1024 * 1024) throw std::runtime_error("样例超过32MiB");
    std::ifstream in(file);
    in >> document;
    if (document.at("schema") != 1 || !document.at("events").is_array())
        throw std::runtime_error("需要schema=1与events数组");
    std::vector<Row> rows;
    for (const auto& item : document.at("events")) {
        if (rows.size() == kMaxEvents) throw std::runtime_error("事件超过200000条");
        const double ms = item.at("at_ms").get<double>();
        const int mask = item.at("held_mask").get<int>();
        if (!std::isfinite(ms) || ms < 0 || ms > 600000 || mask < 0 || mask > 15)
            throw std::runtime_error("时间或WASD位图越界");
        Row row;
        row.event.received_at_ns = 1000000000 + static_cast<std::int64_t>(std::llround(ms * 1e6));
        row.event.epoch = item.value("epoch", std::uint64_t{1});
        row.event.sequence = item.value("sequence", std::uint64_t{rows.size() + 1});
        row.event.held_mask = static_cast<std::uint8_t>(mask);
        row.event.left_down = item.value("left_down", false);
        row.event.state_valid = item.value("state_valid", true);
        row.event.gap = item.value("gap", false);
        row.event.timing_uncertainty_ns = item.value("timing_uncertainty_ns", std::int64_t{-1});
        row.crouch = item.value("crouch", false);
        row.shift = item.value("shift", false);
        row.space = item.value("space", false);
        rows.push_back(row);
    }
    return rows;
}
// 只在全新报告目录生成评估输入副本；源归档保持只读。布局使用现有公开归档契约。
fs::path write_archive(const fs::path& root, const std::vector<Row>& rows) {
    const auto path = root / "input-copy";
    fs::create_directory(path);
    std::uint64_t chunks = 0, bytes = 0;
    for (std::size_t first = 0; first < rows.size(); first += 4096) {
        std::ostringstream csv;
        csv << "epoch,sequence,received_at_ns,dx,dy,held_mask,left_down,state_valid,motion_valid,gap,physical_motion_verified,source_loss_verifiable,timing_uncertainty_ns,raw_report_valid,datagram_size,raw_report\n";
        for (std::size_t index = first; index < std::min(first + 4096, rows.size()); ++index) {
            const auto& e = rows[index].event;
            csv << e.epoch << ',' << e.sequence << ',' << e.received_at_ns << ",0,0,"
                << unsigned(e.held_mask) << ',' << e.left_down << ',' << e.state_valid
                << ",0," << e.gap << ",0,0," << e.timing_uncertainty_ns << ",0,0,"
                << std::string(40, '0') << '\n';
        }
        const auto content = csv.str(); bytes += content.size();
        write(path / ("events-" + std::to_string(chunks++) + ".csv"), content);
    }
    std::ostringstream manifest;
    manifest << "XEN_INPUT_TRAINING_V1\n" << chunks << ' ' << rows.size() << " 0 " << bytes
        << ' ' << int(input_training::Status::STOPPED) << " 0 262144\n";
    write(path / "manifest.txt", manifest.str());
    return path;
}
Json existing_assessment(const fs::path& archive) {
    input_training::Session session;
    if (!session.load(archive)) throw std::runtime_error("Xen评估器无法启动回放");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    std::shared_ptr<const input_training::Snapshot> snapshot;
    do {
        snapshot = session.snapshot();
        if (snapshot && snapshot->status != input_training::Status::REPLAYING) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < deadline);
    if (!snapshot || snapshot->status != input_training::Status::STOPPED)
        throw std::runtime_error("Xen回放未完成：" + (snapshot ? snapshot->error : "无快照"));
    Json result{{"total_timings", snapshot->total_timings}, {"invalid_events", snapshot->invalid_events},
        {"retention_limit", 300}, {"timings", Json::array()}};
    for (const auto& t : snapshot->timings) {
        result["timings"].push_back({{"from", std::string(1,t.from)}, {"to", std::string(1,t.to)},
            {"delta_ms", double(t.delta_ns)/1e6}, {"grade", input_training::grade_name(t.grade)},
            {"completed_at_ns", t.completed_at_ns}, {"atomic_ambiguous", t.atomic_ambiguous},
            {"uncertainty_known", t.timing_uncertainty_known}, {"uncertainty_crosses_boundary", t.uncertainty_crosses_boundary}});
    }
    return result;
}
const char* key_name(ref::Key key) {
    switch (key) {
    case ref::Key::Forward: return "W"; case ref::Key::Back: return "S";
    case ref::Key::Left: return "A"; case ref::Key::Right: return "D";
    default: return "modifier";
    }
}
Json reference_assessment(const std::vector<Row>& rows) {
    ref::Engine engine;
    Json result{{"assessments", Json::array()}, {"shots", Json::array()},
        {"discontinuities", Json::array()}, {"atomic_packets", Json::array()}};
    const auto origin = rows.front().event.received_at_ns;
    auto collect = [&](const ref::Output& output, bool ambiguous) {
        if (!output.accepted) throw std::runtime_error("参考模型拒绝输入：" + output.error);
        for (const auto& a : output.assessments)
            result["assessments"].push_back({{"at_ms", a.time_ms-1000}, {"from", key_name(a.from)},
                {"to", key_name(a.to)}, {"delta_ms", a.diff_ms}, {"timing", a.timing},
                {"perfect", a.perfect}, {"success", a.success}, {"packet_order_assumed", ambiguous}});
        for (const auto& s : output.shots)
            result["shots"].push_back({{"at_ms", s.time_ms-1000}, {"model_error", s.error},
                {"estimated_speed", s.estimated_speed}, {"accuracy_threshold", s.accuracy_threshold},
                {"speed_ratio", s.speed_ratio}, {"reason", s.reason}, {"model_stable", s.stable},
                {"sequence_index", s.sequence_index}, {"held", s.held}, {"delayed", s.delayed},
                {"crouching", s.crouching}, {"axis_conflict", s.axis_conflict},
                {"counter_strafe", s.counter_strafe}, {"packet_order_assumed", ambiguous}});
        if (result["shots"].size() + result["assessments"].size() > 20000)
            throw std::runtime_error("参考记录超过20000条预算");
    };
    bool synced = false;
    bool order_assumed = false;
    Row previous;
    bool have_previous = false;
    for (const auto& row : rows) {
        const auto& e = row.event;
        const double time = 1000.0 + double(e.received_at_ns-origin)/1e6;
        const bool broken = e.gap || !e.state_valid || !e.epoch || !e.sequence ||
            (have_previous && (e.epoch != previous.event.epoch || e.sequence != previous.event.sequence+1));
        if (broken) {
            engine.reset(time); synced = false; order_assumed = false;
            result["discontinuities"].push_back({{"at_ms", time-1000}, {"sequence", e.sequence}});
        }
        if (!synced) {
            if (!broken && !e.held_mask && !e.left_down && !row.crouch && !row.shift && !row.space) {
                engine.reset(time); synced = true; order_assumed = false;
            }
            previous = row; have_previous = true;
            continue;
        }
        collect(engine.tick(time), order_assumed);
        const unsigned changed = e.held_mask ^ previous.event.held_mask;
        unsigned edges = (e.left_down != previous.event.left_down) + (row.crouch != previous.crouch);
        for (unsigned bit=1; bit<=8; bit<<=1) edges += (changed & bit) != 0;
        const bool ambiguous = edges > 1;
        order_assumed |= ambiguous;
        if (ambiguous) result["atomic_packets"].push_back({{"at_ms", time-1000}, {"sequence",e.sequence}});
        // 快照不能还原同包边沿顺序：固定先UP后DOWN，报告显式标记假设。
        const ref::Key keys[]{ref::Key::Forward,ref::Key::Left,ref::Key::Back,ref::Key::Right};
        for (bool down : {false,true}) for (unsigned index=0; index<4; ++index)
            if ((changed & (1U<<index)) && bool(e.held_mask & (1U<<index)) == down)
                collect(engine.handle_event({time,keys[index],down}), order_assumed);
        if (row.crouch != previous.crouch) collect(engine.handle_event({time,ref::Key::Crouch,row.crouch}), order_assumed);
        if (row.shift != previous.shift) collect(engine.handle_event({time,ref::Key::Shift,row.shift}), order_assumed);
        if (row.space != previous.space) collect(engine.handle_event({time,ref::Key::Space,row.space}), order_assumed);
        if (e.left_down != previous.event.left_down) collect(engine.handle_event({time,ref::Key::Fire,e.left_down}), order_assumed);
        previous = row; have_previous = true;
    }
    result["ends_with_held_fire"] = rows.back().event.left_down;
    result["synchronized_at_end"] = synced;
    return result;
}

Json h40_plans(const Json& document) {
    Json results = Json::array();
    if (!document.contains("h40_plans")) return results;
    if (!document["h40_plans"].is_array() || document["h40_plans"].size()>32)
        throw std::runtime_error("H40计划最多32项");
    for (const auto& plan : document["h40_plans"]) {
        const int mask = plan.at("held_mask").get<int>();
        const int delay = plan.at("ack_delay_ms").get<int>();
        const int cancel = plan.value("cancel_after_ms", -1);
        if (mask<=0 || mask>15 || (mask&5)==5 || (mask&10)==10 || delay<1 || delay>100 || cancel < -1 || cancel>500)
            throw std::runtime_error("H40计划位图/假ACK/取消时间无效");
        AutoStopConfig config;
        AutoStopController controller(config);
        WasdInputHistory history;
        controller.observe(history.observe(0,1,1,1000000),1000000);
        controller.observe(history.observe(static_cast<std::uint8_t>(mask),1,2,10000000),10000000);
        auto decision=controller.request(1,20000000);
        Json trace = Json::array();
        std::int64_t pending_at=0;
        std::uint64_t last_command=0;
        bool canceled=false;
        for (std::int64_t elapsed=0; elapsed<=500; ++elapsed) {
            const auto now=20000000+elapsed*1000000;
            if (cancel>=0 && elapsed>=cancel) { decision=controller.cancel(1,now); canceled=true; }
            else if (decision.phase==AutoStopPhase::WAITING_ACK && pending_at && now>=pending_at)
                decision=controller.acknowledge(1,decision.command_id,decision.desired_mask,now);
            else decision=controller.tick(now);
            if (decision.command_id!=last_command) {
                trace.push_back({{"after_request_ms",elapsed},{"desired_mask",decision.desired_mask},
                    {"command_id",decision.command_id},{"cancelled",canceled}});
                last_command=decision.command_id;
                pending_at=now+std::int64_t(delay)*1000000;
            }
            if (canceled || decision.phase==AutoStopPhase::COMPLETE_ESTIMATED) break;
        }
        results.push_back({{"label",plan.value("label",std::string{})},{"held_mask",mask},
            {"simulated_ack_delay_ms",delay},{"counter_hold_ms",40},{"after_release_ms",18},
            {"complete_estimated",decision.phase==AutoStopPhase::COMPLETE_ESTIMATED},
            {"ready_after_request_ms",decision.completion_ready_ns ? Json(double(decision.completion_ready_ns-20000000)/1e6):Json(nullptr)},
            {"trace",trace},{"physical_output",false},{"fire_permission",decision.fire_permitted}});
    }
    return results;
}
}
int run_main(int argc,char** argv) {
    try {
        if (argc==2 && std::string(argv[1])=="--help") {
            std::cout << "XenReferenceCompare --events <schema1.json> --output <new-directory>\n"
                         "XenReferenceCompare --archive <input-training-directory> --output <new-directory>\n"
                         "离线Basic模型比较；无设备输出、无GSI连接、无生产控制许可。\n";
            return 0;
        }
        if (argc!=5 || std::string(argv[3])!="--output" ||
            (std::string(argv[1])!="--events" && std::string(argv[1])!="--archive"))
            throw std::runtime_error("参数错误；使用--help");
        const fs::path input=fs::absolute(fs::u8path(argv[2])), output=fs::absolute(fs::u8path(argv[4]));
        if (fs::exists(output)) throw std::runtime_error("报告目录必须不存在，拒绝覆盖");
        Json document;
        std::vector<Row> rows;
        const bool archive=std::string(argv[1])=="--archive";
        if (archive) {
            input_training::ArchiveSummary summary;
            std::string error;
            if (!input_training::visit_archive(input,[&](const input_training::Event& e){
                if (rows.size()==kMaxEvents) throw std::runtime_error("事件超过200000条");
                rows.push_back({e});
            },summary,error)) throw std::runtime_error(error);
            if (summary.status!=input_training::Status::STOPPED || summary.dropped || summary.trailing_gap)
                throw std::runtime_error("归档存在截断、丢包或末尾缺口，不能作为完整比较输入");
        } else rows=read_fixture(input,document);
        if (rows.empty()) throw std::runtime_error("输入不能为空");
        const auto start=rows.front().event.received_at_ns;
        auto last=start;
        for (const auto& row:rows) {
            if (row.event.received_at_ns<last || row.event.received_at_ns<0 || row.event.received_at_ns-start>kMaxDurationNs)
                throw std::runtime_error("输入时间倒退或超过600秒预算");
            last=row.event.received_at_ns;
        }
        Json report{{"schema",1},{"reference_commit",kCommit},{"input",path_text(input)},
            {"source_kind",archive?"recorded_archive":"fixture"},{"event_count",rows.size()},
            {"source_origin_ns",start},{"physical_output",false},{"game_stop_verified",false},
            {"reference_mode","Basic"},{"modifiers_available",!archive},
            {"gsi_context_available",false},{"reference",reference_assessment(rows)},
            {"h40_simulated_plans",h40_plans(document)}};
        if (!fs::create_directories(output)) throw std::runtime_error("无法创建独立报告目录");
        // 两个评估器均只使用已校验的同一份内存输入副本，避免源文件并发变化。
        report["xen_input_training"]=existing_assessment(write_archive(output,rows));
        write(output/"report.json",report.dump(2)+"\n");
        std::ostringstream md;
        md << "# 独立参考离线比较\n\n输入：`" << path_text(input) << "`\n\n"
           << "参考提交：`" << kCommit << "`，Basic固定默认参数。\n\n"
           << "| 项目 | 数量 |\n|---|---:|\n| 输入事件 | " << rows.size()
           << " |\n| 参考换向评分 | " << report["reference"]["assessments"].size()
           << " |\n| Xen换向评分（全部/最多保留300） | " << report["xen_input_training"]["total_timings"]
           << " |\n| 参考射击采样 | " << report["reference"]["shots"].size()
           << " |\n| 输入断点 | " << report["reference"]["discontinuities"].size()
           << " |\n| 同包多边沿 | " << report["reference"]["atomic_packets"].size() << " |\n\n"
           << "参考换向评分与Xen输入评估器来自同一输入。Xen评分不是当前急停控制器。"
           << "H40计划单独调用当前纯控制器，以显式假ACK模拟40ms反向、18ms释放后等待；不是Worker/设备验收。\n\n"
           << "参考射击标签只表示输入模型估计；不表示实际开枪、真实速度或停稳。"
           << "Basic无GSI、武器、焦点、死亡、目标或功能键生命周期。原归档不包含蹲/走/跳时，这些上下文未知。\n\n"
           << "同包边沿采用先UP后DOWN顺序，packet_order_assumed标记本段顺序假设（含后续受影响样本，直到重同步）；不能据此接受参考的perfect标签。"
           << "断档后参考模型等待全释放重同步，Xen评估保留自身恢复规则，故有效窗口可能不同。末尾不虚构UP，也不外推后续射击。参考短tap按原模型在DOWN+18ms计划点结算，可能晚于UP。\n\n"
           << "详细数值见[report.json](report.json)。未启动游戏、设备或生产程序。\n\n";
        md << "## 参考换向记录（最多展示100条）\n\n| 时刻ms | 换向 | 间隔ms | perfect | 顺序假设 |\n|---:|---|---:|---|---|\n";
        std::size_t shown=0;
        for (const auto& row:report["reference"]["assessments"]) {
            if (shown++==100) break;
            md << "| " << row["at_ms"] << " | " << row["from"].get<std::string>() << "→" << row["to"].get<std::string>()
               << " | " << row["delta_ms"] << " | " << row["perfect"] << " | " << row["packet_order_assumed"] << " |\n";
        }
        md << "\n## Xen现有输入评估（最多展示100条）\n\n| 时刻ms | 换向 | 间隔ms | 分类 | 同包歧义 |\n|---:|---|---:|---|---|\n";
        shown=0;
        for (const auto& row:report["xen_input_training"]["timings"]) {
            if (shown++==100) break;
            md << "| " << double(row["completed_at_ns"].get<std::int64_t>()-start)/1e6 << " | "
               << row["from"].get<std::string>() << "→" << row["to"].get<std::string>() << " | " << row["delta_ms"]
               << " | " << row["grade"].get<std::string>() << " | " << row["atomic_ambiguous"] << " |\n";
        }
        md << "\n## 参考射击评分（最多展示100条，均为估计）\n\n| 时刻ms | 误差 | 模型稳定 | 原因 | 顺序假设 |\n|---:|---:|---|---|---|\n";
        shown=0;
        for (const auto& row:report["reference"]["shots"]) {
            if (shown++==100) break;
            md << "| " << row["at_ms"] << " | " << row["model_error"] << " | " << row["model_stable"] << " | "
               << row["reason"].get<std::string>() << " | " << row["packet_order_assumed"] << " |\n";
        }
        md << "\n## 当前H40计划模拟\n\n| 方向位图 | 假ACK延迟ms | 估计完成 | 请求至估计完成ms |\n|---:|---:|---|---:|\n";
        for (const auto& row:report["h40_simulated_plans"])
            md << "| " << row["held_mask"] << " | " << row["simulated_ack_delay_ms"] << " | " << row["complete_estimated"]
               << " | " << row["ready_after_request_ms"] << " |\n";
        write(output/"REPORT.md",md.str());
        std::cout << path_text(output/"REPORT.md") << '\n';
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

#ifdef _WIN32
int wmain(int argc,wchar_t** argv) {
    // Windows宽字符入口保留中文路径，内部命令参数统一UTF-8。
    std::vector<std::string> strings;
    for (int index=0; index<argc; ++index) {
        const auto text=fs::path(argv[index]).u8string();
        strings.emplace_back(reinterpret_cast<const char*>(text.data()),text.size());
    }
    std::vector<char*> pointers;
    for (auto& value:strings) pointers.push_back(value.data());
    return run_main(argc,pointers.data());
}
#else
int main(int argc,char** argv) { return run_main(argc,argv); }
#endif
