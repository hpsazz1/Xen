"""只在主机道具制作期间监听 F10；单次采图发送使用可终止子进程。"""

from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import subprocess
import sys
import time
import uuid

try:
    from .capture_workflow import ContextReader, DEFAULT_DESTINATION, DEFAULT_ROOT, SHARED_STANDING, LocalSavePending, capture_once, transfer_capture, write_json
except ImportError:
    from capture_workflow import ContextReader, DEFAULT_DESTINATION, DEFAULT_ROOT, SHARED_STANDING, LocalSavePending, capture_once, transfer_capture, write_json


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


class SingleJob:
    """只持有一个有期限的子进程；忙时丢弃按键，不排队补采。"""
    def __init__(self, launch, clock=time.monotonic, timeout=30):
        self.launch, self.clock, self.timeout = launch, clock, timeout
        self.process = None
        self.started = 0
        self.returncode = None

    def start(self, arguments):
        if self.process is not None:
            return False
        self.process = self.launch(arguments)
        self.started = self.clock()
        self.returncode = None
        return True

    def tick(self):
        if self.process is None:
            return None
        result = self.process.poll()
        if result is not None:
            self.returncode = result
            self.process = None
            return "finished"
        if self.clock() - self.started >= self.timeout:
            self.stop()
            return "timeout"
        return None

    def stop(self):
        process, self.process = self.process, None
        if process is None or process.poll() is not None:
            return
        process.terminate()
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                raise RuntimeError("制作子进程停止未确认，请核对进程及待发送记录")


def completion_notice(event, status, returncode):
    if event == "timeout":
        return "制作超过30秒，已停止本次作业；本地已有资源保留，发送结果未确认，不自动重采。"
    if event != "finished":
        return None
    if isinstance(status, dict) and status.get("terminal") is True:
        state = status.get("state")
        if (state == "sent" and returncode == 0) or (state in ("failed", "pending") and returncode not in (None, 0)):
            return None
    return "作业已退出（退出码" + str(returncode) + "），制作/发送结果未确认；保留本地恢复记录，不自动重采。"


def run_worker(args):
    status = Path(args.job_status)
    def report(state, message, terminal=False):
        write_json(status, {"state": state, "message": message, "terminal": terminal or state in ("sent", "failed")})
    metadata = None
    try:
        expected = {"window": args.window, "process": args.process}
        report("checking", "正在核对辅机当前方案")
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


def run_hotkey(args):
    if sys.platform != "win32":
        raise RuntimeError("F10 采集只支持Windows主机")
    ContextReader(args.host, args.web_port)
    root = args.root.resolve()
    jobs = root / "capture-jobs"
    jobs.mkdir(parents=True, exist_ok=True)
    user = ctypes.WinDLL("user32", use_last_error=True)
    user.RegisterHotKey.argtypes, user.RegisterHotKey.restype = [wintypes.HWND, ctypes.c_int, wintypes.UINT, wintypes.UINT], wintypes.BOOL
    user.UnregisterHotKey.argtypes = [wintypes.HWND, ctypes.c_int]
    user.PeekMessageW.argtypes = [ctypes.POINTER(wintypes.MSG), wintypes.HWND, wintypes.UINT, wintypes.UINT, wintypes.UINT]
    key_id, no_repeat, vk_f10 = 0x584E, 0x4000, 0x79
    if not user.RegisterHotKey(None, key_id, no_repeat, vk_f10):
        raise RuntimeError("F10注册失败，可能被其他程序占用：" + str(ctypes.get_last_error()))
    log = None
    def launch(arguments):
        nonlocal log
        log = open(arguments[-1] + ".log", "ab", buffering=0)
        try:
            return subprocess.Popen(arguments[:-1], stdin=subprocess.DEVNULL, stdout=log, stderr=log,
                                    creationflags=subprocess.CREATE_NO_WINDOW)
        except Exception:
            log.close()
            log = None
            raise
    job, current_status, last_status = SingleJob(launch), None, None
    print("F10制作已启用：试投成功→恢复原站位/正确瞄角→按F10；自动采图、命名、打包并发送。Ctrl+C退出。", flush=True)
    message = wintypes.MSG()
    try:
        while True:
            while user.PeekMessageW(ctypes.byref(message), None, 0, 0, 1):
                if message.message == 0x12:
                    return
                if message.message != 0x312 or message.wParam != key_id:
                    continue
                if job.process is not None:
                    print("本次制作尚未结束，忽略重复F10。", flush=True)
                    continue
                try:
                    foreground = foreground_identity()
                    current_status = jobs / (uuid.uuid4().hex + ".json")
                    arguments = [sys.executable, "-X", "utf8", "-B", str(Path(__file__).resolve()),
                                 "--worker", "--host", args.host, "--web-port", str(args.web_port),
                                 "--root", str(root), "--destination", str(args.destination),
                                 "--shared-standing", str(args.shared_standing), "--window", str(foreground["window"]),
                                 "--process", str(foreground["process"]), "--job-status", str(current_status)]
                    job.start(arguments + [str(current_status)])
                    last_status = None
                except Exception as error:
                    print("未开始：" + str(error), flush=True)
            event = job.tick()
            if current_status and current_status.exists():
                try:
                    state = json.loads(current_status.read_text(encoding="utf-8"))
                    if not isinstance(state, dict) or not isinstance(state.get("message"), str):
                        raise ValueError("作业记录无效")
                    if state != last_status:
                        print(state["message"], flush=True)
                        last_status = state
                except (OSError, ValueError, KeyError):
                    pass
            if event in ("finished", "timeout"):
                if log:
                    log.close()
                    log = None
                notice = completion_notice(event, last_status, job.returncode)
                if notice:
                    print(notice, flush=True)
                current_status = None
            time.sleep(.02)
    finally:
        user.UnregisterHotKey(None, key_id)
        if job.process is not None:
            print("正在停止制作作业；如已开始发送，请以资源回读为准，未确认的记录保留。", flush=True)
        job.stop()
        if log:
            log.close()


def main():
    parser = argparse.ArgumentParser(description="主机F10自动采集道具双图并发送辅机，无游戏输入")
    parser.add_argument("--host", required=True, help="辅机局域网IPv4地址")
    parser.add_argument("--web-port", type=int, default=5015)
    parser.add_argument("--root", type=Path, default=DEFAULT_ROOT)
    parser.add_argument("--destination", type=Path, default=Path(DEFAULT_DESTINATION))
    parser.add_argument("--shared-standing", type=Path, default=SHARED_STANDING)
    parser.add_argument("--worker", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--job-status", help=argparse.SUPPRESS)
    parser.add_argument("--window", type=int, help=argparse.SUPPRESS)
    parser.add_argument("--process", type=int, help=argparse.SUPPRESS)
    args = parser.parse_args()
    try:
        if args.worker:
            if not args.job_status or not args.window or not args.process:
                raise ValueError("制作子进程缺少前台窗口或作业记录")
            return run_worker(args)
        run_hotkey(args)
        return 0
    except KeyboardInterrupt:
        print("F10制作已关闭。", flush=True)
        return 0
    except Exception as error:
        print("F10制作未启动或已停止：" + str(error), flush=True)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
