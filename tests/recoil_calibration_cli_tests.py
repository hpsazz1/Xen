"""独立校准CLI离线及授权负测；从不调用授权有效的真实run。"""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

exe = Path(sys.argv[1]).resolve()


def command(*args, ok=True):
    result = subprocess.run([str(exe), *map(str, args)], capture_output=True, timeout=15)
    assert (result.returncode == 0) == ok, (args, result.returncode, result.stdout, result.stderr)
    return result


with tempfile.TemporaryDirectory(prefix="xen-calibration-cli-") as temp:
    root = Path(temp)
    profile = root / "profile.json"
    profile.write_text(json.dumps({
        "schema_version": 1, "id": "synthetic", "weapon_id": "ak47", "revision": 1,
        "unit": "device_counts", "sample_semantics": "cumulative", "fire_mode": "automatic",
        "state": "SCHEMA_VALID", "source": {}, "calibration": {}, "phase_tolerance_ms": None,
        "recovery_ms": None, "points": [[0, 0, 0], [20, 2, 3]]}), encoding="utf-8")
    config = root / "config.ini"
    config.write_text("[gsi]\nenabled=true\nexpected_player_id=76561198000000000\n"
                      "[source_context]\nenabled=true\nhost=127.0.0.1\nport=5014\nprocess_name=synthetic.exe\n", encoding="utf-8")
    request = root / "request.json"
    value = {"environment": {"weapon_id": "ak47", "game_build": "synthetic", "input_path": "kmbox_net",
                              "conditions": "synthetic-only", "sensitivity": 1.0},
             "limits": {"max_firing_sessions": 1, "max_session_duration_ms": 1000, "max_firing_duration_ms": 100,
                        "max_sent_l1_counts": 20, "max_command_l1_counts": 10, "rolling_window_ms": 16,
                        "rolling_window_counts": 14, "command_phase_budget_ms": 20},
             "hold_virtual_key": 18, "cancel_virtual_key": 35}
    request.write_text(json.dumps(value), encoding="utf-8")
    session = root / "session"
    command("prepare", profile, config, request, session)
    assert not (session / "CONSUMED").exists()
    assert not (session / "batches").exists()
    assert (session / "profile.json").read_bytes() == profile.read_bytes()
    digest = hashlib.sha256((session / "manifest.json").read_bytes()).hexdigest()
    command("inspect", session)
    command("run", session, ok=False)
    command("run", session, "--confirm", "CALIBRATE-" + digest, ok=False)
    command("run", session, "--allow-physical-output", "--confirm", "WRONG", ok=False)
    assert not (session / "CONSUMED").exists(), "授权负测必须停在设备工厂和消费之前"
    command("prepare", profile, config, request, session, ok=False)
    (session / "profile.json").write_bytes(profile.read_bytes() + b" ")
    command("inspect", session, ok=False)
    command("run", session, "--allow-physical-output", "--confirm", "CALIBRATE-" + digest, ok=False)
    assert not (session / "CONSUMED").exists()
    # 变更候选和重复会话均未触发真实连接；不提供任何成功的run路径。
print("recoil_calibration_cli_tests passed")
