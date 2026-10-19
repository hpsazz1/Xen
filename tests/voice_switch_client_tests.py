"""语音客户端协议、轮次与可选真实中文模型回归。"""

import json
import argparse
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.voice_switch.client import Controller, JsonLines, MAX_LINE, SherpaEngine


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
        self.sent, self.tones = [], []
        self.engine, self.audio = Engine(), Audio()
        self.now = 0
        self.c = Controller(self.engine, self.audio, self.sent.append, self.tones.append, lambda: self.now)

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
