# SPDX-License-Identifier: GPL-2.0-or-later
"""顺序解码 SYNC 蓝底，按实际 MKV Block 时间核对索引；不从帧率猜 PTS。"""
from __future__ import annotations

import argparse
from collections import Counter
from fractions import Fraction
import json
import math
import mmap
from pathlib import Path
import statistics
import struct
import sys

import cv2
import numpy as np


def read_mkv_video_blocks(path: Path) -> dict:
    """读取 OBS MKV 的视频 Block 时间，不应用播放器的起点归一化。

    仅支持已知长度 Cluster、单路 AVC、无视频 lacing 和默认 TrackTimestampScale。
    返回 pts_ticks 与 timestamp_scale_ns；二者乘积为容器原始呈现时间纳秒。
    不能将此轻量读取器用于任意 Matroska 文件或 B 帧重排后按序号配帧。
    """
    with path.open("rb") as file, mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
        if data[:4] != b"\x1a\x45\xdf\xa3":
            raise ValueError("不是受支持的 Matroska/EBML 文件")

        def vint(position: int, limit: int, identifier: bool = False) -> tuple[int, int]:
            if position >= limit or not data[position]:
                raise ValueError("EBML VINT 截断或无效")
            length = 9 - data[position].bit_length()
            if position + length > limit or (identifier and length > 4):
                raise ValueError("EBML VINT 长度无效")
            value = int.from_bytes(data[position:position + length], "big")
            return (value if identifier else value & ((1 << (7 * length)) - 1)), length

        def elements(start: int, end: int):
            position = start
            while position < end:
                element, head = vint(position, end, True)
                size, width = vint(position + head, end)
                begin = position + head + width
                if size == (1 << (7 * width)) - 1:
                    if element != 0x18538067:
                        raise ValueError("不支持未知长度的非 Segment 元素")
                    size = end - begin
                finish = begin + size
                if finish > end:
                    raise ValueError("EBML 元素越过文件或父元素边界")
                yield element, begin, finish
                position = finish

        def unsigned(start: int, end: int) -> int:
            return int.from_bytes(data[start:end], "big")

        scale, tracks, blocks = 1_000_000, {}, []
        for element, begin, finish in elements(0, len(data)):
            if element != 0x18538067:
                continue
            for child, start, end in elements(begin, finish):
                if child == 0x1549A966:
                    for field, first, last in elements(start, end):
                        if field == 0x2AD7B1:
                            scale = unsigned(first, last)
                elif child == 0x1654AE6B:
                    for entry, first, last in elements(start, end):
                        if entry != 0xAE:
                            continue
                        track = {"codec_delay_ns": 0, "timestamp_scale": 1.0}
                        for field, a, b in elements(first, last):
                            if field in (0xD7, 0x83, 0x56AA, 0x23E383):
                                key = {0xD7: "number", 0x83: "type", 0x56AA: "codec_delay_ns",
                                       0x23E383: "default_duration_ns"}[field]
                                track[key] = unsigned(a, b)
                            elif field == 0x86:
                                track["codec"] = data[a:b].decode("ascii")
                            elif field == 0x23314F:
                                if b - a not in (4, 8):
                                    raise ValueError("无效 TrackTimestampScale")
                                track["timestamp_scale"] = struct.unpack(">f" if b - a == 4 else ">d", data[a:b])[0]
                        tracks[track["number"]] = track
                elif child == 0x1F43B675:
                    children = list(elements(start, end))
                    cluster_time = next((unsigned(a, b) for field, a, b in children if field == 0xE7), None)
                    if cluster_time is None:
                        raise ValueError("Cluster 缺少 Timestamp")
                    for field, a, b in children:
                        payloads = [(field, a, b)] if field == 0xA3 else list(elements(a, b)) if field == 0xA0 else []
                        for kind, first, last in payloads:
                            if kind not in (0xA3, 0xA1):
                                continue
                            number, width = vint(first, last)
                            if first + width + 3 > last:
                                raise ValueError("Block 头截断")
                            relative = int.from_bytes(data[first + width:first + width + 2], "big", signed=True)
                            blocks.append((number, cluster_time + relative, data[first + width + 2]))
        video_tracks = [track for track in tracks.values() if track.get("type") == 1]
        if len(video_tracks) != 1 or scale <= 0:
            raise ValueError("要求单路视频及有效 TimestampScale")
        video = video_tracks[0]
        if video.get("codec") != "V_MPEG4/ISO/AVC" or video["timestamp_scale"] != 1 or video["codec_delay_ns"]:
            raise ValueError("当前验证器仅核对默认时间比例且无 CodecDelay 的 AVC 视频轨")
        selected = [(pts, flags) for number, pts, flags in blocks if number == video["number"]]
        if not selected or any(flags & 0x06 for _, flags in selected):
            raise ValueError("视频 Block 缺失或使用不支持的 lacing")
        timestamps = [pts for pts, _ in selected]
        if any(b < a for a, b in zip(timestamps, timestamps[1:])):
            raise ValueError("视频 Block PTS 非递减条件不成立，不能按解码序号绑定")
        return {"timestamp_scale_ns": scale, "video_track": video, "tracks": list(tracks.values()),
                "pts_ticks": timestamps, "video_blocks": len(timestamps)}


