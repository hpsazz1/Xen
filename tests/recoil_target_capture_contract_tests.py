"""通过假设备 C++ 采集器的真实归档验证 Python CLI；不连接真实输入设备。"""

import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture-exe", required=True, type=Path)
    args = parser.parse_args()
    script = Path(__file__).resolve().parents[1] / "scripts" / "recoil_target_iteration.py"
    with tempfile.TemporaryDirectory(prefix="xen-target-contract-") as temporary:
        root = Path(temporary)
        generated = subprocess.run([str(args.capture_exe.resolve()), "--write-fixture", str(root)],
                                   capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=90)
        check(generated.returncode == 0, f"C++ 假设备 fixture 失败：{generated.stdout}\n{generated.stderr}")
        registry = root / "usage.json"
        def cli(action, *arguments, expected=0):
            output = root / f"report-{action}-{len(list(root.glob('report-*.json')))}.json"
            completed = subprocess.run([sys.executable, str(script), action, *map(str, arguments),
                                        "--registry", str(registry), "--output", str(output)],
                                       capture_output=True, text=True, encoding="utf-8", timeout=30)
            check(output.is_file(), f"CLI 没有生成机器可读报告：{completed.stderr}")
            report = json.loads(output.read_text(encoding="utf-8"))
            check(completed.returncode == expected,
                  f"CLI {action} 退出码不符：{completed.returncode}，报告：{report}")
            return report
        runs = [root / f"fit-{index}" for index in range(5)]
        for run in runs:
            pending = cli("inspect", run, expected=2)
            check(not pending["analysis_eligible"], "未审核真实归档不应自动通过")
            reviewed = cli("review", run, "--identity-confirmed", "--observation", "合成假设备身份确认，无真实物理验收")
            check(reviewed["analysis_eligible"] and not reviewed["physical_verified"], "人工记录不得冒充物理通过")
            checked = cli("inspect", run)
            check(checked["analysis_eligible"], "审核后的 C++ 原始合同应可分析")
        report = cli("fit", *runs, "--parent", root / "baseline.json")
        check(report["analysis_eligible"] and len(report["run_inputs"]) == 5, "五轮 C++ 合同未完成分析")
        check(report["candidate"] is None and not report["executable_candidate_eligible"], "缺少 L 标定不得导出曲线")
        check(report["physical_verified"] is False, "假设备不能授予物理验收")
        check(report["zero_net_ledger_rows"] > 0, "应覆盖零总量量化账本合同")
        for point in report["trajectory"]:
            check(point["residual_median_counts"] == [0, 0], "前馈单独回放不应产生反馈残差")
            check(point["algebraic_parent_plus_residual_counts"] == point["parent_counts"],
                  "不得重复累加父基线")
        refused = cli("fit", *([runs[0]] * 5), "--parent", root / "baseline.json", expected=2)
        check(refused["candidate"] is None and "重复 Run" in refused["error"], "重复轮必须拒绝并返回原因")
    print("C++ 假设备归档 → Python inspect/review/fit 合同通过；无真实设备或物理验收。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
