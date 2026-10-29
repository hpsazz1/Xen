#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""独立录制器合成图、回环HTTP和同步索引集成验证；不连接真实辅机或输入设备。"""

import argparse
import json
import math
import re
import socket
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path


PAINT_KEYS = {
    "schema", "stage", "marker_id", "source_session", "duration_ms",
    "source_paint_begin_steady_ns", "source_paint_end_steady_ns",
    "source_paint_system_unix_ns", "source_send_steady_ns", "source_send_system_unix_ns",
}
RECEIPT_KEYS = PAINT_KEYS | {
    "source_reply_received_steady_ns", "aux_received_steady_ns", "aux_replied_steady_ns",
}
STAMP_KEYS = PAINT_KEYS - {"schema", "stage", "marker_id", "source_session", "duration_ms"}
IDENTIFIER = re.compile(r"[A-Za-z0-9._-]{1,96}\Z")


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def stamp(document, key):
    value = document.get(key)
    require(isinstance(value, str) and value.isascii() and value.isdigit() and len(value) <= 19,
            f"{key}必须为正整数字符串")
    number = int(value)
    require(0 < number <= 2**63 - 1, f"{key}超出有效时钟范围")
    return number


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, "HTTP消息含重复JSON字段")
        result[key] = value
    return result


class AuxiliaryServer(HTTPServer):
    """仅模拟既有paint/receipt协议；queued不代表真实Runtime已经归档。"""

    def __init__(self):
        super().__init__(("127.0.0.1", 0), AuxiliaryHandler)
        self.paints = {}
        self.receipts = {}
        self.requests = []
        self.failures = []

    @property
    def url(self):
        return f"http://127.0.0.1:{self.server_address[1]}"


class AuxiliaryHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_):
        pass  # 不把HTTP请求或其他应用日志混入测试结果。

    def do_POST(self):
        received = time.monotonic_ns()
        try:
            require(self.path == "/sync-marker.json", "同步请求路径不符")
            require(self.headers.get_content_type() == "application/json", "同步消息不是JSON")
            length = int(self.headers.get("Content-Length", "0"))
            require(0 < length <= 8192, "HTTP请求体长度无效")
            document = json.loads(self.rfile.read(length).decode("utf-8"), object_pairs_hook=unique_object)
            require(isinstance(document, dict), "HTTP消息必须为对象")
            stage = document.get("stage")
            require(stage in ("paint", "receipt"), "同步阶段无效")
            require(set(document) == (PAINT_KEYS if stage == "paint" else RECEIPT_KEYS),
                    "paint必须严格10字段，receipt必须严格13字段")
            require(type(document["schema"]) is int and document["schema"] == 1, "协议版本无效")
            require(type(document["duration_ms"]) is int and document["duration_ms"] == 2000, "显示时长合同无效")
            for key in ("marker_id", "source_session"):
                require(isinstance(document[key], str) and IDENTIFIER.fullmatch(document[key]), f"{key}格式无效")
            for key in STAMP_KEYS:
                stamp(document, key)
            begin = stamp(document, "source_paint_begin_steady_ns")
            end = stamp(document, "source_paint_end_steady_ns")
            sent = stamp(document, "source_send_steady_ns")
            require(begin <= end <= sent and end - begin <= 2_000_000_000, "绘制/发送时序无效")
            marker_id = document["marker_id"]
            if stage == "paint":
                require(marker_id not in self.server.paints, "本测试不允许重复paint或隐藏重试")
                reply = {
                    "state": "queued", "archive_status": "pending_app_delivery", "marker_id": marker_id,
                    "aux_received_steady_ns": str(received), "aux_replied_steady_ns": str(time.monotonic_ns()),
                }
                self.server.paints[marker_id] = {"document": document, "reply": reply}
            else:
                require(marker_id in self.server.paints and marker_id not in self.server.receipts,
                        "receipt缺少对应paint或出现重复回执")
                paint = self.server.paints[marker_id]
                reply = paint["reply"]
                source_received = stamp(document, "source_reply_received_steady_ns")
                expected = dict(paint["document"], stage="receipt",
                                source_reply_received_steady_ns=document["source_reply_received_steady_ns"],
                                aux_received_steady_ns=reply["aux_received_steady_ns"],
                                aux_replied_steady_ns=reply["aux_replied_steady_ns"])
                require(document == expected, "receipt未原样回显paint和本服务时间戳")
                elapsed = stamp(reply, "aux_replied_steady_ns") - stamp(reply, "aux_received_steady_ns")
                require(0 <= source_received - sent - elapsed <= 2_000_000_000, "四戳网络往返无效")
                self.server.receipts[marker_id] = document
            self.server.requests.append(document)
            self.respond(200, reply)
        except Exception as failure:
            self.server.failures.append(str(failure))
            self.respond(400, {"state": "rejected", "reason": str(failure)})

    def do_GET(self):
        self.server.failures.append("出现协议外GET请求")
        self.respond(405, {"state": "rejected", "reason": "只接受同步POST"})

    def respond(self, status, document):
        body = json.dumps(document, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        # WinHTTP每次请求结束即关闭连接；明确单次响应，避免服务端继续读取已关闭的keep-alive。
        self.send_header("Connection", "close")
        self.close_connection = True
        self.end_headers()
        self.wfile.write(body)


def validate_sidecar(recording, auxiliary, refusal):
    videos = list(recording.glob("*.mkv"))
    require(len(videos) == 1 and videos[0].stat().st_size > 0, "必须保留唯一非空MKV录像")
    indexes = list(recording.glob("*.sync.jsonl"))
    require(indexes == [videos[0].with_suffix(".sync.jsonl")], "录像和sidecar必须同名且唯一")
    rows = [json.loads(line, object_pairs_hook=unique_object)
            for line in indexes[0].read_text(encoding="utf-8").splitlines() if line.strip()]
    require(rows and rows[0].get("type") == "session" and rows[-1].get("type") == "closed", "索引缺少首session或末closed")
    require(sum(row.get("type") == "session" for row in rows) == 1 and
            sum(row.get("type") == "closed" for row in rows) == 1, "会话或封尾记录重复")
    session = rows[0]
    require(session.get("schema") == 1 and session.get("video_filename") == videos[0].name and
            session.get("auxiliary_enabled") is True, "sidecar会话身份或辅机开关不符")
    markers = [row for row in rows if row.get("type") == "encoded_marker"]
    syncs = [row for row in rows if row.get("type") == "auxiliary_sync"]
    require(len(markers) == len(syncs) == 2, "首尾应各有一个编码标记及辅机同步结果")
    require(len({row["marker_id"] for row in markers}) == 2 and
            {row["marker_id"] for row in markers} == {row["marker_id"] for row in syncs}, "标记编号缺失、重复或交叉错配")
    for marker in markers:
        require(marker.get("matched") is True and marker.get("error") == "", "编码标记未实际匹配")
        require(stamp(marker, "render_end_steady_ns") >= stamp(marker, "render_begin_steady_ns") and
                stamp(marker, "render_end_system_unix_ns") >= stamp(marker, "render_begin_system_unix_ns"), "标记绘制时间倒退")
        stamp(marker, "render_frame_time_ns")
        require(type(marker.get("video_pts")) is int and marker["video_pts"] >= 0 and
                marker.get("timebase_num", 0) > 0 and marker.get("timebase_den", 0) > 0, "编码PTS有理数无效")
        seconds = marker["video_pts"] * marker["timebase_num"] / marker["timebase_den"]
        require(math.isclose(marker["video_seconds"], seconds, abs_tol=1e-9), "索引秒数与编码PTS不一致")
    require(markers[0]["video_seconds"] < markers[1]["video_seconds"], "首尾编码标记时序倒退")
    closed = rows[-1]
    require(closed.get("stop_completed") is True and closed.get("last_error") == "" and
            closed.get("local_write_failure") is False, "录制或本地索引未成功封尾")
    require((closed.get("width"), closed.get("height"), closed.get("fps")) == (1280, 720, 120), "未使用指定合成图和120FPS")
    require(closed.get("markers_requested") == closed.get("markers_matched") == 2 and
            closed.get("encoded_packets", 0) > 0 and closed.get("total_frames", 0) > 0, "标记数量或实际编码计数不符")
    for key in ("markers_dropped", "markers_pending", "render_queue_gaps", "packet_queue_gaps", "sync_queue_dropped"):
        require(closed.get(key) == 0, f"存在{key}缺口")
    if refusal:
        require(closed.get("auxiliary_queued") == 0 and closed.get("auxiliary_failed") == 2 and
                all(row.get("state") == "failed" and row.get("reason") for row in syncs), "网络拒绝不能抹除本地标记，应保留两条同步失败")
    else:
        require(not auxiliary.failures and len(auxiliary.requests) == 4 and
                len(auxiliary.paints) == len(auxiliary.receipts) == 2, "回环端未完整收到严格的两对paint/receipt")
        require(closed.get("auxiliary_queued") == 2 and closed.get("auxiliary_failed") == 0, "辅机同步计数不符")
        for sync in syncs:
            marker_id = sync["marker_id"]
            marker = next(item for item in markers if item["marker_id"] == marker_id)
            require(sync.get("state") == "queued" and sync.get("archive_status") == "pending_app_delivery" and
                    sync.get("paint") == auxiliary.paints[marker_id]["document"] and
                    sync.get("receipt") == auxiliary.receipts[marker_id], "sidecar未保存实际发送和确认的同一消息")
            paint, receipt = sync["paint"], sync["receipt"]
            require(paint["source_session"] == session["source_session"] and
                    paint["source_paint_begin_steady_ns"] == marker["render_begin_steady_ns"] and
                    paint["source_paint_end_steady_ns"] == marker["render_end_steady_ns"] and
                    paint["source_paint_system_unix_ns"] == marker["render_begin_system_unix_ns"], "网络同步未绑定实际编码可见帧的绘制区间")
            sent, received = stamp(paint, "source_send_steady_ns"), stamp(receipt, "source_reply_received_steady_ns")
            aux_received, aux_replied = stamp(receipt, "aux_received_steady_ns"), stamp(receipt, "aux_replied_steady_ns")
            rtt = (received - sent) - (aux_replied - aux_received)
            mapping = sync["clock_mapping"]
            offset = aux_received - sent - rtt // 2
            require(int(mapping["network_round_trip_ns"]) == rtt and int(mapping["offset_aux_minus_source_ns"]) == offset and
                    int(mapping["network_uncertainty_ns"]) == (rtt + 1) // 2 and int(mapping["offset_rounding_ns"]) == rtt % 2 and
                    int(mapping["mapped_paint_begin_aux_steady_ns"]) == stamp(marker, "render_begin_steady_ns") + offset,
                    "sidecar四戳映射或整数舍入不符")
    return {"video": str(videos[0]), "sidecar": str(indexes[0]), "marker_ids": [row["marker_id"] for row in markers], "closed": closed}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--recorder", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--obs-root", type=Path)
    parser.add_argument("--case", choices=("positive", "network-refusal"), default="positive")
    parser.add_argument("--timeout", type=float, default=45)
    args = parser.parse_args()
    output = args.output.resolve()
    report = {"schema": 1, "case": args.case, "success": False,
              "limits": ["只验证合成图、回环协议和本地封尾；不证明真实辅机归档、视频逐帧像素或游戏急停效果。"]}
    prepared, auxiliary, thread, refusal_socket = False, None, None, None
    try:
        require(args.recorder.resolve().is_file(), "指定录制程序不存在")
        require(args.timeout >= 15, "子进程超时至少15秒")
        require(not output.exists() or (output.is_dir() and not any(output.iterdir())), "输出目录已有内容，拒绝覆盖或清理")
        output.mkdir(parents=True, exist_ok=True)
        prepared = True
        if args.case == "positive":
            auxiliary = AuxiliaryServer()
            thread = threading.Thread(target=auxiliary.serve_forever, daemon=True)
            thread.start()
            url = auxiliary.url
        else:
            # 保持端口绑定但不listen，稳定制造本机连接拒绝，不接触任何外部地址。
            refusal_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            refusal_socket.bind(("127.0.0.1", 0))
            url = f"http://127.0.0.1:{refusal_socket.getsockname()[1]}"
        recording = output / "recording"
        command = [str(args.recorder.resolve()), "--synthetic", "--width", "1280", "--height", "720",
                   "--fps", "120", "--duration", "7", "--no-audio", "--aux-url", url, "--output", str(recording)]
        if args.obs_root:
            command += ["--obs-root", str(args.obs_root.resolve())]
        report["command"] = command
        with (output / "stdout.log").open("xb") as stdout, (output / "stderr.log").open("xb") as stderr:
            process = subprocess.run(command, stdout=stdout, stderr=stderr, timeout=args.timeout, check=False)
        report["returncode"] = process.returncode
        require(process.returncode == 0, "录制器进程未正常完成")
        if auxiliary:
            auxiliary.shutdown()
            thread.join(timeout=3)
            require(not thread.is_alive(), "本机HTTP服务未正常结束")
        report.update(validate_sidecar(recording, auxiliary, args.case == "network-refusal"))
        report["success"] = True
    except Exception as failure:
        report["error"] = str(failure)
    finally:
        if auxiliary:
            auxiliary.shutdown()
            auxiliary.server_close()
            if thread:
                thread.join(timeout=3)
            report["requests"] = auxiliary.requests
            report["http_failures"] = auxiliary.failures
        if refusal_socket:
            refusal_socket.close()
        if prepared:
            with (output / "integration-report.json").open("x", encoding="utf-8") as stream:
                json.dump(report, stream, ensure_ascii=False, indent=2)
                stream.write("\n")
    print(json.dumps({"success": report["success"], "output": str(output), "error": report.get("error", "")}, ensure_ascii=False))
    return 0 if report["success"] else 1


if __name__ == "__main__":
    sys.exit(main())
