import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("auto_stop_hud", ROOT / "scripts" / "analyze_auto_stop_hud.py")
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("无法加载 HUD 分析器")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def frame(index, x, velocity):
    return {"file": str(index), "png": str(index), "time_ns": 1000000000 + index * 10000000,
            "position": [x, 0, 0], "velocity": velocity, "issues": []}


def analyze(rows):
    return MODULE.analyze(rows, 0.5, 0.1, 0.05, 0.01)[0]


def main():
    prefix = [frame(0, 0, 0), frame(1, 0.5, 5), frame(2, 1, 5)]
    stable = prefix + [frame(i, 1, 0) for i in range(3, 35)]
    valid = analyze(stable)
    assert valid["status"] == "CLIENT_ZERO_AND_POSITION_STABLE", "完整静止尾段应保留观测"
    assert valid["final_stable_position_xyz"] == [1, 0, 0]
    drift = prefix + [frame(i, 1, 0) for i in range(3, 15)] + [
        frame(i, 1 + (i - 14) * 0.01, 0) for i in range(15, 35)]
    result = analyze(drift)
    assert result["status"] == "PARTIAL", "首个稳定窗口后的细小累计漂移不得报最终稳定"
    assert result["first_persistent_zero_ns"] == valid["first_persistent_zero_ns"], "首次零事实应保留"
    assert result["final_stable_position_xyz"] is None and result["overshoot"] is None
    assert result["tail_verification"]["position_left_initial_stable_box"]
    missing = prefix + [frame(i, 1, 0) for i in range(3, 35)]
    missing[20]["issues"] = ["OCR_MISSING"]
    result = analyze(missing)
    assert result["status"] == "PARTIAL" and result["final_stable_position_xyz"] is None, "尾段缺测不得填零"
    assert analyze(prefix + [frame(3, 1, 0)])["status"] == "UNRESOLVED", "短尾段证据不足"
    print("HUD 首次零窗口、后续漂移、缺测及短尾段回归通过")


if __name__ == "__main__":
    main()
