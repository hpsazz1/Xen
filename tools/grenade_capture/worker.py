"""OBS 插件启动的单次 F10 采集作业；无常驻监听、语音或游戏输入。"""

from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
from pathlib import Path
import sys
import time

try:
    from .capture_workflow import ContextReader, SHARED_STANDING, LocalSavePending, capture_once, transfer_capture, write_json
except ImportError:
    from capture_workflow import ContextReader, SHARED_STANDING, LocalSavePending, capture_once, transfer_capture, write_json


def foreground_identity():
    if sys.platform != "win32":
        raise RuntimeError("F10 采集只支持Windows主机")
    user, kernel = ctypes.WinDLL("user32", use_last_error=True), ctypes.WinDLL("kernel32", use_last_error=True)
    user.GetForegroundWindow.restype = wintypes.HWND
    user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    kernel.OpenProcess.argtypes, kernel.OpenProcess.restype = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD], wintypes.HANDLE
    kernel.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    window, process = user.GetForegroundWindow(), wintypes.DWORD()
    if not window or not user.GetWindowThreadProcessId(window, ctypes.byref(process)):
        raise ValueError("没有可核对的前台游戏窗口")
    handle = kernel.OpenProcess(0x1000, False, process.value)
    if not handle:
        raise ValueError("无法核对前台游戏窗口")
    try:
        size = wintypes.DWORD(32768)
        path = ctypes.create_unicode_buffer(size.value)
        if not kernel.QueryFullProcessImageNameW(handle, 0, path, ctypes.byref(size)) or Path(path.value).name.lower() != "cs2.exe":
            raise ValueError("请恢复正确瞄角并切回CS2，再按F10")
    finally:
        kernel.CloseHandle(handle)
    return {"window": int(window), "process": process.value}


def run_worker(args):
    status = Path(args.job_status)
    # 状态目录由插件选择；作业只写自己的单份状态文件，不注册热键或常驻运行。
    status.parent.mkdir(parents=True, exist_ok=True)

    def report(state, message, terminal=False):
        write_json(status, {"state": state, "message": message, "terminal": terminal or state in ("sent", "failed")})

    metadata = None
    try:
        expected = {"window": args.window, "process": args.process}
        report("checking", "正在读取当前方案")
        metadata = capture_once(ContextReader(args.host, args.web_port), args.root,
                                shared_standing=args.shared_standing, foreground=foreground_identity,
                                expected_foreground=expected, report=report)
        transfer_capture(args.root, metadata["plan_id"][:-5], args.destination,
                         deadline=time.monotonic() + 20, report=report)
        return 0
    except Exception as error:
        pending = bool(metadata) or isinstance(error, LocalSavePending)
        state = "pending" if pending else "failed"
        prefix = "本地已保存，发送未确认：" if metadata else "" if pending else "采集未完成："
        report(state, prefix + str(error), terminal=True)
        return 2 if pending else 1


def main(argv=None):
    parser = argparse.ArgumentParser(description="OBS 道具插件内部单次采集作业")
    parser.add_argument("--host", required=True)
    parser.add_argument("--web-port", type=int, default=5015)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--destination", type=Path, required=True)
    parser.add_argument("--shared-standing", type=Path, default=SHARED_STANDING)
    parser.add_argument("--job-status", type=Path, required=True)
    parser.add_argument("--window", type=int, required=True)
    parser.add_argument("--process", type=int, required=True)
    args = parser.parse_args(argv)
    try:
        return run_worker(args)
    except Exception as error:
        print("采集作业未完成：" + str(error), flush=True)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
