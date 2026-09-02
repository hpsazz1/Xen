import importlib.util
import hashlib
import json
import math
import pathlib
import sys
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "scripts" / "design_mouse_effect_probe_prbs.py"
SPEC = importlib.util.spec_from_file_location("probe_prbs_design", MODULE_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("无法加载 mouse effect probe PRBS designer")
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def semantic_sha256(value: dict, field: str) -> str:
    payload = dict(value)
    payload.pop(field, None)
    return hashlib.sha256(json.dumps(
        payload,
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
        allow_nan=False,
    ).encode("utf-8")).hexdigest()


def json_bytes(value: dict) -> bytes:
    return (json.dumps(value, ensure_ascii=False, indent=2,
                       allow_nan=False) + "\n").encode("utf-8")


def valid_physical_a_analysis() -> dict:
    return {
        "status": "VALID",
        "machine_visible_effect_observed": True,
        "human_physical_acceptance": "NOT_INFERRED_BY_ANALYZER",
        "method": {"timestamp_semantic": "NDI_SDK_SUBMISSION_NOT_EXPOSURE"},
        "run_binding": {
            "run_uuid": "fixture-run",
            "sequence_sha256": "a" * 64,
        },
        "geometry": {
            "left_roi": {"left_margin_px": 16, "right_margin_px": 208},
            "right_roi": {"left_margin_px": 208, "right_margin_px": 16},
        },
        "zero_input_baseline": {
            "left_exact_state_count": 1,
            "right_exact_state_count": 1,
        },
        "pulse_responses": [
            {"onset": {"first_changed_frame_lag": 4},
             "x_px_per_count": 0.55}
            for _ in range(4)
        ],
        "witness_state_summary": {"statistical_independence_claimed": False},
    }


def valid_a2_dependency_decision() -> dict:
    return {
        "schema_version": 1,
        "evidence_type": "mouse_effect_probe_a2_dependency_holdout_decision",
        "status": "A2_DEPENDENCY_GREEN",
        "invalid_reasons": [],
        "physical_output_capability": False,
        "production_aim_changed": False,
        "run_role": "p-holdout",
        "profile": "dependency_calibration_a2_p_holdout",
        "scope_id": "b" * 64,
        "run_uuid": "6056cd77-5e96-4276-9278-7e3b6c6ea0a2",
        "sequence_sha256": "c" * 64,
        "candidate_sha256": "d" * 64,
        "candidate_run_uuid": "f00d86dc-9d2f-4d5b-9dfb-eec2bc3c56d0",
        "candidate_values_changed": False,
        "holdout_used_for_tuning": False,
        "a2_dependency_gate_claimed": True,
        "physical_b_authorized": False,
        "independence": {
            "different_run_uuid": True,
            "different_activation_epoch": True,
            "different_sidecar_manifest": True,
            "same_analyzer": True,
            "same_capture_source": True,
        },
        "human_observation": {
            "observation_sha256": "e" * 64,
            "visible_effect_reported": True,
            "manual_mouse_or_wasd_used": False,
            "left_right_witness_consistent": True,
            "occlusion_or_scene_cut_reported": False,
            "anomaly_or_emergency_stop_reported": False,
        },
        "comparisons": {
            "tail_support": {
                "candidate_upper_lag": 7,
                "holdout_observed_upper_lag": 5,
                "passed": True,
            },
            "mapping_uncertainty": {
                "candidate_upper_px": 1.342895110591,
                "holdout_upper_px": 1.332698341823,
                "passed": True,
            },
            "single_count_gain_upper_scope": {
                "candidate_upper_px": 1.342895105713,
                "holdout_upper_px": 1.332698363329,
                "passed": True,
            },
            "witness_occlusion_margin": {
                "candidate_usable_margin_lower_px": 14.657104894287,
                "holdout_usable_margin_lower_px": 14.667301636671,
                "passed": True,
            },
            "physical_b_prefix_candidate": {
                "candidate_allowed_prefix_counts": 9,
                "holdout_allowed_prefix_counts": 10,
                "passed": True,
                "physical_b_authorized": False,
            },
        },
    }


def test_maximum_length_period_uses_explicit_recurrence() -> None:
    bits = MODULE.generate_maximum_length_period(
        order=3,
        feedback_mask=0x3,
        seed=1,
        phase=0,
    )
    expect(bits == [1, 0, 0, 1, 0, 1, 1],
           "固定 LSB 输出/右移 recurrence 必须生成已知 7-bit 完整周期")


def test_candidate_definitions_do_not_mix_command_and_position() -> None:
    bits = [1, 0, 0, 1, 0, 1, 1]
    direct = MODULE.build_candidate_sequence(
        period_bits=bits,
        input_definition="direct_command_counts",
        guard_sample_count=2,
        pair_repetitions=1,
        role="estimation",
    )
    position = MODULE.build_candidate_sequence(
        period_bits=bits,
        input_definition="cumulative_position_counts",
        guard_sample_count=2,
        pair_repetitions=1,
        role="estimation",
    )

    expect(direct["period"]["identification_input"] ==
           [1, -1, -1, 1, -1, 1, 1],
           "direct-command 定义必须把 bipolar PRBS 直接发布为相对命令")
    expect(position["period"]["identification_input"] == bits,
           "position 定义必须把二值 PRBS 解释为累计位置状态")
    expect(position["period"]["inverted_identification_input"] ==
           [-1, 0, 0, -1, 0, -1, -1],
           "position 反相周期必须是 0/-1 的精确符号反相")
    expect(direct["summary"]["net_command_dx_counts"] == 0 and
           direct["summary"]["max_abs_position_x_counts"] == 2,
           "direct-command 完整正/反相周期必须净零并保留真实前缀风险")
    expect(position["summary"]["net_command_dx_counts"] == 0 and
           position["summary"]["max_abs_position_x_counts"] == 1,
           "position 差分命令必须回零并把任意前缀限制为 1 count")
    expect(all(sample["command_dx_counts"] in (-1, 0, 1) and
               sample["command_dy_counts"] == 0
               for sample in position["samples"]),
           "position PRBS 的因果差分不得产生 ±2 count 或任何 Y 命令")


def test_period_audit_reports_exact_rank_singular_values_and_frequency() -> None:
    direct = MODULE.audit_identification_period(
        [1, -1, -1, 1, -1, 1, 1],
        horizons=[4, 7, 8],
    )
    position = MODULE.audit_identification_period(
        [1, 0, 0, 1, 0, 1, 1],
        horizons=[4],
    )

    direct_h4 = direct["horizons"][0]["circular_window_matrix"]
    expect(direct_h4["shape"] == [7, 4] and
           direct_h4["rank"] == 4 and
           direct_h4["full_column_rank"] is True,
           "完整 bipolar 周期的 H=4 circular window matrix 必须满列秩")
    expect(all(abs(value - expected) < 1e-12 for value, expected in zip(
               direct_h4["singular_values"],
               [math.sqrt(8.0), math.sqrt(8.0), math.sqrt(8.0), 2.0])) and
           abs(direct_h4["condition_number"] - math.sqrt(2.0)) < 1e-12,
           "H=4 singular values/condition 必须对应已知 m-sequence Gram 矩阵")
    expect(direct["frequency"]["dc_power"] == 1.0 and
           direct["frequency"]["non_dc_line_count"] == 6 and
           abs(direct["frequency"]["non_dc_power_min"] - 8.0) < 1e-12 and
           abs(direct["frequency"]["non_dc_power_max"] - 8.0) < 1e-12,
           "bipolar maximum-length period 必须报告完整非 DC 频线能量")
    expect(direct["horizons"][2]["circular_window_matrix"]
           ["full_column_rank"] is False,
           "H 大于完整 period 行数时必须显式拒绝满列秩声明")
    expect(all("causal_zero_guard_convolution_matrix" not in horizon
               for horizon in direct["horizons"]),
           "通用 period 审计不得构造会静默补零未知历史的 causal matrix")
    expect(position["frequency"]["dc_power"] == 16.0 and
           abs(position["frequency"]["non_dc_power_min"] - 2.0) < 1e-12,
           "0/1 position period 必须保留 DC 与非 DC 频带分账")


def test_design_selects_bounded_position_input_and_keeps_prepare_blocked() -> None:
    analysis = {
        "schema_version": 1,
        "evidence_type": "mouse_effect_probe_physical_background_response",
        "status": "VALID",
        "machine_visible_effect_observed": True,
        "human_physical_acceptance": "NOT_INFERRED_BY_ANALYZER",
        "method": {
            "timestamp_semantic": "NDI_SDK_SUBMISSION_NOT_EXPOSURE",
        },
        "run_binding": {
            "run_uuid": "fixture-run",
            "sequence_sha256": "a" * 64,
        },
        "geometry": {
            "image_width": 320,
            "image_height": 320,
            "left_roi": {
                "x": 16, "y": 48, "width": 96, "height": 224,
                "left_margin_px": 16, "right_margin_px": 208,
                "top_margin_px": 48, "bottom_margin_px": 48,
            },
            "right_roi": {
                "x": 208, "y": 48, "width": 96, "height": 224,
                "left_margin_px": 208, "right_margin_px": 16,
                "top_margin_px": 48, "bottom_margin_px": 48,
            },
        },
        "zero_input_baseline": {
            "left_exact_state_count": 1,
            "right_exact_state_count": 1,
        },
        "pulse_responses": [
            {
                "command_dx_counts": command,
                "onset": {"first_changed_frame_lag": 4},
                "x_px_per_count": gain,
            }
            for command, gain in ((1, 0.55), (-1, 0.55),
                                  (-1, 0.52), (1, 0.52))
        ],
        "witness_state_summary": {
            "transition_observation_count": 4,
            "unique_exact_state_count": 3,
            "distinct_excursion_count": 2,
            "statistical_independence_claimed": False,
        },
    }
    design = MODULE.design_physical_b_candidates(
        analysis,
        orders=[5, 6, 7],
        horizons=[4, 8, 16, 32],
        guard_sample_count=32,
    )

    selected = design["selected_candidate"]
    expect(selected["input_definition"] == "cumulative_position_counts" and
           selected["lfsr"]["order"] == 6 and
           selected["lfsr"]["period_sample_count"] == 63 and
           selected["lfsr"]["feedback_mask"] == 0x27 and
           selected["lfsr"]["phase"] == 49,
           "安全/短周期优先后必须穷举 order=6 recurrence/phase 并按 exact schedule 择优")
    expect(selected["sequence"]["summary"]["max_abs_position_x_counts"] == 1 and
           selected["sequence"]["summary"]["net_command_dx_counts"] == 0 and
           len(selected["sequence"]["sequence_semantic_sha256"]) == 64,
           "选中候选必须前缀 1 count、完整回零并绑定 exact sequence SHA")

    order5 = next(candidate for candidate in design["candidates"]
                  if candidate["input_definition"] ==
                     "cumulative_position_counts" and
                     candidate["lfsr"]["order"] == 5)
    direct6 = next(candidate for candidate in design["candidates"]
                   if candidate["input_definition"] ==
                      "direct_command_counts" and
                      candidate["lfsr"]["order"] == 6)
    expect(order5["all_horizons_full_rank"] is False and
           direct6["all_horizons_full_rank"] is True and
           direct6["sequence"]["summary"]["max_abs_position_x_counts"] == 5,
           "31-sample 候选必须因 H=32 失败，direct command 必须保留 5-count 前缀事实")

    safety = design["physical_prepare_gate"]
    expect(safety["ready"] is False and
           safety["witness_geometry"]["image_edge_margin_px"] == 16.0 and
           abs(safety["witness_geometry"]
               ["run_local_prefix_displacement_reference_px"] - 0.55) < 1e-12 and
           safety["allowed_prefix_counts"] is None and
           set(safety["blockers"]) == {
               "INDEPENDENT_NONDEGENERATE_NOISE_MISSING",
               "INDEPENDENT_TAIL_SUPPORT_MISSING",
               "OCCLUSION_MARGIN_MISSING",
               "MAPPING_UNCERTAINTY_PX_MISSING",
               "GENERAL_GAIN_UPPER_BOUND_MISSING",
           },
           "A 级退化 baseline 只能给 reference margin，不能推导 Physical 安全额度")
    expect(design["guard"]["candidate_sample_count"] == 32 and
           design["guard"]["covers_observed_lag"] is True and
           design["guard"]["tail_support_proven"] is False,
           "guard 只能记录候选包络，不能冒充已测 tail 上界")
    recurrence = design["recurrence_selection"]
    expect(recurrence["evaluated_recurrence_phase_count"] == 378 and
           recurrence["condition_round_decimal_places"] == 12 and
           recurrence["primary"]["feedback_mask"] == 0x27 and
           recurrence["primary"]["phase"] == 49,
           "recurrence selector 必须记录完整搜索规模、数值平局规则和主候选")
    expect(design["cross_run_holdout_candidate"]["lfsr"]["feedback_mask"] ==
           0x33 and
           design["cross_run_holdout_candidate"]["lfsr"]["phase"] == 21 and
           design["cross_run_holdout_candidate"]["role"] ==
           "cross_run_holdout",
           "跨 Run 留出必须预注册次优且不同的 primitive recurrence 并保留完整 block")

    schedule = selected["exact_schedule_audit"]
    expect(schedule["all_input_matrices_full_column_rank"] is True and
           schedule["all_augmented_designs_identifiable"] is True,
           "选中 exact schedule 的每个 block/H 必须同时通过 U_H 与 nuisance 增广 rank")
    expect(all(block["prehistory_source"] == "exact_sequence_samples" and
               block["unknown_prehistory_sample_count"] == 0
               for horizon in schedule["horizons"]
               for block in horizon["blocks"]),
           "实际 block matrix 必须消费确切 pre-guard，不得静默 zero-pad 未知历史")
    expect(all(block["input_matrix"]["full_column_rank"] and
               block["augmented_design_matrix"]["rank"] ==
               block["nuisance_matrix"]["rank"] + horizon["horizon"]
               for horizon in schedule["horizons"]
               for block in horizon["blocks"]),
           "每个 block/H 的 [D,U_H] 必须增加恰好 H 个可辨识方向")
    frequencies = schedule["frequency"]
    expect(abs(frequencies["identification_input_full_sequence"]
               ["dc_power"]) < 1e-12 and
           abs(frequencies["actual_command_full_sequence"]
               ["dc_power"]) < 1e-12,
           "完整 p/-p 设计必须分别报告 position input 与 difference command 的净零 DC")
    first_block = frequencies["blocks"][0]
    expect(first_block["net_command_dx_counts"] == 0 and
           first_block["returns_to_block_anchor"] is True and
           first_block["identification_input"]["dc_power"] == 1024.0 and
           abs(first_block["actual_command"]["dc_power"]) < 1e-12,
           "position block 必须显式回零，并分账 p 的 DC 与差分 command 的零 DC")
    expect(direct6["exact_schedule_audit"]["all_blocks_return_to_anchor"]
           is False,
           "direct-command 单 period 净 ±1，只能作共享位置状态的对照块")


def test_cli_binds_source_and_refuses_artifact_overwrite() -> None:
    analysis = {
        "status": "VALID",
        "machine_visible_effect_observed": True,
        "human_physical_acceptance": "NOT_INFERRED_BY_ANALYZER",
        "method": {"timestamp_semantic": "NDI_SDK_SUBMISSION_NOT_EXPOSURE"},
        "run_binding": {"run_uuid": "cli-fixture", "sequence_sha256": "b" * 64},
        "geometry": {
            "left_roi": {"left_margin_px": 16, "right_margin_px": 208},
            "right_roi": {"left_margin_px": 208, "right_margin_px": 16},
        },
        "zero_input_baseline": {
            "left_exact_state_count": 1, "right_exact_state_count": 1,
        },
        "pulse_responses": [
            {"onset": {"first_changed_frame_lag": 4},
             "x_px_per_count": 0.55}
            for _ in range(4)
        ],
        "witness_state_summary": {"statistical_independence_claimed": False},
    }
    with tempfile.TemporaryDirectory(prefix="xen-probe-prbs-") as directory:
        root = pathlib.Path(directory)
        source = root / "physical-a.json"
        output = root / "physical-b-design.json"
        source_bytes = (json.dumps(analysis, ensure_ascii=False, indent=2) +
                        "\n").encode("utf-8")
        source.write_bytes(source_bytes)
        arguments = [
            "--physical-a-analysis", str(source),
            "--output", str(output),
            "--orders", "5,6,7",
            "--horizons", "4,8,16,32",
            "--guard-samples", "32",
        ]
        expect(MODULE.main(arguments) == 0 and output.is_file(),
               "正式 CLI 必须生成只读离线设计 artifact")
        artifact_bytes = output.read_bytes()
        artifact = json.loads(artifact_bytes)
        design_payload = dict(artifact)
        claimed_design_sha = design_payload.pop("design_semantic_sha256")
        recomputed_design_sha = hashlib.sha256(json.dumps(
            design_payload,
            ensure_ascii=False,
            sort_keys=True,
            separators=(",", ":"),
            allow_nan=False,
        ).encode("utf-8")).hexdigest()
        selected_sequence = dict(
            artifact["selected_candidate"]["sequence"])
        claimed_sequence_sha = selected_sequence.pop(
            "sequence_semantic_sha256")
        recomputed_sequence_sha = hashlib.sha256(json.dumps(
            selected_sequence,
            ensure_ascii=False,
            sort_keys=True,
            separators=(",", ":"),
            allow_nan=False,
        ).encode("utf-8")).hexdigest()
        expect(artifact["source_physical_a"]["analysis_file_sha256"] ==
               hashlib.sha256(source_bytes).hexdigest() and
               artifact["physical_output_capability"] is False and
               artifact["physical_b_launch_authorized"] is False and
               claimed_design_sha == recomputed_design_sha and
               claimed_sequence_sha == recomputed_sequence_sha,
               "artifact 必须绑定源字节、可复算两级语义 SHA 且明确没有 Physical 能力/授权")
        expect(MODULE.main(arguments) == 1 and output.read_bytes() == artifact_bytes,
               "既有设计 artifact 必须拒绝覆盖且保持字节不变")


def test_primary_prepare_plan_binds_frozen_design_and_a2_green() -> None:
    design = MODULE.design_physical_b_candidates(
        valid_physical_a_analysis(),
        orders=[5, 6, 7],
        horizons=[4, 8, 16, 32],
        guard_sample_count=32,
    )
    design["design_semantic_sha256"] = semantic_sha256(
        design, "design_semantic_sha256")
    design_content = json_bytes(design)
    decision = valid_a2_dependency_decision()
    decision_content = json_bytes(decision)

    plan = MODULE.bind_physical_b_primary_prepare_plan(
        design,
        design_content,
        hashlib.sha256(design_content).hexdigest(),
        decision,
        decision_content,
        hashlib.sha256(decision_content).hexdigest(),
    )

    sequence = plan["primary_sequence"]
    gate = plan["physical_b_primary_prepare_gate"]
    expect(plan["status"] == "READY_FOR_PHYSICAL_B_PRIMARY_PREPARE" and
           plan["evidence_type"] ==
           "mouse_effect_probe_physical_b_primary_f0" and
           plan["physical_output_capability"] is False and
           plan["physical_b_launch_authorized"] is False and
           plan["production_aim_changed"] is False,
           "F0 只能授权后续 Primary Prepare，不能携带 Launch 或生产 Aim 能力")
    expect(plan["model_contract"]["identification_input_definition"] ==
           "cumulative_position_counts" and
           plan["model_contract"]["actuator_audit_input"] ==
           "completed_command_dx_counts" and
           plan["candidate_horizons"] == [4, 8, 16, 32] and
           plan["deletion_control_horizons"] == [4] and
           plan["acceptance_eligible_horizons"] == [8, 16, 32],
           "A2 tail=7 必须只排除 H=4，不能事后重选输入定义或 recurrence")
    expect(plan["analysis_contract"]["contract_semantic_sha256"] ==
           plan["analyzer"]["contract_semantic_sha256"] and
           plan["analyzer"]["file"] ==
           "analyze_mouse_effect_probe_b.py" and
           len(plan["analyzer"]["file_sha256"]) == 64 and
           plan["analysis_contract"]["selection"]
               ["selected_must_strictly_beat_h4"] is True and
           plan["physical_b_primary_prepare_gate"]
               ["mapping_uncertainty_upper_px"] == 1.342895110591,
           "F0 必须绑定可执行 analyzer、精确 selection 规则和 A2 mapping budget")
    expect(sequence["profile"] == "physical_b_prbs_primary" and
           sequence["offline_sequence_semantic_sha256"] ==
           design["selected_candidate"]["sequence"]
                 ["sequence_semantic_sha256"] and
           sequence["sample_count"] == 416 and
           sequence["max_abs_prefix_x_counts"] == 1 and
           sequence["pair_roles"] ==
           ["estimation", "within_run_validation"] and
           sequence["lfsr"]["feedback_mask"] == 0x27 and
           sequence["lfsr"]["phase"] == 49,
           "Primary F0 必须原样绑定已冻结 exact sequence 与整 pair 角色")
    expect(gate["ready"] is True and
           gate["a2_tail_upper_lag"] == 7 and
           gate["guard_sample_count"] == 32 and
           gate["actual_prefix_counts"] == 1 and
           gate["allowed_prefix_counts"] == 9 and
           gate["holdout_used_for_tuning"] is False,
           "A2 green、guard、prefix 与 holdout 禁调参必须同时闭合")
    expect(plan["cross_run_holdout"]["preregistered"] is True and
           plan["cross_run_holdout"]["prepare_allowed"] is False and
           plan["cross_run_holdout"]["sequence_semantic_sha256"] ==
           design["cross_run_holdout_candidate"]["sequence"]
                 ["sequence_semantic_sha256"],
           "cross-Run recurrence 只能预注册，当前不得物化或 Prepare")
    expect(plan["f0_semantic_sha256"] ==
           semantic_sha256(plan, "f0_semantic_sha256"),
           "F0 必须有可复算的规范语义 SHA-256")

    tuned = valid_a2_dependency_decision()
    tuned["holdout_used_for_tuning"] = True
    tuned_content = json_bytes(tuned)
    rejected = False
    try:
        MODULE.bind_physical_b_primary_prepare_plan(
            design,
            design_content,
            hashlib.sha256(design_content).hexdigest(),
            tuned,
            tuned_content,
            hashlib.sha256(tuned_content).hexdigest(),
        )
    except ValueError:
        rejected = True
    expect(rejected, "使用 holdout 调参的 A2 decision 必须 fail closed")


def test_primary_prepare_plan_cli_is_bound_and_refuses_overwrite() -> None:
    design = MODULE.design_physical_b_candidates(
        valid_physical_a_analysis(),
        orders=[5, 6, 7],
        horizons=[4, 8, 16, 32],
        guard_sample_count=32,
    )
    design["design_semantic_sha256"] = semantic_sha256(
        design, "design_semantic_sha256")
    decision = valid_a2_dependency_decision()
    with tempfile.TemporaryDirectory(prefix="xen-probe-b-f0-") as directory:
        root = pathlib.Path(directory)
        design_path = root / "offline-design.json"
        decision_path = root / "a2-decision.json"
        output_path = root / "primary-f0.json"
        design_content = json_bytes(design)
        decision_content = json_bytes(decision)
        design_path.write_bytes(design_content)
        decision_path.write_bytes(decision_content)
        arguments = [
            "bind-primary",
            "--offline-design", str(design_path),
            "--expected-offline-design-sha256",
            hashlib.sha256(design_content).hexdigest(),
            "--a2-decision", str(decision_path),
            "--expected-a2-decision-sha256",
            hashlib.sha256(decision_content).hexdigest(),
            "--output", str(output_path),
        ]
        expect(MODULE.main(arguments) == 0 and output_path.is_file(),
               "bind-primary CLI 必须发布不可变 F0 artifact")
        artifact_bytes = output_path.read_bytes()
        artifact = json.loads(artifact_bytes)
        expect(artifact["source_offline_design"]["path"] ==
               str(design_path) and
               artifact["source_a2_dependency_decision"]["path"] ==
               str(decision_path) and
               artifact["f0_semantic_sha256"] ==
               semantic_sha256(artifact, "f0_semantic_sha256"),
               "CLI F0 必须绑定绝对源路径、文件哈希与可复算语义 SHA")
        expect(MODULE.main(arguments) == 1 and
               output_path.read_bytes() == artifact_bytes,
               "F0 输出已存在时必须拒绝覆盖并保持字节不变")

        wrong_output = root / "wrong-hash.json"
        wrong_arguments = list(arguments)
        wrong_arguments[wrong_arguments.index("--output") + 1] = \
            str(wrong_output)
        wrong_arguments[
            wrong_arguments.index(
                "--expected-a2-decision-sha256") + 1] = "0" * 64
        expect(MODULE.main(wrong_arguments) == 1 and
               not wrong_output.exists(),
               "A2 decision expected SHA 不匹配时不得发布 F0")


if __name__ == "__main__":
    test_maximum_length_period_uses_explicit_recurrence()
    test_candidate_definitions_do_not_mix_command_and_position()
    test_period_audit_reports_exact_rank_singular_values_and_frequency()
    test_design_selects_bounded_position_input_and_keeps_prepare_blocked()
    test_cli_binds_source_and_refuses_artifact_overwrite()
    test_primary_prepare_plan_binds_frozen_design_and_a2_green()
    test_primary_prepare_plan_cli_is_bound_and_refuses_overwrite()
    print("Mouse Effect Probe PRBS design 测试全部通过。")
