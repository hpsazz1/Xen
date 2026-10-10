#include <data_collection/data_collection.h>
#include <log/log.h>
#include <nlohmann/json.hpp>
#include <opencv2/imgcodecs.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

namespace {
int failures = 0;
void expect(bool value, const char* message) { if (!value) { ++failures; std::cerr << "失败：" << message << '\n'; } }
nlohmann::json read(const std::filesystem::path& path) { std::ifstream stream(path); return nlohmann::json::parse(stream); }
void settle(data_collection::Collector& collector) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (collector.snapshot().queued && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(2));
}
// 生产 try_lock 可以拒绝任一帧；重试下一帧必须重新送入同一人工请求。
void manual(data_collection::Collector& collector, CapturedFrame& frame, std::span<const Detection> detections, DetectionStatus status) {
    collector.request_sample();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do { collector.offer(frame, detections, status, 7); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    while (collector.snapshot().manual_pending && std::chrono::steady_clock::now() < deadline);
    settle(collector);
}
void automatic(data_collection::Collector& collector, CapturedFrame& frame,
               std::span<const Detection> detections, bool allowed = true) {
    const auto before = collector.snapshot();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
        collector.offer(frame, detections, DetectionStatus::SUCCESS, 7, allowed);
        settle(collector);
        const auto after = collector.snapshot();
        if (after.saved != before.saved || after.filtered != before.filtered ||
            after.duplicates != before.duplicates) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
}
void configured_log_level_test(const data_collection::Config& config) {
    for (const auto level : {LogLevel::OFF, LogLevel::INFO}) {
        LogConfig logging;
        logging.enable_console = logging.enable_file = false;
        logging.module_levels.emplace("data", level);
        Log::init(logging);
        {
            data_collection::Collector collector;
            std::string error;
            expect(collector.start(config, error), "日志等级回归使用生产采集入口");
            collector.stop();
            const auto lines = Log::get_ring_buffer();
            expect(level == LogLevel::OFF ? lines.empty() : !lines.empty(),
                   "data 模块 OFF 必须过滤实际采集日志，INFO 正常输出");
        }
        Log::shutdown();
    }
}
}
int make_fixture(const std::filesystem::path& root) {
    try {
        if (!std::filesystem::create_directory(root)) throw std::runtime_error("fixture 目录已存在，拒绝覆盖");
        const auto model = root / "fixture-identity-only.onnx";
        { std::ofstream output(model); output << "合成集成测试身份文件，并非可推理 ONNX"; }
        data_collection::Config config;
        config.root_directory = root / "raw";
        const auto encoded_path = model.u8string();
        config.model_path.assign(reinterpret_cast<const char*>(encoded_path.data()), encoded_path.size());
        config.class_names = {"person"};
        config.exploration_seed = 123;
        const Detection box{2, 3, 24, 27, 0.75f, 0};
        cv::RNG random(20260912);
        for (int session = 0; session < 3; ++session) {
            data_collection::Collector collector;
            std::string error;
            if (!collector.start(config, error)) throw std::runtime_error(error);
            CapturedFrame frame;
            frame.width = frame.height = 32;
            frame.bgr = cv::Mat(32, 32, CV_8UC3);
            for (int sample = 0; sample < 2; ++sample) {
                random.fill(frame.bgr, cv::RNG::UNIFORM, 0, 256);
                frame.timing.sequence = static_cast<std::uint64_t>(session * 2 + sample + 1);
                manual(collector, frame, sample == 0 ? std::span(&box, 1) : std::span<const Detection>{}, DetectionStatus::SUCCESS);
            }
            collector.stop();
            if (collector.snapshot().saved != 2) throw std::runtime_error("fixture 样本未全部提交");
        }
        std::cout << "已生成 3 个独立合成会话，每会话 1 个正框候选和 1 个空框待审样本。\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring(argv[1]) == L"--fixture") return make_fixture(std::filesystem::absolute(std::filesystem::path(argv[2])));
    if (argc != 1) { std::cerr << "参数应为 --fixture <新目录>\n"; return 2; }
    const auto root = std::filesystem::temp_directory_path() / ("xen-data-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(root);
    try {
        const auto model = root / "fixture.onnx";
        { std::ofstream file(model); file << "模型身份测试"; }
        data_collection::Config config;
        config.root_directory = root / "raw";
        config.model_path = model.string();
        config.class_names = {"person"};
        config.interval_ms = 0;
        config.exploration_interval_ms = 60000;
        configured_log_level_test(config);
        data_collection::Collector collector;
        std::string error;
        {
            data_collection::Collector quality;
            expect(quality.start(config, error), "采集质量回归启动");
            CapturedFrame candidate;
            candidate.width = candidate.height = 32;
            candidate.bgr = cv::Mat::zeros(32, 32, CV_8UC3);
            automatic(quality, candidate, {});
            expect(quality.snapshot().saved == 0 && quality.snapshot().filtered == 1,
                   "自动纯黑转场被过滤且可见");
            manual(quality, candidate, {}, DetectionStatus::SUCCESS);
            expect(quality.snapshot().saved == 1, "手动可保留纯黑诊断样本");
            quality.stop();
            expect(quality.start(config, error), "暗光负样本启动");
            candidate.bgr.setTo(cv::Scalar(2, 2, 2));
            candidate.bgr.at<cv::Vec3b>(17, 17) = {4, 4, 4};
            automatic(quality, candidate, {});
            expect(quality.snapshot().saved == 1, "暗光有纹理无框探索样本仍保留");
            candidate.bgr.setTo(cv::Scalar(180, 100, 40));
            automatic(quality, candidate, {});
            expect(quality.snapshot().saved == 1 && quality.snapshot().filtered == 1,
                   "移动墙面空框不再每秒novel入库");
            const Detection pair[] = {{1, 2, 10, 15, .8f, 0}, {20, 20, 30, 31, .8f, 0}};
            automatic(quality, candidate, pair, false);
            expect(quality.snapshot().saved == 1 && quality.snapshot().filtered == 2,
                   "已确认非采集场景拒绝自动有框样本");
            quality.request_sample();
            while (quality.snapshot().manual_pending)
                quality.offer(candidate, pair, DetectionStatus::SUCCESS, 7, false);
            settle(quality);
            expect(quality.snapshot().saved == 2, "手动绕过场景资格过滤");
            const Detection reversed[] = {pair[1], pair[0]};
            automatic(quality, candidate, reversed);
            expect(quality.snapshot().saved == 2 && quality.snapshot().duplicates > 0,
                   "相同检测集合换序不会制造新样本");
            quality.stop();
            expect(quality.start(config, error), "纯白转场启动");
            candidate.bgr.setTo(cv::Scalar(255, 255, 255));
            automatic(quality, candidate, pair);
            expect(quality.snapshot().saved == 0 && quality.snapshot().filtered == 1,
                   "纯白转场即使误检也自动过滤");
            quality.stop();
        }
        expect(collector.start(config, error), "启动成功");
        CapturedFrame frame;
        frame.width = frame.height = 32;
        frame.bgr = cv::Mat(32, 32, CV_8UC3, cv::Scalar(10, 20, 30));
        frame.timing.sequence = 42;
        const Detection box{1, 2, 20, 25, 0.4f, 0};
        collector.request_sample();
        while (collector.snapshot().manual_pending) collector.offer(frame, std::span(&box, 1), DetectionStatus::SUCCESS, 7);
        frame.bgr.setTo(cv::Scalar(99, 99, 99));
        settle(collector);
        const auto session = collector.snapshot().session_directory;
        const auto record = read(session / "samples/1.json");
        expect(record["sequence"] == 42 && record["detector_generation"] == 7, "图片和检测同帧同代");
        expect(record["review_state"] == "PRELABELED" && record["detections"].size() == 1, "成功框只是预标注");
        expect(record["image_sha256"].get<std::string>().size() == 64 && read(session / "session.json")["model_sha256"].get<std::string>().size() == 64, "图像模型都有身份");
        frame.bgr.setTo(cv::Scalar(99, 99, 99));
        auto image = cv::imread((session / "images/1.png").string());
        expect(image.at<cv::Vec3b>(0, 0) == cv::Vec3b(10, 20, 30), "原图独立且无叠框");
        manual(collector, frame, {}, DetectionStatus::SUCCESS);
        expect(read(session / "samples/2.json")["review_state"] == "PRELABELED", "成功空结果不能成为已核验负样本");
        manual(collector, frame, std::span(&box, 1), DetectionStatus::INFERENCE_FAILED);
        const auto failed = read(session / "samples/3.json");
        expect(failed["review_state"] == "RAW" && failed["detections"].empty(), "失败不产生伪标签");
        const auto count = collector.snapshot().saved;
        collector.set_paused(true);
        collector.request_sample();
        collector.offer(frame, {}, DetectionStatus::SUCCESS, 7);
        expect(collector.snapshot().saved == count && !collector.snapshot().manual_pending, "暂停拒绝请求");
        collector.set_paused(false);
        manual(collector, frame, {}, DetectionStatus::SUCCESS);
        collector.offer(frame, {}, DetectionStatus::SUCCESS, 7);
        expect(collector.snapshot().duplicates > 0, "重复画面去重");
        for (int attempt = 0; attempt < 100 && !collector.snapshot().paused; ++attempt) {
            collector.offer(frame, {}, DetectionStatus::SUCCESS, 8);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        expect(collector.snapshot().paused && collector.snapshot().saved == 4, "模型变代不会混入旧会话");
        collector.stop();
        expect(!collector.snapshot().active && collector.snapshot().queued == 0, "停止清空队列");
        const auto stopped_drops = collector.snapshot().dropped;
        std::thread observer([&] { for (int i = 0; i < 10000; ++i) (void)collector.snapshot(); });
        for (int i = 0; i < 10000; ++i) collector.offer(frame, {}, DetectionStatus::SUCCESS, 7);
        observer.join();
        expect(collector.snapshot().dropped == stopped_drops, "关闭后持续送帧无锁丢弃计数");
        expect(collector.start(config, error), "停止后可重开");
        expect(collector.snapshot().session_directory != session, "新会话不覆盖旧数据");
        for (int sample = 0; sample < 305; ++sample) {
            ++frame.timing.sequence;
            manual(collector, frame, {}, DetectionStatus::SUCCESS);
        }
        expect(collector.snapshot().saved == 305 && !collector.snapshot().paused &&
               collector.snapshot().error.empty(), "持续采集超过旧300张上限");
        const auto policy = read(collector.snapshot().session_directory / "session.json")["policy"];
        expect(policy.value("continuous", false) && !policy.contains("max_samples") &&
               !policy.contains("max_bytes"), "新会话明确连续采集且不宣称累计配额");
        collector.set_paused(true);
        collector.set_paused(false);
        manual(collector, frame, {}, DetectionStatus::SUCCESS);
        expect(collector.snapshot().saved == 306, "超过旧上限后仍可暂停并继续");
        collector.stop();
        expect(collector.start(config, error), "真实写入失败场景启动");
        const auto failed_session = collector.snapshot().session_directory;
        // 用目录占用待写JSON路径，确定性触发实际文件写入失败，不耗满用户磁盘。
        std::filesystem::create_directory(failed_session / "samples/1.pending");
        manual(collector, frame, {}, DetectionStatus::SUCCESS);
        expect(collector.snapshot().saved == 0 && collector.snapshot().paused &&
               !collector.snapshot().error.empty(), "取消配额后真实写入失败仍暂停且不虚报成功");
        expect(!std::filesystem::exists(failed_session / "samples/1.json"), "写入失败没有提交标记");
        collector.set_paused(false);
        expect(collector.snapshot().paused, "写入故障不能由继续按钮掩盖");
        collector.stop();
        expect(collector.start(config, error), "GPU 拒绝场景启动");
        frame.storage = CapturedFrameStorage::D3D11_BGRA8;
        for (int attempt = 0; attempt < 100 && !collector.snapshot().paused; ++attempt) {
            collector.offer(frame, {}, DetectionStatus::SUCCESS, 7);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        expect(collector.snapshot().paused && !collector.snapshot().error.empty(), "GPU 明确拒绝");
        collector.stop();
        expect(collector.start(config, error), "非法尺寸场景启动");
        frame.storage = CapturedFrameStorage::CPU_BGR;
        frame.width = 33;
        for (int attempt = 0; attempt < 100 && !collector.snapshot().paused; ++attempt) {
            collector.offer(frame, {}, DetectionStatus::SUCCESS, 7);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        expect(collector.snapshot().paused, "尺寸契约检查");
        collector.stop();
        config.queue_capacity = 1;
        config.buffer_bytes = 2 * 1024 * 1024;
        expect(collector.start(config, error), "有界队列场景启动");
        frame.width = frame.height = 512;
        frame.bgr = cv::Mat(512, 512, CV_8UC3);
        cv::randu(frame.bgr, 0, 255);
        bool bounded = true;
        for (int i = 0; i < 40; ++i) {
            collector.request_sample();
            collector.offer(frame, {}, DetectionStatus::SUCCESS, 7);
            bounded &= collector.snapshot().queued <= 2;
        }
        collector.stop();
        expect(bounded && collector.snapshot().dropped > 0, "慢写盘只允许固定槽且丢弃候选");
        config.root_directory = model;
        expect(!collector.start(config, error) && !error.empty(), "文件路径不能当输出目录");
        config.root_directory = root / "raw";
        config.model_path = (root / "missing.onnx").string();
        expect(!collector.start(config, error), "不存在的模型拒绝启动");
    } catch (const std::exception& error) { ++failures; std::cerr << error.what() << '\n'; }
    // 本测试创建的唯一临时根是绝对路径，且始终位于系统临时目录。
    if (root.parent_path() == std::filesystem::temp_directory_path() && root.filename().string().starts_with("xen-data-test-")) std::filesystem::remove_all(root);
    return failures ? 1 : 0;
}
