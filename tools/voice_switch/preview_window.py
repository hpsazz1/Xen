"""真正不激活的 Win32 双图窗；菜单可交互，但参考图不经过 Tk 的映射激活流程。"""

import ctypes as c
from ctypes import wintypes as w

try:
    from .reference_assets import load_reference, png_size
except ImportError:
    from reference_assets import load_reference, png_size


class PreviewWindow:
    def __init__(self, root, corner="right"):
        self.root, self.corner = root, corner
        self.user, self.gdi = c.WinDLL("user32"), c.WinDLL("gdi32")
        self.plus, self.kernel = c.WinDLL("gdiplus"), c.WinDLL("kernel32")
        self.images, self.reference, self.message = [], None, ""
        self.image_sizes = []
        self.width, self.height = 344, 120
        p, i, u = c.c_void_p, c.c_int, c.c_uint
        def bind(library, name, result, *arguments):
            function = getattr(library, name)
            function.restype, function.argtypes = result, list(arguments)
            return function
        bind(self.kernel, "GetModuleHandleW", p, w.LPCWSTR)
        bind(self.user, "CreateWindowExW", p, w.DWORD, w.LPCWSTR, w.LPCWSTR, w.DWORD,
             i, i, i, i, p, p, p, p)
        bind(self.user, "DefWindowProcW", c.c_ssize_t, p, u, c.c_size_t, c.c_ssize_t)
        for name in ("DestroyWindow", "ShowWindow"):
            bind(self.user, name, w.BOOL, p, *([i] if name == "ShowWindow" else []))
        bind(self.user, "SetWindowPos", w.BOOL, p, p, i, i, i, i, u)
        bind(self.user, "SetLayeredWindowAttributes", w.BOOL, p, w.DWORD, c.c_ubyte, w.DWORD)
        bind(self.user, "InvalidateRect", w.BOOL, p, p, w.BOOL)
        bind(self.user, "FillRect", i, p, c.POINTER(w.RECT), p)
        bind(self.user, "DrawTextW", i, p, w.LPCWSTR, i, c.POINTER(w.RECT), u)
        bind(self.gdi, "CreateCompatibleDC", p, p)
        bind(self.gdi, "DeleteDC", w.BOOL, p)
        bind(self.gdi, "DeleteObject", w.BOOL, p)
        bind(self.gdi, "SelectObject", p, p, p)
        bind(self.gdi, "GetStockObject", p, i)
        bind(self.gdi, "CreateSolidBrush", p, w.DWORD)
        bind(self.gdi, "CreatePen", p, i, i, w.DWORD)
        bind(self.gdi, "SetBkMode", i, p, i)
        bind(self.gdi, "SetTextColor", w.DWORD, p, w.DWORD)
        bind(self.gdi, "SetStretchBltMode", i, p, i)
        bind(self.gdi, "StretchBlt", w.BOOL, p, i, i, i, i, p, i, i, i, i, w.DWORD)
        bind(self.gdi, "Rectangle", w.BOOL, p, i, i, i, i)
        bind(self.gdi, "Ellipse", w.BOOL, p, i, i, i, i)
        bind(self.gdi, "MoveToEx", w.BOOL, p, i, i, p)
        bind(self.gdi, "LineTo", w.BOOL, p, i, i)
        bind(self.gdi, "CreateFontW", p, *([i] * 5), *([w.DWORD] * 8), w.LPCWSTR)
        bind(self.plus, "GdiplusStartup", i, c.POINTER(c.c_size_t), p, p)
        bind(self.plus, "GdiplusShutdown", None, c.c_size_t)
        bind(self.plus, "GdipCreateBitmapFromFile", i, w.LPCWSTR, c.POINTER(p))
        bind(self.plus, "GdipCreateHBITMAPFromBitmap", i, p, c.POINTER(p), u)
        bind(self.plus, "GdipDisposeImage", i, p)
        class Startup(c.Structure):
            _fields_ = [("version", u), ("callback", p), ("background", w.BOOL), ("external", w.BOOL)]
        self.token = c.c_size_t()
        if self.plus.GdiplusStartup(c.byref(self.token), c.byref(Startup(1, None, False, False)), None):
            raise RuntimeError("Windows 图片解码初始化失败")
        self.font = self.gdi.CreateFontW(-14, 0, 0, 0, 400, 0, 0, 0, 1, 0, 0, 5, 0, "Microsoft YaHei UI")
        self.proc_type = c.WINFUNCTYPE(c.c_ssize_t, p, u, c.c_size_t, c.c_ssize_t)
        class Paint(c.Structure):
            _fields_ = [("hdc", p), ("erase", w.BOOL), ("rect", w.RECT), ("restore", w.BOOL),
                        ("update", w.BOOL), ("reserved", c.c_byte * 32)]
        self.Paint = Paint
        bind(self.user, "BeginPaint", p, p, c.POINTER(Paint))
        bind(self.user, "EndPaint", w.BOOL, p, c.POINTER(Paint))
        class WindowClass(c.Structure):
            _fields_ = [("style", u), ("proc", self.proc_type), ("class_extra", i), ("window_extra", i),
                        ("instance", p), ("icon", p), ("cursor", p), ("background", p),
                        ("menu", w.LPCWSTR), ("name", w.LPCWSTR)]
        self.procedure = self.proc_type(self._window_message)
        self.class_name = "XenGrenadeReferencePreview"
        instance = self.kernel.GetModuleHandleW(None)
        descriptor = WindowClass(0, self.procedure, 0, 0, instance, None, None, None, None, self.class_name)
        bind(self.user, "RegisterClassW", w.ATOM, c.POINTER(WindowClass))
        if not self.user.RegisterClassW(c.byref(descriptor)):
            raise RuntimeError("参考窗注册失败")
        style = 0x08000000 | 0x00080000 | 0x00000020 | 0x00000080 | 0x00000008
        self.handle = self.user.CreateWindowExW(style, self.class_name, "Xen 道具参考", 0x80000000,
                                               0, 0, self.width, self.height, None, None, instance, None)
        if not self.handle:
            raise RuntimeError("参考窗创建失败")
        self.user.SetLayeredWindowAttributes(self.handle, 0, 245, 2)

    def _window_message(self, handle, message, wp, lp):
        if message == 0x21:
            return 3  # MA_NOACTIVATE
        if message == 0x84:
            return -1  # HTTRANSPARENT
        if message == 0x14:
            return 1
        if message == 0xF:
            paint = self.Paint()
            dc = self.user.BeginPaint(handle, c.byref(paint))
            try:
                self._paint(dc)
            finally:
                self.user.EndPaint(handle, c.byref(paint))
            return 0
        return self.user.DefWindowProcW(handle, message, wp, lp)

    def _clear(self):
        for bitmap in self.images:
            self.gdi.DeleteObject(bitmap)
        self.images.clear()
        self.image_sizes.clear()

    def _text(self, dc, text, top, height, color=0x00F7E8DC):
        rectangle = w.RECT(12, top, 332, top + height)
        self.gdi.SetTextColor(dc, color)
        self.user.DrawTextW(dc, str(text), -1, c.byref(rectangle), 0x10 | 0x800)

    def _paint(self, dc):
        brush = self.gdi.CreateSolidBrush(0x00281C15)
        rectangle = w.RECT(0, 0, self.width, self.height)
        self.user.FillRect(dc, c.byref(rectangle), brush)
        self.gdi.DeleteObject(brush)
        old_font = self.gdi.SelectObject(dc, self.font)
        self.gdi.SetBkMode(dc, 1)
        try:
            if self.reference is None:
                self._text(dc, self.message, 14, 100, 0x008CCEFF)
                return
            reference, g = self.reference, self.reference["geometry"]
            self._text(dc, reference["plan"].get("name", "道具参考"), 10, 40)
            y = 54
            for index, bitmap in enumerate(self.images):
                width, height = self.image_sizes[index]
                scale = min(320 / width, 180 / height)
                dw, dh = round(width * scale), round(height * scale)
                x = (344 - dw) // 2
                self._text(dc, "站位参考" if index == 0 else "瞄点参考 · 方框为 320×320 ROI", y, 22, 0x00CEB8A8)
                y += 24
                source = self.gdi.CreateCompatibleDC(dc)
                previous = self.gdi.SelectObject(source, bitmap)
                self.gdi.SetStretchBltMode(dc, 4)
                self.gdi.StretchBlt(dc, x, y, dw, dh, source, 0, 0, width, height, 0x00CC0020)
                self.gdi.SelectObject(source, previous)
                self.gdi.DeleteDC(source)
                if index == 0 and "standing_marker" in reference:
                    marker = reference["standing_marker"]
                    cx = x + round(marker["x"] * (dw - 1))
                    cy = y + round(marker["y"] * (dh - 1))
                    pen = self.gdi.CreatePen(0, 3, 0x0000D7FF)
                    old_pen = self.gdi.SelectObject(dc, pen)
                    old_brush = self.gdi.SelectObject(dc, self.gdi.GetStockObject(5))
                    self.gdi.Ellipse(dc, cx - 11, cy - 11, cx + 12, cy + 12)
                    self.gdi.SelectObject(dc, old_pen); self.gdi.SelectObject(dc, old_brush)
                    self.gdi.DeleteObject(pen)
                    label = w.RECT(max(x, min(cx + 14, x + dw - 65)),
                                   max(y, min(cy - 10, y + dh - 21)), x + dw, y + dh)
                    self.gdi.SetTextColor(dc, 0x0000D7FF)
                    self.user.DrawTextW(dc, marker["label"], -1, c.byref(label), 0x20 | 0x800)
                if index == 1:
                    sx, sy = dw / width, dh / height
                    left, top = round(x + g["roi_x"] * sx), round(y + g["roi_y"] * sy)
                    pen = self.gdi.CreatePen(0, 2, 0x00C5DD53)
                    old_pen = self.gdi.SelectObject(dc, pen)
                    old_brush = self.gdi.SelectObject(dc, self.gdi.GetStockObject(5))
                    self.gdi.Rectangle(dc, left, top, round(left + 320 * g["scale_x"] * sx), round(top + 320 * g["scale_y"] * sy))
                    cx = round(left + reference.get("target_x", 160) * g["scale_x"] * sx)
                    cy = round(top + reference.get("target_y", 160) * g["scale_y"] * sy)
                    self.gdi.MoveToEx(dc, cx - 5, cy, None); self.gdi.LineTo(dc, cx + 6, cy)
                    self.gdi.MoveToEx(dc, cx, cy - 5, None); self.gdi.LineTo(dc, cx, cy + 6)
                    self.gdi.SelectObject(dc, old_pen); self.gdi.SelectObject(dc, old_brush)
                    self.gdi.DeleteObject(pen)
                y += dh + 12
            self._text(dc, (reference.get("standing_note", "") + "\n" + self.message).strip(), y, 90)
        finally:
            self.gdi.SelectObject(dc, old_font)

    def show(self, value):
        self._clear()
        self.reference = None
        try:
            reference, directory = load_reference(self.root, value["reference_id"], value["plan_id"])
            for key in ("standing_image", "aim_image"):
                image, bitmap = c.c_void_p(), c.c_void_p()
                if self.plus.GdipCreateBitmapFromFile(str(directory / reference[key]), c.byref(image)):
                    raise ValueError("参考 PNG 无法解码")
                try:
                    if self.plus.GdipCreateHBITMAPFromBitmap(image, c.byref(bitmap), 0xFF000000):
                        raise ValueError("参考 PNG 无法显示")
                finally:
                    self.plus.GdipDisposeImage(image)
                self.images.append(bitmap.value)
                self.image_sizes.append(png_size(directory / reference[key]))
            self.reference = reference
            self.height = 54 + sum(24 + round(height * min(320 / width, 180 / height)) + 12
                                   for width, height in self.image_sizes) + 90
            self.message = {"selected": "站好位置、朝向接近后，按小键盘上键对齐。",
                            "aligning": "正在对齐；完成后参考图自动隐藏。",
                            "failed": value.get("message") or "未完成对齐，请调整后再试。"}[value["state"]]
        except (OSError, ValueError) as error:
            self._clear()
            self.height, self.message = 120, "参考图未就绪\n" + str(error)
        self._show()

    def notice(self, text):
        self._clear()
        self.reference, self.height, self.message = None, 120, text
        self._show()

    def _show(self):
        x = 16 if self.corner == "left" else max(0, self.user.GetSystemMetrics(0) - 360)
        self.user.SetWindowPos(self.handle, c.c_void_p(-1), x, 16, self.width, self.height, 0x0010 | 0x0040)
        self.user.InvalidateRect(self.handle, None, True)

    def hide(self):
        self.user.ShowWindow(self.handle, 0)
        self._clear()
        self.reference = None

    def close(self):
        self._clear()
        self.user.DestroyWindow(self.handle)
        self.gdi.DeleteObject(self.font)
        self.plus.GdiplusShutdown(self.token)
