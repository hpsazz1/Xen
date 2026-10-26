"""用户显式调用时通过 Windows GDI 采集主屏，无键盘注入或游戏进程访问。"""

from __future__ import annotations

import ctypes
from ctypes import wintypes
from pathlib import Path
import struct
import sys
import zlib


def png_rgb(width, height, rows):
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(b"".join(b"\0" + row for row in rows))) + chunk(b"IEND", b""))


def capture_primary(path):
    if sys.platform != "win32":
        raise RuntimeError("全屏采图只支持 Windows 主机")
    user = ctypes.WinDLL("user32", use_last_error=True)
    gdi = ctypes.WinDLL("gdi32", use_last_error=True)
    ptr, integer = ctypes.c_void_p, ctypes.c_int
    user.GetDC.argtypes, user.GetDC.restype = [ptr], ptr
    user.ReleaseDC.argtypes = [ptr, ptr]
    gdi.CreateCompatibleDC.argtypes, gdi.CreateCompatibleDC.restype = [ptr], ptr
    gdi.CreateCompatibleBitmap.argtypes, gdi.CreateCompatibleBitmap.restype = [ptr, integer, integer], ptr
    gdi.SelectObject.argtypes, gdi.SelectObject.restype = [ptr, ptr], ptr
    gdi.DeleteObject.argtypes = [ptr]
    gdi.DeleteDC.argtypes = [ptr]
    gdi.BitBlt.argtypes = [ptr, integer, integer, integer, integer, ptr, integer, integer, wintypes.DWORD]
    gdi.GetDIBits.argtypes = [ptr, ptr, wintypes.UINT, wintypes.UINT, ptr, ptr, wintypes.UINT]
    # 显式采图进程采用物理像素；不把系统 DPI 逻辑坐标写入参考图几何。
    user.SetProcessDPIAware()
    width, height = user.GetSystemMetrics(0), user.GetSystemMetrics(1)
    if min(width, height) < 320 or max(width, height) > 8192:
        raise RuntimeError("主屏尺寸无效")
    screen = user.GetDC(None)
    memory = gdi.CreateCompatibleDC(screen) if screen else None
    bitmap = gdi.CreateCompatibleBitmap(screen, width, height) if memory else None
    previous = None
    try:
        if not bitmap:
            raise ctypes.WinError(ctypes.get_last_error())
        previous = gdi.SelectObject(memory, bitmap)
        if not gdi.BitBlt(memory, 0, 0, width, height, screen, 0, 0, 0x00CC0020 | 0x40000000):
            raise ctypes.WinError(ctypes.get_last_error())
        gdi.SelectObject(memory, previous)
        previous = None
        header = ctypes.create_string_buffer(struct.pack("<IiiHHIIiiII", 40, width, -height, 1, 32, 0, 0, 0, 0, 0, 0))
        pixels = ctypes.create_string_buffer(width * height * 4)
        if gdi.GetDIBits(screen, bitmap, 0, height, pixels, header, 0) != height:
            raise RuntimeError("GDI 未返回完整主屏图像")
        raw = pixels.raw
        rows = []
        for y in range(height):
            row = raw[y * width * 4:(y + 1) * width * 4]
            rgb = bytearray(width * 3)
            rgb[0::3], rgb[1::3], rgb[2::3] = row[2::4], row[1::4], row[0::4]
            rows.append(bytes(rgb))
        Path(path).write_bytes(png_rgb(width, height, rows))
        return width, height
    finally:
        if previous:
            gdi.SelectObject(memory, previous)
        if bitmap:
            gdi.DeleteObject(bitmap)
        if memory:
            gdi.DeleteDC(memory)
        if screen:
            user.ReleaseDC(None, screen)
