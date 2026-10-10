"""仅创建证据文件：同目录完整暂存、原子不覆盖发布、只清理自有暂存。"""
from __future__ import annotations
import json
import os
from pathlib import Path
import tempfile
from typing import Any


class _WindowsPublishedFile:
    """先锁定自有暂存，再用同一句柄原子发布和回滚，禁止路径替换。"""
    def __init__(self, temporary: Path):
        import ctypes
        from ctypes import wintypes
        self.ctypes = ctypes
        self.wintypes = wintypes
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        self.kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD,
            wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        self.kernel.CreateFileW.restype = wintypes.HANDLE
        self.kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        self.kernel.CloseHandle.restype = wintypes.BOOL
        self.kernel.SetFileInformationByHandle.argtypes = [wintypes.HANDLE, ctypes.c_int,
                                                          ctypes.c_void_p, wintypes.DWORD]
        self.kernel.SetFileInformationByHandle.restype = wintypes.BOOL
        # DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, OPEN_EXISTING。
        self.handle = self.kernel.CreateFileW(str(temporary), 0x00010080, 0x1,
                                              None, 3, 0x80, None)
        if self.handle == ctypes.c_void_p(-1).value:
            raise ctypes.WinError(ctypes.get_last_error())

    def publish(self, path: Path):
        ctypes, wintypes = self.ctypes, self.wintypes
        name = str(path)
        class RenameInformation(ctypes.Structure):
            _fields_ = [("ReplaceIfExists", wintypes.BOOL),
                        ("RootDirectory", wintypes.HANDLE),
                        ("FileNameLength", wintypes.DWORD),
                        ("FileName", wintypes.WCHAR * (len(name.encode("utf-16-le")) // 2 + 1))]
        rename = RenameInformation()
        rename.ReplaceIfExists = False
        rename.RootDirectory = None
        rename.FileNameLength = len(name.encode("utf-16-le"))
        rename.FileName = name
        # FileRenameInfo=3，ReplaceIfExists=false；完整字节已写完且句柄始终
        # 禁止其他调用写入/删除，发布与归属保护之间没有再次打开路径的窗口。
        if not self.kernel.SetFileInformationByHandle(
                self.handle, 3, ctypes.byref(rename), ctypes.sizeof(rename)):
            error = ctypes.get_last_error()
            if error in (80, 183):
                raise FileExistsError(f"证据发布路径已被其他调用占用: {path}")
            raise ctypes.WinError(error)

    def rollback(self):
        # FILE_DISPOSITION_INFO.DeleteFile 是 BOOLEAN（1字节）。
        disposition = self.ctypes.c_ubyte(1)
        if not self.kernel.SetFileInformationByHandle(
                self.handle, 4, self.ctypes.byref(disposition), self.ctypes.sizeof(disposition)):
            raise self.ctypes.WinError(self.ctypes.get_last_error())

    def close(self):
        if self.handle is not None:
            self.kernel.CloseHandle(self.handle)
            self.handle = None


def publish_json_new(path: Path, value: Any, *, collision_type=FileExistsError,
                     newline: str | None = None) -> None:
    publish_json_pair_new(((path, value),), collision_type=collision_type,
                          newline=newline)


def publish_json_pair_new(items, *, collision_type=FileExistsError,
                          newline: str | None = "\n") -> None:
    """预先完成全部序列化；每件以原子 create-if-absent 发布。"""
    entries = [(Path(path).resolve(), value) for path, value in items]
    if len({path for path, _ in entries}) != len(entries):
        raise collision_type("证据输出路径不得重复")
    if len(entries) > 1 and os.name != "nt":
        raise OSError("双证据事务要求 Windows 文件句柄归属保护，不支持不安全的路径回滚")
    pending = []
    published = []
    try:
        for path, value in entries:
            if path.exists():
                raise collision_type(f"拒绝覆盖既有证据: {path}")
            path.parent.mkdir(parents=True, exist_ok=True)
            descriptor, name = tempfile.mkstemp(prefix=f".{path.name}.incoming-",
                                                dir=path.parent)
            temporary = Path(name)
            pending.append((temporary, path))
            with os.fdopen(descriptor, "w", encoding="utf-8", newline=newline) as stream:
                json.dump(value, stream, ensure_ascii=False, indent=2, allow_nan=False)
                stream.write("\n")
                stream.flush()
                os.fsync(stream.fileno())
        for temporary, path in pending:
            if len(entries) > 1:
                owned = _WindowsPublishedFile(temporary)
                try:
                    owned.publish(path)
                except Exception as error:
                    owned.close()
                    if isinstance(error, FileExistsError):
                        raise collision_type(str(error)) from error
                    raise
                published.append(owned)
            else:
                try:
                    os.link(temporary, path)
                except FileExistsError as error:
                    raise collision_type(f"证据发布路径已被其他调用占用: {path}") from error
    except Exception:
        for owned in reversed(published):
            owned.rollback()
        raise
    finally:
        for owned in reversed(published):
            owned.close()
        for temporary, _ in pending:
            temporary.unlink(missing_ok=True)
