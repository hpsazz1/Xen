"""显式 --run 才短暂显示合成双图；不采屏、不打开麦克风、不连接设备。"""

import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest import mock
import subprocess

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.voice_switch.guidance import GuidanceWindow
from tools.voice_switch.reference_tool import add_image, begin
from tools.voice_switch.screen_capture import png_rgb


class WindowTests(unittest.TestCase):
    def test_original_pixels_display_without_activation_then_hide(self):
        user = ctypes.WinDLL("user32", use_last_error=True)
        pointer = ctypes.c_void_p
        user.GetForegroundWindow.restype = pointer
        user.IsWindowVisible.argtypes = [pointer]
        user.GetWindowLongPtrW.argtypes, user.GetWindowLongPtrW.restype = [pointer, ctypes.c_int], ctypes.c_ssize_t
        user.GetWindowRect.argtypes = [pointer, ctypes.POINTER(wintypes.RECT)]
        user.GetWindowThreadProcessId.argtypes = [pointer, ctypes.POINTER(wintypes.DWORD)]
        user.GetWindowTextW.argtypes = [pointer, wintypes.LPWSTR, ctypes.c_int]
        user.PostMessageW.argtypes = [pointer, ctypes.c_uint, ctypes.c_size_t, ctypes.c_ssize_t]
        callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, pointer, ctypes.c_ssize_t)
        user.EnumWindows.argtypes = [callback_type, ctypes.c_ssize_t]
        user.EnumChildWindows.argtypes = [pointer, callback_type, ctypes.c_ssize_t]
        foreground = user.GetForegroundWindow()

        def find_window(title_text="Xen 道具参考"):
            handles = []
            @callback_type
            def visit(handle, _):
                process = wintypes.DWORD()
                user.GetWindowThreadProcessId(handle, ctypes.byref(process))
                title = ctypes.create_unicode_buffer(100)
                user.GetWindowTextW(handle, title, 100)
                if process.value == window.process.pid and title.value == title_text:
                    handles.append(handle)
                return True
            user.EnumWindows(visit, 0)
            return handles[0] if handles else None

        def wait(condition):
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                if condition():
                    return True
                time.sleep(0.025)
            return False

        with tempfile.TemporaryDirectory(prefix="xen-guidance-window-test-") as temporary:
            base = Path(temporary)
            plan = base / "synthetic-only.json"
            plan.write_text(json.dumps({"name": "离线窗口检查合成参考", "map": "de_test",
                                       "actions": [{"type": "button", "button": "left", "duration_ms": 1000}]}), encoding="utf-8")
            image = base / "synthetic.png"
            image.write_bytes(png_rgb(640, 360, [bytes([10, 50, 90]) * 640] * 360))
            identity = begin(base / "references", plan, True)
            add_image(base / "references", identity, "standing", image)
            add_image(base / "references", identity, "aim", image)
            original_popen = subprocess.Popen
            def launch(arguments, **kwargs):
                arguments = list(arguments)
                arguments[4] = str(Path(__file__).resolve())
                arguments.insert(5, "--child")
                return original_popen(arguments, **kwargs)
            with mock.patch("tools.voice_switch.guidance.subprocess.Popen", side_effect=launch):
                window = GuidanceWindow(base / "references")
            try:
                self.assertTrue(wait(find_window), "提示窗线程未创建窗口")
                handle = find_window()
                self.assertFalse(user.IsWindowVisible(handle), "初始窗口必须隐藏")
                window.publish({"state": "selected", "plan_id": plan.name, "reference_id": identity})
                self.assertTrue(wait(lambda: user.IsWindowVisible(handle)), "选中后双图未显示")
                rectangle = wintypes.RECT()
                user.GetWindowRect(handle, ctypes.byref(rectangle))
                self.assertGreater(rectangle.bottom - rectangle.top, 300, "两图应实际解码；不能仅显示错误条")
                expected = 0x08000000 | 0x00080000 | 0x00000020 | 0x00000080
                self.assertEqual(user.GetWindowLongPtrW(handle, -20) & expected, expected)
                self.assertEqual(user.GetForegroundWindow(), foreground,
                                 f"显示参考图不能抢前台焦点，提示窗句柄={handle}")
                window.publish({"state": "aligned"})
                self.assertTrue(wait(lambda: not user.IsWindowVisible(handle)), "对齐完成没有隐藏")
                window.publish({"type": "catalog_show", "map": "de_test", "epoch": 1, "revision": 1, "generation": 9,
                                "entries": [{"id": plan.name, "name": "测试 T1 道具", "command": "", "team": "T"}]})
                self.assertTrue(wait(lambda: find_window("Xen 当前地图道具")), "菜单没有按需创建")
                menu = find_window("Xen 当前地图道具")
                self.assertTrue(wait(lambda: user.IsWindowVisible(menu)), "清单未显示")
                picks = []
                def picked():
                    picks.extend(window.poll())
                    return bool(picks)
                self.assertTrue(wait(picked), "清单点击没有发出选择消息")
                self.assertEqual(len(picks), 1, "点击应只提交一次目录选择")
                self.assertEqual((picks[0]["type"], picks[0]["plan_id"], picks[0]["generation"]),
                                 ("catalog_pick", plan.name, 9))
                self.assertTrue(wait(lambda: not user.IsWindowVisible(menu)), "选中后菜单没有收起")
                self.assertEqual(user.GetForegroundWindow(), foreground, "点选后没有恢复原前台窗口")
                window.publish({"state": "selected", "plan_id": plan.name, "reference_id": identity})
                self.assertTrue(wait(lambda: user.IsWindowVisible(handle)), "菜单选中后双图未显示")
                time.sleep(0.05)
                self.assertEqual(user.GetForegroundWindow(), foreground, "菜单曾打开后，双图仍不能抢焦点")
            finally:
                window.close()
                self.assertFalse(window.thread.is_alive(), "窗口线程没有结束")


if __name__ == "__main__":
    if "--child" in sys.argv:
        sys.argv.remove("--child")
        from tkinter import ttk
        from tools.voice_switch.guidance import main
        original_init = ttk.Treeview.__init__
        def initialize(tree, *args, **kwargs):
            original_init(tree, *args, **kwargs)
            def click_visible_row():
                for parent in tree.get_children():
                    for identity in tree.get_children(parent):
                        box = tree.bbox(identity)
                        if tree.winfo_ismapped() and box:
                            # 使用 Tk 官方事件入口测试实际绑定，不发送系统鼠标或设备输入。
                            tree.event_generate("<ButtonRelease-1>", x=box[0]+100,
                                                y=box[1]+box[3]//2, when="tail")
                            return
                tree.after(50, click_visible_row)
            tree.after(300, click_visible_row)
        ttk.Treeview.__init__ = initialize
        main()
        raise SystemExit(0)
    if "--run" not in sys.argv:
        raise SystemExit("需要显式 --run；会短暂显示测试合成双图，不连接设备。")
    unittest.main(argv=[sys.argv[0]])
