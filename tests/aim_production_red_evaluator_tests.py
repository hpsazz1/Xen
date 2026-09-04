#!/usr/bin/env python3
"""Production red schema/evaluator 的公开文件接口回归。"""

from __future__ import annotations

import hashlib
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "scripts" / "evaluate_aim_production_red.py"
SPEC = importlib.util.spec_from_file_location(
    "evaluate_aim_production_red", MODULE_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("无法加载 production red evaluator")
EVALUATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EVALUATOR)


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def file_sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def canonical_sha256(value: object) -> str:
    payload = json.dumps(
        value, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def write_manifest(
        root: pathlib.Path,
        rows: list[dict],
        *,
        stem: str = "trace",
        measured_reference: dict | None = None) -> pathlib.Path:
    if rows and all("source_relative_path" in row for row in rows):
        source_path = root / f"{stem}-source.json"
        source_path.write_text(
            json.dumps({"fixture": stem, "kind": "source"}, sort_keys=True),
            encoding="utf-8")
        for row in rows:
            row["source_relative_path"] = source_path.name
            row["source_sha256"] = file_sha256(source_path)
    trace_path = root / f"{stem}.jsonl"
    trace_path.write_text(
        "".join(json.dumps(row, sort_keys=True) + "\n" for row in rows),
        encoding="utf-8")
    manifest = {
        "red_schema": 1,
        "evidence_type": "aim_production_red_manifest",
        "asset_id": rows[0].get("asset_id", f"{stem}-fixture"),
        "generator": {
            "name": "aim_production_red_evaluator_tests",
            "version": 1,
            "sha256": "a" * 64,
        },
        "configuration_sha256": "b" * 64,
        "physical_output_capability": False,
        "physical_dispatch_count": 0,
        "candidate_uses_f1": False,
        "production_plant_profile_id": "P-LEGACY-D3-G05215",
        "mandatory_block_profiles": [{
            "block_id": "DEV-PIX-S0",
            "plant_profile_id": "P-LEGACY-D3-G05215",
        }],
        "traces": [{
            "trace_id": "DEV-PIX-S0:P-LEGACY-D3-G05215:B0",
            "block_id": "DEV-PIX-S0",
            "role": "development",
            "variant": "B0",
            "plant_profile_id": "P-LEGACY-D3-G05215",
            "relative_path": trace_path.name,
            "sha256": file_sha256(trace_path),
            "sample_count": len(rows),
            "score_begin": 0,
            "score_end": len(rows),
        }],
    }
    if measured_reference is not None:
        reference_path = root / f"{stem}-reference.json"
        reference_path.write_text(
            json.dumps({"fixture": stem}, sort_keys=True), encoding="utf-8")
        reference = json.loads(json.dumps(measured_reference))
        uncertainty = reference["measurement_uncertainty"]
        uncertainty["source_relative_path"] = reference_path.name
        uncertainty["source_sha256"] = file_sha256(reference_path)
        manifest["traces"][0]["measured_reference"] = reference
    manifest_path = root / f"{stem}-manifest.json"
    manifest_path.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    return manifest_path


def valid_row(
        index: int,
        outside: bool,
        sample_count: int,
        *,
        block_id: str = "DEV-PIX-S0",
        sequence_base: int = 100,
        asset_id: str = "ordered-tail-fixture") -> dict:
    captured_at_ns = index * 10
    matched_box = [1.0, -1.0, 3.0, 1.0] if outside else [
        -1.0, -1.0, 1.0, 1.0]
    return {
        "red_schema": 1,
        "asset_id": asset_id,
        "source_relative_path": "authoritative/source.json",
        "source_sha256": "c" * 64,
        "extractor": {
            "name": "aim_production_red_evaluator_tests",
            "version": 1,
            "sha256": "d" * 64,
        },
        "configuration_sha256": "b" * 64,
        "block_id": block_id,
        "role": "development",
        "reset": index == 0,
        "sample_index_in_block": index,
        "score_begin": 0,
        "score_end": sample_count,
        "plant_profile_id": "P-LEGACY-D3-G05215",
        "source_sequence": sequence_base + index,
        "source_timestamp": 10_000 + sequence_base + index,
        "source_clock_session_id": "synthetic:ordered-tail",
        "captured_at_ns": captured_at_ns,
        "control_at_ns": captured_at_ns + 5,
        "controller_dt_ns": 10,
        "observation_age_ns": 5,
        "backend_completed_at_ns": captured_at_ns + 6,
        "world_delta_x": 0.0,
        "world_delta_y": 0.0,
        "box_width": 2.0,
        "box_height": 2.0,
        "pose": "fixed",
        "visible": True,
        "target_id": "target-1",
        "aim_status": "SUCCESS",
        "matched_observation_box": matched_box,
        "base_point": [2.0 if outside else 0.5, 0.0],
        "delay_compensated_point": [2.0 if outside else 0.5, 0.0],
        "prediction_point": [2.0 if outside else 0.5, 0.0],
        "control_center": [0.0, 0.0],
        "controller_x": {
            "proportional_counts": 0.0,
            "feedforward_counts": 0.0,
            "desired_counts": 0.0,
            "filtered_counts": 0.0,
            "shaped_counts": 0.0,
        },
        "issued_dx": 0,
        "issued_dy": 0,
        "backend_completed_dx": 0,
        "backend_completed_dy": 0,
        "completion_status": "COMPLETED",
        "completion_zero_reason": "ZERO_COMMAND",
        "protocol_acknowledged": True,
        "aim_actual_history_dx": 0,
        "aim_actual_history_dy": 0,
        "plant_input_dx": 0,
        "plant_input_dy": 0,
        "plant_due_queue": [],
        "plant_prefix": [0.0, 0.0],
        "camera_visible_effect": [0.0, 0.0],
        "observed_box": matched_box,
        "quantization_zero_x": False,
        "quantization_zero_y": False,
        "limit_signature": "NONE",
        "lock_active": True,
        "backend_failure": False,
        "physical_dispatch_count": 0,
    }


def measured_reference(
        rows: list[dict], outside_samples: int, longest: int) -> dict:
    outside = [row["matched_observation_box"][0] > 0.0 for row in rows]
    identities = [[
        row["source_sequence"], row["source_timestamp"],
        row["source_clock_session_id"]] for row in rows]
    return {
        "source_identity_sha256": canonical_sha256(identities),
        "outside_sequence_sha256": canonical_sha256(outside),
        "metrics": {
            "outside_samples": outside_samples,
            "outside_duration_ns": outside_samples * 10,
            "longest_outside_samples": longest,
            "longest_outside_ns": longest * 10,
            "outside_area_px_ns": float(outside_samples * 10),
            "max_excess_x_px": 1.0 if outside_samples else 0.0,
            "max_abs_error_x_px": 2.0 if outside_samples else 0.5,
        },
        "measurement_uncertainty": {
            "source_relative_path": "reference/ordered-tail.json",
            "source_sha256": "e" * 64,
            "absolute": {
                "outside_area_px_ns": 0.0,
                "max_excess_x_px": 0.0,
                "max_abs_error_x_px": 0.0,
            },
        },
    }


def write_multi_manifest(
        root: pathlib.Path,
        definitions: list[tuple[dict, list[dict], dict | None]],
        *,
        asset_id: str,
        candidate_uses_f1: bool = False,
        production_plant_profile_id: str | None = None) -> pathlib.Path:
    traces = []
    for descriptor, rows, reference in definitions:
        stem = descriptor["trace_id"].replace(":", "-")
        source_stem = descriptor["block_id"].replace(":", "-")
        source_path = root / f"{source_stem}-source.json"
        if not source_path.exists():
            source_path.write_text(json.dumps({
                "fixture": source_stem,
                "kind": "source",
            }, sort_keys=True), encoding="utf-8")
        for row in rows:
            row["source_relative_path"] = source_path.name
            row["source_sha256"] = file_sha256(source_path)
        trace_path = root / f"{stem}.jsonl"
        trace_path.write_text(
            "".join(json.dumps(row, sort_keys=True) + "\n" for row in rows),
            encoding="utf-8")
        trace = {
            **descriptor,
            "relative_path": trace_path.name,
            "sha256": file_sha256(trace_path),
            "sample_count": len(rows),
            "score_begin": 0,
            "score_end": len(rows),
        }
        if reference is not None:
            reference_path = root / f"{stem}-reference.json"
            reference_path.write_text(
                json.dumps({"fixture": stem}, sort_keys=True),
                encoding="utf-8")
            materialized = json.loads(json.dumps(reference))
            uncertainty = materialized["measurement_uncertainty"]
            uncertainty["source_relative_path"] = reference_path.name
            uncertainty["source_sha256"] = file_sha256(reference_path)
            trace["measured_reference"] = materialized
        traces.append(trace)
    baseline_descriptors = [trace for trace in traces
                            if trace["variant"] == "B0"]
    if production_plant_profile_id is None:
        production_plant_profile_id = baseline_descriptors[0][
            "plant_profile_id"]
    mandatory_pairs = sorted({
        (trace["block_id"], trace["plant_profile_id"])
        for trace in baseline_descriptors
    })
    manifest = {
        "red_schema": 1,
        "evidence_type": "aim_production_red_manifest",
        "asset_id": asset_id,
        "generator": {
            "name": "aim_production_red_evaluator_tests",
            "version": 1,
            "sha256": "a" * 64,
        },
        "configuration_sha256": "b" * 64,
        "physical_output_capability": False,
        "physical_dispatch_count": 0,
        "candidate_uses_f1": candidate_uses_f1,
        "production_plant_profile_id": production_plant_profile_id,
        "mandatory_block_profiles": [{
            "block_id": block_id,
            "plant_profile_id": profile_id,
        } for block_id, profile_id in mandatory_pairs],
        "traces": traces,
    }
    path = root / "multi-manifest.json"
    path.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    return path


def test_missing_source_and_completed_ledgers_fail_closed() -> None:
    with tempfile.TemporaryDirectory(prefix="xen-production-red-") as text:
        root = pathlib.Path(text)
        manifest_path = write_manifest(root, [{
            "block_id": "DEV-PIX-S0",
            "sample_index_in_block": 0,
            "issued_dx": 1,
            "issued_dy": 0,
        }])

        report = EVALUATOR.evaluate_manifest(manifest_path)

        expect(report["status"] == "RED_INPUT_INCOMPLETE",
               "缺 source/completed ledger 必须在计算 metric 前 fail closed")
        expect("MISSING_SOURCE_IDENTITY" in report["failure_codes"],
               "报告必须明确 source identity 缂失")
        expect("MISSING_COMPLETED_LEDGER" in report["failure_codes"],
               "报告必须明确独立 completed ledger 缺失")
        expect(report["physical_output_capability"] is False and
               report["production_aim_changed"] is False,
               "evaluator 必须明确保持 output-off 且不修改生产 Aim")


def test_run_order_is_preserved_in_longest_outside_metric() -> None:
    with tempfile.TemporaryDirectory(prefix="xen-production-red-") as text:
        root = pathlib.Path(text)
        contiguous = [valid_row(index, outside, 4) for index, outside in
                      enumerate([True, True, False, False])]
        split = [valid_row(index, outside, 4) for index, outside in
                 enumerate([True, False, True, False])]
        contiguous_report = EVALUATOR.evaluate_manifest(write_manifest(
            root, contiguous, stem="contiguous",
            measured_reference=measured_reference(contiguous, 2, 2)))
        split_report = EVALUATOR.evaluate_manifest(write_manifest(
            root, split, stem="split",
            measured_reference=measured_reference(split, 2, 1)))

        contiguous_metrics = contiguous_report["trace_results"][0]["metrics"]
        split_metrics = split_report["trace_results"][0]["metrics"]
        expect(contiguous_metrics["p50_abs_error_x_px"] ==
               split_metrics["p50_abs_error_x_px"] and
               contiguous_metrics["p95_abs_error_x_px"] ==
               split_metrics["p95_abs_error_x_px"],
               "相同误差 multiset 的诊断分位数必须相同")
        expect(contiguous_metrics["longest_outside_samples"] == 2 and
               split_metrics["longest_outside_samples"] == 1,
               "PBR-RUN-ORDER 必须让连续 run 随真实行序变化")
        expect(contiguous_report["status"] == "BASELINE_RED_LOCKED" and
               split_report["status"] == "BASELINE_RED_LOCKED",
               "与各自权威逐帧 reference 精确一致的 B0 必须锁定 red")


def test_block_regression_cannot_be_hidden_by_aggregate_improvement() -> None:
    with tempfile.TemporaryDirectory(prefix="xen-production-red-") as text:
        root = pathlib.Path(text)
        asset_id = "block-conjunction-fixture"
        block_a_b0 = [valid_row(
            index, outside, 4, block_id="BLOCK-A", sequence_base=100,
            asset_id=asset_id) for index, outside in
            enumerate([True, True, True, True])]
        block_a_candidate = [valid_row(
            index, False, 4, block_id="BLOCK-A", sequence_base=100,
            asset_id=asset_id) for index in range(4)]
        block_b_b0 = [valid_row(
            index, outside, 4, block_id="BLOCK-B", sequence_base=200,
            asset_id=asset_id) for index, outside in
            enumerate([True, False, False, False])]
        block_b_candidate = [valid_row(
            index, outside, 4, block_id="BLOCK-B", sequence_base=200,
            asset_id=asset_id) for index, outside in
            enumerate([True, True, False, False])]
        base_descriptor = {
            "role": "development",
            "plant_profile_id": "P-LEGACY-D3-G05215",
        }
        path = write_multi_manifest(root, [
            ({**base_descriptor, "trace_id": "BLOCK-A:B0",
              "block_id": "BLOCK-A", "variant": "B0"},
             block_a_b0, measured_reference(block_a_b0, 4, 4)),
            ({**base_descriptor, "trace_id": "BLOCK-A:C",
              "block_id": "BLOCK-A", "variant": "C"},
             block_a_candidate, None),
            ({**base_descriptor, "trace_id": "BLOCK-B:B0",
              "block_id": "BLOCK-B", "variant": "B0"},
             block_b_b0, measured_reference(block_b_b0, 1, 1)),
            ({**base_descriptor, "trace_id": "BLOCK-B:C",
              "block_id": "BLOCK-B", "variant": "C"},
             block_b_candidate, None),
        ], asset_id=asset_id)

        report = EVALUATOR.evaluate_manifest(path)

        expect(report["aggregate_diagnostics"]["B0"][
                   "outside_area_px_ns"] == 50.0 and
               report["aggregate_diagnostics"]["C"][
                   "outside_area_px_ns"] == 20.0,
               "负控必须确实构造 aggregate 改善")
        expect(report["status"] == "CANDIDATE_DELETED" and
               "AGGREGATE_MASKED_BLOCK_REGRESSION" in
               report["failure_codes"],
               "PBR-BLOCK-MIX 必须拒绝 aggregate 掩盖的单 block 退化")
        comparisons = {item["block_id"]: item for item in
                       report["block_comparisons"]}
        expect(comparisons["BLOCK-A"]["tail_pass"] is True and
               comparisons["BLOCK-B"]["tail_pass"] is False,
               "evaluator 必须逐 block 保存 conjunction 结果")


def test_unfaithful_six_three_replay_is_rejected_before_candidate() -> None:
    with tempfile.TemporaryDirectory(prefix="xen-production-red-") as text:
        root = pathlib.Path(text)
        rows = [valid_row(index, outside, 8) for index, outside in enumerate(
            [True, True, True, False, True, True, True, False])]
        reference = measured_reference(rows, 6, 3)
        reference["metrics"].update({
            "outside_samples": 41,
            "outside_duration_ns": 410,
            "longest_outside_samples": 7,
            "longest_outside_ns": 70,
            "outside_area_px_ns": 410.0,
            "max_abs_error_x_px": 17.158,
        })
        report = EVALUATOR.evaluate_manifest(write_manifest(
            root, rows, stem="unfaithful-six-three",
            measured_reference=reference))

        expect(report["status"] == "BASELINE_REPLAY_FIDELITY_INVALID",
               "PBR-FIDELITY 必须在候选不存在时拒绝 6/3 近似")
        result = report["trace_results"][0]
        differences = result["fidelity_differences"]
        expect(differences["outside_samples"] == {
                   "measured": 41, "replay": 6} and
               differences["longest_outside_samples"] == {
                   "measured": 7, "replay": 3},
               "fidelity 报告必须列出逐 block measured/B0 tail 差异")


def test_requested_or_ack_cannot_leak_into_actual_plant_input() -> None:
    with tempfile.TemporaryDirectory(prefix="xen-production-red-") as text:
        root = pathlib.Path(text)
        rows = [valid_row(0, False, 1)]
        rows[0].update({
            "issued_dx": 3,
            "backend_completed_dx": 0,
            "completion_zero_reason": "BACKEND_REJECTED",
            "protocol_acknowledged": True,
            "aim_actual_history_dx": 3,
            "plant_input_dx": 3,
            "camera_visible_effect": [1.0, 0.0],
        })
        report = EVALUATOR.evaluate_manifest(write_manifest(
            root, rows, stem="requested-leak",
            measured_reference=measured_reference(rows, 0, 0)))

        expect(report["status"] == "RED_INVALID" and
               "REQUESTED_OR_ACK_LEAKED_INTO_PLANT" in
               report["failure_codes"],
               "PBR-ACTUAL 必须阻止 requested/ACK 冒充 completed actual input")


def test_schema_contract_is_versioned_and_hash_bound() -> None:
    contract = EVALUATOR.red_schema_contract()
    required_rows = set(contract["required_trace_row_fields"])
    expect(contract["red_schema"] == 1 and
           contract["input_evidence_type"] ==
           "aim_production_red_manifest",
           "red schema 必须有稳定版本和 evidence type")
    expect({
        "source_sequence", "source_timestamp", "source_clock_session_id",
        "issued_dx", "issued_dy", "backend_completed_dx",
        "backend_completed_dy", "plant_input_dx", "plant_input_dy",
        "physical_dispatch_count",
    }.issubset(required_rows),
           "schema 必须显式分离 source、issued、completed、plant 和安全账")
    expect(contract["contract_semantic_sha256"] == canonical_sha256({
        key: value for key, value in contract.items()
        if key != "contract_semantic_sha256"
    }), "schema 合同自身必须具有可复算的 semantic SHA-256")


def test_source_asset_hash_drift_is_invalid() -> None:
    with tempfile.TemporaryDirectory(prefix="xen-production-red-") as text:
        root = pathlib.Path(text)
        rows = [valid_row(0, True, 1)]
        path = write_manifest(
            root, rows, stem="source-drift",
            measured_reference=measured_reference(rows, 1, 1))
        source_path = root / rows[0]["source_relative_path"]
        source_path.write_text("drift", encoding="utf-8")

        report = EVALUATOR.evaluate_manifest(path)

        expect(report["status"] == "RED_INVALID" and
               "SOURCE_ASSET_SHA256_MISMATCH" in report["failure_codes"],
               "row 声称的权威 source 文件发生漂移时必须 fail closed")


def test_candidate_off_must_reproduce_baseline_rows_exactly() -> None:
    with tempfile.TemporaryDirectory(prefix="xen-production-red-") as text:
        root = pathlib.Path(text)
        asset_id = "candidate-deletion-fixture"
        baseline = [valid_row(
            index, outside, 3, block_id="BLOCK-D0", asset_id=asset_id)
            for index, outside in enumerate([True, True, False])]
        candidate = [valid_row(
            index, outside, 3, block_id="BLOCK-D0", asset_id=asset_id)
            for index, outside in enumerate([True, False, False])]
        candidate_off = json.loads(json.dumps(baseline))
        candidate_off[1].update({
            "issued_dx": 1,
            "backend_completed_dx": 1,
            "completion_zero_reason": "NONZERO_COMMAND",
            "aim_actual_history_dx": 1,
            "plant_input_dx": 1,
        })
        common = {
            "block_id": "BLOCK-D0",
            "role": "development",
            "plant_profile_id": "P-LEGACY-D3-G05215",
        }
        path = write_multi_manifest(root, [
            ({**common, "trace_id": "BLOCK-D0:B0", "variant": "B0"},
             baseline, measured_reference(baseline, 2, 2)),
            ({**common, "trace_id": "BLOCK-D0:C", "variant": "C"},
             candidate, None),
            ({**common, "trace_id": "BLOCK-D0:C-D0", "variant": "C-D0"},
             candidate_off, None),
        ], asset_id=asset_id)

        report = EVALUATOR.evaluate_manifest(path)

        expect(report["status"] == "CANDIDATE_DELETED" and
               "CANDIDATE_DELETION_NOT_BASELINE" in
               report["failure_codes"],
               "PBR-D0 必须逐行拒绝无法完整删除回 B0 的 candidate")


def test_x_candidate_cannot_change_y_ledger() -> None:
    with tempfile.TemporaryDirectory(prefix="xen-production-red-") as text:
        root = pathlib.Path(text)
        asset_id = "x-only-preservation-fixture"
        baseline = [valid_row(
            index, outside, 3, block_id="RED-Y-ONLY", asset_id=asset_id)
            for index, outside in enumerate([True, True, False])]
        candidate = [valid_row(
            index, outside, 3, block_id="RED-Y-ONLY", asset_id=asset_id)
            for index, outside in enumerate([True, False, False])]
        candidate[1].update({
            "issued_dy": 1,
            "backend_completed_dy": 1,
            "completion_zero_reason": "NONZERO_COMMAND",
            "aim_actual_history_dy": 1,
            "plant_input_dy": 1,
        })
        candidate_off = json.loads(json.dumps(baseline))
        common = {
            "block_id": "RED-Y-ONLY",
            "role": "development",
            "plant_profile_id": "P-LEGACY-D3-G05215",
        }
        path = write_multi_manifest(root, [
            ({**common, "trace_id": "RED-Y-ONLY:B0", "variant": "B0"},
             baseline, measured_reference(baseline, 2, 2)),
            ({**common, "trace_id": "RED-Y-ONLY:C", "variant": "C"},
             candidate, None),
            ({**common, "trace_id": "RED-Y-ONLY:C-D0",
              "variant": "C-D0"}, candidate_off, None),
        ], asset_id=asset_id)

        report = EVALUATOR.evaluate_manifest(path)

        expect(report["status"] == "CANDIDATE_DELETED" and
               "X_ONLY_CONTRACT_BROKEN" in report["failure_codes"],
               "PBR-YXY 必须让任一 Y ledger 漂移覆盖 X tail 改善并删除候选")


def set_completed_x(row: dict, value: int) -> None:
    row.update({
        "issued_dx": value,
        "backend_completed_dx": value,
        "completion_zero_reason": (
            "ZERO_COMMAND" if value == 0 else "NONZERO_COMMAND"),
        "aim_actual_history_dx": value,
        "plant_input_dx": value,
    })


def test_completed_command_reversal_absolute_gate_cannot_be_masked() -> None:
    with tempfile.TemporaryDirectory(prefix="xen-production-red-") as text:
        root = pathlib.Path(text)
        asset_id = "reversal-fixture"
        baseline = [valid_row(
            index, index < 2, 9, block_id="DEV-REV-FIXED",
            asset_id=asset_id) for index in range(9)]
        candidate = [valid_row(
            index, index == 0, 9, block_id="DEV-REV-FIXED",
            asset_id=asset_id) for index in range(9)]
        for row, value in zip(baseline, [1, 1, -1, -1, 1, 1, -1, -1, -1]):
            set_completed_x(row, value)
        for row, value in zip(candidate, [1, -1, 1, -1, 1, -1, 1, -1, -1]):
            set_completed_x(row, value)
        candidate_off = json.loads(json.dumps(baseline))
        common = {
            "block_id": "DEV-REV-FIXED",
            "role": "development",
            "plant_profile_id": "P-LEGACY-D3-G05215",
            "change_points": [
                {"sample_index": 2, "new_direction": -1},
                {"sample_index": 4, "new_direction": 1},
                {"sample_index": 6, "new_direction": -1},
            ],
            "max_nonzero_completed_command_reversals": 3,
        }
        path = write_multi_manifest(root, [
            ({**common, "trace_id": "DEV-REV-FIXED:B0",
              "variant": "B0"}, baseline,
             measured_reference(baseline, 2, 2)),
            ({**common, "trace_id": "DEV-REV-FIXED:C", "variant": "C"},
             candidate, None),
            ({**common, "trace_id": "DEV-REV-FIXED:C-D0",
              "variant": "C-D0"}, candidate_off, None),
        ], asset_id=asset_id)

        report = EVALUATOR.evaluate_manifest(path)

        expect(report["status"] == "CANDIDATE_DELETED" and
               "NATURAL_REVERSAL_RED" in report["failure_codes"],
               "PBR-REV 必须让 7>3 的 completed command 往返覆盖 tail 改善")
        comparison = report["reversal_comparisons"][0]
        expect(comparison["baseline_reversals"] == 3 and
               comparison["candidate_reversals"] == 7 and
               comparison["absolute_limit"] == 3,
               "换向报告必须精确保留首次候选 7>3 类型失败")


def test_static_block_rejects_new_completed_command() -> None:
    with tempfile.TemporaryDirectory(prefix="xen-production-red-") as text:
        root = pathlib.Path(text)
        asset_id = "static-preservation-fixture"
        tail_b0 = [valid_row(
            index, outside, 3, block_id="TAIL", sequence_base=100,
            asset_id=asset_id) for index, outside in
            enumerate([True, True, False])]
        tail_c = [valid_row(
            index, outside, 3, block_id="TAIL", sequence_base=100,
            asset_id=asset_id) for index, outside in
            enumerate([True, False, False])]
        static_b0 = [valid_row(
            index, False, 3, block_id="RED-STATIC", sequence_base=200,
            asset_id=asset_id) for index in range(3)]
        static_c = json.loads(json.dumps(static_b0))
        set_completed_x(static_c[1], 1)
        common = {
            "role": "development",
            "plant_profile_id": "P-LEGACY-D3-G05215",
        }
        definitions = [
            ({**common, "trace_id": "TAIL:B0", "block_id": "TAIL",
              "block_kind": "tail", "variant": "B0"}, tail_b0,
             measured_reference(tail_b0, 2, 2)),
            ({**common, "trace_id": "TAIL:C", "block_id": "TAIL",
              "block_kind": "tail", "variant": "C"}, tail_c, None),
            ({**common, "trace_id": "TAIL:C-D0", "block_id": "TAIL",
              "block_kind": "tail", "variant": "C-D0"},
             json.loads(json.dumps(tail_b0)), None),
            ({**common, "trace_id": "RED-STATIC:B0",
              "block_id": "RED-STATIC", "block_kind": "static",
              "variant": "B0"}, static_b0,
             measured_reference(static_b0, 0, 0)),
            ({**common, "trace_id": "RED-STATIC:C",
              "block_id": "RED-STATIC", "block_kind": "static",
              "variant": "C"}, static_c, None),
            ({**common, "trace_id": "RED-STATIC:C-D0",
              "block_id": "RED-STATIC", "block_kind": "static",
              "variant": "C-D0"}, json.loads(json.dumps(static_b0)), None),
        ]
        report = EVALUATOR.evaluate_manifest(write_multi_manifest(
            root, definitions, asset_id=asset_id))

        expect(report["status"] == "CANDIDATE_DELETED" and
               "STATIC_REGRESSION" in report["failure_codes"],
               "PBR-STATIC 必须拒绝静止 block 新增 completed command")


def test_cli_writes_hash_bound_report() -> None:
    with tempfile.TemporaryDirectory(prefix="xen-production-red-") as text:
        root = pathlib.Path(text)
        rows = [valid_row(index, outside, 2)
                for index, outside in enumerate([True, False])]
        manifest_path = write_manifest(
            root, rows, stem="cli",
            measured_reference=measured_reference(rows, 1, 1))
        output_path = root / "evaluation.json"
        completed = subprocess.run([
            str(pathlib.Path(sys.executable)),
            str(MODULE_PATH),
            "--manifest", str(manifest_path),
            "--output", str(output_path),
        ], check=False, capture_output=True, text=True)

        expect(completed.returncode == 0 and output_path.is_file(),
               "公开 CLI 必须原子写出 evaluator report")
        report = json.loads(output_path.read_text(encoding="utf-8"))
        claimed = report.pop("evaluation_semantic_sha256")
        expect(report["status"] == "BASELINE_RED_LOCKED" and
               report["manifest_file_sha256"] == file_sha256(manifest_path) and
               report["schema_contract_semantic_sha256"] ==
               EVALUATOR.red_schema_contract()["contract_semantic_sha256"] and
               claimed == canonical_sha256(report),
               "CLI report 必须绑定 manifest、schema contract 与自身语义哈希")


def test_source_order_and_clock_session_are_fail_closed() -> None:
    with tempfile.TemporaryDirectory(prefix="xen-production-red-") as text:
        root = pathlib.Path(text)
        rows = [valid_row(index, index == 0, 2) for index in range(2)]
        rows[1]["source_sequence"] = rows[0]["source_sequence"]
        rows[1]["source_clock_session_id"] = "synthetic:other-session"
        report = EVALUATOR.evaluate_manifest(write_manifest(
            root, rows, stem="source-order",
            measured_reference=measured_reference(rows, 1, 1)))

        expect(report["status"] == "RED_INVALID" and
               "SOURCE_SEQUENCE_NOT_STRICTLY_INCREASING" in
               report["failure_codes"] and
               "SOURCE_CLOCK_SESSION_MIXED" in report["failure_codes"],
               "PBR-SOURCE 必须拒绝重复 sequence 与跨 session 混杂")


def test_any_physical_capability_is_rejected() -> None:
    with tempfile.TemporaryDirectory(prefix="xen-production-red-") as text:
        root = pathlib.Path(text)
        rows = [valid_row(0, True, 1)]
        path = write_manifest(
            root, rows, stem="physical-capability",
            measured_reference=measured_reference(rows, 1, 1))
        manifest = json.loads(path.read_text(encoding="utf-8"))
        manifest["physical_output_capability"] = True
        manifest["physical_dispatch_count"] = 1
        path.write_text(json.dumps(manifest), encoding="utf-8")

        report = EVALUATOR.evaluate_manifest(path)

        expect(report["status"] == "RED_INVALID" and
               report["failure_codes"] == [
                   "OUTPUT_OFF_SAFETY_VIOLATION"],
               "evaluator manifest 只要具有物理能力或 dispatch 就必须拒绝")


def test_all_frozen_plant_profiles_must_pass() -> None:
    with tempfile.TemporaryDirectory(prefix="xen-production-red-") as text:
        root = pathlib.Path(text)
        asset_id = "plant-envelope-fixture"

        def rows(pattern: list[bool]) -> list[dict]:
            return [valid_row(
                index, outside, 4, block_id="TAIL",
                asset_id=asset_id) for index, outside in enumerate(pattern)]

        legacy_b0 = rows([True, True, False, False])
        legacy_c = rows([True, False, False, False])
        f1_b0 = rows([True, True, False, False])
        f1_c = rows([True, True, True, False])
        for row in [*f1_b0, *f1_c]:
            row["plant_profile_id"] = "P-F1-MID"
        definitions = []
        for profile, baseline, candidate in (
                ("P-LEGACY-D3-G05215", legacy_b0, legacy_c),
                ("P-F1-MID", f1_b0, f1_c)):
            common = {
                "block_id": "TAIL",
                "role": "development",
                "block_kind": "tail",
                "plant_profile_id": profile,
            }
            definitions.extend([
                ({**common, "trace_id": f"TAIL:{profile}:B0",
                  "variant": "B0"}, baseline,
                 measured_reference(baseline, 2, 2)),
                ({**common, "trace_id": f"TAIL:{profile}:C",
                  "variant": "C"}, candidate, None),
            ])
        report = EVALUATOR.evaluate_manifest(write_multi_manifest(
            root, definitions, asset_id=asset_id))

        expect(report["status"] == "CANDIDATE_DELETED" and
               report["failure_codes"] == ["MODEL_MISMATCH_RED"],
               "PBR-F1-ENVELOPE 必须拒绝只在部分冻结 profile 改善的候选: " +
               json.dumps(report, ensure_ascii=False, sort_keys=True))
        by_profile = {item["plant_profile_id"]: item["tail_pass"]
                      for item in report["block_comparisons"]}
        expect(by_profile == {
            "P-LEGACY-D3-G05215": True,
            "P-F1-MID": False,
        }, "profile 结论必须逐项保存而不是挑选 winner")


def test_f1_off_must_have_deletion_leverage() -> None:
    with tempfile.TemporaryDirectory(prefix="xen-production-red-") as text:
        root = pathlib.Path(text)
        asset_id = "f1-deletion-fixture"
        baseline = [valid_row(
            index, outside, 3, block_id="TAIL", asset_id=asset_id)
            for index, outside in enumerate([True, True, False])]
        candidate = [valid_row(
            index, outside, 3, block_id="TAIL", asset_id=asset_id)
            for index, outside in enumerate([True, False, False])]
        common = {
            "block_id": "TAIL",
            "role": "development",
            "block_kind": "tail",
            "plant_profile_id": "P-F1-MID",
        }
        for row in [*baseline, *candidate]:
            row["plant_profile_id"] = "P-F1-MID"
        definitions = [
            ({**common, "trace_id": "TAIL:B0", "variant": "B0"},
             baseline, measured_reference(baseline, 2, 2)),
            ({**common, "trace_id": "TAIL:C", "variant": "C"},
             candidate, None),
            ({**common, "trace_id": "TAIL:C-F1-OFF",
              "variant": "C-F1-OFF"},
             json.loads(json.dumps(candidate)), None),
            ({**common, "trace_id": "TAIL:C-D0", "variant": "C-D0"},
             json.loads(json.dumps(baseline)), None),
        ]
        report = EVALUATOR.evaluate_manifest(write_multi_manifest(
            root, definitions, asset_id=asset_id, candidate_uses_f1=True))

        expect(report["status"] == "CANDIDATE_DELETED" and
               report["failure_codes"] == ["F1_DELETION_NO_LEVERAGE"],
               "PBR-F1-OFF 必须删除和 F1-off 没有可复现差异的候选")


def test_valid_visible_candidate_stops_at_development_green() -> None:
    with tempfile.TemporaryDirectory(prefix="xen-production-red-") as text:
        root = pathlib.Path(text)
        asset_id = "development-green-fixture"
        baseline = [valid_row(
            index, outside, 3, block_id="TAIL", asset_id=asset_id)
            for index, outside in enumerate([True, True, False])]
        candidate = [valid_row(
            index, outside, 3, block_id="TAIL", asset_id=asset_id)
            for index, outside in enumerate([True, False, False])]
        common = {
            "block_id": "TAIL",
            "role": "development",
            "block_kind": "tail",
            "plant_profile_id": "P-LEGACY-D3-G05215",
        }
        report = EVALUATOR.evaluate_manifest(write_multi_manifest(root, [
            ({**common, "trace_id": "TAIL:B0", "variant": "B0"},
             baseline, measured_reference(baseline, 2, 2)),
            ({**common, "trace_id": "TAIL:C", "variant": "C"},
             candidate, None),
            ({**common, "trace_id": "TAIL:C-D0", "variant": "C-D0"},
             json.loads(json.dumps(baseline)), None),
        ], asset_id=asset_id))

        expect(report["status"] == "CANDIDATE_DEVELOPMENT_GREEN" and
               report["failure_codes"] == [],
               "可见 blocks 全过也只能停在 development green，不能冒充 holdout")


def test_declared_mandatory_baseline_pair_cannot_be_omitted() -> None:
    with tempfile.TemporaryDirectory(prefix="xen-production-red-") as text:
        root = pathlib.Path(text)
        rows = [valid_row(0, True, 1)]
        path = write_manifest(
            root, rows, stem="mandatory-pair",
            measured_reference=measured_reference(rows, 1, 1))
        manifest = json.loads(path.read_text(encoding="utf-8"))
        manifest["mandatory_block_profiles"].append({
            "block_id": "RED-STATIC",
            "plant_profile_id": "P-LEGACY-D3-G05215",
        })
        path.write_text(json.dumps(manifest), encoding="utf-8")

        report = EVALUATOR.evaluate_manifest(path)

        expect(report["status"] == "RED_INPUT_INCOMPLETE" and
               report["failure_codes"] == [
                   "MANDATORY_BASELINE_TRACE_MISSING"],
               "manifest 已冻结的 mandatory block/profile 不得被静默省略")


def main() -> int:
    test_missing_source_and_completed_ledgers_fail_closed()
    test_run_order_is_preserved_in_longest_outside_metric()
    test_block_regression_cannot_be_hidden_by_aggregate_improvement()
    test_unfaithful_six_three_replay_is_rejected_before_candidate()
    test_requested_or_ack_cannot_leak_into_actual_plant_input()
    test_schema_contract_is_versioned_and_hash_bound()
    test_source_asset_hash_drift_is_invalid()
    test_candidate_off_must_reproduce_baseline_rows_exactly()
    test_x_candidate_cannot_change_y_ledger()
    test_completed_command_reversal_absolute_gate_cannot_be_masked()
    test_static_block_rejects_new_completed_command()
    test_cli_writes_hash_bound_report()
    test_source_order_and_clock_session_are_fail_closed()
    test_any_physical_capability_is_rejected()
    test_all_frozen_plant_profiles_must_pass()
    test_f1_off_must_have_deletion_leverage()
    test_valid_visible_candidate_stops_at_development_green()
    test_declared_mandatory_baseline_pair_cannot_be_omitted()
    print("aim production red evaluator tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
