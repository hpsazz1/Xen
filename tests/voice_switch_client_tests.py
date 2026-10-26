"""语音客户端协议、轮次与可选真实中文模型回归。"""

import json
import argparse
from pathlib import Path
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.voice_switch.client import Controller, JsonLines, MAX_LINE, SherpaEngine
from tools.voice_switch import client


class Engine:
    def __init__(self):
        self.results = {"k0"}
        self.begins = 0

    def prepare(self, entries):
        return "keywords", {"k" + str(n): x["id"] for n, x in enumerate(entries)}

    def begin(self, keywords):
        self.begins += 1

    def accept(self, samples):
        pass

    def end(self):
        return self.results


class Audio:
    def __init__(self):
        self.started = 0
        self.active = False

    def start(self):
        self.started += 1
        self.active = True

    def stop(self):
        self.active = False

    def read(self):
        return []


class ClientTests(unittest.TestCase):
    def setUp(self):
        self.sent, self.tones, self.guidance = [], [], []
        self.engine, self.audio = Engine(), Audio()
        self.now = 0
        self.c = Controller(self.engine, self.audio, self.sent.append, self.tones.append,
                            lambda: self.now, self.guidance.append)

    def catalog(self, epoch=1, revision=1, name="de_test"):
        self.c.handle({"type": "catalog", "map": name, "epoch": epoch, "revision": revision,
                       "entries": [{"id": "plan-a", "command": "跳箱"}, {"id": "plan-b", "command": "上桥"}]})

    def begin(self, ident=1):
        self.c.handle({"type": "begin", "id": ident, "map": "de_test", "epoch": 1, "revision": 1})

    def end(self, ident=1):
        self.c.handle({"type": "end", "id": ident})

    def test_success_waits_for_matching_final_ack(self):
        self.catalog()
        self.begin()
        self.end()
        self.assertEqual(self.sent[-1]["type"], "select")
        self.assertFalse(self.tones)
        self.c.handle({"type": "result", "id": 1, "ok": True, "plan_id": "plan-a"})
        self.assertEqual(self.tones, [True])
        self.c.handle({"type": "result", "id": 1, "ok": True, "plan_id": "plan-a"})
        self.assertEqual(self.tones, [True])

    def test_wrong_plan_ack_is_not_success(self):
        self.catalog()
        self.begin()
        self.end()
        self.c.handle({"type": "result", "id": 1, "ok": True, "plan_id": "plan-b"})
        self.assertEqual(self.tones, [False])

    def test_no_capture_before_catalog_or_for_stale_begin(self):
        self.begin()
        self.assertEqual(self.audio.started, 0)
        self.catalog()
        self.begin()
        self.assertEqual(self.audio.started, 0)
        self.begin(2)
        self.assertEqual(self.audio.started, 1)

    def test_busy_does_not_queue_or_restart(self):
        self.catalog()
        self.begin()
        self.begin(2)
        self.end(2)
        self.assertEqual(self.audio.started, 1)
        self.end()
        self.begin(3)
        self.assertEqual(self.audio.started, 1)

    def test_catalog_change_cancels_and_late_end_is_ignored(self):
        self.catalog()
        self.begin()
        self.catalog(epoch=2, name="de_other")
        self.end()
        self.assertEqual(self.tones, [False])
        self.assertFalse(any(x["type"] == "select" for x in self.sent))

    def test_identical_catalog_does_not_cancel(self):
        self.catalog()
        self.begin()
        self.catalog()
        self.end()
        self.assertEqual(self.sent[-1]["type"], "select")

    def test_ambiguous_or_empty_result_never_selects(self):
        self.catalog()
        self.engine.results = {"k0", "k1"}
        self.begin()
        self.end()
        self.assertEqual(self.sent[-1]["reason"], "ambiguous")
        self.engine.results = set()
        self.begin(2)
        self.end(2)
        self.assertEqual(self.sent[-1]["reason"], "no_match")
        self.assertEqual(self.tones, [False, False])

    def test_cancel_timeout_disconnect_each_emit_once(self):
        self.catalog()
        self.begin()
        self.c.handle({"type": "cancel", "id": 1})
        self.end()
        self.begin(2)
        self.end(2)
        self.now = 9
        self.c.tick()
        self.c.handle({"type": "result", "id": 2, "ok": True, "plan_id": "plan-a"})
        self.begin(3)
        self.c.disconnected()
        self.c.disconnected()
        self.assertEqual(self.tones, [False, False, False])

    def test_capture_duration_is_bounded(self):
        self.catalog()
        self.begin()
        self.now = 31
        self.c.tick()
        self.assertFalse(self.audio.active)
        self.assertEqual(self.sent[-1]["reason"], "capture_timeout")

    def test_server_rejection_during_capture_stops_microphone(self):
        self.catalog()
        self.begin()
        self.c.handle({"type": "result", "id": 1, "ok": False, "reason": "stopped"})
        self.assertFalse(self.audio.active)
        self.assertEqual(self.tones, [False])

    def test_fragmented_utf8_and_line_bounds(self):
        frame = json.dumps({"type": "测试"}, ensure_ascii=False).encode("utf-8") + b"\n"
        parser = JsonLines()
        out = []
        for byte in frame + frame:
            out += parser.feed(bytes([byte]))
        self.assertEqual(len(out), 2)
        for data in (b"x" * (MAX_LINE + 1), b"[]\n", b"\xff\n", b"\n"):
            with self.assertRaises((ValueError, UnicodeError)):
                JsonLines().feed(data)

    def test_boolean_id_is_rejected(self):
        with self.assertRaises(ValueError):
            self.c.handle({"type": "begin", "id": True})

    def guide(self, revision, state="selected", identity="smoke-a"):
        self.c.handle({"type": "guidance", "revision": revision, "state": state,
                       "plan_id": identity + ".json", "reference_id": identity, "message": ""})

    def test_guidance_lives_beyond_voice_round_and_never_plays_tone(self):
        self.catalog()
        self.begin()
        self.end()
        self.c.handle({"type": "result", "id": 1, "ok": True, "plan_id": "plan-a"})
        self.guide(1)
        self.guide(2, "aligning")
        self.guide(3, "aligned")
        self.assertIsNone(self.c.round)
        self.assertEqual([item["state"] for item in self.guidance], ["selected", "aligning", "aligned"])
        self.assertEqual(self.tones, [True])

    def test_guidance_rejects_late_revision_and_wrong_selection(self):
        self.guide(2)
        self.guide(2, "aligned")
        self.guide(1, "hidden")
        self.guide(3, "selected", "smoke-b")
        self.guide(4, "aligned", "smoke-a")
        self.assertEqual(len(self.guidance), 2)
        self.guide(5, "aligned", "smoke-b")
        self.assertEqual(self.guidance[-1]["state"], "aligned")

    def test_disconnect_hides_and_new_connection_accepts_initial_revision(self):
        self.guide(50)
        self.c.disconnected()
        self.assertEqual(self.guidance[-1]["state"], "hidden")
        self.guide(0, "aligning")
        self.assertEqual(self.guidance[-1]["state"], "aligning")
        self.assertFalse(self.tones)

    def test_guidance_cannot_submit_paths_or_invalid_revision(self):
        for identity in ("../outside", "x/y", "C:\\image", "CON"):
            with self.assertRaises(ValueError):
                self.guide(1, identity=identity)
        with self.assertRaises(ValueError):
            self.guide(True)
        self.assertFalse(self.guidance)

    def menu_catalog(self, revision=1):
        self.c.handle({"type": "catalog", "domain": "grenade", "map": "de_test", "epoch": 1,
                       "revision": revision, "entries": [
                           {"id": "smoke-a.json", "command": "", "name": "测试 T1 窗烟", "team": "T"},
                           {"id": "smoke-b.json", "command": "二号窗烟", "name": "测试 CT来源5号 烟", "team": "CT"}]})

    def test_running_client_consumes_guidance_without_host_window(self):
        connection = mock.MagicMock()
        connection.__enter__.return_value = connection
        messages = [
            {"type": "catalog_show", "map": "de_test", "epoch": 1, "revision": 1},
            {"type": "guidance", "revision": 1, "state": "selected",
             "plan_id": "smoke-a.json", "reference_id": "smoke-a", "message": "已选择"},
        ]
        connection.recv.side_effect = [
            ("\n".join(json.dumps(value) for value in messages) + "\n").encode(), KeyboardInterrupt]
        with mock.patch.object(client.socket, "create_connection", return_value=connection), \
                mock.patch.object(client.select, "select", return_value=([connection], [], [])), \
                mock.patch.object(client, "Microphone", return_value=self.audio), \
                mock.patch("tools.voice_switch.guidance.subprocess.Popen", side_effect=AssertionError("主机不得创建显示进程")) as process:
            with self.assertRaises(KeyboardInterrupt):
                client.run("test-host", 5014, Path("unused"), voice=False)
        process.assert_not_called()
        self.assertFalse(self.audio.active)
        self.assertEqual(self.audio.started, 0)

    def pick(self, identity="smoke-a.json", revision=1, generation=None):
        self.c.handle_ui({"type": "catalog_pick", "plan_id": identity, "map": "de_test", "epoch": 1,
                          "revision": revision, "generation": self.c.ui_generation if generation is None else generation})

    def test_menu_mode_does_not_require_engine_or_microphone(self):
        self.c = Controller(None, self.audio, self.sent.append, self.tones.append,
                            lambda: self.now, self.guidance.append, voice_enabled=False)
        self.menu_catalog()
        self.assertEqual(len(self.c.catalog.entries), 2)
        self.assertFalse(self.c.catalog.labels)
        self.assertFalse(self.sent[-1]["voice_enabled"])
        self.c.handle({"type": "catalog_show", "map": "de_test", "epoch": 1, "revision": 1})
        self.assertFalse(self.guidance, "旧主机清单消息不得弹窗")
        self.pick()
        self.assertFalse(any(item["type"] == "catalog_select" for item in self.sent))
        self.c.handle({"type": "catalog_result", "id": 1, "plan_id": "smoke-a.json", "ok": True})
        self.assertIsNone(self.c.menu_pending)
        self.assertEqual(self.audio.started, 0)
        self.assertFalse(self.tones)

    def test_menu_catalog_retains_empty_commands_but_kws_does_not(self):
        self.menu_catalog()
        self.assertEqual(self.c.catalog.labels, {"k0": "smoke-b.json"})
        self.assertEqual(len(self.c.catalog.entries), 2)

    def test_stale_catalog_or_old_ui_connection_cannot_select(self):
        self.menu_catalog()
        self.pick(revision=0)
        self.pick(generation=-1)
        self.pick(identity="missing.json")
        self.assertFalse(any(item["type"] == "catalog_select" for item in self.sent))
        before = len(self.guidance)
        self.c.handle({"type": "catalog_show", "map": "de_test", "epoch": 1, "revision": 0})
        self.assertEqual(len(self.guidance), before)

    def test_retired_menu_clicks_and_results_never_select(self):
        self.menu_catalog()
        self.pick()
        self.pick("smoke-b.json")
        self.assertEqual(sum(item["type"] == "catalog_select" for item in self.sent), 0)
        self.c.handle({"type": "catalog_result", "id": 9, "plan_id": "smoke-a.json", "ok": True})
        self.assertIsNone(self.c.menu_pending)
        self.c.handle({"type": "catalog_result", "id": 1, "plan_id": "smoke-b.json", "ok": True})
        self.assertFalse(self.guidance)
        self.assertIsNone(self.c.menu_pending)
        self.assertFalse(self.tones)

    def test_menu_timeout_and_catalog_change_do_not_play_voice_tones(self):
        self.menu_catalog()
        self.pick()
        self.now = 9
        self.c.tick()
        self.assertFalse(self.guidance, "旧清单点击不得建立待处理轮次或超时提示")
        self.pick()
        self.menu_catalog(revision=2)
        self.assertIsNone(self.c.menu_pending)
        count = len(self.guidance)
        self.c.handle({"type": "catalog_result", "id": 2, "plan_id": "smoke-a.json", "ok": True})
        self.assertEqual(len(self.guidance), count)
        self.assertFalse(self.tones)


