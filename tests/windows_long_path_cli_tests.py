"""真实离线 CLI 的深路径发布、冲突保留及失败清理合同。"""
import argparse
import hashlib
import json
import pathlib
import subprocess

from aim_production_red_producer_integration_tests import run_contract
from xen_owned_test_directory import owned_test_directory


def run(executable, *arguments):
    return subprocess.run([str(executable), *map(str, arguments)],
                          capture_output=True, text=True, encoding="utf-8",
                          errors="replace", timeout=60)


def snapshot(root):
    return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in root.rglob("*") if p.is_file()}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--producer", required=True, type=pathlib.Path)
    parser.add_argument("--sequence", required=True, type=pathlib.Path)
    parser.add_argument("--evaluator", required=True, type=pathlib.Path)
    parser.add_argument("--powershell-executable", required=True, type=pathlib.Path)
    parser.add_argument("--test-root", required=True, type=pathlib.Path)
    args = parser.parse_args()
    with owned_test_directory(args.test_root, args.powershell_executable) as owned:
        root = owned / "Unicode 空格 🧪"
        while len(str(root)) < 330:
            root /= "deep path boundary-0123456789"
        root.mkdir(parents=True)
        print(f"真实 CLI 普通绝对路径：{len(str(root))} 字符；含空格、中文与非 BMP 字符")
        assert run_contract(args, root) == 0
        before = snapshot(root)
        base_args = ("--plan", root / "plan.json", "--config", root / "config.ini",
                     "--measured-reference-source", root / "measured.csv")
        result = run(args.producer, *base_args, "--output-directory", root / "bundle")
        assert result.returncode != 0 and snapshot(root) == before, result
        foreign = root / "foreign.incoming"
        foreign.mkdir()
        (foreign / "keep.bin").write_bytes(b"foreign\x00bytes\xff")
        before = snapshot(root)
        result = run(args.producer, *base_args, "--output-directory", root / "foreign")
        assert result.returncode != 0 and snapshot(root) == before, result
        # 已取得本轮 incoming 后的失败必须清理本轮，而不能影响同级既有数据。
        bad_plan = root / "bad plan.json"
        bad_plan.write_text("{}", encoding="utf-8")
        before = snapshot(root)
        result = run(args.producer, "--plan", bad_plan, *base_args[2:],
                     "--output-directory", root / "failed")
        assert result.returncode != 0 and snapshot(root) == before, result
        assert not (root / "failed").exists() and not (root / "failed.incoming").exists()

        sequence = root / "序列 🧪 with spaces.json"
        sequence_args = ("--output", sequence, "--profile", "physical-b-holdout",
                         "--guard-samples", 32, "--lfsr-order", 6,
                         "--feedback-mask", 51, "--seed", 1, "--phase", 21,
                         "--offline-sequence-semantic-sha256",
                         "e0dffb8b72d6326803a84a2ca37a9cb5d016c9bcddd14728b9e736547e1082f4")
        result = run(args.sequence, *sequence_args)
        assert result.returncode == 0, result
        assert str(sequence) in result.stdout, result.stdout
        document = json.loads(sequence.read_text(encoding="utf-8"))
        assert document["schema"] == 5 and len(document["samples"]) == 288
        before = snapshot(root)
        result = run(args.sequence, *sequence_args)
        assert result.returncode != 0 and snapshot(root) == before, result
        assert not list(root.glob("*.pending-*"))
        print("producer完整证据、输出禁用、UTF-8路径、既有output/incoming保留、失败归属清理及序列原子发布通过")
    assert not owned.exists(), "owned fixture 必须完整清理"
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
