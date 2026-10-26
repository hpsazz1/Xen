"""独立于收音轮次的道具双图提示；只消费资源 ID 和状态，不接收实时视频。"""

from __future__ import annotations

import ctypes
import json
import logging
import math
from pathlib import Path
import queue
import subprocess
import sys
import threading
import time

try:
    from .reference_assets import load_reference, resource_directory
except ImportError:
    from reference_assets import load_reference, resource_directory


LOG = logging.getLogger("voice_switch")
STATES = {"hidden", "selected", "aligning", "aligned", "failed"}


class GuidanceState:
    def __init__(self, publish=None):
        self.publish = publish or (lambda value: None)
        self.revision = -1
        self.identity = None

    def hide(self, reset=False):
        self.identity = None
        if reset:
            self.revision = -1
        self.publish({"state": "hidden"})

    def handle(self, message):
        revision = message.get("revision")
        state = message.get("state")
        plan, reference = message.get("plan_id"), message.get("reference_id")
        if (type(revision) is not int or not 0 <= revision < 2**64 or not isinstance(state, str) or state not in STATES
                or not isinstance(plan, str) or not isinstance(reference, str)
                or not isinstance(message.get("message", ""), str)):
            raise ValueError("引导状态格式无效")
        if revision <= self.revision:
            return
        if state != "hidden":
            resource_directory(".", reference)
            if not plan.endswith(".json") or "/" in plan or "\\" in plan or len(plan) > 160:
                raise ValueError("引导方案 ID 无效")
            if self.identity is not None and state != "selected" and (plan, reference) != self.identity:
                return
        self.revision = revision
        self.identity = None if state == "hidden" else (plan, reference)
        self.publish(dict(message))


class GuidanceWindow:
    def __init__(self, root, corner="right"):
        self.messages = queue.Queue(maxsize=1)
        self.events = queue.Queue(maxsize=8)
        self.process = subprocess.Popen(
            [sys.executable, "-X", "utf8", "-B", str(Path(__file__).resolve()), "--window",
             "--root", str(root), "--corner", corner], stdin=subprocess.PIPE,
            encoding="utf-8", stdout=subprocess.PIPE,
            creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0)
        self.thread = threading.Thread(target=self._write, daemon=True, name="grenade-guidance-writer")
        self.thread.start()
        self.reader = threading.Thread(target=self._read, daemon=True, name="grenade-menu-reader")
        self.reader.start()

    def publish(self, value):
        # 只保留最新状态，渲染或资源读取慢时不阻塞语音网络。
        try:
            self.messages.put_nowait(value)
        except queue.Full:
            try:
                self.messages.get_nowait()
            except queue.Empty:
                pass
            try:
                self.messages.put_nowait(value)
            except queue.Full:
                pass

    def close(self):
        self.publish(None)
        self.thread.join(timeout=2)
        try:
            self.process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            self.process.terminate()
            self.process.wait(timeout=2)
        if self.process.stdin:
            self.process.stdin.close()
        self.reader.join(timeout=1)
        if self.process.stdout:
            self.process.stdout.close()

    def poll(self):
        values = []
        while True:
            try:
                values.append(self.events.get_nowait())
            except queue.Empty:
                return values

    def _read(self):
        try:
            for line in self.process.stdout:
                value = json.loads(line)
                if isinstance(value, dict):
                    try:
                        self.events.put_nowait(value)
                    except queue.Full:
                        LOG.warning("菜单请求繁忙，本次点击忽略")
        except (OSError, ValueError):
            LOG.warning("菜单窗口消息未完成")

    def _write(self):
        try:
            while True:
                value = self.messages.get()
                self.process.stdin.write(json.dumps(value, ensure_ascii=False) + "\n")
                self.process.stdin.flush()
                if value is None:
                    return
        except (OSError, ValueError):
            LOG.warning("双图窗口已经退出，语音识别继续运行")


