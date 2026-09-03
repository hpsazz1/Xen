#!/usr/bin/env python3
"""Physical B holdout Prepare 的离线集成合同。"""

from __future__ import annotations

import argparse
import copy
import hashlib
import importlib.util
import json
import pathlib
import shutil
import subprocess
import sys
import uuid


def load_module(path: pathlib.Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"无法加载测试模块: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path: pathlib.Path, value: dict) -> None:
    path.write_text(
        json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n",
        encoding="utf-8",
    )


def run_prepare(
        powershell: pathlib.Path,
        script: pathlib.Path,
        arguments: dict[str, str]) -> subprocess.CompletedProcess[str]:
    command = [
        str(powershell), "-NoProfile", "-ExecutionPolicy", "Bypass",
        "-File", str(script),
    ]
    for name, value in arguments.items():
        command.extend((f"-{name}", value))
    return subprocess.run(
        command,
        text=True,
        encoding="utf-8",
        errors="replace",
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--prepare-script", type=pathlib.Path, required=True)
    parser.add_argument("--tool-root", type=pathlib.Path, required=True)
    parser.add_argument("--python", type=pathlib.Path, required=True)
    parser.add_argument("--powershell", type=pathlib.Path, required=True)
    parser.add_argument("--test-root", type=pathlib.Path, required=True)
    options = parser.parse_args()

    root = options.test_root.resolve()
    root.mkdir(parents=True, exist_ok=True)
    case = root / f"case-{uuid.uuid4().hex}"
    inputs = case / "inputs"
    inputs.mkdir(parents=True)
    test_helpers = load_module(
        pathlib.Path(__file__).resolve().with_name(
            "mouse_effect_probe_b_holdout_analysis_tests.py"),
        "mouse_effect_probe_b_holdout_test_helpers",
    )
    holdout_analyzer = test_helpers.HOLDOUT
    sequence = test_helpers.frozen_holdout_sequence()
    f1 = test_helpers.frozen_f1()
    primary_analyzer_sha = sha256(
        options.prepare_script.resolve().with_name(
            "analyze_mouse_effect_probe_b.py"))
    f1["bindings"]["analyzer_file_sha256"] = primary_analyzer_sha
    f1["f1_semantic_sha256"] = test_helpers.canonical(
        f1, "f1_semantic_sha256")

    config = "\n".join((
        "[capture]",
        "backend=ndi",
        "ndi_source_name=HPSAZZ (Xen-ROI-320)",
        "ndi_clock_sync_url=udp://192.168.3.10:5011",
        "ndi_frame_layout=center_crop_1_to_1",
        "ndi_source_width=2560",
        "ndi_source_height=1440",
        "roi_width=320",
        "roi_height=320",
        "center_roi=true",
        "roi_x=0",
        "roi_y=0",
        "ndi_discovery_timeout_ms=10000",
        "ndi_receive_timeout_ms=50",
        "ndi_disconnect_timeout_ms=2000",
        "ndi_clock_sync_interval_ms=250",
        "ndi_clock_sync_timeout_ms=200",
        "ndi_clock_mapping_max_age_ms=1000",
        "ndi_require_frame_metadata=false",
        "",
        "[mouse]",
        "backend=kmbox_net",
        "allow_send_input=false",
        "",
    ))
    config_path = inputs / "config.ini"
    config_path.write_text(config, encoding="utf-8")
    obs_path = inputs / "obs-source-binding.json"
    write_json(obs_path, {
        "evidence_type": "test_obs_source_binding",
        "source_name": "HPSAZZ (Xen-ROI-320)",
    })

    report = {
        "run_uuid": f1["bindings"]["run_uuid"],
        "activation_epoch": f1["bindings"]["activation_epoch"],
        "result": {"events": [
            {
                "source_clock_session_id": "primary-source-session",
                "source_time_at_steady_ns": 1000,
                "source_timestamp": 10000,
            },
            {
                "source_clock_session_id": "primary-source-session",
                "source_time_at_steady_ns": 2000,
                "source_timestamp": 20000,
            },
        ]},
    }
    report_path = inputs / "command-report.json"
    write_json(report_path, report)
    analysis = {
        "schema_version": 2,
        "evidence_type": "mouse_effect_probe_physical_b_primary_analysis",
        "status": "PRIMARY_CORE_ONLY",
        "physical_output_capability": False,
        "production_aim_changed": False,
        "run_uuid": f1["bindings"]["run_uuid"],
        "activation_epoch": f1["bindings"]["activation_epoch"],
        "scope_id": "scope-fixture",
        "profile": "physical_b_prbs_primary",
        "f1": f1,
        "bindings": {
            "command_report_file_sha256": sha256(report_path),
            "analyzer_file_sha256": primary_analyzer_sha,
        },
        "cross_run_holdout_prepare_authorized": False,
        "holdout_used_for_tuning": False,
    }
    analysis["analysis_semantic_sha256"] = \
        test_helpers.canonical(analysis, "analysis_semantic_sha256")
    analysis_path = inputs / "physical-b-primary-analysis.json"
    write_json(analysis_path, analysis)
    offline = {
        "schema_version": 2,
        "evidence_type": "mouse_effect_probe_physical_b_offline_design",
        "status": "VALID_OFFLINE_DESIGN",
        "physical_output_capability": False,
        "physical_b_launch_authorized": False,
        "production_aim_changed": False,
        "cross_run_holdout_candidate": {
            "role": "cross_run_holdout",
            "input_definition": "cumulative_position_counts",
            "lfsr": f1["holdout"]["lfsr"],
            "sequence": sequence,
        },
    }
    offline["design_semantic_sha256"] = \
        test_helpers.canonical(offline, "design_semantic_sha256")
    offline_path = inputs / "offline-design.json"
    write_json(offline_path, offline)

    run = case / "holdout"
    arguments = {
        "ToolRoot": str(options.tool_root.resolve()),
        "PythonExecutable": str(options.python.resolve()),
        "ConfigPath": str(config_path),
        "ObsSourceBindingPath": str(obs_path),
        "PrimaryAnalysisPath": str(analysis_path),
        "ExpectedPrimaryAnalysisSha256": sha256(analysis_path),
        "OfflineDesignPath": str(offline_path),
        "ExpectedOfflineDesignSha256": sha256(offline_path),
        "RunDirectory": str(run),
        "PublishedRunDirectory": str(run),
        "PrepareAuthorization":
            "XEN_MOUSE_EFFECT_PROBE_B_HOLDOUT_PREPARE_ONLY",
    }
    completed = run_prepare(
        options.powershell.resolve(), options.prepare_script.resolve(), arguments)
    if completed.returncode != 0:
        raise AssertionError(
            f"Physical B holdout Prepare 失败: {completed.stdout}")

    task = json.loads((run / "task.json").read_text(encoding="utf-8-sig"))
    plan = json.loads((run / "holdout-plan.json").read_text(encoding="utf-8-sig"))
    generated = json.loads((run / "sequence.json").read_text(encoding="utf-8-sig"))
    binding = json.loads((run / "probe-binding.json").read_text(
        encoding="utf-8-sig"))
    summary = json.loads((run / "prepare-summary.json").read_text(
        encoding="utf-8-sig"))
    if not (
            task["schema_version"] == 7 and
            task["status"] == "PREPARED" and
            task["profile"] == "physical_b_prbs_holdout" and
            task["run_role"] == "cross_run_holdout" and
            task["cross_run_holdout_prepare_authorized"] is True and
            task["holdout_used_for_tuning"] is False and
            task["physical_output_confirmation"] ==
            "XEN_MOUSE_EFFECT_PROBE_B_HOLDOUT_SENDS_REAL_KMBOX_INPUT" and
            task["sequence_sample_count"] == 288 and
            task["expected_nonzero_transition_count"] == 68 and
            task["run_uuid"] != plan["primary"]["run_uuid"] and
            task["activation_epoch"] != plan["primary"]["activation_epoch"] and
            plan["schema_version"] == 2 and
            plan["status"] == "READY_FOR_PHYSICAL_B_HOLDOUT_PREPARE" and
            plan["primary"]["source_clock_session_id"] ==
            "primary-source-session" and
            plan["primary"]["timing_observation"] == {
                "identity_basis":
                    "run_uuid_activation_epoch_source_time_range",
                "source_clock_session_id": "primary-source-session",
                "source_time_at_steady_ns": {"first": 1000, "last": 2000},
                "source_timestamp": {"first": 10000, "last": 20000},
            } and
            task["primary"]["timing_observation"] ==
            plan["primary"]["timing_observation"] and
            binding["primary_timing_observation"] ==
            plan["primary"]["timing_observation"] and
            task["holdout"]["independence_contract_semantic_sha256"] ==
            plan["contract"]["contract_semantic_sha256"] and
            task["files"]["physical_b_analyzer"]["sha256"] ==
            plan["bindings"]["primary_analyzer_file_sha256"] and
            task["files"]["primary_command_report"]["sha256"] ==
            plan["bindings"]["primary_command_report_file_sha256"] and
            generated["profile"] == "physical_b_prbs_holdout" and
            len(generated["samples"]) == 288 and
            len(generated["blocks"]) == 2 and
            summary["status"] == "PREPARED_NOT_LAUNCHED" and
            summary["independence_contract_semantic_sha256"] ==
            plan["contract"]["contract_semantic_sha256"] and
            summary["physical_launch_executed"] is False):
        raise AssertionError("holdout task/plan/sequence/summary 合同不闭合")
    for name, identity in task["files"].items():
        path = pathlib.Path(identity["path"])
        if not path.is_file() or path.stat().st_size != identity["size"] or \
                sha256(path) != identity["sha256"]:
            raise AssertionError(f"holdout file identity 不闭合: {name}")
    task_markdown = (run / "TASK.md").read_text(encoding="utf-8-sig")
    if "-AllowPhysicalOutput" not in task_markdown or \
            "XEN_MOUSE_EFFECT_PROBE_B_HOLDOUT_SENDS_REAL_KMBOX_INPUT" \
            not in task_markdown or \
            "只能由用户在本机前台执行" not in task_markdown:
        raise AssertionError("holdout TASK.md 缺少唯一用户前台 Launch 命令")
    for forbidden in (
            "command-report.json", "safety-ledger.json", "launch-summary.json",
            "pixel-evidence"):
        if (run / forbidden).exists():
            raise AssertionError(f"Prepare 不得产生 Launch 产物: {forbidden}")

    unauthorized = case / "unauthorized"
    bad = dict(arguments)
    bad["RunDirectory"] = str(unauthorized)
    bad["PublishedRunDirectory"] = str(unauthorized)
    bad["PrepareAuthorization"] = "WRONG"
    rejected = run_prepare(
        options.powershell.resolve(), options.prepare_script.resolve(), bad)
    if rejected.returncode == 0 or unauthorized.exists():
        raise AssertionError("错误 Prepare token 必须在创建 Run 前拒绝")

    wrong_hash = case / "wrong-hash"
    bad = dict(arguments)
    bad["RunDirectory"] = str(wrong_hash)
    bad["PublishedRunDirectory"] = str(wrong_hash)
    bad["ExpectedPrimaryAnalysisSha256"] = "0" * 64
    rejected = run_prepare(
        options.powershell.resolve(), options.prepare_script.resolve(), bad)
    incoming = list(case.glob(".wrong-hash.incoming-*"))
    if rejected.returncode == 0 or wrong_hash.exists() or incoming:
        raise AssertionError("错误 Primary hash 必须拒绝且不遗留 staging")

    analyzer_mismatch = case / "analyzer-mismatch"
    mismatched_analysis = copy.deepcopy(analysis)
    mismatched_analysis["bindings"]["analyzer_file_sha256"] = "b" * 64
    mismatched_analysis["analysis_semantic_sha256"] = test_helpers.canonical(
        mismatched_analysis, "analysis_semantic_sha256")
    mismatched_analysis_path = inputs / "mismatched-primary-analysis.json"
    write_json(mismatched_analysis_path, mismatched_analysis)
    bad = dict(arguments)
    bad["RunDirectory"] = str(analyzer_mismatch)
    bad["PublishedRunDirectory"] = str(analyzer_mismatch)
    bad["PrimaryAnalysisPath"] = str(mismatched_analysis_path)
    bad["ExpectedPrimaryAnalysisSha256"] = sha256(mismatched_analysis_path)
    rejected = run_prepare(
        options.powershell.resolve(), options.prepare_script.resolve(), bad)
    incoming = list(case.glob(".analyzer-mismatch.incoming-*"))
    if rejected.returncode == 0 or analyzer_mismatch.exists() or incoming:
        raise AssertionError(
            "Primary analyzer 绑定漂移必须拒绝且不遗留 staging")

    # 只清理本测试拥有的 UUID case；不触碰其他构建或 Run。
    shutil.rmtree(case)
    print("Physical B holdout Prepare integration passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
