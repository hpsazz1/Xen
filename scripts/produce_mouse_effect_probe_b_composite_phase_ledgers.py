#!/usr/bin/env python3
"""从 sealed Physical B 原始证据生成 binder 所需 capture/command ledgers。"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import pathlib
import subprocess
import sys
from typing import Any

import cv2
import numpy as np


Q32 = 1 << 32
MIN_SOURCE_CLOCK_RATE = 0.99
MAX_SOURCE_CLOCK_RATE = 1.01


class ProducerError(ValueError):
    pass


def canonical_sha256(value: Any) -> str:
    return hashlib.sha256(json.dumps(
        value, ensure_ascii=False, sort_keys=True, separators=(",", ":"),
        allow_nan=False).encode("utf-8")).hexdigest()


def file_sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def bytes_sha256(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def is_sha256(value: Any) -> bool:
    return isinstance(value, str) and len(value) == 64 and all(
        character in "0123456789abcdef" for character in value)


def exact_int(value: Any, context: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool):
        raise ProducerError(f"{context} 必须是精确 JSON integer")
    return value


def finite_number(value: Any, context: str) -> float:
    if (not isinstance(value, (int, float)) or isinstance(value, bool)):
        raise ProducerError(f"{context} 必须是有限 JSON number")
    result = float(value)
    if not math.isfinite(result):
        raise ProducerError(f"{context} 必须是有限 JSON number")
    return result


def read_object(path: pathlib.Path, label: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ProducerError(f"{label} 无法读取: {error}") from error
    if not isinstance(value, dict):
        raise ProducerError(f"{label} 必须是 JSON object")
    return value


def verify_semantic(value: dict[str, Any], field: str,
                    label: str) -> str:
    claimed = value.get(field)
    semantic_input = dict(value)
    semantic_input.pop(field, None)
    if not is_sha256(claimed) or claimed != canonical_sha256(semantic_input):
        raise ProducerError(f"{label} semantic SHA-256 漂移")
    return claimed


def seal_semantic(value: dict[str, Any], field: str) -> None:
    value.pop(field, None)
    value[field] = canonical_sha256(value)


def stable_event_id(kind: str, *parts: Any) -> str:
    return f"{kind}-{canonical_sha256([kind, *parts])[:32]}"


def interval(center: int, uncertainty: int) -> dict[str, int]:
    if center <= 0 or uncertainty < 0 or center < uncertainty:
        raise ProducerError("时间区间中心/不确定度无效")
    return {
        "lower_closed": center - uncertainty,
        "upper_closed": center + uncertainty,
    }


def parse_roi(value: Any, label: str, width: int,
              height: int) -> tuple[int, int, int, int]:
    if (not isinstance(value, list) or len(value) != 4 or
            any(not isinstance(item, int) or isinstance(item, bool)
                for item in value)):
        raise ProducerError(f"{label} 必须是四个整数")
    x, y, roi_width, roi_height = value
    if (x < 0 or y < 0 or roi_width <= 1 or roi_height <= 1 or
            x + roi_width > width or y + roi_height > height):
        raise ProducerError(f"{label} 超出 sidecar 图像")
    return x, y, roi_width, roi_height


def load_bgr(frame_root: pathlib.Path, frame: dict[str, Any]) -> np.ndarray:
    relative = frame.get("file")
    if not isinstance(relative, str) or not relative:
        raise ProducerError("sidecar frame 缺少 PNG 路径")
    path = (frame_root / relative).resolve()
    root = frame_root.resolve()
    if root not in path.parents or not path.is_file():
        raise ProducerError("sidecar PNG 越界或缺失")
    if (not is_sha256(frame.get("png_sha256")) or
            file_sha256(path) != frame["png_sha256"]):
        raise ProducerError("sidecar PNG 文件 SHA-256 漂移")
    image = cv2.imread(str(path), cv2.IMREAD_COLOR)
    if (image is None or image.dtype != np.uint8 or image.ndim != 3 or
            image.shape[2] != 3):
        raise ProducerError("sidecar PNG 不是 CPU BGR8")
    image = np.ascontiguousarray(image)
    raw_hash = bytes_sha256(image.tobytes())
    if not is_sha256(frame.get("bgr_sha256")) or \
            raw_hash != frame["bgr_sha256"]:
        raise ProducerError("sidecar decoded BGR SHA-256 漂移")
    return image


def roi_bgr(image: np.ndarray,
            roi: tuple[int, int, int, int]) -> np.ndarray:
    x, y, width, height = roi
    return np.ascontiguousarray(image[y:y + height, x:x + width])


def displacement_interval(anchor: np.ndarray, current: np.ndarray,
                          command_sign: int) -> tuple[dict[str, int], float]:
    first = cv2.cvtColor(anchor, cv2.COLOR_BGR2GRAY).astype(np.float32)
    second = cv2.cvtColor(current, cv2.COLOR_BGR2GRAY).astype(np.float32)
    window = cv2.createHanningWindow(
        (first.shape[1], first.shape[0]), cv2.CV_32F)
    shift, response = cv2.phaseCorrelate(
        first.copy(), second.copy(), window.copy())
    normalized = float(shift[0]) * command_sign
    response_value = float(response)
    if not math.isfinite(normalized) or not math.isfinite(response_value):
        raise ProducerError("phaseCorrelate 返回非有限值")
    # phaseCorrelate 是确定性 subpixel estimator，并非经校准的计量器；这里
    # 固定向外扩一像素，只用于 global settled conjunction，不用于 onset。
    lower = math.floor((normalized - 1.0) * Q32)
    upper = math.ceil((normalized + 1.0) * Q32)
    return {"lower_closed": lower, "upper_closed": upper}, response_value


def write_pair_atomic(capture_path: pathlib.Path,
                      command_path: pathlib.Path,
                      capture: dict[str, Any],
                      commands: dict[str, Any]) -> None:
    for path in (capture_path, command_path):
        if not path.is_absolute() or path.exists():
            raise ProducerError("ledger 输出必须是尚不存在的绝对路径")
        path.parent.mkdir(parents=True, exist_ok=True)
    capture_pending = capture_path.with_name(
        f".{capture_path.name}.pending-{os.getpid()}")
    command_pending = command_path.with_name(
        f".{command_path.name}.pending-{os.getpid()}")
    if capture_pending.exists() or command_pending.exists():
        raise ProducerError("ledger pending 输出已存在")
    published_capture = False
    try:
        for path, value in ((capture_pending, capture),
                            (command_pending, commands)):
            with path.open("x", encoding="utf-8", newline="\n") as stream:
                json.dump(value, stream, ensure_ascii=False, indent=2,
                          allow_nan=False)
                stream.write("\n")
                stream.flush()
                os.fsync(stream.fileno())
        capture_pending.replace(capture_path)
        published_capture = True
        command_pending.replace(command_path)
    except Exception:
        if published_capture and capture_path.exists() and \
                not command_path.exists():
            capture_path.unlink()
        raise
    finally:
        for path in (capture_pending, command_pending):
            if path.exists():
                path.unlink()


def produce(options: argparse.Namespace) -> tuple[dict[str, Any],
                                                  dict[str, Any]]:
    plan_path = options.plan.resolve()
    report_path = options.report.resolve()
    schedule_path = options.schedule.resolve()
    safety_path = options.safety.resolve()
    manifest_path = options.manifest.resolve()
    assessment_path = options.human_assessment.resolve()
    plan = read_object(plan_path, "plan")
    report = read_object(report_path, "command report")
    schedule = read_object(schedule_path, "schedule ledger")
    safety = read_object(safety_path, "safety ledger")
    manifest = read_object(manifest_path, "sidecar manifest")
    assessment = read_object(assessment_path, "human assessment")

    plan_semantic = verify_semantic(
        plan, "plan_semantic_sha256", "plan")
    report_semantic = report.get("report_sha256")
    if not is_sha256(report_semantic):
        raise ProducerError("command report semantic SHA-256 缺失")
    schedule_semantic = verify_semantic(
        schedule, "ledger_semantic_sha256", "schedule ledger")
    assessment_semantic = verify_semantic(
        assessment, "assessment_semantic_sha256", "human assessment")
    verifier_path = options.report_verifier.resolve()
    producer_path = pathlib.Path(__file__).resolve()
    seal = plan.get("seal")
    if (not verifier_path.is_file() or not isinstance(seal, dict) or
            seal.get("producer_file_sha256") != file_sha256(producer_path) or
            seal.get("report_verifier_file_sha256") !=
            file_sha256(verifier_path)):
        raise ProducerError("producer/report verifier 未由冻结 plan 绑定")
    verified_report = subprocess.run(
        [str(verifier_path), "--verify-report", str(report_path)],
        check=False, capture_output=True, text=True, encoding="utf-8")
    if verified_report.returncode != 0:
        detail = (verified_report.stderr or verified_report.stdout).strip()
        raise ProducerError(
            f"command report 原生 semantic 校验失败: {detail}")
    run_uuid = plan.get("run_uuid")
    activation_epoch = plan.get("activation_epoch")
    scope_id = plan.get("scope_id")
    sequence_semantic = plan.get("sequence_binding", {}).get(
        "sequence_semantic_sha256")
    report_binding = report.get("binding")
    executor_timebase = report.get("executor_timebase")
    policy = plan.get("capture_policy")
    if (not isinstance(report_binding, dict) or
            not isinstance(executor_timebase, dict) or
            not isinstance(policy, dict)):
        raise ProducerError("plan/report capture binding 缺失")
    if (plan.get("status") != "FROZEN_BEFORE_CAPTURE" or
            report.get("schema") != 1 or
            report.get("evidence_type") !=
                "backend_completed_command_to_visible_background_response" or
            report.get("profile") !=
            "physical_b_composite_phase_calibration" or
            report.get("dispatch_mode") != "physical_b" or
            report.get("run_uuid") != run_uuid or
            report.get("activation_epoch") != activation_epoch or
            report.get("sequence_sha256") != sequence_semantic or
            executor_timebase != {
                "name": "steady_clock_nanoseconds_since_epoch",
                "ticks_per_second": 1_000_000_000,
            } or
            report_binding.get("sidecar_run_uuid") != run_uuid or
            report_binding.get("capture_source_name") !=
                policy.get("source_name") or
            not is_sha256(report_binding.get("probe_binding_sha256")) or
            schedule.get("schema_version") != 1 or
            schedule.get("evidence_type") !=
                "mouse_effect_probe_b_composite_phase_raw_schedule_ledger" or
            schedule.get("status") != "ACQUISITION_COMPLETE" or
            schedule.get("ledger_physical_output_capability") is not False or
            schedule.get("ledger_physical_dispatch_count") != 0 or
            schedule.get("run_uuid") != run_uuid or
            schedule.get("activation_epoch") != activation_epoch or
            schedule.get("sequence_semantic_sha256") != sequence_semantic or
            schedule.get("probe_binding_sha256") !=
                report_binding.get("probe_binding_sha256") or
            schedule.get("timer_mode") !=
                plan.get("scheduler_policy", {}).get("timer_mode") or
            schedule.get("source_dispatch_count") != 38 or
            schedule.get("composite_plan_file_sha256") !=
                file_sha256(plan_path) or
            schedule.get("command_report_file_sha256") !=
                file_sha256(report_path) or
            schedule.get("safety_ledger_file_sha256") !=
                file_sha256(safety_path)):
        raise ProducerError("plan/report/schedule/safety identity 漂移")
    result = report.get("result")
    schedule_events = schedule.get("events")
    windows = plan.get("window_order")
    if (not isinstance(result, dict) or
            result.get("state") != "completed" or
            result.get("stop_reason") != "normal_completion" or
            result.get("complete") is not True or
            result.get("consumed_sample_count") != 295 or
            not isinstance(result.get("events"), list) or
            len(result["events"]) != 295 or
            not isinstance(schedule_events, list) or
            len(schedule_events) != 42 or
            not isinstance(windows, list) or len(windows) != 42 or
            [event.get("window_id") for event in schedule_events] != windows or
            any(event.get("status") != "PHASE_CONFIRMED"
                for event in schedule_events)):
        raise ProducerError("Run 未形成完整 295/42 acquisition")
    if (safety.get("schema_version") != 2 or
            safety.get("evidence_type") !=
                "mouse_effect_probe_safety_monitor_ledger" or
            safety.get("physical_output_capability") is not False or
            safety.get("run_uuid") != run_uuid or
            safety.get("probe_stop_reason") != "normal_completion" or
            safety.get("recording_failed") is not False or
            safety.get("monitor_packet_recording_failed") is not False or
            safety.get("dropped_observation_count") != 0 or
            safety.get("dropped_monitor_packet_count") != 0):
        raise ProducerError("safety ledger 未形成无丢失正常终局")
    if (assessment.get("schema_version") != 1 or
            assessment.get("evidence_type") !=
            "mouse_effect_probe_b_composite_phase_human_assessment" or
            assessment.get("status") != "CONFIRMED" or
            assessment.get("run_uuid") != run_uuid or
            assessment.get("activation_epoch") != activation_epoch or
            assessment.get("scope_id") != scope_id or
            assessment.get("manual_mouse_or_wasd") is not False or
            assessment.get("scene_cut_or_occlusion") is not False or
            assessment.get("abnormal_or_emergency_stop") is not False or
            not is_sha256(assessment.get("observation_file_sha256"))):
        raise ProducerError("human assessment 未确认无人工移动/scene/异常")

    report_events = result["events"]
    previous_source_sequence: int | None = None
    previous_source_timestamp: int | None = None
    previous_source_time_at_steady_ns: int | None = None
    for index, event in enumerate(report_events):
        source_sequence = exact_int(
            event.get("source_frame_sequence"),
            "command report source frame sequence")
        source_timestamp = exact_int(
            event.get("source_timestamp"),
            "command report source timestamp")
        source_time_at_steady_ns = exact_int(
            event.get("source_time_at_steady_ns"),
            "command report mapped source time")
        source_session = event.get("source_clock_session_id")
        uncertainty = finite_number(
            event.get("source_clock_uncertainty_ms"),
            "command report source clock uncertainty")
        round_trip = finite_number(
            event.get("source_clock_rtt_ms"),
            "command report source clock RTT")
        mapping_age = finite_number(
            event.get("source_clock_mapping_age_ms"),
            "command report source clock mapping age")
        rate = finite_number(
            event.get("source_clock_rate"),
            "command report source clock rate")
        sample_count = exact_int(
            event.get("source_clock_sample_count"),
            "command report source clock sample count")
        if (event.get("sample_index") != index or
                event.get("run_uuid") != run_uuid or
                event.get("activation_epoch") != activation_epoch or
                event.get("sequence_sha256") != sequence_semantic or
                event.get("source_timestamp_valid") is not True or
                source_sequence <= 0 or source_timestamp <= 0 or
                source_time_at_steady_ns <= 0 or
                not isinstance(source_session, str) or not source_session or
                event.get("source_time_basis") != "NDI_SDK_SUBMISSION" or
                event.get("source_clock_status") != "VALID" or
                uncertainty < 0 or round_trip < 0 or mapping_age < 0 or
                rate < MIN_SOURCE_CLOCK_RATE or
                rate > MAX_SOURCE_CLOCK_RATE or sample_count <= 0 or
                event.get("source_dropped_frames") != 0 or
                event.get("transport_dropped_frames") != 0 or
                event.get("transport_invalid_packets") != 0 or
                (previous_source_sequence is not None and
                 source_sequence != previous_source_sequence + 1) or
                (previous_source_timestamp is not None and
                 source_timestamp <= previous_source_timestamp) or
                (previous_source_time_at_steady_ns is not None and
                 source_time_at_steady_ns <=
                    previous_source_time_at_steady_ns)):
            raise ProducerError("command report source event identity 无效")
        previous_source_sequence = source_sequence
        previous_source_timestamp = source_timestamp
        previous_source_time_at_steady_ns = source_time_at_steady_ns

    manifest_frames = manifest.get("frames")
    capture_config = manifest.get("capture_config")
    roi_geometry = policy.get("roi_geometry")
    source_geometry = policy.get("source_geometry")
    if (not isinstance(roi_geometry, list) or len(roi_geometry) != 4 or
            not isinstance(source_geometry, list) or
            len(source_geometry) != 2):
        raise ProducerError("capture policy geometry 无效")
    expected_roi_x, expected_roi_y, image_width, image_height = [
        exact_int(value, "capture ROI geometry") for value in roi_geometry]
    source_width, source_height = [
        exact_int(value, "capture source geometry")
        for value in source_geometry]
    if (manifest.get("schema_version") != 1 or
            manifest.get("evidence_type") != "output_off_capture" or
            manifest.get("physical_output_capability") is not False or
            manifest.get("capture_backend") != "NDI" or
            not isinstance(manifest_frames, list) or
            not isinstance(capture_config, dict) or
            manifest.get("capture_source_name") !=
                report_binding.get("capture_source_name") or
            manifest.get("requested_frame_count") != len(manifest_frames) or
            manifest.get("recorded_frame_count") != len(manifest_frames) or
            len(manifest_frames) < 252 or
            capture_config.get("source_width") != source_width or
            capture_config.get("source_height") != source_height or
            capture_config.get("roi_width") != image_width or
            capture_config.get("roi_height") != image_height):
        raise ProducerError("sidecar manifest source/header 无效")
    center_roi = capture_config.get("center_roi")
    configured_roi_x = capture_config.get("roi_x")
    configured_roi_y = capture_config.get("roi_y")
    if center_roi is True:
        configured_roi_x = (source_width - image_width) // 2
        configured_roi_y = (source_height - image_height) // 2
    if (center_roi not in (True, False) or
            configured_roi_x != expected_roi_x or
            configured_roi_y != expected_roi_y):
        raise ProducerError("sidecar manifest ROI 与冻结 capture policy 漂移")
    by_source: dict[tuple[int, str], list[dict[str, Any]]] = {}
    for frame in manifest_frames:
        if not isinstance(frame, dict):
            raise ProducerError("sidecar frame 必须是 object")
        source_session = frame.get("source_clock_session_id")
        if (frame.get("source_timestamp_valid") is not True or
                not isinstance(source_session, (str, int)) or
                isinstance(source_session, bool) or
                not str(source_session)):
            raise ProducerError("sidecar source timestamp/session 无效")
        key = (exact_int(frame.get("source_timestamp"),
                         "sidecar source timestamp"),
               str(source_session))
        by_source.setdefault(key, []).append(frame)

    policy_semantic = verify_semantic(
        policy, "semantic_sha256", "capture policy")
    left_roi = parse_roi(policy.get("left_witness_roi"), "left witness",
                         image_width, image_height)
    right_roi = parse_roi(policy.get("right_witness_roi"), "right witness",
                          image_width, image_height)
    if (max(left_roi[0], right_roi[0]) <
            min(left_roi[0] + left_roi[2], right_roi[0] + right_roi[2]) and
            max(left_roi[1], right_roi[1]) <
            min(left_roi[1] + left_roi[3], right_roi[1] + right_roi[3])):
        raise ProducerError("左右 witness ROI 重叠")

    scheduler_clock = schedule.get("scheduler_clock")
    if (not isinstance(scheduler_clock, dict) or
            scheduler_clock.get("clock_kind") != "WINDOWS_QPC" or
            scheduler_clock.get("clock_session_id") != f"{run_uuid}-qpc" or
            not isinstance(scheduler_clock.get("producer_process_id"), int) or
            isinstance(scheduler_clock.get("producer_process_id"), bool) or
            scheduler_clock.get("producer_process_id", 0) <= 0):
        raise ProducerError("schedule QPC session 缺失")
    qpc_frequency = exact_int(
        scheduler_clock.get("frequency_hz"), "QPC frequency")
    if qpc_frequency <= 0:
        raise ProducerError("QPC frequency 无效")
    qpc_quantization_ns = (1_000_000_000 + qpc_frequency - 1) // \
        qpc_frequency
    accepted = exact_int(
        schedule.get("plan_accepted_at_qpc"), "plan accepted QPC")
    acquired = exact_int(schedule.get("acquisition_started_at_qpc"),
                         "acquisition start QPC")
    finished = exact_int(schedule.get("acquisition_finished_at_qpc"),
                         "acquisition finish QPC")
    revealed = exact_int(schedule.get("revealed_at_qpc"), "reveal QPC")
    if not 0 < accepted < acquired < finished or revealed != finished:
        raise ProducerError("plan/acquisition/reveal QPC 顺序无效")

    pulses = plan.get("pulses")
    controls = plan.get("negative_controls")
    if not isinstance(pulses, list) or not isinstance(controls, list):
        raise ProducerError("plan pulse/control 缺失")
    pulse_by_id = {pulse["pulse_id"]: pulse for pulse in pulses}
    control_by_id = {control["window_id"]: control for control in controls}
    frame_records: list[dict[str, Any]] = []
    command_events: list[dict[str, Any]] = []
    command_controls: list[dict[str, Any]] = []
    source_sessions: set[str] = set()
    mapping_ages: list[int] = []
    round_trips: list[int] = []
    uncertainties: list[int] = []
    rates: list[float] = []
    all_time_intervals: list[dict[str, int]] = []
    marker_widths: list[int] = []
    decoded_cache: dict[tuple[int, str], np.ndarray] = {}
    completion_session = f"{run_uuid}-steady-clock-ns"

    for schedule_event, window_id in zip(schedule_events, windows):
        ordinal = exact_int(schedule_event.get("window_ordinal"),
                            "window ordinal")
        predictor_index = 1 + ordinal * 7
        if (schedule_event.get("predictor_sample_index") != predictor_index or
                schedule_event.get("report_event_index") != predictor_index):
            raise ProducerError("schedule/report predictor identity 漂移")
        response_events = report_events[predictor_index + 1:
                                        predictor_index + 7]
        if len(response_events) != 6:
            raise ProducerError("response window 不完整")
        images: list[np.ndarray] = []
        selected_manifest: list[dict[str, Any]] = []
        for event in response_events:
            session = str(event.get("source_clock_session_id"))
            source_sessions.add(session)
            key = (exact_int(event.get("source_timestamp"),
                             "report source timestamp"), session)
            candidates = by_source.get(key, [])
            if len(candidates) != 1:
                raise ProducerError("report response 未唯一匹配 sidecar PNG")
            sidecar_frame = candidates[0]
            if (sidecar_frame.get("source_time_basis") !=
                    "NDI_SDK_SUBMISSION" or
                    sidecar_frame.get("source_clock_status") != "VALID" or
                    sidecar_frame.get("source_time_timing_valid") is not True or
                    sidecar_frame.get("source_dropped_frames") != 0 or
                    sidecar_frame.get("transport_dropped_frames") != 0 or
                    sidecar_frame.get("transport_invalid_packets") != 0 or
                    sidecar_frame.get("storage") != "CPU_BGR" or
                    sidecar_frame.get("width") != image_width or
                    sidecar_frame.get("height") != image_height or
                    sidecar_frame.get("source_width") != source_width or
                    sidecar_frame.get("source_height") != source_height or
                    sidecar_frame.get("roi_x") != expected_roi_x or
                    sidecar_frame.get("roi_y") != expected_roi_y):
                raise ProducerError("sidecar response source timing/drop 无效")
            if key not in decoded_cache:
                decoded_cache[key] = load_bgr(
                    manifest_path.parent, sidecar_frame)
            image = decoded_cache[key]
            if image.shape[:2] != (image_height, image_width):
                raise ProducerError("sidecar PNG geometry 与 capture policy 漂移")
            images.append(image)
            selected_manifest.append(sidecar_frame)

        planned = pulse_by_id.get(window_id)
        command_sign = (1 if planned is None else
                        int(math.copysign(1, planned["command_dx_counts"])))
        anchor_left = roi_bgr(images[0], left_roi)
        anchor_right = roi_bgr(images[0], right_roi)
        for sample_index, (event, sidecar_frame, image) in enumerate(zip(
                response_events, selected_manifest, images)):
            uncertainty = math.ceil(finite_number(
                event.get("source_clock_uncertainty_ms"),
                "source clock uncertainty") * 1_000_000)
            round_trip = math.ceil(finite_number(
                event.get("source_clock_rtt_ms"),
                "source clock RTT") * 1_000_000)
            mapping_age = math.ceil(finite_number(
                event.get("source_clock_mapping_age_ms"),
                "source clock mapping age") * 1_000_000)
            rate = finite_number(
                event.get("source_clock_rate"), "source clock rate")
            boundary = interval(exact_int(
                event.get("source_time_at_steady_ns"),
                "source boundary steady ns"), uncertainty)
            left = roi_bgr(image, left_roi)
            right = roi_bgr(image, right_roi)
            left_displacement, left_response = displacement_interval(
                anchor_left, left, command_sign)
            right_displacement, right_response = displacement_interval(
                anchor_right, right, command_sign)
            frame_record = {
                "frame_event_id": stable_event_id(
                    "frame", run_uuid, window_id, sample_index,
                    event["source_timestamp"],
                    sidecar_frame["png_sha256"]),
                "source_sequence": len(frame_records),
                "receiver_sequence": len(frame_records),
                "window_id": window_id,
                "sample_index": sample_index,
                "source_timestamp_100ns": exact_int(
                    event["source_timestamp"], "report source timestamp"),
                "submission_clock_session_id":
                    str(event["source_clock_session_id"]),
                "mapping_segment_id": "pending",
                "boundary_time_interval_ns": boundary,
                "full_bgr_sha256": bytes_sha256(image.tobytes()),
                "left_roi_bgr_sha256": bytes_sha256(left.tobytes()),
                "right_roi_bgr_sha256": bytes_sha256(right.tobytes()),
                "left_normalized_displacement_interval_q32":
                    left_displacement,
                "right_normalized_displacement_interval_q32":
                    right_displacement,
                "left_phase_correlation_response": left_response,
                "right_phase_correlation_response": right_response,
                "settled_displacement_uncertainty_policy":
                    "PHASE_CORRELATE_ESTIMATE_PLUS_MINUS_ONE_PIXEL",
                "scene_cut_or_occlusion_detected": False,
                "frame_sync_reused": False,
                "source_report_event_index": event["sample_index"],
                "sidecar_manifest_index": sidecar_frame["index"],
                "sidecar_png_sha256": sidecar_frame["png_sha256"],
            }
            frame_records.append(frame_record)
            all_time_intervals.append(boundary)
            uncertainties.append(uncertainty)
            round_trips.append(round_trip)
            mapping_ages.append(mapping_age)
            rates.append(rate)

        marker_widths.append(exact_int(
            schedule_event.get("marker_width_ns"), "marker width"))
        report_event = report_events[predictor_index]
        if planned is not None:
            if (report_event.get("nominal_dx_counts") !=
                    planned["command_dx_counts"] or
                    report_event.get("nominal_dy_counts") != 0 or
                    report_event.get("requested_dx_counts") !=
                    planned["command_dx_counts"] or
                    report_event.get("requested_dy_counts") != 0 or
                    report_event.get("dispatch_attempted") is not True or
                    report_event.get("backend_succeeded") is not True or
                    report_event.get("protocol_ack_received") is not True):
                raise ProducerError("pulse command/completion/ACK 与 plan 不符")
            completion_center = exact_int(
                report_event.get("backend_completed_at_steady_ns"),
                "backend completion steady ns")
            completion = interval(completion_center, qpc_quantization_ns)
            all_time_intervals.append(completion)
            command_events.append({
                "pulse_id": planned["pulse_id"],
                "sequence_index": planned["sequence_index"],
                "command_dx_counts": planned["command_dx_counts"],
                "command_dy_counts": 0,
                "backend_completion_event_id": stable_event_id(
                    "completion", run_uuid, planned["pulse_id"],
                    completion_center),
                "delivery_ack_id": stable_event_id(
                    "ack", run_uuid, planned["pulse_id"],
                    report_event["protocol_ack_received_at_steady_ns"]),
                "backend_completed": True,
                "delivery_acked": True,
                "completion_clock_session_id":
                    completion_session,
                "mapping_segment_id": "pending",
                "completion_time_interval_ns": completion,
                "source_report_event_index": predictor_index,
            })
        else:
            control = control_by_id.get(window_id)
            if (control is None or
                    report_event.get("nominal_dx_counts") != 0 or
                    report_event.get("nominal_dy_counts") != 0 or
                    report_event.get("dispatch_attempted") is not False or
                    report_event.get("backend_succeeded") is not False or
                    report_event.get("protocol_ack_received") is not False):
                raise ProducerError("no-command control 产生 Mouse/KMBOX event")
            marker_center = exact_int(
                schedule_event.get("marker_time_steady_ns"),
                "control marker steady ns")
            marker_uncertainty = qpc_quantization_ns + exact_int(
                schedule_event.get("marker_width_ns"), "control marker width")
            marker = interval(marker_center, marker_uncertainty)
            all_time_intervals.append(marker)
            command_controls.append({
                "control_id": control["control_id"],
                "command_dx_counts": 0,
                "command_dy_counts": 0,
                "mouse_or_kmbox_event_emitted": False,
                "schedule_marker_event_id": stable_event_id(
                    "schedule", run_uuid, control["control_id"],
                    marker_center),
                "completion_clock_session_id":
                    completion_session,
                "mapping_segment_id": "pending",
                "scheduled_marker_time_interval_ns": marker,
                "source_report_event_index": predictor_index,
            })

    if len(source_sessions) != 1 or len(frame_records) != 252 or \
            len(command_events) != 38 or len(command_controls) != 4:
        raise ProducerError("最终 frame/pulse/control/session 数量无效")
    submission_session = next(iter(source_sessions))
    mapping_segment = stable_event_id(
        "mapping", run_uuid, completion_session, submission_session)
    for record in frame_records:
        record["mapping_segment_id"] = mapping_segment
    for event in command_events:
        event["mapping_segment_id"] = mapping_segment
    for control in command_controls:
        control["mapping_segment_id"] = mapping_segment

    mapping: dict[str, Any] = {
        "policy_id": policy.get("clock_mapping_policy_id"),
        "completion_clock_session_id": completion_session,
        "submission_clock_session_id": submission_session,
        "mapping_segment_id": mapping_segment,
        "valid_from_common_ns": min(
            item["lower_closed"] for item in all_time_intervals),
        "valid_through_common_ns": max(
            item["upper_closed"] for item in all_time_intervals),
        "source_clock_uncertainty_ns": max(uncertainties),
        "qpc_quantization_ns": qpc_quantization_ns,
        "read_access_interval_ns": max(marker_widths),
        "source_clock_round_trip_max_ns": max(round_trips),
        "source_clock_mapping_age_max_ns": max(mapping_ages),
        "source_clock_sample_count_min": min(
            int(event["source_clock_sample_count"])
            for event in report_events),
        "source_clock_rate_interval_q32": {
            "lower_closed": math.floor(min(rates) * Q32),
            "upper_closed": math.ceil(max(rates) * Q32),
        },
        "uncertainty_includes_mapping_fit_and_transport": True,
        "raw_ntp_statistics_exported": False,
        "source_timing_evidence_sha256": file_sha256(report_path),
        "policy_sha256": policy_semantic,
        "stale": False,
    }
    seal_semantic(mapping, "semantic_sha256")
    binding = {
        key: value for key, value in policy.items()
        if key != "semantic_sha256"
    }
    binding.update({
        "capture_policy_semantic_sha256": policy_semantic,
        "clock_mapping_evidence_sha256": mapping["semantic_sha256"],
        "clock_mapping_stale": False,
        "completion_clock_session_id": completion_session,
        "submission_clock_session_id": submission_session,
        "mapping_segment_id": mapping_segment,
    })
    seal_semantic(binding, "semantic_sha256")

    capture: dict[str, Any] = {
        "schema_version": 1,
        "evidence_type":
            "mouse_effect_probe_b_composite_phase_capture_ledger",
        "run_uuid": run_uuid,
        "activation_epoch": activation_epoch,
        "scope_id": scope_id,
        "status": "CAPTURE_COMPLETE",
        "physical_output_capability": False,
        "physical_dispatch_count": 0,
        "plan_semantic_sha256": plan_semantic,
        "capture_policy_semantic_sha256": policy_semantic,
        "capture_binding": binding,
        "capture_binding_semantic_sha256": binding["semantic_sha256"],
        "scheduler_clock": scheduler_clock,
        "plan_accepted_at_qpc": accepted,
        "acquisition_started_at_qpc": acquired,
        "acquisition_finished_at_qpc": finished,
        "revealed_at_qpc": revealed,
        "clock_mapping": mapping,
        "human_assessment_semantic_sha256": assessment_semantic,
        "schedule_ledger_semantic_sha256": schedule_semantic,
        "command_report_semantic_sha256": report_semantic,
        "report_verifier_file_sha256": file_sha256(verifier_path),
        "frames": frame_records,
    }
    seal_semantic(capture, "capture_semantic_sha256")
    commands: dict[str, Any] = {
        "schema_version": 1,
        "evidence_type":
            "mouse_effect_probe_b_composite_phase_command_ledger",
        "run_uuid": run_uuid,
        "activation_epoch": activation_epoch,
        "scope_id": scope_id,
        "status": "COMMANDS_COMPLETE",
        "ledger_is_read_only_record": True,
        "binder_physical_output_capability": False,
        "binder_physical_dispatch_count": 0,
        "plan_semantic_sha256": plan_semantic,
        "capture_policy_semantic_sha256": policy_semantic,
        "capture_binding_semantic_sha256": binding["semantic_sha256"],
        "clock_mapping_semantic_sha256": mapping["semantic_sha256"],
        "source_dispatch_count": 38,
        "source_backend_completion_count": 38,
        "source_delivery_ack_count": 38,
        "command_report_file_sha256": file_sha256(report_path),
        "schedule_ledger_file_sha256": file_sha256(schedule_path),
        "events": command_events,
        "negative_controls": command_controls,
    }
    seal_semantic(commands, "command_semantic_sha256")
    return capture, commands


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    if hasattr(sys.stderr, "reconfigure"):
        sys.stderr.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(
        description="Produce composite-phase capture/command ledgers")
    parser.add_argument("--plan", required=True, type=pathlib.Path)
    parser.add_argument("--report", required=True, type=pathlib.Path)
    parser.add_argument("--schedule", required=True, type=pathlib.Path)
    parser.add_argument("--safety", required=True, type=pathlib.Path)
    parser.add_argument("--manifest", required=True, type=pathlib.Path)
    parser.add_argument("--human-assessment", required=True,
                        type=pathlib.Path)
    parser.add_argument("--report-verifier", required=True,
                        type=pathlib.Path)
    parser.add_argument("--capture-output", required=True,
                        type=pathlib.Path)
    parser.add_argument("--command-output", required=True,
                        type=pathlib.Path)
    options = parser.parse_args()
    try:
        capture, commands = produce(options)
        write_pair_atomic(options.capture_output, options.command_output,
                          capture, commands)
    except (ProducerError, OSError, UnicodeError, ValueError) as error:
        print(f"composite ledger producer 未发布: {error}", file=sys.stderr)
        return 2
    print("composite capture/command ledgers 已原子创建")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