class RealModelTests(unittest.TestCase):
    model = None
    samples = None

    @classmethod
    def setUpClass(cls):
        cls.engine = SherpaEngine(cls.model)

    def recognize(self, commands, filename=None):
        import numpy as np
        import wave
        keywords, labels = self.engine.prepare([{"id": text, "command": text} for text in commands])
        self.engine.begin(keywords)
        if filename:
            with wave.open(str(self.samples / filename)) as source:
                self.assertEqual((source.getframerate(), source.getnchannels(), source.getsampwidth()), (16000, 1, 2))
                audio = np.frombuffer(source.readframes(source.getnframes()), dtype=np.int16).astype(np.float32) / 32768
            for offset in range(0, len(audio), 320):
                self.engine.accept(audio[offset:offset + 320])
        return {labels[item] for item in self.engine.end()}

    def test_official_samples_have_expected_distinct_keywords(self):
        commands = ["文森特卡索", "法国", "落实", "女儿", "周望军", "蒋友伯"]
        self.assertEqual(self.recognize(commands, "3.wav"), {"文森特卡索", "法国"})
        self.assertEqual(self.recognize(commands, "4.wav"), {"女儿", "蒋友伯"})
        self.assertEqual(self.recognize(commands, "5.wav"), {"周望军", "落实"})

    def test_new_stream_does_not_inherit_previous_candidates(self):
        self.assertEqual(self.recognize(["法国"], "3.wav"), {"法国"})
        self.assertEqual(self.recognize(["女儿"], "3.wav"), set())

    def test_silence_and_homophones_do_not_select(self):
        self.assertEqual(self.recognize(["跳箱"]), set())
        self.assertEqual(self.engine.prepare([{"id": "a", "command": "跳箱"},
                                             {"id": "b", "command": "跳香"}]), ("", {}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", type=Path)
    parser.add_argument("--samples", type=Path)
    args = parser.parse_args()
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(ClientTests)
    if args.model or args.samples:
        if not args.model or not args.samples:
            parser.error("真实模型回归需要同时指定 --model 和 --samples")
        RealModelTests.model, RealModelTests.samples = args.model, args.samples
        suite.addTests(unittest.defaultTestLoader.loadTestsFromTestCase(RealModelTests))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(0 if result.wasSuccessful() else 1)
