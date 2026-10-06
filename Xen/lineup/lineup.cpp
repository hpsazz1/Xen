#include "lineup/lineup.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <list>
#include <nlohmann/json.hpp>
#include <opencv2/calib3d.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
namespace lineup {
namespace {
constexpr double kMaxAgeMs = 750.0;
constexpr const char *kAlgorithm = "orb-mutual-ratio-ransac-local-v1";
using Json = nlohmann::json;
bool safe_id(const std::string &id) {
    return !id.empty() && id.size() <= 96 && std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '-' || c == '_';
    });
}
bool inside(cv::Point2d p, cv::Size size) {
    return std::isfinite(p.x) && std::isfinite(p.y) && p.x >= 0 && p.y >= 0 && p.x < size.width &&
           p.y < size.height;
}
void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
cv::Mat gray(const cv::Mat &image) {
    cv::Mat result;
    cv::cvtColor(image, result, cv::COLOR_BGR2GRAY);
    return result;
}
void features(const cv::Mat &image, const cv::Mat &mask, std::vector<cv::KeyPoint> &points,
              cv::Mat &descriptors) {
    cv::ORB::create(2200)->detectAndCompute(image, mask, points, descriptors);
}
void write_image(const std::filesystem::path &path, const cv::Mat &image) {
    std::vector<unsigned char> encoded;
    require(cv::imencode(".png", image, encoded), "image_encode_failed");
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char *>(encoded.data()),
                 static_cast<std::streamsize>(encoded.size()));
    output.flush();
    require(output.good(), "image_write_failed");
    output.close();
    require(!output.fail(), "image_close_failed");
}
cv::Mat read_image(const std::filesystem::path &path, int flags) {
    const auto size = std::filesystem::file_size(path);
    require(size > 0 && size <= 256 * 1024 * 1024, "invalid_image_file_size");
    std::vector<unsigned char> encoded(static_cast<std::size_t>(size));
    std::ifstream input(path, std::ios::binary);
    input.read(reinterpret_cast<char *>(encoded.data()),
               static_cast<std::streamsize>(encoded.size()));
    require(input.good(), "image_read_failed");
    return cv::imdecode(encoded, flags);
}
Json geometry(const CapturedFrame &frame) {
    return {{"width", frame.width},
            {"height", frame.height},
            {"source_width", frame.source_width},
            {"source_height", frame.source_height},
            {"encoded_width", frame.encoded_width},
            {"encoded_height", frame.encoded_height},
            {"roi_x", frame.roi_x},
            {"roi_y", frame.roi_y},
            {"scale_x", frame.source_pixels_per_pixel_x},
            {"scale_y", frame.source_pixels_per_pixel_y}};
}
bool roi_geometry(const CapturedFrame &f) {
    if (f.storage != CapturedFrameStorage::CPU_BGR || f.bgr.empty() || f.bgr.type() != CV_8UC3 ||
        f.width != f.bgr.cols || f.height != f.bgr.rows || f.width < 64 || f.height < 64 ||
        f.width > 8192 || f.height > 8192 || static_cast<std::int64_t>(f.width) * f.height > 33554432 ||
        f.encoded_width < f.width || f.encoded_height < f.height ||
        !std::isfinite(f.roi_x) || !std::isfinite(f.roi_y) || f.roi_x < 0 || f.roi_y < 0 ||
        !std::isfinite(f.source_pixels_per_pixel_x) || !std::isfinite(f.source_pixels_per_pixel_y) ||
        f.source_pixels_per_pixel_x <= 0 || f.source_pixels_per_pixel_y <= 0) return false;
    if (f.source_width == 0 && f.source_height == 0) return !f.source_mapping_verified;
    return f.source_width > 0 && f.source_height > 0 &&
           f.roi_x + f.width * f.source_pixels_per_pixel_x <= f.source_width + .01 &&
           f.roi_y + f.height * f.source_pixels_per_pixel_y <= f.source_height + .01;
}
bool full_geometry(const CapturedFrame &f) {
    return f.storage == CapturedFrameStorage::CPU_BGR && !f.bgr.empty() &&
           f.bgr.type() == CV_8UC3 && f.width == f.bgr.cols && f.height == f.bgr.rows &&
           f.width >= 64 && f.height >= 64 && f.width <= 8192 && f.height <= 8192 &&
           static_cast<std::int64_t>(f.width) * f.height <= 33554432 && f.source_width > 0 &&
           f.source_height > 0 && f.encoded_width == f.width && f.encoded_height == f.height &&
           std::isfinite(f.roi_x) && std::isfinite(f.roi_y) && std::abs(f.roi_x) < 1e-6 &&
           std::abs(f.roi_y) < 1e-6 && std::isfinite(f.source_pixels_per_pixel_x) &&
           std::isfinite(f.source_pixels_per_pixel_y) && f.source_pixels_per_pixel_x > 0 &&
           f.source_pixels_per_pixel_y > 0 &&
           std::abs(f.width * f.source_pixels_per_pixel_x - f.source_width) < .01 &&
           std::abs(f.height * f.source_pixels_per_pixel_y - f.source_height) < .01;
}
} // namespace
struct Engine::Impl {
    CapturedFrame reference;
    cv::Mat mask;
    cv::Mat gray_image;
    cv::Mat descriptors;
    std::vector<cv::KeyPoint> points;
    ReferenceInfo info;
    Json metadata;
    bool ready = false;
    ReferenceMode mode = ReferenceMode::FULL_FRAME;
};
const char *status_name(Status status) noexcept {
    switch (status) {
    case Status::VALID:
        return "VALID";
    case Status::NOT_FOUND:
        return "NOT_FOUND";
    case Status::UNRELIABLE:
        return "UNRELIABLE";
    case Status::EXPIRED:
        return "EXPIRED";
    case Status::INVALID_FRAME:
        return "INVALID_FRAME";
    case Status::NO_REFERENCE:
        return "NO_REFERENCE";
    default:
        return "ERROR";
    }
}
Engine::Engine() : impl_(std::make_unique<Impl>()) {}
Engine::~Engine() = default;
Engine::Engine(Engine &&) noexcept = default;
Engine &Engine::operator=(Engine &&) noexcept = default;
void Engine::clear() noexcept {
    if (impl_) {
        impl_->ready = false;
        impl_->reference = {};
        impl_->mask.release();
        impl_->gray_image.release();
        impl_->descriptors.release();
        impl_->points.clear();
        impl_->metadata = {};
    }
}
bool Engine::validate_frame(const CapturedFrame &f, const std::string &session,
                            Clock::time_point now, std::string &error, ReferenceMode mode) noexcept {
    error.clear();
    if (!(mode == ReferenceMode::ROI ? roi_geometry(f) : full_geometry(f))) {
        error = mode == ReferenceMode::ROI ? "invalid_roi_geometry" : "invalid_full_frame_geometry";
        return false;
    }
    if (session.empty() || session.size() > 256 || f.timing.sequence == 0) {
        error = "missing_frame_identity";
        return false;
    }
    if (f.timing.captured_at == Clock::time_point{}) {
        error = "missing_capture_time";
        return false;
    }
    const double age =
        std::chrono::duration<double, std::milli>(now - f.timing.captured_at).count();
    if (age < 0 || age > kMaxAgeMs) {
        error = "expired_capture_time";
        return false;
    }
    if (f.timing.source_time_timing_valid) {
        const double source_age =
            std::chrono::duration<double, std::milli>(now - f.timing.source_time_at).count();
        if (f.timing.source_clock_status != SourceClockStatus::VALID ||
            f.timing.source_time_at == Clock::time_point{} ||
            !std::isfinite(f.timing.source_clock_uncertainty_ms) ||
            f.timing.source_clock_uncertainty_ms < 0 ||
            source_age < -f.timing.source_clock_uncertainty_ms || source_age > kMaxAgeMs) {
            error = "expired_source_time";
            return false;
        }
    }
    return true;
}
bool Engine::create_reference(const CapturedFrame &f, const std::string &session,
                              const std::string &id, const std::string &label, cv::Point2d aim,
                              const cv::Mat &mask, Clock::time_point now,
                              std::string &error, ReferenceMode mode) noexcept {
    clear();
    try {
        if (!validate_frame(f, session, now, error, mode))
            return false;
        impl_->mode = mode;
        require(safe_id(id), "invalid_reference_id");
        require(inside(aim, f.bgr.size()), "invalid_aim_point");
        require(mask.empty() || (mask.type() == CV_8UC1 && mask.size() == f.bgr.size()),
                "invalid_static_mask");
        impl_->reference = f;
        impl_->reference.bgr = f.bgr.clone();
        impl_->reference.bgr_storage.reset();
        impl_->mask = mask.empty() ? cv::Mat(f.bgr.size(), CV_8UC1, cv::Scalar(255)) : mask.clone();
        cv::threshold(impl_->mask, impl_->mask, 0, 255, cv::THRESH_BINARY);
        require(cv::countNonZero(impl_->mask) >= 1024, "insufficient_static_mask");
        impl_->gray_image = gray(impl_->reference.bgr);
        features(impl_->gray_image, impl_->mask, impl_->points, impl_->descriptors);
        impl_->info = {id,  label,        {session, f.timing.sequence},
                       aim, f.bgr.size(), {f.source_width, f.source_height}};
        impl_->metadata = {
            {"schema_version", mode == ReferenceMode::ROI ? 2 : 1},
            {"frame_mode", mode == ReferenceMode::ROI ? "roi" : "full_frame"},
            {"source_mapping_verified", f.source_mapping_verified},
            {"algorithm_version", kAlgorithm},
            {"id", id},
            {"label", label},
            {"session_id", session},
            {"sequence", f.timing.sequence},
            {"geometry", geometry(f)},
            {"aim", {aim.x, aim.y}},
            {"normalized_aim", {aim.x / f.width, aim.y / f.height}},
            {"captured_steady_ns", std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       f.timing.captured_at.time_since_epoch())
                                       .count()},
            {"source_time_valid", f.timing.source_time_timing_valid},
            {"source_timestamp_valid", f.timing.source_timestamp_valid},
            {"source_sequence", f.timing.source_sequence},
            {"source_sequence_valid", f.timing.source_sequence_valid},
            {"source_timecode", f.timing.source_timecode},
            {"source_timecode_valid", f.timing.source_timecode_valid},
            {"source_clock_uncertainty_ms", f.timing.source_clock_uncertainty_ms},
            {"source_timestamp", f.timing.source_timestamp},
            {"source_clock_session_id", f.timing.source_clock_session_id},
            {"source_time_basis", SourceTimeBasisName(f.timing.source_time_basis)},
            {"source_steady_ns", f.timing.source_time_timing_valid
                                     ? Json(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                f.timing.source_time_at.time_since_epoch())
                                                .count())
                                     : Json(nullptr)}};
        impl_->ready = true;
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        clear();
        return false;
    }
}
bool Engine::annotate_reference(cv::Point2d aim, const cv::Mat &static_mask,
                                const std::string &new_id, std::string &error) noexcept {
    try {
        error.clear();
        require(impl_->ready, "no_reference");
        require(safe_id(new_id), "invalid_reference_id");
        require(inside(aim, impl_->reference.bgr.size()), "invalid_aim_point");
        require(static_mask.empty() ||
                    (static_mask.type() == CV_8UC1 && static_mask.size() == impl_->mask.size()),
                "invalid_static_mask");
        cv::Mat mask = static_mask.empty() ? impl_->mask.clone() : static_mask.clone();
        cv::threshold(mask, mask, 0, 255, cv::THRESH_BINARY);
        require(cv::countNonZero(mask) >= 1024, "insufficient_static_mask");
        std::vector<cv::KeyPoint> points;
        cv::Mat descriptors;
        features(impl_->gray_image, mask, points, descriptors);
        Json metadata = impl_->metadata;
        metadata["id"] = new_id;
        metadata["aim"] = {aim.x, aim.y};
        metadata["normalized_aim"] = {aim.x / impl_->reference.width,
                                      aim.y / impl_->reference.height};
        impl_->mask = std::move(mask);
        impl_->points = std::move(points);
        impl_->descriptors = std::move(descriptors);
        impl_->metadata = std::move(metadata);
        impl_->info.id = new_id;
        impl_->info.aim = aim;
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}
bool Engine::save_reference(const std::filesystem::path &root, std::string &error) const noexcept {
    std::filesystem::path temporary;
    try {
        error.clear();
        require(impl_->ready, "no_reference");
        std::filesystem::create_directories(root);
        const auto destination = root / impl_->info.id;
        require(!std::filesystem::exists(destination), "reference_already_exists");
        static std::atomic<std::uint64_t> sequence{0};
        for (int attempts = 0; attempts < 10; ++attempts) {
            temporary = root / (".pending-" + impl_->info.id + "-" +
                                std::to_string(Clock::now().time_since_epoch().count()) + "-" +
                                std::to_string(++sequence));
            if (std::filesystem::create_directory(temporary))
                break;
            temporary.clear();
        }
        require(!temporary.empty(), "cannot_create_temporary_directory");
        write_image(temporary / "raw.png", impl_->reference.bgr);
        write_image(temporary / "mask.png", impl_->mask);
        std::ofstream output(temporary / "reference.json", std::ios::binary);
        output << impl_->metadata.dump(2);
        output.flush();
        require(output.good(), "metadata_write_failed");
        output.close();
        require(!output.fail(), "metadata_close_failed");
        std::filesystem::rename(temporary, destination);
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        // 仅清理由本调用新建的临时目录，不触碰已发布记录。
        if (!temporary.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(temporary, ignored);
        }
        return false;
    }
}
bool Engine::load_reference(const std::filesystem::path &directory, std::string &error) noexcept {
    clear();
    try {
        require(std::filesystem::file_size(directory / "reference.json") < 1024 * 1024,
                "metadata_too_large");
        std::ifstream input(directory / "reference.json", std::ios::binary);
        Json j;
        input >> j;
        require((j.at("schema_version") == 1 || (j.at("schema_version") == 2 && j.value("frame_mode", "") == "roi")) && j.at("algorithm_version") == kAlgorithm,
                "unsupported_reference_version");
        auto g = j.at("geometry");
        CapturedFrame f;
        f.source_mapping_verified = j.value("source_mapping_verified", false);
        f.width = g.at("width");
        f.height = g.at("height");
        f.source_width = g.at("source_width");
        f.source_height = g.at("source_height");
        f.encoded_width = g.at("encoded_width");
        f.encoded_height = g.at("encoded_height");
        f.roi_x = g.at("roi_x");
        f.roi_y = g.at("roi_y");
        f.source_pixels_per_pixel_x = g.at("scale_x");
        f.source_pixels_per_pixel_y = g.at("scale_y");
        require(f.width >= 64 && f.height >= 64 && f.width <= 8192 && f.height <= 8192 &&
                    static_cast<std::int64_t>(f.width) * f.height <= 33554432,
                "invalid_reference_dimensions");
        f.bgr = read_image(directory / "raw.png", cv::IMREAD_COLOR);
        cv::Mat mask = read_image(directory / "mask.png", cv::IMREAD_GRAYSCALE);
        require(!mask.empty(), "missing_mask");
        f.timing.sequence = j.at("sequence");
        f.timing.captured_at = Clock::now();
        cv::Point2d aim(j.at("aim").at(0).get<double>(), j.at("aim").at(1).get<double>());
        require(std::abs(j.at("normalized_aim").at(0).get<double>() - aim.x / f.width) < 1e-8 &&
                    std::abs(j.at("normalized_aim").at(1).get<double>() - aim.y / f.height) < 1e-8,
                "inconsistent_aim_coordinates");
        if (!create_reference(f, j.at("session_id"), j.at("id"), j.at("label"), aim, mask,
                              f.timing.captured_at, error, j.at("schema_version") == 2 ? ReferenceMode::ROI : ReferenceMode::FULL_FRAME))
            return false;
        impl_->metadata = std::move(j);
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        clear();
        return false;
    }
}
std::optional<ReferenceInfo> Engine::reference_info() const {
    return impl_->ready ? std::optional<ReferenceInfo>(impl_->info) : std::nullopt;
}
cv::Mat Engine::reference_image() const {
    return impl_->ready ? impl_->reference.bgr.clone() : cv::Mat{};
}
PreparedFrame Engine::prepare(const CapturedFrame &f, const std::string &session, ReferenceMode mode) {
    const auto started = Clock::now();
    PreparedFrame result;
    result.identity_ = {session, f.timing.sequence};
    result.mode_ = mode;
    result.pixels_ = f.bgr.data;
    result.gray_image_ = gray(f.bgr);
    features(result.gray_image_, {}, result.points_, result.descriptors_);
    result.feature_ms = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
    return result;
}
std::size_t Engine::memory_bytes() const noexcept {
    return impl_->reference.bgr.total() * impl_->reference.bgr.elemSize() +
           impl_->mask.total() * impl_->mask.elemSize() +
           impl_->gray_image.total() * impl_->gray_image.elemSize() +
           impl_->descriptors.total() * impl_->descriptors.elemSize() +
           impl_->points.capacity() * sizeof(cv::KeyPoint) + 1024 * 1024; // Conservative metadata/allocator allowance.
}
Observation Engine::locate(const CapturedFrame &f, const std::string &session,
                           Clock::time_point now, ReferenceMode mode) noexcept {
    // Preserve validation order and error statuses for the existing API.
    const auto preparation_started = Clock::now();
    PreparedFrame prepared;
    std::string error;
    if (impl_->ready && validate_frame(f, session, now, error, mode)) {
        try { prepared = prepare(f, session, mode); }
        catch (const std::exception &e) {
            Observation failed; failed.status = Status::ERROR; failed.reason = e.what(); return failed;
        }
    }
    auto result = locate(f, session, now + (Clock::now() - preparation_started), prepared, mode);
    result.processing_ms += prepared.feature_ms;
    return result;
}
Observation Engine::locate(const CapturedFrame &f, const std::string &session,
                           Clock::time_point now, const PreparedFrame &prepared, ReferenceMode mode) noexcept {
    Observation result;
    result.frame = {session, f.timing.sequence};
    const auto started = Clock::now();
    auto finish = [&](Status status, const std::string &reason) {
        result.status = status;
        result.reason = reason;
        result.processing_ms =
            std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        return result;
    };
    try {
        if (!impl_->ready)
            return finish(Status::NO_REFERENCE, "no_reference");
        result.reference_id = impl_->info.id;
        if (mode != impl_->mode) return finish(Status::INVALID_FRAME, "reference_mode_mismatch");
        std::string error;
        if (!validate_frame(f, session, now, error, mode))
            return finish(error.find("expired") != std::string::npos ? Status::EXPIRED
                                                                     : Status::INVALID_FRAME,
                          error);
        result.local_age_ms =
            std::chrono::duration<double, std::milli>(now - f.timing.captured_at).count();
        if (f.timing.source_time_timing_valid)
            result.source_age_ms =
                std::chrono::duration<double, std::milli>(now - f.timing.source_time_at).count();
        if (geometry(f) != geometry(impl_->reference) ||
            (mode == ReferenceMode::ROI && f.source_mapping_verified != impl_->reference.source_mapping_verified))
            return finish(Status::INVALID_FRAME, "geometry_changed_recapture_required");
        if (impl_->descriptors.rows < 12)
            return finish(Status::NOT_FOUND, "insufficient_reference_features");
        if (prepared.mode_ != mode || prepared.identity_.session_id != session || prepared.identity_.sequence != f.timing.sequence ||
            prepared.pixels_ != f.bgr.data)
            return finish(Status::INVALID_FRAME, "prepared_frame_mismatch");
        const auto &current = prepared.gray_image_;
        const auto &desc = prepared.descriptors_;
        const auto &points = prepared.points_;
        if (desc.rows < 12)
            return finish(Status::NOT_FOUND, "insufficient_current_features");
        cv::BFMatcher matcher(cv::NORM_HAMMING);
        std::vector<std::vector<cv::DMatch>> forward, backward;
        matcher.knnMatch(impl_->descriptors, desc, forward, 2);
        matcher.knnMatch(desc, impl_->descriptors, backward, 2);
        std::vector<cv::Point2f> source, target;
        for (const auto &pair : forward) {
            if (pair.size() != 2 || pair[0].distance >= .72f * pair[1].distance)
                continue;
            const auto &reverse = backward[pair[0].trainIdx];
            if (reverse.size() != 2 || reverse[0].trainIdx != pair[0].queryIdx ||
                reverse[0].distance >= .72f * reverse[1].distance)
                continue;
            source.push_back(impl_->points[pair[0].queryIdx].pt);
            target.push_back(points[pair[0].trainIdx].pt);
        }
        result.matches = static_cast<int>(source.size());
        if (source.size() < 12)
            return finish(Status::NOT_FOUND, "insufficient_unambiguous_matches");
        cv::Mat inlier_mask;
        cv::Mat transform =
            cv::findHomography(source, target, cv::RANSAC, 2.0, inlier_mask, 2000, .995);
        if (transform.empty() || !cv::checkRange(transform))
            return finish(Status::UNRELIABLE, "degenerate_homography");
        if (mode == ReferenceMode::ROI) {
            // ROI is a nearby visual alignment aid, not a large-viewpoint extrapolator.
            std::vector<cv::Point2f> probes{{float(f.width / 2), float(f.height / 2)},
                {float(f.width / 2 + 20), float(f.height / 2)},
                {float(f.width / 2), float(f.height / 2 + 20)}}, mapped;
            cv::perspectiveTransform(probes, mapped, transform);
            const auto dx = mapped[1] - mapped[0], dy = mapped[2] - mapped[0];
            const double sx = cv::norm(dx) / 20, sy = cv::norm(dy) / 20;
            const double rotation = std::abs(std::atan2(dx.y, dx.x));
            const double skew = std::abs(dx.dot(dy)) / std::max(1e-9, cv::norm(dx) * cv::norm(dy));
            if (!std::isfinite(sx) || !std::isfinite(sy) || sx < .75 || sx > 1.33 || sy < .75 || sy > 1.33 ||
                rotation > .262 || skew > .25)
                return finish(Status::UNRELIABLE, "roi_viewpoint_change_too_large");
        }
        std::vector<cv::Point2f> inlier_source, inlier_target, projected;
        cv::perspectiveTransform(source, projected, transform);
        double residual = 0;
        for (int i = 0; i < static_cast<int>(source.size()); ++i)
            if (inlier_mask.at<unsigned char>(i)) {
                inlier_source.push_back(source[i]);
                inlier_target.push_back(target[i]);
                residual += cv::norm(projected[i] - target[i]);
            }
        result.inliers = static_cast<int>(inlier_source.size());
        result.inlier_ratio = double(result.inliers) / source.size();
        if (result.inliers < 12 || result.inlier_ratio < .6)
            return finish(Status::UNRELIABLE, "inconsistent_matches");
        result.residual_px = residual / result.inliers;
        std::vector<cv::Point2f> hull, target_hull;
        cv::convexHull(inlier_source, hull);
        cv::convexHull(inlier_target, target_hull);
        result.coverage = std::min(cv::contourArea(hull), cv::contourArea(target_hull)) /
                          (double(f.width) * f.height);
        if (result.coverage < .08 || result.residual_px > 1.5)
            return finish(Status::UNRELIABLE, "insufficient_spatial_support");
        const auto bounds = cv::boundingRect(hull);
        if (bounds.width < f.width * .2 || bounds.height < f.height * .2)
            return finish(Status::UNRELIABLE, "degenerate_feature_distribution");
        // 不对特征凸包外的天空点外推；拒绝优先于伪精确坐标。
        if (cv::pointPolygonTest(hull, impl_->info.aim, true) < 0)
            return finish(Status::UNRELIABLE, "aim_outside_feature_support");
        std::vector<cv::Point2f> aim_input{
            {static_cast<float>(impl_->info.aim.x), static_cast<float>(impl_->info.aim.y)}},
            aim_output;
        cv::perspectiveTransform(aim_input, aim_output, transform);
        cv::Point2d aim = aim_output.front();
        if (!inside(aim, f.bgr.size()) || cv::pointPolygonTest(target_hull, aim, true) < 0)
            return finish(Status::UNRELIABLE, "aim_outside_current_support");
        // 原图尺度局部验证：将当前帧逆采样到参考坐标，计算静态瞄点邻域的相关性。
        cv::Mat aligned, valid(f.bgr.size(), CV_8UC1, cv::Scalar(255)), aligned_valid;
        cv::warpPerspective(current, aligned, transform, f.bgr.size(),
                            cv::INTER_LINEAR | cv::WARP_INVERSE_MAP);
        cv::warpPerspective(valid, aligned_valid, transform, f.bgr.size(),
                            cv::INTER_NEAREST | cv::WARP_INVERSE_MAP);
        const int radius = 20;
        const int cx = cvRound(impl_->info.aim.x), cy = cvRound(impl_->info.aim.y);
        if (cx < radius || cy < radius || cx + radius >= f.width || cy + radius >= f.height)
            return finish(Status::UNRELIABLE, "local_patch_outside_image");
        cv::Rect patch(cx - radius, cy - radius, 2 * radius + 1, 2 * radius + 1);
        cv::Mat patch_mask;
        cv::bitwise_and(impl_->mask(patch), aligned_valid(patch), patch_mask);
        if (cv::countNonZero(patch_mask) < patch.area() * .75)
            return finish(Status::UNRELIABLE, "local_patch_not_static");
        cv::Scalar mean_a, sd_a, mean_b, sd_b;
        cv::meanStdDev(impl_->gray_image(patch), mean_a, sd_a, patch_mask);
        cv::meanStdDev(aligned(patch), mean_b, sd_b, patch_mask);
        if (sd_a[0] < 8 || sd_b[0] < 8)
            return finish(Status::UNRELIABLE, "local_patch_weak_texture");
        cv::Mat a, b;
        impl_->gray_image(patch).convertTo(a, CV_32F);
        aligned(patch).convertTo(b, CV_32F);
        result.local_correlation =
            cv::mean((a - mean_a[0]).mul(b - mean_b[0]), patch_mask)[0] / (sd_a[0] * sd_b[0]);
        if (result.local_correlation < .80)
            return finish(Status::UNRELIABLE, "local_patch_disagrees");
        // 计算可能跨越有效期，发布前以本次调用经过时长再次核验。
        if (!validate_frame(f, session, now + (Clock::now() - started), error, mode))
            return finish(Status::EXPIRED, error);
        result.aim = aim;
        if (mode == ReferenceMode::FULL_FRAME || f.source_mapping_verified)
            result.source_aim = cv::Point2d(f.roi_x + aim.x * f.source_pixels_per_pixel_x,
                                        f.roi_y + aim.y * f.source_pixels_per_pixel_y);
        return finish(Status::VALID, "matched_visual_reference_only");
    } catch (const std::exception &e) {
        return finish(Status::ERROR, e.what());
    }
}
struct ReferenceCache::Impl {
    struct Entry { std::filesystem::path path; std::string signature; std::unique_ptr<Engine> engine; std::size_t bytes; };
    std::list<Entry> entries;
    std::unique_ptr<Engine> oversized;
    std::size_t max_entries, max_bytes;
    Stats stats;
};
ReferenceCache::ReferenceCache(std::size_t entries, std::size_t bytes) : impl_(std::make_unique<Impl>()) {
    impl_->max_entries = entries; impl_->max_bytes = bytes;
}
ReferenceCache::~ReferenceCache() = default;
void ReferenceCache::clear() { impl_->entries.clear(); impl_->oversized.reset(); impl_->stats.entries = impl_->stats.bytes = 0; }
ReferenceCache::Stats ReferenceCache::stats() const { return impl_->stats; }
Engine *ReferenceCache::get(const std::filesystem::path &directory, std::string &error) {
    auto &s = *impl_;
    error.clear();
    s.oversized.reset();
    try {
        const auto path = std::filesystem::weakly_canonical(directory);
        std::string signature;
        for (auto name : {"reference.json", "raw.png", "mask.png"}) {
            const auto file = path / name;
            signature += std::to_string(std::filesystem::file_size(file)) + ":" +
                         std::to_string(std::filesystem::last_write_time(file).time_since_epoch().count()) + ";";
        }
        for (auto it = s.entries.begin(); it != s.entries.end(); ++it) {
            if (it->path != path) continue;
            if (it->signature == signature) {
                ++s.stats.hits;
                s.entries.splice(s.entries.begin(), s.entries, it);
                return s.entries.front().engine.get();
            }
            s.stats.bytes -= it->bytes;
            s.entries.erase(it); --s.stats.entries; break;
        }
        ++s.stats.misses;
        auto engine = std::make_unique<Engine>();
        if (!engine->load_reference(path, error)) return nullptr;
        const auto bytes = engine->memory_bytes();
        if (!s.max_entries || bytes > s.max_bytes) {
            s.oversized = std::move(engine); return s.oversized.get();
        }
        while (!s.entries.empty() && (s.entries.size() >= s.max_entries || s.stats.bytes + bytes > s.max_bytes)) {
            s.stats.bytes -= s.entries.back().bytes; s.entries.pop_back(); ++s.stats.evictions;
        }
        s.stats.bytes += bytes;
        s.entries.push_front({path, signature, std::move(engine), bytes});
        s.stats.entries = s.entries.size();
        return s.entries.front().engine.get();
    } catch (const std::exception &e) { error = e.what(); return nullptr; }
}
} // namespace lineup
