"""本地按住说话客户端；仅发送辅机目录身份，不发送音频或文件路径。"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import io
import json
import logging
from logging.handlers import RotatingFileHandler
import math
from pathlib import Path
import queue
import select
import socket
import struct
import threading
import time
import wave


LOG = logging.getLogger("voice_switch")
MAX_LINE = 65536
RATE = 16000


def unsigned(value):
    return type(value) is int and 0 <= value < 2**64


class JsonLines:
    """跨 TCP 分片的有界帧解析；坏帧关闭连接，避免猜测协议。"""

    def __init__(self):
        self.buffer = bytearray()

    def feed(self, data):
        messages = []
        self.buffer.extend(data)
        while b"\n" in self.buffer:
            line, _, tail = self.buffer.partition(b"\n")
            self.buffer = bytearray(tail)
            if not line or len(line) > MAX_LINE:
                raise ValueError("无效协议帧长度")
            message = json.loads(line.decode("utf-8"))
            if not isinstance(message, dict):
                raise ValueError("协议帧必须为对象")
            messages.append(message)
        if len(self.buffer) > MAX_LINE:
            raise ValueError("协议帧超过 64 KiB")
        return messages


@dataclass(frozen=True)
class Catalog:
    map: str
    epoch: int
    revision: int
    keywords: str
    labels: dict


@dataclass
class Round:
    id: int
    catalog: Catalog
    started: float
    phase: str = "capture"
    deadline: float = 0.0
    plan_id: str = ""


class Controller:
    """轮次与回执契约；可用生产接口注入音频和识别器做专项验证。"""

    def __init__(self, engine, audio, send, tone, clock=time.monotonic):
        self.engine, self.audio = engine, audio
        self.send, self.tone, self.clock = send, tone, clock
        self.catalog = None
        self.round = None
        self.last_id = -1

    def finish(self, ok, reason):
        current = self.round
        if current is None:
            return
        self.round = None
        try:
            self.audio.stop()
        except Exception:
            LOG.exception("停止收音失败")
        self.tone(ok)
        LOG.info("轮次=%s 结果=%s 原因=%s 耗时_ms=%.1f", current.id, ok,
                 reason, (self.clock() - current.started) * 1000)

    def fail(self, reason):
        if self.round is None:
            return
        ident = self.round.id
        self.finish(False, reason)
        self.send({"type": "fail", "id": ident, "reason": reason})

    def disconnected(self):
        self.finish(False, "disconnected")
        self.catalog = None
        self.last_id = -1

    def handle(self, message):
        kind = message.get("type")
        if kind == "catalog":
            map_name = message.get("map")
            epoch, revision = message.get("epoch"), message.get("revision")
            entries = message.get("entries")
            if (not isinstance(map_name, str) or not unsigned(epoch)
                    or not unsigned(revision) or not isinstance(entries, list)):
                raise ValueError("目录格式无效")
            identities = set()
            for entry in entries:
                if (not isinstance(entry, dict) or not isinstance(entry.get("id"), str)
                        or not entry["id"] or entry["id"] in identities
                        or not isinstance(entry.get("command"), str)
                        or not entry["command"].strip()):
                    raise ValueError("目录条目无效")
                identities.add(entry["id"])
            if self.catalog and (map_name, epoch, revision) == (
                    self.catalog.map, self.catalog.epoch, self.catalog.revision):
                # 相同版本重传不取消当前轮次；正式服务端保证版本对应相同内容。
                self.send({"type": "ready", "epoch": epoch, "revision": revision})
                return
            if self.round:
                self.fail("catalog_changed")
            self.catalog = None
            keywords, labels = self.engine.prepare(entries)
            self.catalog = Catalog(map_name, epoch, revision, keywords, labels)
            self.send({"type": "ready", "epoch": epoch, "revision": revision})
            LOG.info("目录就绪 地图=%s 候选=%s 版本=%s/%s", map_name, len(labels), epoch, revision)
            return
        ident = message.get("id")
        if not unsigned(ident):
            raise ValueError("轮次编号无效")
        if kind == "begin":
            if self.round or ident <= self.last_id:
                return
            self.last_id = ident
            c = self.catalog
            self.round = Round(ident, c, self.clock(), deadline=self.clock() + 10.0)
            if c is None or not c.map or (message.get("map"), message.get("epoch"),
                    message.get("revision")) != (c.map, c.epoch, c.revision):
                self.fail("not_ready")
                return
            if not c.labels:
                self.fail("no_recognizable_candidates")
                return
            try:
                self.engine.begin(c.keywords)
                self.audio.start()
                LOG.info("轮次=%s 已开始收音", ident)
            except Exception:
                LOG.exception("开始收音失败")
                self.fail("audio_start_failed")
            return
        current = self.round
        if current is None or current.id != ident:
            return
        if kind == "cancel":
            self.finish(False, "cancelled")
        elif kind == "end" and current.phase == "capture":
            try:
                self.audio.stop()
                self.pump_audio()
                if self.round is not current:
                    return
                labels = self.engine.end()
                if len(labels) != 1:
                    self.fail("ambiguous" if labels else "no_match")
                    return
                plan = current.catalog.labels.get(next(iter(labels)))
                if plan is None:
                    self.fail("no_match")
                    return
                current.phase = "result"
                current.deadline = self.clock() + 8.0
                current.plan_id = plan
                self.send({"type": "select", "id": ident, "map": current.catalog.map,
                           "epoch": current.catalog.epoch, "revision": current.catalog.revision,
                           "plan_id": plan})
                LOG.info("轮次=%s 已提交选择", ident)
            except (OSError, ConnectionError):
                raise
            except Exception:
                LOG.exception("结束识别失败")
                self.fail("recognition_failed")
        elif kind == "result" and (current.phase == "result" or message.get("ok") is False):
            ok = (current.phase == "result" and message.get("ok") is True
                  and message.get("plan_id") == current.plan_id)
            self.finish(ok, message.get("reason", "result"))

    def pump_audio(self):
        if self.round is None or self.round.phase != "capture":
            return
        try:
            for samples in self.audio.read():
                self.engine.accept(samples)
        except Exception:
            LOG.exception("采音或流式识别失败")
            self.fail("audio_processing_failed")

    def tick(self):
        self.pump_audio()
        if self.round and self.clock() >= self.round.deadline:
            self.fail("capture_timeout" if self.round.phase == "capture" else "result_timeout")


class SherpaEngine:
    def __init__(self, model):
        import numpy as np
        import sherpa_onnx
        self.np, self.sherpa = np, sherpa_onnx
        self.tokens = model / "tokens.txt"
        self.allowed = {line.split()[0] for line in self.tokens.read_text(encoding="utf-8").splitlines() if line.strip()}
        def model_file(part):
            path = model / (part + "-epoch-12-avg-2-chunk-16-left-64.int8.onnx")
            if not path.is_file():
                raise ValueError("模型文件缺失：" + str(path))
            return str(path)
        self.kws = sherpa_onnx.KeywordSpotter(tokens=str(self.tokens),
            encoder=model_file("encoder"), decoder=model_file("decoder"), joiner=model_file("joiner"),
            keywords_file=str(Path(__file__).with_name("empty_keywords.txt")), num_threads=2, provider="cpu")
        self.stream = None
        self.matches = set()

    def prepare(self, entries):
        groups = {}
        for index, entry in enumerate(entries):
            # 中文限定口令，不猜数字、标点或拉丁缩写的读法。
            command = entry["command"].strip()
            if not command or any(not ('\u3400' <= char <= '\u9fff') for char in command):
                LOG.warning("候选=%s 需要中文口令，未加入识别", entry["id"])
                continue
            # 与官方 ppinyin 单元一致，直接复用 pypinyin，避免引入无关 BPE 依赖。
            from pypinyin import pinyin
            from pypinyin.contrib.tone_convert import to_initials, to_finals_tone
            tokens = []
            for syllable in pinyin(command):
                tokens.extend(part for part in (to_initials(syllable[0], strict=False),
                    to_finals_tone(syllable[0], strict=False)) if part)
            if not tokens or any(token not in self.allowed for token in tokens):
                LOG.warning("候选=%s 含模型不支持的发音", entry["id"])
                continue
            groups.setdefault(tuple(tokens), []).append(("k" + str(index), entry["id"]))
        lines, labels = [], {}
        for tokens, candidates in groups.items():
            if len(candidates) != 1:
                LOG.warning("同音口令歧义，排除 %s 个候选", len(candidates))
                continue
            label, identity = candidates[0]
            labels[label] = identity
            lines.append(" ".join(tokens) + " @" + label)
        return "/".join(lines), labels

    def begin(self, keywords):
        self.stream = self.kws.create_stream(keywords)
        self.matches = set()

    def decode(self):
        while self.kws.is_ready(self.stream):
            self.kws.decode_stream(self.stream)
            result = self.kws.get_result(self.stream)
            if result:
                self.matches.add(result)
                self.kws.reset_stream(self.stream)

    def accept(self, samples):
        self.stream.accept_waveform(RATE, samples)
        self.decode()

    def end(self):
        self.accept(self.np.zeros(int(0.66 * RATE), dtype=self.np.float32))
        self.stream.input_finished()
        self.decode()
        return self.matches


class Microphone:
    def __init__(self):
        self.stream = None
        self.blocks = queue.Queue(maxsize=64)
        self.error = False

    def start(self):
        import sounddevice as sd
        self.blocks = queue.Queue(maxsize=64)
        self.error = False
        def callback(data, frames, timing, status):
            if status:
                self.error = True
            try:
                self.blocks.put_nowait(data[:, 0].copy())
            except queue.Full:
                self.error = True
        self.stream = sd.InputStream(samplerate=RATE, channels=1, dtype="float32",
                                     blocksize=320, callback=callback)
        self.stream.start()

    def stop(self):
        stream, self.stream = self.stream, None
        if stream:
            try:
                stream.stop()
            finally:
                stream.close()

    def read(self):
        if self.error:
            raise RuntimeError("音频溢出或设备失效")
        for _ in range(64):
            try:
                yield self.blocks.get_nowait()
            except queue.Empty:
                return


def tone_wave(ok):
    """两种固定短音，以默认输出设备播放；不合成语音。"""
    frequency = 880 if ok else 330
    count = int(RATE * 0.12)
    pcm = bytearray()
    for index in range(count):
        envelope = min(1.0, index / 160, (count - index) / 160)
        pcm.extend(struct.pack("<h", int(6000 * envelope * math.sin(2 * math.pi * frequency * index / RATE))))
    output = io.BytesIO()
    with wave.open(output, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(RATE)
        wav.writeframes(pcm)
    return output.getvalue()


class Tones:
    def __init__(self):
        self.queue = queue.Queue()
        self.data = {ok: tone_wave(ok) for ok in (False, True)}
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def __call__(self, ok):
        self.queue.put(ok)

    def run(self):
        import winsound
        while True:
            ok = self.queue.get()
            try:
                winsound.PlaySound(self.data[ok], winsound.SND_MEMORY | winsound.SND_NODEFAULT)
            except RuntimeError:
                LOG.exception("提示音播放失败")
            finally:
                self.queue.task_done()


def run(host, port, model):
    # 初始化时加载音频依赖，按下时才打开麦克风，避免首次导入延迟吞掉首音。
    import sounddevice
    engine = SherpaEngine(model)
    tones = Tones()
    audio = Microphone()
    controller = None
    try:
        while True:
            try:
                with socket.create_connection((host, port), timeout=3.0) as connection:
                    connection.settimeout(2.0)
                    def send(message):
                        payload = json.dumps(message, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
                        if len(payload) > MAX_LINE:
                            raise ValueError("发送帧超过 64 KiB")
                        connection.sendall(payload + b"\n")
                    controller = Controller(engine, audio, send, tones)
                    parser = JsonLines()
                    LOG.info("已连接辅机，等待目录")
                    while True:
                        if select.select([connection], [], [], 0.02)[0]:
                            data = connection.recv(8192)
                            if not data:
                                raise ConnectionError("辅机断开")
                            for message in parser.feed(data):
                                controller.handle(message)
                        controller.tick()
            except (OSError, ValueError, RuntimeError):
                LOG.exception("连接不可用，三秒后重新连接；不重放旧请求")
                if controller:
                    controller.disconnected()
                time.sleep(3.0)
    finally:
        if controller:
            controller.disconnected()
        audio.stop()
        tones.queue.join()


def main():
    parser = argparse.ArgumentParser(description="Xen 本地语音切换；Ctrl+C 退出")
    parser.add_argument("--host", required=True, help="辅机地址")
    parser.add_argument("--port", type=int, default=5014)
    parser.add_argument("--model", type=Path, default=Path(__file__).resolve().parent / "model")
    args = parser.parse_args()
    log_directory = Path(__file__).resolve().parent / "logs"
    log_directory.mkdir(parents=True, exist_ok=True)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s",
        handlers=[logging.StreamHandler(), RotatingFileHandler(log_directory / "voice.log",
            maxBytes=2*1024*1024, backupCount=2, encoding="utf-8")])
    try:
        run(args.host, args.port, args.model)
    except KeyboardInterrupt:
        LOG.info("语音工具已退出")
    except Exception:
        LOG.exception("语音工具启动失败；请检查依赖与模型目录")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
