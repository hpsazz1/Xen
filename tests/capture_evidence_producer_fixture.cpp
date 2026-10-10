#include "capture_evidence/capture_evidence.h"
#include <nlohmann/json.hpp>
#include <opencv2/imgcodecs.hpp>
#include <fstream>
#include <iostream>

// 只读取测试生成的像素与时序，直接调用真实Recorder；不创建ICapture或任何输入设备。
int wmain(int argc, wchar_t** argv) {
    if (argc != 4) return 2;
    try {
        const std::filesystem::path input_path(argv[1]);
        std::ifstream input(input_path);
        const auto source = nlohmann::json::parse(input);
        const auto& geometry = source.at("capture_config");
        capture_evidence::CaptureEvidenceConfig config;
        config.source_binding_path = argv[2];
        config.output_directory = argv[3];
        config.capture.backend = CaptureBackend::NDI;
        config.capture.ndi_source_name = source.at("capture_source_name").get<std::string>();
        config.capture.ndi_frame_layout = NetworkFrameLayout::CENTER_CROP_1_TO_1;
        config.capture.ndi_source_width = geometry.at("source_width");
        config.capture.ndi_source_height = geometry.at("source_height");
        config.capture.roi_width = geometry.at("roi_width");
        config.capture.roi_height = geometry.at("roi_height");
        config.capture.center_roi = geometry.at("center_roi");
        config.capture.roi_x = geometry.at("roi_x");
        config.capture.roi_y = geometry.at("roi_y");
        config.require_source_timing = true;
        config.requested_frame_count = source.at("frames").size();
        capture_evidence::CaptureEvidenceRecorder recorder;
        std::string error;
        if (!recorder.start(config, error)) throw std::runtime_error(error);
        for (const auto& sample : source.at("frames")) {
            CapturedFrame frame;
            const auto image = input_path.parent_path() / sample.at("file").get<std::string>();
            frame.bgr = cv::imread(image.string());
            frame.width = frame.bgr.cols; frame.height = frame.bgr.rows;
            frame.source_width = config.capture.ndi_source_width;
            frame.source_height = config.capture.ndi_source_height;
            frame.encoded_width = frame.width; frame.encoded_height = frame.height;
            frame.roi_x = config.capture.roi_x; frame.roi_y = config.capture.roi_y;
            frame.source_pixels_per_pixel_x = frame.source_pixels_per_pixel_y = 1;
            auto& timing = frame.timing;
            timing.sequence = sample.at("index").get<std::uint64_t>() + 1;
            timing.source_sequence = timing.sequence;
            timing.source_sequence_valid = true;
            timing.source_timestamp = sample.at("source_timestamp");
            timing.source_timestamp_valid = true;
            timing.source_time_basis = SourceTimeBasis::NDI_SDK_SUBMISSION;
            timing.source_clock_status = SourceClockStatus::VALID;
            timing.source_time_timing_valid = true;
            timing.source_time_at = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(timing.source_timestamp * 100));
            timing.captured_at = timing.source_time_at + std::chrono::milliseconds(1);
            timing.source_clock_session_id = sample.at("source_clock_session_id");
            timing.source_clock_uncertainty_ms = .01; timing.source_clock_round_trip_ms = .004;
            timing.source_clock_rate = 1; timing.source_clock_mapping_age_ms = 1;
            timing.source_clock_sample_count = 8;
            if (!recorder.record(frame, error)) throw std::runtime_error(error);
        }
        if (!recorder.finish(error)) throw std::runtime_error(error);
        std::cout << "real Recorder published " << recorder.recorded_frame_count() << " synthetic frames; no capture/output\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
