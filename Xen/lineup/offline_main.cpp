#include "lineup/lineup.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <vector>
namespace {
using Json = nlohmann::json;
void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
std::string utf8(const std::filesystem::path &path) {
    const auto value = path.u8string();
    return std::string(value.begin(), value.end());
}
cv::Mat read_image(const std::filesystem::path &path) {
    const auto size = std::filesystem::file_size(path);
    require(size > 0 && size <= 256 * 1024 * 1024, "invalid_image_file_size");
    std::vector<unsigned char> encoded(static_cast<std::size_t>(size));
    std::ifstream input(path, std::ios::binary);
    input.read(reinterpret_cast<char *>(encoded.data()),
               static_cast<std::streamsize>(encoded.size()));
    require(input.good(), "image_read_failed");
    return cv::imdecode(encoded, cv::IMREAD_COLOR);
}
void write_image(const std::filesystem::path &path, const cv::Mat &image) {
    std::vector<unsigned char> encoded;
    require(cv::imencode(".png", image, encoded), "image_encode_failed");
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char *>(encoded.data()),
                 static_cast<std::streamsize>(encoded.size()));
    stream.flush();
    require(stream.good(), "image_write_failed");
    stream.close();
    require(!stream.fail(), "image_close_failed");
}
double percentile(std::vector<double> values, double p) {
    std::sort(values.begin(), values.end());
    return values[static_cast<std::size_t>(std::ceil((values.size() - 1) * p))];
}
void write_json(const std::filesystem::path &path, const Json &value) {
    std::ofstream output(path, std::ios::binary);
    output << value.dump(2);
    output.flush();
    require(output.good(), "output_write_failed");
    output.close();
    require(!output.fail(), "output_close_failed");
}
// 新帧几何只读取该帧的显式声明，不由参考图补造。
std::string read_roi_geometry(const Json &entry, CapturedFrame &frame) {
    if (!entry.is_object() || !entry.contains("geometry"))
        return "offline_roi_geometry_unknown";
    if (!entry.contains("source_mapping_verified") ||
        !entry.at("source_mapping_verified").is_boolean())
        return "offline_roi_mapping_declaration_unknown";
    try {
        const auto &g = entry.at("geometry");
        for (const auto *key : {"width", "height", "source_width", "source_height",
                                "encoded_width", "encoded_height"})
            if (!g.at(key).is_number_integer())
                return "offline_roi_geometry_invalid";
        frame.width = g.at("width").get<int>();
        frame.height = g.at("height").get<int>();
        frame.source_width = g.at("source_width").get<int>();
        frame.source_height = g.at("source_height").get<int>();
        frame.encoded_width = g.at("encoded_width").get<int>();
        frame.encoded_height = g.at("encoded_height").get<int>();
        frame.roi_x = g.at("roi_x").get<double>();
        frame.roi_y = g.at("roi_y").get<double>();
        frame.source_pixels_per_pixel_x = g.at("scale_x").get<double>();
        frame.source_pixels_per_pixel_y = g.at("scale_y").get<double>();
        frame.source_mapping_verified = entry.at("source_mapping_verified").get<bool>();
        return {};
    } catch (const Json::exception &) {
        return "offline_roi_geometry_invalid";
    }
}
} // namespace
#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
#else
int main(int argc, char **argv) {
#endif
    try {
        std::filesystem::path reference, frames, output;
        for (int i = 1; i < argc; ++i) {
            const std::string key = std::filesystem::path(argv[i]).string();
            if (key == "--help") {
                std::cout << "--reference <reference-dir> --frames <image-dir> --output "
                             "<new-dir>\nOptional frames/manifest.json: "
                             "{\"dataset_kind\":\"synthetic|real_holdout|unspecified\",\"frames\":{"
                             "\"image.png\":{\"aim\":[x,y],\"negative\":false}}}\nCoordinates are "
                             "original image pixels. Clock age is offline processing only.\n";
                std::cout << "ROI schema-2 references require each frame entry to provide geometry "
                             "{width,height,source_width,source_height,encoded_width,encoded_height,"
                             "roi_x,roi_y,scale_x,scale_y} and boolean source_mapping_verified. "
                             "ROI aim/truth coordinates are local image pixels.\n";
                return 0;
            }
            require(i + 1 < argc, "missing_argument");
            if (key == "--reference")
                reference = argv[++i];
            else if (key == "--frames")
                frames = argv[++i];
            else if (key == "--output")
                output = argv[++i];
            else
                throw std::runtime_error("unknown_argument");
        }
        require(!reference.empty() && !frames.empty() && !output.empty(),
                "reference_frames_output_required");
        require(!std::filesystem::exists(output), "output_already_exists");
        lineup::Engine engine;
        std::string error;
        if (!engine.load_reference(reference, error))
            throw std::runtime_error(error);
        const auto info = *engine.reference_info();
        Json reference_metadata;
        {
            std::ifstream input(reference / "reference.json");
            input >> reference_metadata;
        }
        const auto mode = reference_metadata.at("schema_version") == 2
                              ? lineup::ReferenceMode::ROI : lineup::ReferenceMode::FULL_FRAME;
        const bool roi = mode == lineup::ReferenceMode::ROI;
        Json manifest = {{"dataset_kind", "unspecified"}, {"frames", Json::object()}};
        if (std::filesystem::exists(frames / "manifest.json")) {
            std::ifstream input(frames / "manifest.json");
            input >> manifest;
            require(manifest.at("frames").is_object(), "invalid_manifest");
        }
        std::vector<std::filesystem::path> paths;
        for (const auto &entry : std::filesystem::directory_iterator(frames))
            if (entry.is_regular_file()) {
                auto ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp")
                    paths.push_back(entry.path());
            }
        std::sort(paths.begin(), paths.end());
        require(!paths.empty(), "no_input_images");
        std::filesystem::create_directories(output / "previews");
        if (!engine.save_reference(output / "reference", error))
            throw std::runtime_error(error);
        std::ofstream csv(output / "observations.csv", std::ios::binary);
        csv << "sequence,status,x,y,inliers,inlier_ratio,coverage,residual_px,processing_ms,"
               "independent_error_px\n";
        Json observations = Json::array();
        std::vector<double> errors, durations;
        int valid_count = 0, negative_count = 0, false_positive_count = 0, positive_count = 0,
            positive_valid = 0;
        std::uint64_t sequence = 0;
        for (const auto &path : paths) {
            const auto name = utf8(path.filename());
            const Json truth =
                manifest["frames"].contains(name) ? manifest["frames"][name] : Json::object();
            require(truth.is_object(), "invalid_frame_manifest_entry");
            CapturedFrame frame;
            frame.bgr = read_image(path);
            frame.width = frame.encoded_width = frame.bgr.cols;
            frame.height = frame.encoded_height = frame.bgr.rows;
            std::string geometry_error;
            if (roi) {
                geometry_error = read_roi_geometry(truth, frame);
            } else {
                // 保持旧全帧回放兼容，下方输出明确标识为旧参考推定。
                frame.source_width = info.source_size.width;
                frame.source_height = info.source_size.height;
                frame.source_pixels_per_pixel_x =
                    double(info.source_size.width) / info.image_size.width;
                frame.source_pixels_per_pixel_y =
                    double(info.source_size.height) / info.image_size.height;
            }
            frame.timing.sequence = ++sequence;
            frame.timing.captured_at = lineup::Clock::now();
            lineup::Observation observation;
            if (geometry_error.empty()) {
                observation = engine.locate(frame, "offline-replay", frame.timing.captured_at, mode);
                durations.push_back(observation.processing_ms);
            } else {
                observation.status = lineup::Status::INVALID_FRAME;
                observation.reason = geometry_error;
                observation.frame = {"offline-replay", sequence};
                observation.reference_id = info.id;
            }
            Json row = {{"file", utf8(path.filename())},
                        {"session_id", observation.frame.session_id},
                        {"sequence", sequence},
                        {"reference_id", observation.reference_id},
                        {"status", lineup::status_name(observation.status)},
                        {"reason", observation.reason},
                        {"inliers", observation.inliers},
                        {"matches", observation.matches},
                        {"inlier_ratio", observation.inlier_ratio},
                        {"coverage", observation.coverage},
                        {"residual_px", observation.residual_px},
                        {"local_correlation", observation.local_correlation},
                        {"processing_ms", observation.processing_ms},
                        {"processing_measured", geometry_error.empty()},
                        {"frame_mode", roi ? "roi" : "full_frame"},
                        {"coordinate_space", "local_image_pixels"},
                        {"geometry_provenance", roi ? "per_frame_manifest" : "legacy_reference_inference"},
                        {"input_geometry", roi && truth.contains("geometry") ? truth.at("geometry") : Json(nullptr)},
                        {"source_mapping_verified", roi && geometry_error.empty() && frame.source_mapping_verified},
                        {"source_aim", nullptr},
                        {"source_age_ms", nullptr},
                        {"independent_error_px", nullptr},
                        {"aim", nullptr}};
            if (!geometry_error.empty())
                row["processing_ms"] = nullptr;
            const bool negative = truth.value("negative", false);
            if (negative)
                ++negative_count;
            if (truth.contains("aim") && !negative)
                ++positive_count;
            if (observation.status == lineup::Status::VALID) {
                ++valid_count;
                if (negative)
                    ++false_positive_count;
                row["aim"] = {observation.aim->x, observation.aim->y};
                if (roi && frame.source_mapping_verified && observation.source_aim)
                    row["source_aim"] = {observation.source_aim->x, observation.source_aim->y};
                if (truth.contains("aim") && !negative) {
                    const auto &aim = truth.at("aim");
                    require(aim.is_array() && aim.size() == 2, "invalid_ground_truth");
                    const double x = aim[0].get<double>(), y = aim[1].get<double>();
                    require(std::isfinite(x) && std::isfinite(y), "nonfinite_ground_truth");
                    const double distance = cv::norm(*observation.aim - cv::Point2d(x, y));
                    errors.push_back(distance);
                    row["independent_error_px"] = distance;
                    ++positive_valid;
                }
            }
            if (!frame.bgr.empty()) {
                cv::Mat preview = frame.bgr.clone();
                if (observation.aim)
                    cv::drawMarker(preview, *observation.aim, cv::Scalar(0, 0, 255),
                                   cv::MARKER_CROSS, 21, 2);
                write_image(output / "previews" / (std::to_string(sequence) + ".png"), preview);
            }
            csv << sequence << ',' << lineup::status_name(observation.status) << ',';
            if (observation.aim)
                csv << observation.aim->x << ',' << observation.aim->y;
            else
                csv << ',';
            csv << ',' << observation.inliers << ',' << observation.inlier_ratio << ','
                << observation.coverage << ',' << observation.residual_px << ',';
            if (geometry_error.empty())
                csv << observation.processing_ms;
            csv << ',';
            if (!row["independent_error_px"].is_null())
                csv << row["independent_error_px"].get<double>();
            csv << '\n';
            observations.push_back(std::move(row));
        }
        csv.flush();
        require(csv.good(), "csv_write_failed");
        csv.close();
        require(!csv.fail(), "csv_close_failed");
        write_json(output / "observations.json", observations);
        Json summary = {
            {"dataset_kind", manifest.value("dataset_kind", "unspecified")},
            {"frame_mode", roi ? "roi" : "full_frame"},
            {"coordinate_space", "local_image_pixels"},
            {"geometry_provenance", roi ? "per_frame_manifest" : "legacy_reference_inference"},
            {"frames", paths.size()},
            {"valid", valid_count},
            {"labelled_positive_frames", positive_count},
            {"labelled_positive_valid", positive_valid},
            {"negative_frames", negative_count},
            {"false_positive_frames", false_positive_count},
            {"independent_error_samples", errors.size()},
            {"error_p95_px", errors.empty() ? Json(nullptr) : Json(percentile(errors, .95))},
            {"error_max_px", errors.empty()
                                 ? Json(nullptr)
                                 : Json(*std::max_element(errors.begin(), errors.end()))},
            {"processing_p50_ms", durations.empty() ? Json(nullptr) : Json(percentile(durations, .5))},
            {"processing_p95_ms", durations.empty() ? Json(nullptr) : Json(percentile(durations, .95))},
            {"processing_p99_ms", durations.empty() ? Json(nullptr) : Json(percentile(durations, .99))},
            {"source_age_known", false},
            {"physical_throw_validation", "not_run"},
            {"accuracy_claim", "Only supplied independent labels are evaluated; algorithm residual "
                               "is not ground truth."}};
        write_json(output / "summary.json", summary);
        std::cout << summary.dump(2) << '\n';
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
