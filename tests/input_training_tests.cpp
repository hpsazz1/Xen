#include "input_training/input_training.h"

#include <chrono>
#include <condition_variable>
#include <cmath>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
using namespace input_training;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Fixture {
    std::filesystem::path root;
    unsigned next = 0;
    Fixture() : root(std::filesystem::temp_directory_path() / ("xen-input-training-test-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {}
    ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
    std::filesystem::path directory() { return root / ("recording-" + std::to_string(++next)); }
};
Event event(std::uint64_t sequence, std::int64_t time, std::uint8_t mask = 0, bool held = false, int dx = 0, int dy = 0) {
    Event value;
    value.epoch = 1; value.sequence = sequence; value.received_at_ns = time; value.held_mask = mask;
    value.left_down = held; value.dx = dx; value.dy = dy;
    value.state_valid = value.motion_valid = value.physical_motion_verified = true;
    return value;
}
std::shared_ptr<const Snapshot> record(const std::filesystem::path& directory, std::vector<Event> events, Limits limits = {}) {
    auto batch = std::make_shared<std::vector<Event>>(std::move(events));
    Session session;
    check(session.start(directory, limits, [batch] { ReadBatch value; value.events.swap(*batch); return value; }), "生产 Session 启动");
    session.stop();
    auto result = session.snapshot();
    check(result && (result->status == Status::STOPPED || result->status == Status::LIMIT), "生产 Session 正常收尾");
    return result;
}
std::shared_ptr<const Snapshot> replay(const std::filesystem::path& directory, Limits limits = {}) {
    Session session;
    check(session.load(directory, limits), "生产回放入口启动");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (session.snapshot()->status == Status::REPLAYING && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const auto result = session.snapshot(); session.stop();
    check(result->status != Status::REPLAYING, "回放需在有界时间结束");
    return result;
}
void timing_tests(Fixture& fixture) {
    for (const auto [delta, expected] : std::vector<std::pair<std::int64_t, Grade>>{
        {2000000, Grade::PERFECT}, {2000001, Grade::EXCELLENT}, {10000000, Grade::EXCELLENT},
        {10000001, Grade::LATE}, {120000000, Grade::LATE}, {120000001, Grade::UNCLASSIFIED},
        {-2000000, Grade::PERFECT}, {-10000000, Grade::EXCELLENT}, {-10000001, Grade::EARLY},
        {-120000001, Grade::UNCLASSIFIED}}) {
        std::vector<Event> events{event(1, 0), event(2, 1000, kA)};
        if (delta > 0) {
            events.push_back(event(3, 2000)); events.push_back(event(4, 2000 + delta, kD));
        } else {
            events.push_back(event(3, 2000, kA | kD)); events.push_back(event(4, 2000 - delta, kD));
        }
        auto result = record(fixture.directory(), events);
        check(result->timings.size() == 1, "每条换键只配对一次");
        check(result->timings[0].delta_ns == delta && result->timings[0].grade == expected, "2/10ms 原值边界及120ms未分类");
    }
    auto result = record(fixture.directory(), {event(1,0),event(2,1,kD),event(3,2),event(4,3000002,kA),
        event(5,4000000,kA | kW),event(6,5000000,kA),event(7,25000000,kA | kS)});
    check(result->timings.size() == 2 && result->timings[0].from == 'D' && result->timings[0].to == 'A' &&
        result->timings[1].from == 'W' && result->timings[1].to == 'S', "反向与 W/S 独立配对");
    result = record(fixture.directory(), {event(1,0),event(2,1,kA),event(3,2,kD)});
    check(result->timings.size() == 1 && result->timings[0].atomic_ambiguous &&
        result->timings[0].grade == Grade::UNCLASSIFIED, "同包松按不可称完美");
    auto release = event(3, 1000000), press = event(4, 3000000, kD);
    release.timing_uncertainty_ns = press.timing_uncertainty_ns = 1;
    result = record(fixture.directory(), {event(1,0),event(2,1,kA),release,press});
    check(result->timings[0].grade == Grade::UNCLASSIFIED && result->timings[0].uncertainty_crosses_boundary,
        "已知误差跨越2ms边界必须不确定");
    result = record(fixture.directory(), {event(1,0),event(2,1,kA),event(3,2),event(4,3,kA),event(5,4),event(6,5,kD)});
    check(result->timings.size() == 1 && result->timings[0].release_sequence == 5 && result->unpaired_edges >= 1,
        "同向重按不能复用旧松开");
}
void hold_tests(Fixture& fixture) {
    std::vector<Event> events{event(1,0),event(2,1000,0,true)};
    for (int index = 0; index < 400; ++index) events.push_back(event(index+3,2000+index*1000,0,true,index%2 ? -1 : 1,0));
    events.push_back(event(403, 500000, 0, false));
    events[40].raw_report_valid = true; events[40].raw_report[10] = 0xa5;
    Limits limits; limits.chunk_events = 32;
    const auto directory = fixture.directory();
    auto result = record(directory, events, limits);
    check(result->holds.size() == 1 && result->holds[0]->complete_received_stream, "按下至松开完整接收");
    auto hold = result->holds[0];
    check(hold->points.size() == 402 && result->chunks > 1, "超过128点按原始事件分chunk保存");
    check(hold->points[1].x == 1 && hold->points[2].x == 0 && std::abs(hold->points[2].turn_radians) > 3,
        "2ms内往返和拐点不能被聚合抹去");
    check(hold->points.back().event.sequence == 403 && hold->points.back().raw_index == 402,
        "末次松开保留原始索引，记录与 Recoil EXHAUSTED 无依赖");
    check(!hold->source_loss_verifiable, "本地连续序号不升级为源无损");
    const auto offline = replay(directory);
    check(offline->status == Status::STOPPED && offline->replay_source && offline->received_events == result->received_events,
        "原始文件回放同生产契约且永久保留离线来源");
    check(offline->holds[0]->points.size() == hold->points.size() && offline->holds[0]->points[39].event.raw_report[10] == 0xa5,
        "回放不覆盖原始点和20字节报告");
    result = record(fixture.directory(), {event(1,0),event(2,1,0,true,1,0),event(3,2,0,false,2,0)});
    check(result->holds[0]->boundary_ambiguous && result->holds[0]->points.back().x == 3,
        "同包位移保持原始数值且标边界歧义");
    auto down = event(2,1,0,true), up = event(3,2);
    down.motion_valid = up.motion_valid = false; down.dx = 123; up.dx = 456;
    result = record(fixture.directory(), {event(1,0),down,up});
    check(result->holds[0]->complete_received_stream && !result->holds[0]->motion_available &&
        result->holds[0]->points.back().x == 0 && result->holds[0]->points[0].event.dx == 123,
        "位移语义不可用仍保存完整按住原始报告，但不得积分");
    result = record(fixture.directory(), {event(1,0,0,true),event(2,1)});
    check(!result->holds[0]->complete_received_stream && result->holds[0]->end == HoldEnd::MISSING_START,
        "开头已按住不能伪造按下");
    result = record(fixture.directory(), {event(1,0),event(2,1,0,true)});
    check(result->holds[0]->end == HoldEnd::MISSING_END, "停止时没有松开就是缺尾");
}
void gap_tests(Fixture& fixture) {
    auto result = record(fixture.directory(), {event(1,0),event(2,1,kA,true),event(4,3,0,true),event(5,4,kD),
        event(6,5),event(7,6,kA),event(8,7),event(9,8,kD)});
    check(result->holds.size() == 2 && result->holds[0]->end == HoldEnd::GAP &&
        result->holds[1]->end == HoldEnd::MISSING_START, "gap切断轨迹，新held从缺头恢复");
    check(result->timings.size() == 2 && result->timings[0].from == 'D', "缺口前旧边沿不能跨gap评分，后续合法切换恢复");
    auto stale = event(1,0,kD,true);
    result = record(fixture.directory(), {event(1,0),event(2,1,kA,true),stale,stale,event(3,2),event(4,3,kD)});
    check(result->invalid_events == 2 && result->timings.empty() && result->holds.size() == 1,
        "乱序重复报告拒绝且保留水位，不建立旧baseline");
    auto other_epoch = event(1,5,kD,true); other_epoch.epoch = 2;
    auto older_epoch = event(10,6,kA,true);
    auto finish = event(2,7); finish.epoch = 2;
    result = record(fixture.directory(), {event(1,0),event(2,1,kA,true),other_epoch,older_epoch,finish});
    check(result->timings.empty() && result->invalid_events >= 2, "epoch变化与旧epoch迟到都不能配对");
    Session session;
    auto calls = std::make_shared<unsigned>(0);
    check(session.start(fixture.directory(), {}, [calls] { ReadBatch batch;
        if ((*calls)++ == 0) batch.events = {event(1,0),event(2,1,0,true)};
        else { batch.gap = true; batch.dropped_events = 7; }
        return batch;
    }), "gap-only Reader启动");
    const auto until = std::chrono::steady_clock::now()+std::chrono::seconds(1);
    while (session.snapshot()->received_events < 2 && std::chrono::steady_clock::now()<until)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    session.stop(); result = session.snapshot();
    check(result->holds[0]->end == HoldEnd::GAP && result->dropped_events >= 7 && result->dropped_events % 7 == 0,
        "空批gap损坏当前hold并保存来源丢弃计数");
}
void archive_tests(Fixture& fixture) {
    std::vector<Event> events{event(1,0),event(2,1,0,true),event(3,2,0,true,1,1),event(4,3,0,true,1,1),event(5,4,0,true,1,1),event(6,5)};
    Limits limits; limits.max_hold_events = 3;
    auto directory = fixture.directory();
    auto result = record(directory, events, limits);
    check(result->status == Status::LIMIT && result->holds[0]->end == HoldEnd::LIMIT && !result->holds[0]->complete_received_stream,
        "hold预算终止记录且不称完整");
    auto offline = replay(directory);
    check(offline->status == Status::LIMIT && offline->holds[0]->points.size() == result->holds[0]->points.size(),
        "回放使用保存的hold预算，不能补出原记录不存在的点");
    limits = {}; limits.max_run_events = 3;
    result = record(fixture.directory(), events, limits);
    check(result->status == Status::LIMIT && result->received_events == 3, "Run记录数预算有界");
    limits = {}; limits.chunk_events = 1; limits.max_chunks = 2;
    result = record(fixture.directory(), events, limits);
    check(result->status == Status::LIMIT && result->received_events == 2 && result->chunks == 2, "文件数量预算有界");
    limits = {}; limits.max_run_bytes = 1024;
    result = record(fixture.directory(), events, limits);
    check(result->status == Status::LIMIT && result->received_events == 0, "Run字节预算先于写入检查");
    Session existing;
    check(!existing.start(directory), "不能覆盖旧归档目录");
    directory = fixture.directory();
    {
        Session failed;
        check(failed.start(directory, {}, [directory] { std::ofstream conflict(directory / "events-0.csv"); conflict << "conflict";
            ReadBatch batch; batch.events = {event(1,0)}; return batch; }), "发布冲突测试启动");
        failed.stop(); result = failed.snapshot();
        check(result->status == Status::FAILED && !std::filesystem::exists(directory / "manifest.txt"),
            "原子发布冲突失败且不写成功manifest");
    }
    directory = fixture.directory(); record(directory, {event(1,0),event(2,1)});
    { std::ofstream broken(directory / "events-0.csv",std::ios::app); broken << "not-an-event\n"; }
    check(replay(directory)->status == Status::FAILED, "损坏原始事件不能伪装合法空或完整回放");
    directory = fixture.directory(); record(directory, events);
    std::filesystem::remove(directory / "events-0.csv");
    check(replay(directory)->status == Status::FAILED, "缺少chunk需显式失败");
}
void timeout_test(Fixture& fixture) {
    struct Gate { std::mutex mutex; std::condition_variable condition; bool entered = false, release = false; };
    auto gate = std::make_shared<Gate>();
    Session session; Limits limits; limits.stop_timeout_ms = 20;
    check(session.start(fixture.directory(), limits, [gate] {
        std::unique_lock lock(gate->mutex); gate->entered = true; gate->condition.notify_all();
        gate->condition.wait(lock,[&] {return gate->release;}); return ReadBatch{};
    }), "慢Reader测试启动");
    { std::unique_lock lock(gate->mutex); check(gate->condition.wait_for(lock,std::chrono::seconds(1),[&] {return gate->entered;}),"慢Reader已进入"); }
    const auto before = std::chrono::steady_clock::now(); session.stop();
    check(std::chrono::steady_clock::now()-before < std::chrono::milliseconds(500) && session.snapshot()->status == Status::STOP_TIMEOUT,
        "后台阻塞时stop仍有界，不能join无限等待");
    { std::lock_guard lock(gate->mutex); gate->release = true; } gate->condition.notify_all();
    std::this_thread::sleep_for(std::chrono::milliseconds(50)); session.stop();
}
}
int main() {
    try {
        Fixture fixture;
        timing_tests(fixture); hold_tests(fixture); gap_tests(fixture); archive_tests(fixture); timeout_test(fixture);
        std::cout << "输入评估生产接口专项通过；未使用设备或真实鼠标输出\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