def summarize(values: list[float]) -> dict:
    finite = [value for value in values if math.isfinite(value)]
    if not finite:
        return {"count": 0}
    return {"count": len(finite), "first": finite[0], "last": finite[-1], "min": min(finite),
            "max": max(finite), "mean": statistics.fmean(finite), "median": statistics.median(finite)}


def scan_video(path: Path) -> tuple[dict, list[float], list[np.ndarray]]:
    capture = cv2.VideoCapture(str(path))
    if not capture.isOpened():
        raise ValueError("OpenCV 无法打开视频")
    positions, intervals, images = [], [], []
    active = False
    try:
        fps = capture.get(cv2.CAP_PROP_FPS)
        while True:
            okay, frame = capture.read()
            if not okay:
                break
            index = len(positions)
            positions.append(capture.get(cv2.CAP_PROP_POS_MSEC))
            region = frame[8:80, 8:568]
            fraction = float(np.mean(np.all(np.abs(region.astype(np.int16) - [120, 66, 18]) <= 25, axis=2))) if region.size else 0.0
            blue = fraction > 0.55
            if blue and not active:
                intervals.append({"start_frame": index, "end_frame_exclusive": None,
                                  "opencv_start_msec": positions[-1], "blue_fraction_at_start": fraction})
                if len(images) < 16:
                    images.append(region.copy())
            if active and not blue:
                intervals[-1]["end_frame_exclusive"] = index
            active = blue
        if active:
            intervals[-1]["end_frame_exclusive"] = len(positions)
        return {"decoded_frames": len(positions), "reported_fps": fps, "visible_intervals": intervals,
                "opencv_position_msec": summarize(positions),
                "opencv_frame_interval_msec": summarize([b - a for a, b in zip(positions, positions[1:])])}, positions, images
    finally:
        capture.release()


def contact_sheet(path: Path, images: list[np.ndarray], intervals: list[dict], markers: list[dict]) -> None:
    tiles = []
    for index, image in enumerate(images):
        width = min(1600, image.shape[1])
        image = cv2.resize(image, (width, max(1, round(image.shape[0] * width / image.shape[1]))))
        tile = np.zeros((image.shape[0] + 48, 1600, 3), np.uint8)
        tile[48:, :width] = image
        label = markers[index].get("marker_id", "unknown") if index < len(markers) else "no_index"
        text = f"{index + 1}: frame={intervals[index]['start_frame']}  {label}"
        cv2.putText(tile, text, (8, 29), cv2.FONT_HERSHEY_SIMPLEX, 0.65, (255, 255, 255), 1, cv2.LINE_AA)
        tiles.append(tile)
    if not tiles:
        tiles = [np.zeros((100, 1600, 3), np.uint8)]
        cv2.putText(tiles[0], "No visible SYNC marker", (20, 60), cv2.FONT_HERSHEY_SIMPLEX, 1, (255, 255, 255), 2)
    path.parent.mkdir(parents=True, exist_ok=True)
    okay, encoded = cv2.imencode(".png", np.vstack(tiles))
    if not okay:
        raise ValueError("contact sheet 编码失败")
    path.write_bytes(encoded.tobytes())


