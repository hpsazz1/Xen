#include "lineup/lineup.h"
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <opencv2/imgproc.hpp>
#include <stdexcept>
namespace {
void check(bool ok, const std::string &message) {
    if (!ok)
        throw std::runtime_error(message);
}
CapturedFrame frame(const cv::Mat &image, std::uint64_t sequence = 1) {
    CapturedFrame f;
    f.bgr = image;
    f.width = f.encoded_width = f.source_width = image.cols;
    f.height = f.encoded_height = f.source_height = image.rows;
    f.timing.sequence = sequence;
    f.timing.captured_at = lineup::Clock::now();
    return f;
}
cv::Mat fixture() {
    cv::Mat image(480, 640, CV_8UC3);
    cv::RNG random(4418);
    random.fill(image, cv::RNG::UNIFORM, 0, 255);
    cv::GaussianBlur(image, image, {3, 3}, .6);
    for (int i = 0; i < 120; ++i)
        cv::circle(image, {random.uniform(20, 620), random.uniform(20, 460)}, random.uniform(3, 14),
                   {double(random.uniform(0, 255)), double(random.uniform(0, 255)),
                    double(random.uniform(0, 255))},
                   -1);
    return image;
}
} // namespace
int main(int argc, char **argv) {
    const auto root =
        std::filesystem::temp_directory_path() /
        ("xen-lineup-test-" + std::to_string(lineup::Clock::now().time_since_epoch().count()));
    try {
        std::filesystem::create_directory(root);
        std::string error;
        const bool roi_benchmark = argc > 1 && std::string(argv[1]) == "--benchmark-roi";
        const auto mode = roi_benchmark ? lineup::ReferenceMode::ROI : lineup::ReferenceMode::FULL_FRAME;
        const auto image = roi_benchmark ? fixture()(cv::Rect(160, 80, 320, 320)).clone() : fixture();
        auto f = frame(image);
        auto map_roi = [](CapturedFrame &v) { v.source_width = 1920; v.source_height = 1080; v.roi_x = 800; v.roi_y = 380; v.source_mapping_verified = true; };
        if (roi_benchmark) map_roi(f);
        const cv::Point2d center(image.cols / 2., image.rows / 2.);
        lineup::Engine engine;
        check(engine.locate(f, "a", f.timing.captured_at).status == lineup::Status::NO_REFERENCE,
              "empty reference");
        check(engine.create_reference(f, "a", "reference-1", "synthetic fixture", center, {},
                                      f.timing.captured_at, error, mode),
              error);
        auto prepared = lineup::Engine::prepare(f, "a", mode);
        auto reused = engine.locate(f, "a", f.timing.captured_at, prepared, mode);
        check(reused.status == lineup::Status::VALID && prepared.feature_ms > 0, "prepared identity");
        check(engine.locate(f, "wrong-session", f.timing.captured_at, prepared).status == lineup::Status::INVALID_FRAME,
              "prepared session mismatch");
        auto other_frame = frame(image.clone(), 99);
        check(engine.locate(other_frame, "a", other_frame.timing.captured_at, prepared).status == lineup::Status::INVALID_FRAME,
              "prepared sequence and storage mismatch");
        if (argc > 1 && (std::string(argv[1]) == "--benchmark" || roi_benchmark)) {
            nlohmann::json results = nlohmann::json::array();
            for (int library_size : {4, 40}) {
                const auto library = root / std::to_string(library_size);
                for (int i = 0; i < library_size; ++i) {
                    check(engine.annotate_reference(center, {}, "ref-" + std::to_string(i), error), error);
                    check(engine.save_reference(library, error), error);
                }
                lineup::ReferenceCache cache;
                for (int pass = 0; pass < 3; ++pass) {
                    for (bool optimized : {false, true}) {
                        double total = 0, load = 0, feature = 0, match = 0;
                        int valid = 0;
                        const auto before = cache.stats();
                        for (int start = 0; start < library_size; start += 16) {
                            auto current = frame(image, 100 + start + pass * 100);
                            if (roi_benchmark) map_roi(current);
                            const auto begun = lineup::Clock::now();
                            auto shared = optimized ? lineup::Engine::prepare(current, "bench", mode) : lineup::PreparedFrame{};
                            feature += shared.feature_ms;
                            for (int i = start; i < std::min(start + 16, library_size); ++i) {
                                const auto t = lineup::Clock::now();
                                lineup::Engine baseline;
                                lineup::Engine *candidate = optimized ? cache.get(library / ("ref-" + std::to_string(i)), error) : &baseline;
                                if (!optimized) check(baseline.load_reference(library / ("ref-" + std::to_string(i)), error), error);
                                check(candidate != nullptr, error);
                                load += std::chrono::duration<double, std::milli>(lineup::Clock::now() - t).count();
                                auto observed = optimized ? candidate->locate(current, "bench", current.timing.captured_at, shared, mode)
                                                          : candidate->locate(current, "bench", current.timing.captured_at, mode);
                                match += observed.processing_ms;
                                check(observed.status == lineup::Status::VALID && cv::norm(*observed.aim - center) < .1, "benchmark equivalence");
                                ++valid;
                            }
                            total += std::chrono::duration<double, std::milli>(lineup::Clock::now() - begun).count();
                        }
                        auto after = cache.stats();
                        results.push_back({{"mode", roi_benchmark ? "roi" : "full_frame"}, {"library_size", library_size}, {"pass", pass}, {"optimized", optimized},
                            {"total_ms", total}, {"load_ms", load}, {"shared_feature_ms", feature}, {"locate_ms", match},
                            {"valid", valid}, {"hits", after.hits - before.hits}, {"misses", after.misses - before.misses},
                            {"cache_bytes", after.bytes}, {"cache_entries", after.entries}, {"evictions", after.evictions - before.evictions}});
                    }
                }
            }
            std::cout << results.dump(2) << '\n';
            std::filesystem::remove_all(root);
            return 0;
        }
        const auto cache_root = root / "cache";
        check(engine.save_reference(cache_root, error), error);
        lineup::ReferenceCache cache(1, 128 * 1024 * 1024);
        check(cache.get(cache_root / "reference-1", error) != nullptr, error);
        check(cache.get(cache_root / "reference-1", error) != nullptr && cache.stats().hits == 1, "warm cache");
        check(engine.annotate_reference({321, 240}, {}, "reference-2", error), error);
        check(engine.save_reference(cache_root, error), error);
        check(cache.get(cache_root / "reference-2", error)->reference_info()->aim.x == 321 && cache.stats().evictions == 1,
              "new immutable revision and LRU eviction");
        check(cache.get(cache_root / "reference-1", error) != nullptr && cache.stats().misses == 3, "evicted reference reload");
        auto raw_path = cache_root / "reference-1" / "raw.png";
        std::filesystem::last_write_time(raw_path, std::filesystem::last_write_time(raw_path) + std::chrono::seconds(2));
        check(cache.get(cache_root / "reference-1", error) != nullptr && cache.stats().misses == 4, "file metadata invalidation");
        lineup::ReferenceCache tiny(32, 1);
        check(tiny.get(cache_root / "reference-1", error) != nullptr && tiny.stats().bytes == 0 && tiny.stats().entries == 0,
              "oversized reference not retained");
        lineup::ReferenceCache byte_limited(32, engine.memory_bytes() + 1024);
        check(byte_limited.get(cache_root / "reference-1", error) != nullptr, error);
        check(byte_limited.get(cache_root / "reference-2", error) != nullptr && byte_limited.stats().entries == 1 &&
              byte_limited.stats().evictions == 1 && byte_limited.stats().bytes <= engine.memory_bytes() + 1024,
              "byte budget evicts independently of entry budget");
        cache.clear();
        check(cache.stats().bytes == 0 && cache.stats().entries == 0, "explicit invalidation releases cache");
        check(engine.annotate_reference({320, 240}, {}, "reference-1", error), error);
        auto observation = engine.locate(f, "a", f.timing.captured_at);
        check(observation.status == lineup::Status::VALID, "identity: " + observation.reason);
        check(cv::norm(*observation.aim - cv::Point2d(320, 240)) < .1, "identity coordinate");
        check(observation.frame.session_id == "a" && observation.frame.sequence == 1 &&
                  !observation.source_age_ms,
              "frame binding and unknown clock");
        auto clone = engine.reference_image();
        clone.setTo(cv::Scalar(0, 0, 0));
        check(cv::norm(engine.reference_image(), image) == 0, "owned immutable reference");
        cv::Mat transform = (cv::Mat_<double>(3, 3) << 1, 0, 13, 0, 1, -7, 0, 0, 1);
        cv::Mat moved;
        cv::warpPerspective(image, moved, transform, image.size());
        auto changed = frame(moved, 2);
        observation = engine.locate(changed, "b", changed.timing.captured_at);
        check(observation.status == lineup::Status::VALID, "translation: " + observation.reason);
        check(cv::norm(*observation.aim - cv::Point2d(333, 233)) < 2,
              "known translation independent truth");
        check(observation.frame.session_id == "b" && observation.frame.sequence == 2,
              "new session identity");
        cv::Mat rotation = cv::getRotationMatrix2D({320, 240}, 3.0, 1.0), rotated;
        cv::warpAffine(image, rotated, rotation, image.size());
        auto turned = frame(rotated, 20);
        observation = engine.locate(turned, "b", turned.timing.captured_at);
        check(observation.status == lineup::Status::VALID, "rotation: " + observation.reason);
        check(cv::norm(*observation.aim - cv::Point2d(320, 240)) < 2,
              "known rotation independent truth");
        cv::Mat occluded = image.clone();
        occluded(cv::Rect(285, 205, 70, 70)).setTo(cv::Scalar(0, 0, 0));
        auto blocked = frame(occluded, 21);
        observation = engine.locate(blocked, "b", blocked.timing.captured_at);
        check(observation.status != lineup::Status::VALID && !observation.aim,
              "aim local occlusion rejected");
        check(cv::norm(f.bgr, image) == 0, "raw input unchanged");
        auto stale = f;
        stale.timing.captured_at -= std::chrono::seconds(2);
        observation = engine.locate(stale, "a", f.timing.captured_at);
        check(observation.status == lineup::Status::EXPIRED && !observation.aim,
              "stale retracts result");
        auto invalid = f;
        invalid.roi_x = 1;
        check(engine.locate(invalid, "a", f.timing.captured_at).status ==
                  lineup::Status::INVALID_FRAME,
              "cropped origin rejected");
        invalid = f;
        invalid.encoded_width += 1;
        check(!lineup::Engine::validate_frame(invalid, "a", f.timing.captured_at, error),
              "encoded coverage rejected");
        invalid = f;
        invalid.source_pixels_per_pixel_x = .5;
        check(!lineup::Engine::validate_frame(invalid, "a", f.timing.captured_at, error),
              "source coverage rejected");
        invalid = f;
        invalid.timing.sequence = 0;
        check(!lineup::Engine::validate_frame(invalid, "a", f.timing.captured_at, error),
              "missing sequence rejected");
        check(!lineup::Engine::validate_frame(f, "", f.timing.captured_at, error),
              "missing session rejected");
        invalid = f;
        invalid.timing.source_time_timing_valid = true;
        invalid.timing.source_clock_status = SourceClockStatus::VALID;
        invalid.timing.source_time_at = f.timing.captured_at - std::chrono::seconds(2);
        check(!lineup::Engine::validate_frame(invalid, "a", f.timing.captured_at, error),
              "source stale rejected");
        check(engine.save_reference(root, error), error);
        check(!engine.save_reference(root, error), "published record not overwritten");
        check(engine.save_reference(root / std::filesystem::path(L"中文样本"), error), error);
        lineup::Engine unicode;
        check(unicode.load_reference(root / std::filesystem::path(L"中文样本") / "reference-1",
                                     error),
              error);
        lineup::Engine loaded;
        check(loaded.load_reference(root / "reference-1", error), error);
        check(loaded.annotate_reference({321, 241}, {}, "edited", error), error);
        check(loaded.reference_info()->frame.session_id == "a" &&
                  loaded.reference_info()->frame.sequence == 1,
              "annotation preserves frame identity");
        check(loaded.save_reference(root, error), error);
        nlohmann::json original_metadata, edited_metadata;
        std::ifstream(root / "reference-1" / "reference.json") >> original_metadata;
        std::ifstream(root / "edited" / "reference.json") >> edited_metadata;
        check(original_metadata["captured_steady_ns"] == edited_metadata["captured_steady_ns"],
              "annotation preserves capture time");
        check(loaded.load_reference(root / "reference-1", error), error);
        auto fresh = frame(image, 3);
        observation = loaded.locate(fresh, "restart", fresh.timing.captured_at);
        check(observation.status == lineup::Status::VALID,
              "reload localization: " + observation.reason);
        std::ofstream(root / "not-directory") << "blocked";
        check(!engine.save_reference(root / "not-directory", error), "write failure explicit");
        auto blank = frame(cv::Mat(image.size(), CV_8UC3, cv::Scalar(90, 90, 90)), 4);
        check(loaded.locate(blank, "b", blank.timing.captured_at).status != lineup::Status::VALID,
              "weak texture rejected");
        cv::Mat mask(image.size(), CV_8UC1, cv::Scalar(0));
        check(!loaded.create_reference(f, "a", "empty-mask", "", {320, 240}, mask,
                                       f.timing.captured_at, error),
              "empty mask rejected");
        check(!loaded.reference_info(), "failed creation clears old reference");
        mask(cv::Rect(20, 20, 200, 440)).setTo(255);
        check(loaded.create_reference(f, "a", "extrapolation", "", {500, 240}, mask,
                                      f.timing.captured_at, error),
              error);
        observation = loaded.locate(f, "a", f.timing.captured_at);
        check(observation.status != lineup::Status::VALID && !observation.aim,
              "unsupported aim extrapolation rejected");
        cv::Mat stripe(image.size(), CV_8UC3, cv::Scalar(0));
        image(cv::Rect(0, 220, 640, 25)).copyTo(stripe(cv::Rect(0, 220, 640, 25)));
        auto narrow = frame(stripe, 5);
        check(loaded.create_reference(narrow, "a", "degenerate", "", {320, 240}, {},
                                      narrow.timing.captured_at, error),
              error);
        check(loaded.locate(narrow, "a", narrow.timing.captured_at).status != lineup::Status::VALID,
              "collinear/narrow support rejected");
        cv::Mat checker(image.size(), CV_8UC3, cv::Scalar(0));
        for (int y = 0; y < 480; y += 20)
            for (int x = 0; x < 640; x += 20)
                if ((x / 20 + y / 20) % 2)
                    checker(cv::Rect(x, y, 20, 20)).setTo(255);
        auto repeated = frame(checker, 6);
        check(loaded.create_reference(repeated, "a", "repeat", "", {320, 240}, {},
                                      repeated.timing.captured_at, error),
              error);
        check(loaded.locate(repeated, "a", repeated.timing.captured_at).status !=
                  lineup::Status::VALID,
              "repeated texture ambiguity rejected");
        auto scaled = f;
        scaled.source_width *= 2;
        scaled.source_height *= 2;
        scaled.source_pixels_per_pixel_x = scaled.source_pixels_per_pixel_y = 2;
        check(loaded.create_reference(scaled, "a", "scaled", "", {320, 240}, {},
                                      scaled.timing.captured_at, error),
              error);
        observation = loaded.locate(scaled, "a", scaled.timing.captured_at);
        check(observation.status == lineup::Status::VALID &&
                  cv::norm(*observation.source_aim - cv::Point2d(640, 480)) < .2,
              "pixel to source mapping");
        check(loaded.locate(f, "a", f.timing.captured_at).status == lineup::Status::INVALID_FRAME,
              "geometry change needs recapture");
        check(engine.save_reference(root / "corrupt-image", error), error);
        std::ofstream(root / "corrupt-image" / "reference-1" / "raw.png",
                      std::ios::binary | std::ios::trunc)
            << "not a PNG";
        check(!loaded.load_reference(root / "corrupt-image" / "reference-1", error) &&
                  !loaded.reference_info(),
              "bad raw image rejected");
        cv::Mat excluded(image.size(), CV_8UC1, cv::Scalar(255));
        excluded(cv::Rect(290, 210, 60, 60)).setTo(0);
        check(loaded.create_reference(f, "a", "masked-aim", "", {320, 240}, excluded,
                                      f.timing.captured_at, error),
              error);
        observation = loaded.locate(f, "a", f.timing.captured_at);
        check(observation.status != lineup::Status::VALID && !observation.aim,
              "masked local aim rejected");
        {
            auto roi = frame(image(cv::Rect(160, 80, 320, 320)).clone(), 400);
            map_roi(roi);
            lineup::Engine local;
            check(local.create_reference(roi, "roi", "roi-ref", "ROI", {160, 160}, {}, roi.timing.captured_at, error,
                                         lineup::ReferenceMode::ROI), error);
            auto local_result = local.locate(roi, "roi", roi.timing.captured_at, lineup::ReferenceMode::ROI);
            check(local_result.status == lineup::Status::VALID && local_result.source_aim &&
                  cv::norm(*local_result.source_aim - cv::Point2d(960, 540)) < .1, "320 ROI known source mapping");
            cv::Mat roi_shift = (cv::Mat_<double>(2, 3) << 1, 0, 13, 0, 1, -7), roi_moved;
            cv::warpAffine(roi.bgr, roi_moved, roi_shift, roi.bgr.size());
            auto translated_roi = roi; translated_roi.bgr = roi_moved; translated_roi.timing.sequence = 401;
            local_result = local.locate(translated_roi, "roi", translated_roi.timing.captured_at, lineup::ReferenceMode::ROI);
            check(local_result.status == lineup::Status::VALID && local_result.aim && local_result.source_aim &&
                  cv::norm(*local_result.aim - cv::Point2d(173, 153)) < 2 &&
                  cv::norm(*local_result.source_aim - cv::Point2d(973, 533)) < 2,
                  "ROI known translation local and source error below 2 pixels");
            check(local.locate(roi, "roi", roi.timing.captured_at).status == lineup::Status::INVALID_FRAME, "ROI full mode isolation");
            check(local.save_reference(root, error), error);
            lineup::Engine reopened;
            check(reopened.load_reference(root / "roi-ref", error), error);
            check(reopened.locate(roi, "roi", roi.timing.captured_at, lineup::ReferenceMode::ROI).status == lineup::Status::VALID, "ROI schema2 reload");
            auto shifted = roi; shifted.roi_x += 1;
            check(local.locate(shifted, "roi", shifted.timing.captured_at, lineup::ReferenceMode::ROI).status == lineup::Status::INVALID_FRAME, "ROI origin change rejected");
            auto unknown = roi; unknown.source_mapping_verified = false;
            unknown.source_width = unknown.source_height = 0;
            check(local.create_reference(unknown, "roi", "roi-local", "local", {160, 160}, {}, unknown.timing.captured_at, error,
                                         lineup::ReferenceMode::ROI), error);
            local_result = local.locate(unknown, "roi", unknown.timing.captured_at, lineup::ReferenceMode::ROI);
            check(local_result.status == lineup::Status::VALID && local_result.aim && !local_result.source_aim, "unknown mapping stays local");
            cv::Mat big_rotation = cv::getRotationMatrix2D({160, 160}, 35.0, 1.0), large_view;
            cv::warpAffine(unknown.bgr, large_view, big_rotation, unknown.bgr.size());
            auto large = unknown; large.bgr = large_view;
            check(local.locate(large, "roi", large.timing.captured_at, lineup::ReferenceMode::ROI).status != lineup::Status::VALID,
                  "ROI large viewpoint rejects");
            auto repeated_roi = unknown; repeated_roi.bgr = checker(cv::Rect(0, 0, 320, 320)).clone();
            lineup::Engine ambiguous;
            check(ambiguous.create_reference(repeated_roi, "roi", "roi-repeat", "", {160, 160}, {}, repeated_roi.timing.captured_at, error,
                                             lineup::ReferenceMode::ROI), error);
            check(ambiguous.locate(repeated_roi, "roi", repeated_roi.timing.captured_at, lineup::ReferenceMode::ROI).status != lineup::Status::VALID,
                  "ROI repeated texture rejects");
            auto blank = unknown; blank.bgr = cv::Mat(320, 320, CV_8UC3, cv::Scalar(120, 170, 220));
            check(local.locate(blank, "roi", blank.timing.captured_at, lineup::ReferenceMode::ROI).status != lineup::Status::VALID, "ROI sky rejects");
            cv::Mat displaced;
            cv::Mat shift = (cv::Mat_<double>(2, 3) << 1, 0, 190, 0, 1, 0);
            cv::warpAffine(unknown.bgr, displaced, shift, unknown.bgr.size());
            auto outside = unknown; outside.bgr = displaced;
            check(local.locate(outside, "roi", outside.timing.captured_at, lineup::ReferenceMode::ROI).status != lineup::Status::VALID, "ROI aim outside visible region rejects");
        }
        std::ofstream(root / "reference-1" / "reference.json", std::ios::trunc) << "{broken";
        check(!loaded.load_reference(root / "reference-1", error) && !loaded.reference_info(),
              "bad metadata clears reference");
        loaded.clear();
        check(!loaded.reference_info(), "clear lifecycle");
        std::filesystem::remove_all(root);
        std::cout << "PASS: synthetic behavior only; no real visual accuracy claim\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << "; artifacts: " << root << '\n';
        return 1;
    }
}