class _WindowRenderer:
    def __init__(self, root, corner):
        self.root, self.corner = root, corner
        self.messages = queue.Queue(maxsize=1)

    publish = GuidanceWindow.publish

    def _run(self):
        if sys.platform != "win32":
            LOG.warning("道具窗口只支持 Windows")
            return
        import tkinter as tk
        from tkinter import ttk
        try:
            from .preview_window import PreviewWindow
        except ImportError:
            from preview_window import PreviewWindow
        user = ctypes.WinDLL("user32", use_last_error=True)
        pointer = ctypes.c_void_p
        user.GetForegroundWindow.restype = pointer
        user.SetForegroundWindow.argtypes = [pointer]
        user.GetParent.argtypes, user.GetParent.restype = [pointer], pointer
        user.IsWindow.argtypes = [pointer]
        if hasattr(user, "SetThreadDpiAwarenessContext"):
            user.SetThreadDpiAwarenessContext.argtypes = [pointer]
            user.SetThreadDpiAwarenessContext(pointer(-4))
        from ctypes import wintypes
        user.PeekMessageW.argtypes = [ctypes.POINTER(wintypes.MSG), pointer, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint]
        user.TranslateMessage.argtypes = [ctypes.POINTER(wintypes.MSG)]
        user.DispatchMessageW.argtypes = [ctypes.POINTER(wintypes.MSG)]
        user.DispatchMessageW.restype = ctypes.c_ssize_t
        window, heading, tree = None, None, None
        preview = PreviewWindow(self.root, self.corner)
        menu_open, previous, catalog, current = False, None, None, None
        signature, last_check = None, 0.0

        def create_menu():
            nonlocal window, heading, tree
            window = tk.Tk()
            window.withdraw()
            window.title("Xen 当前地图道具")
            window.attributes("-topmost", True)
            heading = ttk.Label(window, text="当前地图道具", font=("Microsoft YaHei UI", 13, "bold"))
            heading.pack(anchor="w", padx=14, pady=(12, 6))
            content = ttk.Frame(window)
            content.pack(fill="both", expand=True, padx=14)
            tree = ttk.Treeview(content, columns=("command", "reference"), show="tree headings", selectmode="browse")
            tree.heading("#0", text="阵营 / 站位与目标")
            tree.heading("command", text="可选语音口令")
            tree.heading("reference", text="双图参考")
            tree.column("#0", width=390, minwidth=200)
            tree.column("command", width=135, minwidth=80)
            tree.column("reference", width=100, minwidth=80)
            scroll = ttk.Scrollbar(content, orient="vertical", command=tree.yview)
            tree.configure(yscrollcommand=scroll.set)
            scroll.pack(side="right", fill="y")
            tree.pack(side="left", fill="both", expand=True)
            ttk.Label(window, text="点击方案返回游戏；上键对齐，下键投掷，左键或关闭窗口收起清单。语音可单独启用。",
                      wraplength=680).pack(anchor="w", padx=14, pady=12)
            window.geometry("720x560+16+16")
            tree.bind("<ButtonRelease-1>", select_item)
            window.protocol("WM_DELETE_WINDOW", hide_menu)
            window.bind("<Escape>", lambda event: hide_menu())

        def hide_menu():
            nonlocal menu_open, previous
            if menu_open:
                window.withdraw()
                menu_open = False
                if previous and user.IsWindow(previous):
                    user.SetForegroundWindow(previous)
                previous = None

        def show_menu(value):
            nonlocal menu_open, previous, catalog, current
            current = None
            preview.hide()
            catalog = value
            if not menu_open:
                previous = user.GetForegroundWindow()
            if window is None:
                create_menu()
            tree.delete(*tree.get_children())
            heading.configure(text=value.get("map", "当前地图") + " · 选择道具方案")
            groups = (("T", "T 阵营 · 名称有明确来源"), ("CT", "CT 阵营 · 名称有明确来源"), ("", "未标注阵营"))
            for team, title in groups:
                entries = [item for item in value.get("entries", []) if item.get("team", "") == team]
                if not entries:
                    continue
                parent = "group:" + (team or "unknown")
                tree.insert("", "end", iid=parent, text=title, open=True)
                for item in entries:
                    try:
                        load_reference(self.root, item["id"].removesuffix(".json"), item["id"])
                        status = "完整"
                    except (OSError, ValueError):
                        status = "待采集/核对"
                    tree.insert(parent, "end", iid=item["id"], text=item.get("name") or item["id"],
                                values=(item.get("command") or "未设置", status))
            if not value.get("entries"):
                tree.insert("", "end", iid="group:empty", text="当前地图还没有已保存道具方案")
            menu_open = True
            window.deiconify()
            window.lift()
            window.update_idletasks()
            handle = user.GetParent(window.winfo_id()) or window.winfo_id()
            user.SetForegroundWindow(handle)
            tree.focus_set()

        def select_item(event):
            if not menu_open or catalog is None:
                return
            identity = tree.identify_row(event.y)
            if not identity or not any(item["id"] == identity for item in catalog.get("entries", [])):
                return
            message = {"type": "catalog_pick", "plan_id": identity, "map": catalog["map"],
                       "epoch": catalog["epoch"], "revision": catalog["revision"], "generation": catalog["generation"]}
            hide_menu()
            print(json.dumps(message, ensure_ascii=False), flush=True)

        def file_signature(value):
            directory = resource_directory(self.root, value["reference_id"])
            return tuple((path.stat().st_mtime_ns, path.stat().st_size)
                         for path in (directory / "reference.json", directory / "standing.png", directory / "aim.png"))

        def display(value):
            nonlocal current, signature
            current = value
            hide_menu()
            if value["state"] in ("hidden", "aligned"):
                preview.hide()
                signature = None
            else:
                preview.show(value)
                try:
                    signature = file_signature(value)
                except OSError:
                    signature = None

        def poll():
            nonlocal last_check, signature, current
            try:
                item = self.messages.get_nowait()
                if item is None:
                    hide_menu()
                    return False
                kind = item.get("type", "guidance")
                if kind == "catalog_show":
                    show_menu(item)
                elif kind == "catalog_error":
                    hide_menu()
                    current = None
                    preview.notice("方案选择未成功\n" + item.get("message", "请重新打开清单"))
                else:
                    display(item)
            except queue.Empty:
                pass
            now = time.monotonic()
            if current and current["state"] not in ("hidden", "aligned") and now - last_check >= 1:
                last_check = now
                try:
                    changed = file_signature(current) != signature
                except OSError:
                    changed = signature is not None
                if changed:
                    display(current)
            return True

        try:
            while poll():
                if window is not None:
                    window.update()
                else:
                    message = wintypes.MSG()
                    while user.PeekMessageW(ctypes.byref(message), None, 0, 0, 1):
                        user.TranslateMessage(ctypes.byref(message))
                        user.DispatchMessageW(ctypes.byref(message))
                time.sleep(0.01)
        finally:
            preview.close()
            if window is not None:
                window.destroy()


def main():
    import argparse
    parser = argparse.ArgumentParser(description="语音工具私有双图窗口；不单独连接设备")
    parser.add_argument("--window", action="store_true", required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--corner", choices=("left", "right"), default="right")
    args = parser.parse_args()
    renderer = _WindowRenderer(args.root, args.corner)

    def receive():
        try:
            for line in sys.stdin:
                value = json.loads(line)
                renderer.publish(value)
                if value is None:
                    return
        finally:
            renderer.publish(None)

    threading.Thread(target=receive, daemon=True, name="guidance-state-reader").start()
    # Tk 全生命周期始终属于这个子进程的主线程，避免音频线程和 Tcl 交叉销毁。
    renderer._run()


if __name__ == "__main__":
    main()
