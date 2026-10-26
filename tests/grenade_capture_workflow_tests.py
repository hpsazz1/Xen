"""F10制作离线闭环：合成PNG、本地接收目录及回环HTTP，不采屏/按键/连接辅机。"""

from copy import deepcopy
import hashlib
import http.server
import json
from pathlib import Path
import shutil
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.grenade_capture import capture_workflow, worker
from tools.grenade_capture.capture_workflow import (
    ContextReader, LocalSavePending, capture_once, short_name, standing_for, transfer_capture, validate_context)
from tools.grenade_capture.reference_assets import load_reference
from tools.grenade_capture.screen_capture import png_rgb


class CaptureTests(unittest.TestCase):
    def setUp(self):
        fixtures = Path(__file__).resolve().parents[1] / "temp/XEN-GRENADE-OBS-MENU-20261026-001/capture-fixtures"
        fixtures.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="run-", dir=fixtures)
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.root, self.remote = self.base / "host", self.base / "auxiliary"
        self.standing, self.aim = self.base / "standing.png", self.base / "aim.png"
        self.standing.write_bytes(png_rgb(800, 600, [bytes([60, 120, 40]) * 800] * 600))
        self.aim.write_bytes(png_rgb(640, 360, [bytes([30, 80, 160]) * 640] * 360))
        self.plan = {"schema_version": 3, "name": "Mirage T1 VIP R2", "map": "de_mirage", "voice_command": "",
                     "location": "T1", "notes": "原瞄角与动作保持", "mask": {"wasd": 0, "start_ms": -1, "duration_ms": 0},
                     "actions": [{"type": "button", "button": "left", "start_ms": 0, "duration_ms": 200}]}
        (self.remote / "plans").mkdir(parents=True)
        self.plan_path = self.remote / "plans/mirage-t1-vip-r2.json"
        self.plan_path.write_text(json.dumps(self.plan, ensure_ascii=False, indent=2), encoding="utf-8")
        self.context = {"ready": True, "reason": "", "map": "de_mirage", "team": "T", "generation": "service-1",
                        "remaining_ms": 2500, "plan_id": self.plan_path.name, "plan": self.plan,
                        "plan_sha256": hashlib.sha256(self.plan_path.read_bytes()).hexdigest()}
        self.capture_count = 0

    def fake_capture(self, path):
        self.capture_count += 1
        shutil.copyfile(self.aim, path)

    def complete(self, reader=None):
        return capture_once(reader or (lambda: deepcopy(self.context)), self.root, self.fake_capture, self.standing)

    def test_short_names_preserve_alternative_and_existing_position(self):
        self.assertEqual(short_name("Mirage T1 VIP R2", "de_mirage"), "T1 VIP烟")
        self.assertEqual(short_name("Mirage T1 VIP 备用瞄角 R1", "de_mirage"), "T1 VIP烟 备用瞄角")
        self.assertEqual(short_name("Mirage T2 B小烟 R1", "de_mirage"), "T2 B小烟")
        self.assertEqual(short_name("Ancient T来源5号 中路烟 [S102]", "de_ancient"), "T5 中路烟")
        self.assertEqual(short_name("未知站位 A烟"), "未知站位 A烟")

    def test_f10_capture_preserves_originals_and_packages_matching_name(self):
        captured = self.complete()
        manifest, directory = load_reference(self.root / "references", self.plan_path.stem, self.plan_path.name)
        self.assertEqual(self.capture_count, 1)
        self.assertEqual(manifest["plan"], self.plan)
        self.assertEqual(manifest["display_name"], "T1 VIP烟")
        self.assertEqual(manifest["confirmation_source"], "host_f10")
        self.assertTrue(manifest["manual_throw_verified"])
        self.assertEqual(manifest["standing_marker"]["label"], "T1")
        self.assertEqual((directory / "standing.png").read_bytes(), self.standing.read_bytes())
        self.assertEqual((directory / "aim.png").read_bytes(), self.aim.read_bytes())
        self.assertEqual((manifest["geometry"]["source_width"], manifest["geometry"]["source_height"]), (640, 360))
        archive = self.root / captured["package"]
        self.assertEqual(archive.name, "T1 VIP烟.zip")
        with zipfile.ZipFile(archive) as package:
            self.assertEqual(len(package.namelist()), 5)
            self.assertIsNone(package.testzip())
            self.assertEqual(json.loads(package.read("plans/" + self.plan_path.name)), self.plan)

    def test_not_ready_expired_or_wrong_foreground_never_captures(self):
        for change in ({"ready": False, "reason": "请启用道具测试"}, {"remaining_ms": 0},
                       {"_deadline": time.monotonic() - 1}):
            value = deepcopy(self.context)
            value.update(change)
            with self.assertRaises(ValueError):
                self.complete(lambda: value)
        with self.assertRaisesRegex(ValueError, "失焦"):
            capture_once(lambda: deepcopy(self.context), self.root, self.fake_capture, self.standing,
                         foreground=lambda: 2, expected_foreground=1)
        self.assertEqual(self.capture_count, 0)
        self.assertFalse((self.root / "references").exists())

    def test_context_changes_while_capturing_do_not_publish(self):
        for changed in ("generation", "plan_sha256", "team"):
            before, after = deepcopy(self.context), deepcopy(self.context)
            after[changed] = {"generation": "service-2", "plan_sha256": "0" * 64, "team": "CT"}[changed]
            iterator = iter((before, after))
            with self.assertRaisesRegex(ValueError, "状态改变"):
                self.complete(lambda: next(iterator))
        self.assertFalse((self.root / "references").exists())

    def test_missing_other_standing_image_does_not_reuse_spawn_image(self):
        value = deepcopy(self.context)
        value["plan"]["name"] = "T10 B小烟"
        with self.assertRaises(ValueError):
            standing_for(value, self.root, self.standing)
        (self.root / "standing").mkdir(parents=True)
        custom = self.root / "standing" / (self.plan_path.stem + ".png")
        shutil.copyfile(self.standing, custom)
        path, marker = standing_for(value, self.root, self.standing)
        self.assertEqual(path, custom)
        self.assertIsNone(marker)

    def test_recapture_keeps_old_reference_and_package_backup(self):
        first = self.complete()
        old = (self.root / "references" / self.plan_path.stem / "aim.png").read_bytes()
        self.aim.write_bytes(png_rgb(640, 360, [bytes([40, 70, 180]) * 640] * 360))
        second = self.complete()
        self.assertNotEqual(first["capture_id"], second["capture_id"])
        backup = self.root / "reference-backups" / self.plan_path.stem / second["capture_id"]
        self.assertEqual((backup / "aim.png").read_bytes(), old)
        self.assertEqual(len(list((self.root / "package-backups/de_mirage").glob("*.zip"))), 1)

    def test_damaged_old_zip_is_backed_up_and_recapture_continues(self):
        captured = self.complete()
        archive = self.root / captured["package"]
        archive.write_bytes(b"damaged old archive")
        self.complete()
        backups = list((self.root / "package-backups/de_mirage").glob("*.zip"))
        self.assertEqual(len(backups), 1)
        self.assertEqual(backups[0].read_bytes(), b"damaged old archive")
        with zipfile.ZipFile(archive) as value:
            self.assertIsNone(value.testzip())

    def test_failed_local_metadata_write_has_recoverable_transaction(self):
        write = capture_workflow.write_json
        def fail_metadata(path, value):
            if Path(path).parent.name == "captures":
                raise OSError("synthetic metadata failure")
            return write(path, value)
        with patch.object(capture_workflow, "write_json", side_effect=fail_metadata):
            with self.assertRaises(LocalSavePending):
                self.complete()
        journal = self.root / "capture-transactions" / self.plan_path.stem / "pending.json"
        self.assertTrue(journal.is_file())
        previous = json.loads(journal.read_text(encoding="utf-8"))["metadata"]
        self.assertTrue((self.root / previous["package"]).is_file())
        captured = self.complete()
        self.assertFalse(journal.exists())
        self.assertNotEqual(captured["capture_id"], previous["capture_id"])
        self.assertTrue((journal.parent / ("completed-" + previous["capture_id"] + ".json")).is_file())

    def test_interrupted_json_temporary_does_not_block_recovery(self):
        captures = self.root / "captures"
        transactions = self.root / "capture-transactions" / self.plan_path.stem
        captures.mkdir(parents=True)
        transactions.mkdir(parents=True)
        old_capture = captures / (self.plan_path.stem + ".json.pending")
        old_journal = transactions / "pending.json.pending"
        old_capture.write_text("interrupted", encoding="utf-8")
        old_journal.write_text("interrupted", encoding="utf-8")
        captured = self.complete()
        self.assertEqual(captured["status"], "pending")
        self.assertTrue(old_capture.is_file())
        self.assertTrue(old_journal.is_file())

    def test_long_same_names_keep_distinct_archive_identity(self):
        self.context["plan"]["name"] = "T1 " + "长名称" * 50
        first = self.complete()
        first_bytes = (self.root / first["package"]).read_bytes()
        self.context["plan_id"] = "another-plan.json"
        second = self.complete()
        self.assertNotEqual(first["package"], second["package"])
        self.assertEqual((self.root / first["package"]).read_bytes(), first_bytes)
        self.assertIn("another-plan", second["package"])

    def test_real_local_transfer_checks_plan_hash_and_changes_only_resources(self):
        captured = self.complete()
        before_plan = self.plan_path.read_bytes()
        result = transfer_capture(self.root, self.plan_path.stem, self.remote)
        self.assertEqual(result["status"], "sent")
        self.assertEqual(self.plan_path.read_bytes(), before_plan)
        manifest, directory = load_reference(self.remote / "references", self.plan_path.stem, self.plan_path.name)
        self.assertEqual(manifest["reference_revision"], captured["reference_revision"])
        self.assertEqual((directory / "aim.png").read_bytes(), self.aim.read_bytes())
        self.assertEqual((self.remote / captured["package"]).read_bytes(), (self.root / captured["package"]).read_bytes())

    def test_changed_plan_or_network_failure_preserves_local_pending(self):
        captured = self.complete()
        self.plan_path.write_text(json.dumps(self.plan, ensure_ascii=False), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "辅机方案"):
            transfer_capture(self.root, self.plan_path.stem, self.remote)
        with self.assertRaises(ValueError):
            transfer_capture(self.root, self.plan_path.stem, self.base / "unavailable")
        self.assertTrue((self.root / captured["package"]).exists())
        record = json.loads((self.root / "captures" / (self.plan_path.stem + ".json")).read_text(encoding="utf-8"))
        self.assertEqual(record["status"], "pending")
        self.assertFalse((self.remote / "references").exists())

    def test_interrupted_directory_switch_restores_before_retry(self):
        self.complete()
        transfer_capture(self.root, self.plan_path.stem, self.remote)
        target = self.remote / "references" / self.plan_path.stem
        backup = self.remote / "reference-backups" / self.plan_path.stem / "interrupted"
        backup.parent.mkdir(parents=True, exist_ok=True)
        target.rename(backup)
        journal = self.remote / "capture-transfers" / self.plan_path.stem / "pending.json"
        journal.write_text(json.dumps({"identity": self.plan_path.stem, "backup": backup.relative_to(self.remote).as_posix()}), encoding="utf-8")
        transfer_capture(self.root, self.plan_path.stem, self.remote)
        self.assertTrue(target.is_dir())
        load_reference(self.remote / "references", self.plan_path.stem)
        self.assertFalse(journal.exists())

    def test_plan_and_metadata_path_escape_are_rejected(self):
        for change in ({"plan_id": "../bad.json"}, {"map": "../bad", "plan": {**self.plan, "map": "../bad"}}):
            value = deepcopy(self.context)
            value.update(change)
            with self.assertRaises(ValueError):
                validate_context(value)
        self.complete()
        record = self.root / "captures" / (self.plan_path.stem + ".json")
        metadata = json.loads(record.read_text(encoding="utf-8"))
        metadata["package"] = "packages/de_mirage/../../../../escape.zip"
        record.write_text(json.dumps(metadata), encoding="utf-8")
        with self.assertRaises(ValueError):
            transfer_capture(self.root, self.plan_path.stem, self.remote)

    def test_context_reader_uses_real_loopback_http(self):
        payload = json.dumps(self.context).encode()
        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                self.send_response(200)
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)
            def log_message(self, *args):
                pass
        server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            result = ContextReader("127.0.0.1", server.server_port)()
            self.assertGreater(result["_deadline"], time.monotonic())
            self.assertEqual(result["plan_sha256"], self.context["plan_sha256"])
        finally:
            server.shutdown()
            worker.join(2)
            server.server_close()
        for host in ("8.8.8.8", "0.0.0.0", "::1", "example.com"):
            with self.assertRaises(ValueError):
                ContextReader(host)


    # 单作业忙态、超时与退出状态的消费已由 OBS 插件模型负责；此处验证实际 worker 协议。
    def worker_arguments(self):
        self.status_path = self.base / "jobs" / "capture.json"
        return ["--host", "127.0.0.1", "--web-port", "5015", "--root", str(self.root),
                "--destination", str(self.remote), "--shared-standing", str(self.standing),
                "--window", "12", "--process", "34", "--job-status", str(self.status_path)]

    def run_worker(self):
        def capture(*args, **kwargs):
            return capture_once(*args, capture=self.fake_capture, **kwargs)
        with patch.object(worker, "ContextReader", return_value=lambda: deepcopy(self.context)), \
             patch.object(worker, "capture_once", side_effect=capture), \
             patch.object(worker, "foreground_identity", return_value={"window": 12, "process": 34}):
            result = worker.main(self.worker_arguments())
        return result, json.loads(self.status_path.read_text(encoding="utf-8"))

    def test_obs_worker_captures_and_transfers_once_then_reports_terminal_success(self):
        plan_bytes = self.plan_path.read_bytes()
        result, status = self.run_worker()
        self.assertEqual(result, 0)
        self.assertEqual(self.capture_count, 1)
        self.assertEqual((status["state"], status["terminal"]), ("sent", True))
        self.assertIn("T1 VIP烟", status["message"])
        self.assertEqual(self.plan_path.read_bytes(), plan_bytes)
        _, directory = load_reference(self.remote / "references", self.plan_path.stem)
        self.assertEqual((directory / "aim.png").read_bytes(), self.aim.read_bytes())

    def test_obs_worker_failed_context_reports_failure_without_capture(self):
        self.context["ready"] = False
        self.context["reason"] = "当前方案不可用"
        result, status = self.run_worker()
        self.assertEqual(result, 1)
        self.assertEqual(self.capture_count, 0)
        self.assertEqual((status["state"], status["terminal"]), ("failed", True))
        self.assertIn("当前方案不可用", status["message"])
        self.assertFalse((self.remote / "references").exists())

    def test_obs_worker_transfer_failure_reports_pending_and_keeps_local_resources(self):
        with patch.object(worker, "transfer_capture", side_effect=OSError("synthetic disconnected share")):
            result, status = self.run_worker()
        self.assertEqual(result, 2)
        self.assertEqual(self.capture_count, 1)
        self.assertEqual((status["state"], status["terminal"]), ("pending", True))
        self.assertIn("本地已保存，发送未确认", status["message"])
        _, directory = load_reference(self.root / "references", self.plan_path.stem)
        self.assertEqual((directory / "aim.png").read_bytes(), self.aim.read_bytes())
        self.assertFalse((self.remote / "references").exists())

    def test_obs_worker_interrupted_local_publish_reports_pending_not_false_failure(self):
        write = capture_workflow.write_json
        def fail_metadata(path, value):
            if Path(path).parent.name == "captures":
                raise OSError("synthetic metadata failure")
            return write(path, value)
        with patch.object(capture_workflow, "write_json", side_effect=fail_metadata):
            result, status = self.run_worker()
        self.assertEqual(result, 2)
        self.assertEqual((status["state"], status["terminal"]), ("pending", True))
        journal = self.root / "capture-transactions" / self.plan_path.stem / "pending.json"
        self.assertTrue(journal.is_file())
        self.assertFalse((self.remote / "references").exists())


if __name__ == "__main__":
    unittest.main()