def verify(args: argparse.Namespace) -> dict:
    report = {"schema": 1, "video": str(args.video.resolve()), "passed": False, "errors": [],
              "opencv_version": cv2.__version__, "expected_markers": args.expect_markers,
              "pts_tolerance_msec": 0.6, "marker_detection": {"roi_xyxy": [8, 8, 568, 80],
              "rgb": [18, 66, 120], "channel_tolerance": 25, "fraction_threshold_exclusive": 0.55},
              "limitations": ["可见段按顺序对应索引；完整marker_id需人工核对contact sheet，未自动OCR",
                              "仅支持单路AVC、无视频lacing且Block PTS非递减的OBS MKV序号关联"]}
    errors = report["errors"]
    index_path = args.index if args.index is not None else args.video.with_suffix(".sync.jsonl")
    markers, closed = [], None
    if args.index is not None or index_path.exists():
        records = [json.loads(line) for line in index_path.read_text(encoding="utf-8-sig").splitlines() if line.strip()]
        if any(not isinstance(record, dict) for record in records):
            raise ValueError("同步索引含非对象记录")
        markers = [record for record in records if record.get("type") == "encoded_marker"]
        endings = [record for record in records if record.get("type") == "closed"]
        closed = endings[-1] if endings else None
        report.update(index=str(index_path.resolve()), scope="container_pts_and_marker_presence", closed=closed)
        if len(endings) != 1 or not records or records[-1].get("type") != "closed":
            errors.append("同步索引必须有唯一且位于末尾的closed记录")
        if closed is not None:
            if closed.get("stop_completed") is not True or closed.get("last_error") or closed.get("local_write_failure"):
                errors.append("索引封尾存在错误或未完成")
            for name in ("encoder_quiescent", "packet_timing_valid", "packet_mapping_complete"):
                if name in closed and closed[name] is not True:
                    errors.append(f"封尾时间完整性未确认：{name}")
            for name in ("markers_dropped", "markers_pending", "render_queue_gaps", "packet_queue_gaps",
                         "sync_queue_dropped", "render_lagged_frames", "encoder_skipped_frames", "output_dropped_frames"):
                if closed.get(name, 0):
                    errors.append(f"封尾指标非零：{name}={closed[name]}")
        if len(markers) != args.expect_markers or len({marker.get("marker_id") for marker in markers}) != len(markers):
            errors.append("索引marker数量不符或编号重复")
        if any(marker.get("matched") is not True or marker.get("error") for marker in markers):
            errors.append("索引包含未匹配或错误marker")
    else:
        report.update(scope="marker_presence_only", index=None, closed=None)
        report["limitations"].append("没有同步索引，未验证PTS及封尾；passed仅表示可见marker数量检查")
    try:
        container = read_mkv_video_blocks(args.video)
        ticks = container.pop("pts_ticks")
        scale = container["timestamp_scale_ns"]
        container.update(first_video_pts_ticks=ticks[0], last_video_pts_ticks=ticks[-1],
                         frame_interval_ticks=dict(sorted(Counter(b - a for a, b in zip(ticks, ticks[1:])).items())))
        report["container"] = container
    except ValueError as failure:
        ticks, scale = [], 0
        report["container"] = {"available": False, "reason": str(failure)}
        if report["index"] is not None:
            errors.append("有索引但无法取得受支持的容器视频PTS")
    scan, positions, images = scan_video(args.video)
    report["decode"] = scan
    intervals = scan["visible_intervals"]
    if scan["decoded_frames"] == 0:
        errors.append("没有成功解码的视频帧")
    if len(intervals) != args.expect_markers:
        errors.append(f"可见SYNC段数量为{len(intervals)}，预期{args.expect_markers}")
    comparable = bool(ticks) and len(ticks) == len(positions)
    if ticks:
        report["container"]["decoded_count_matches_blocks"] = comparable
        if not comparable:
            errors.append("视频Block数量与解码帧数不同，不能按序号确认PTS")
        else:
            report["opencv_minus_container_msec"] = summarize(
                [position - tick * scale / 1_000_000 for position, tick in zip(positions, ticks)])
        if closed is not None:
            report["container"]["premux_callback_count_minus_blocks"] = closed.get("encoded_packets", 0) - len(ticks)
            report["container"]["premux_count_note"] = "callback先于muxer停止边界判断；数量差不是独立的解码丢帧证据"
    comparisons = []
    for ordinal, (visible, marker) in enumerate(zip(intervals, markers), 1):
        item = {"ordinal": ordinal, "marker_id": marker.get("marker_id"), "visible_start_frame": visible["start_frame"],
                "opencv_pos_msec": visible["opencv_start_msec"], "passed": False}
        try:
            pts, numerator, denominator = (int(marker[name]) for name in ("video_pts", "timebase_num", "timebase_den"))
            if numerator <= 0 or denominator <= 0 or not comparable:
                raise ValueError("索引时间基无效或容器/解码序号未确认")
            actual_tick = ticks[visible["start_frame"]]
            expected, actual = Fraction(pts * numerator, denominator), Fraction(actual_tick * scale, 1_000_000_000)
            error = actual - expected
            item.update(index_video_pts=pts, index_timebase_num=numerator, index_timebase_den=denominator,
                        container_pts_ticks=actual_tick, container_timebase_num=scale, container_timebase_den=1_000_000_000,
                        index_pts_msec=float(expected * 1000), container_pts_msec=float(actual * 1000),
                        error_msec=float(error * 1000), passed=abs(error) <= Fraction(6, 10_000))
            if not item["passed"]:
                errors.append(f"marker {ordinal} 容器PTS误差超过0.6ms：{item['error_msec']:.6f}ms")
        except (ValueError, KeyError, ZeroDivisionError, TypeError) as failure:
            item["reason"] = str(failure)
            errors.append(f"marker {ordinal} 无法核对PTS：{failure}")
        comparisons.append(item)
    report["marker_comparisons"] = comparisons
    report["marker_id_ocr_verified"] = False
    report["container_pts_verified"] = report["index"] is not None and len(comparisons) == args.expect_markers and all(x["passed"] for x in comparisons)
    if args.contact_sheet:
        contact_sheet(args.contact_sheet, images, intervals, markers)
        report["contact_sheet"] = str(args.contact_sheet.resolve())
    report["passed"] = not errors
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--video", required=True, type=Path)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--index", type=Path)
    parser.add_argument("--contact-sheet", type=Path)
    parser.add_argument("--expect-markers", type=int, default=2)
    args = parser.parse_args()
    if args.expect_markers < 1:
        parser.error("--expect-markers 必须大于0")
    args.report = args.report or args.video.with_suffix(".verify.json")
    inputs = {args.video.resolve(), (args.index or args.video.with_suffix(".sync.jsonl")).resolve()}
    outputs = [args.report.resolve()] + ([args.contact_sheet.resolve()] if args.contact_sheet else [])
    if any(path in inputs for path in outputs) or len(set(outputs)) != len(outputs):
        parser.error("报告和contact sheet必须使用独立输出路径，不能覆盖录像、索引或彼此")
    try:
        report = verify(args)
    except Exception as failure:
        report = {"schema": 1, "video": str(args.video.resolve()), "passed": False,
                  "errors": [f"{type(failure).__name__}: {failure}"]}
    try:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    except Exception as failure:
        print(f"FAIL: 无法保存验证报告：{failure}", file=sys.stderr)
        return 1
    print(f"{'PASS' if report['passed'] else 'FAIL'}: {args.report}")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
