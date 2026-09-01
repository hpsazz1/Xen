import hashlib
import importlib.util
import json
import pathlib
import sys
import tempfile

import cv2
import numpy as np


ROOT = pathlib.Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "scripts" / "calibrate_mouse_effect_probe_a2.py"
SPEC = importlib.util.spec_from_file_location("probe_a2_calibration", MODULE_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("无法加载 Physical A2 dependency calibration")
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def _write_json(path: pathlib.Path, value: dict) -> None:
    path.write_text(
        json.dumps(value, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )


def _shift_image(image: np.ndarray, shift_x: float) -> np.ndarray:
    matrix = np.array([[1.0, 0.0, shift_x], [0.0, 1.0, 0.0]], dtype=np.float64)
    return cv2.warpAffine(
        image,
        matrix,
        (image.shape[1], image.shape[0]),
        flags=cv2.INTER_LINEAR,
        borderMode=cv2.BORDER_REFLECT_101,
    )


def _make_s1_session(
    root: pathlib.Path,
    role: str,
    run_uuid: str,
    source_clock_session_id: str,
    source_start_ns: int,
    shifts: list[float],
    *,
    degenerate: bool = False,
) -> pathlib.Path:
    run = root / role
    frame_root = run / "pixel-evidence" / "frames"
    frame_root.mkdir(parents=True)
    rng = np.random.default_rng(20260901)
    base = rng.integers(0, 256, (128, 256, 3), dtype=np.uint8)
    frames: list[dict] = []
    for index, shift in enumerate(shifts):
        bgr = base.copy() if degenerate else _shift_image(base, shift)
        # 两个 capture session 不共享完整 decoded frame，但 witness 内容保持同一 scope。
        if role == "validation":
            bgr[0, 0, 0] = np.uint8((int(bgr[0, 0, 0]) + 1) % 256)
        frame_path = frame_root / f"{index:06d}.png"
        expect(cv2.imwrite(str(frame_path), bgr), "S1 fixture PNG 写入失败")
        source_time_ns = source_start_ns + index * 4_000_000
        frames.append(
            {
                "index": index,
                "file": f"frames/{index:06d}.png",
                "width": 256,
                "height": 128,
                "bgr_sha256": hashlib.sha256(bgr.tobytes()).hexdigest(),
                "source_timestamp": 1_000_000 + index + source_start_ns,
                "source_timestamp_valid": True,
                "source_time_at_steady_ns": source_time_ns,
                "source_time_basis": "NDI_SDK_SUBMISSION",
                "source_time_timing_valid": True,
                "source_clock_status": "VALID",
                "source_clock_session_id": source_clock_session_id,
                "source_clock_uncertainty_ms": 0.05,
                "source_clock_rtt_ms": 0.10,
                "source_clock_mapping_age_ms": 1.0,
                "source_clock_sample_count": 12,
                "source_dropped_frames": 0,
                "transport_dropped_frames": 0,
                "transport_invalid_packets": 0,
            }
        )
    binding_sha = hashlib.sha256(f"binding-{role}".encode()).hexdigest()
    manifest = {
        "schema_version": 1,
        "evidence_type": "output_off_capture",
        "physical_output_capability": False,
        "capture_source_name": "fixture-scope",
        "requested_frame_count": len(frames),
        "recorded_frame_count": len(frames),
        "source_binding": {
            "file": "source-binding.json",
            "sha256": binding_sha,
        },
        "frames": frames,
    }
    session = {
        "schema_version": 1,
        "evidence_type": "mouse_effect_probe_a2_s1_session",
        "status": "RECORDED_UNANALYZED",
        "physical_output_capability": False,
        "probe_started": False,
        "mouse_opened": False,
        "actual_command_zero": True,
        "aim_off": True,
        "run_uuid": run_uuid,
        "run_role": role,
        "scope_id": "fixture-scope",
        "capture_process_session_id": f"capture-{role}",
        "manifest_sha256": "fixture-manifest-sha",
        "obs_source_binding_sha256": binding_sha,
    }
    _write_json(run / "pixel-evidence" / "manifest.json", manifest)
    _write_json(run / "s1-session.json", session)
    return run


def test_s0_uses_untouched_holdout_and_calibrates_spatial_operations() -> None:
    result = MODULE.build_synthetic_calibration()
    expect(result["status"] == "VALID", "S0 合成校准必须通过预注册留出")
    expect(result["physical_output_capability"] is False,
           "S0 产物不得具备物理输出能力")
    expect(result["holdout_contract"]["random_case_split_allowed"] is False,
           "S0 必须按完整 texture/shift case 留出")
    expect(result["holdout_contract"]["holdout_used_for_interval"] is False,
           "untouched holdout 不得参与算法误差区间拟合")
    interval = result["spatial_error_interval_px"]
    expect(np.isfinite(interval["lower"]) and np.isfinite(interval["upper"])
           and interval["lower"] <= interval["upper"],
           "S0 必须给有限有序的 signed spatial error interval")
    expect(result["tail_detection"]["all_injected_tails_detected"] is True,
           "S0 必须能检出预注册的有限 injected tail")
    expect(result["mask_margin"]["internal_invalid_region_detected"] is True,
           "S0 必须覆盖内部 invalid/occluder，而不只检查图像边缘")


def test_s1_requires_two_nonoverlapping_nondegenerate_capture_sessions() -> None:
    shifts = [
        0.00, 0.04, -0.02, 0.03, -0.01, 0.05, -0.03, 0.02,
        0.00, -0.04, 0.01, -0.02, 0.03, -0.05, 0.02, 0.00,
    ] * 4
    with tempfile.TemporaryDirectory(prefix="xen-a2-s1-") as directory:
        root = pathlib.Path(directory)
        primary = _make_s1_session(
            root,
            "primary",
            "11111111-2222-4333-8444-555555555555",
            "clock-primary",
            10_000_000_000,
            shifts,
        )
        validation = _make_s1_session(
            root,
            "validation",
            "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee",
            "clock-validation",
            20_000_000_000,
            shifts,
        )
        result, rows = MODULE.analyze_zero_input_sessions(
            primary,
            validation,
            (16, 16, 80, 96),
            (160, 16, 80, 96),
            block_count=4,
        )
        expect(result["status"] == "VALID" and len(rows) == 128,
               "两个独立 S1 session 的非退化零输入块必须建立有效 noise 证据")
        expect(result["physical_output_capability"] is False
               and result["probe_started"] is False
               and result["mouse_opened"] is False,
               "S1 必须证明无 probe、无 Mouse、无物理输出")
        audit = result["operational_independence"]
        expect(audit["distinct_run_uuid"] is True
               and audit["distinct_capture_process_session"] is True
               and audit["distinct_source_clock_session"] is True
               and audit["source_time_ranges_overlap"] is False
               and audit["decoded_frame_hash_overlap_count"] == 0,
               "S1 primary/validation 不得共享 session、时间窗或 decoded frame")
        expect(result["noise_model"]["iid_claimed"] is False,
               "有限 S1 数据不得声称 i.i.d.")
        expect(result["noise_model"]["left"]["nondegenerate"] is True
               and result["noise_model"]["right"]["nondegenerate"] is True,
               "左右 witness 的去 nuisance residual 都必须非退化")
        plan = MODULE.derive_dependency_calibration_plan(
            MODULE.build_synthetic_calibration(),
            result,
            observed_lag_reference=4,
            candidate_horizons=(4, 8, 16, 32),
        )
        expect(plan["status"] == "VALID_OFFLINE_PLAN"
               and plan["physical_output_capability"] is False
               and plan["physical_launch_authorized"] is False,
               "S0/S1 green 只能生成离线 A2 plan，不得授权 Launch")
        request = plan["sequence_request"]
        expect(request["block_count"] == 4
               and request["guard_sample_count"] > 32
               and request["response_sample_count"] >= 32
               and request["sample_count"] < 2400
               and request["max_abs_prefix_x_counts"] == 1,
               "A2 repetition/guard 必须由覆盖目标与 S1 cadence 派生且仍在短证据容量内")
        expect(plan["roles"]["p_cal"]["profile"]
               != plan["roles"]["p_holdout"]["profile"],
               "P-CAL/P-HOLDOUT 必须预注册不同 profile/方向角色")
        expect(plan["derivation"]["reused_physical_a_guard_or_gain"] is False
               and plan["derivation"]["fixed_speed_used"] is False,
               "A2 plan 不得复用 A1 guard/gain 或固定速度换算")


def test_s1_rejects_reused_or_degenerate_frames_without_epsilon() -> None:
    shifts = [0.0] * 32
    with tempfile.TemporaryDirectory(prefix="xen-a2-s1-red-") as directory:
        root = pathlib.Path(directory)
        primary = _make_s1_session(
            root,
            "primary",
            "11111111-2222-4333-8444-555555555555",
            "clock-primary",
            10_000_000_000,
            shifts,
            degenerate=True,
        )
        validation = _make_s1_session(
            root,
            "validation",
            "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee",
            "clock-validation",
            20_000_000_000,
            shifts,
            degenerate=True,
        )
        result, _ = MODULE.analyze_zero_input_sessions(
            primary,
            validation,
            (16, 16, 80, 96),
            (160, 16, 80, 96),
            block_count=4,
        )
        expect(result["status"] == "INVALID"
               and "NONDEGENERATE_NOISE_MISSING" in result["invalid_reasons"],
               "重复 PNG/全零 residual 必须保持 red，不得填 epsilon")
        expect(result["noise_model"]["artificial_epsilon_added"] is False,
               "S1 绝不允许伪造正宽 noise interval")


def test_s1_allows_separate_timing_observations_of_the_same_clock_epoch() -> None:
    shifts = [
        0.00, 0.04, -0.02, 0.03, -0.01, 0.05, -0.03, 0.02,
        0.00, -0.04, 0.01, -0.02, 0.03, -0.05, 0.02, 0.00,
    ] * 4
    with tempfile.TemporaryDirectory(prefix="xen-a2-s1-clock-") as directory:
        root = pathlib.Path(directory)
        primary = _make_s1_session(
            root,
            "primary",
            "11111111-2222-4333-8444-555555555555",
            "shared-clock-epoch",
            10_000_000_000,
            shifts,
        )
        validation = _make_s1_session(
            root,
            "validation",
            "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee",
            "shared-clock-epoch",
            20_000_000_000,
            shifts,
        )
        result, _ = MODULE.analyze_zero_input_sessions(
            primary,
            validation,
            (16, 16, 80, 96),
            (160, 16, 80, 96),
            block_count=4,
        )
        expect(result["status"] == "VALID",
               "不同 capture/timing observation 不应被稳定 clock epoch 误判为复用")
        audit = result["operational_independence"]
        expect(audit["distinct_source_clock_session"] is False
               and audit["distinct_timing_observation_session"] is True,
               "报告必须分开记录 clock epoch 与本次 timing observation 身份")


if __name__ == "__main__":
    test_s0_uses_untouched_holdout_and_calibrates_spatial_operations()
    test_s1_requires_two_nonoverlapping_nondegenerate_capture_sessions()
    test_s1_rejects_reused_or_degenerate_frames_without_epsilon()
    test_s1_allows_separate_timing_observations_of_the_same_clock_epoch()
    print("Mouse Effect Probe A2 dependency calibration 测试全部通过。")
