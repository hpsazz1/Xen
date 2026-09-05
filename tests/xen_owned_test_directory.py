"""通过正式 PowerShell 路径契约管理 Python 测试的本轮目录。"""

from __future__ import annotations

from collections.abc import Iterator
from contextlib import contextmanager
import pathlib
import subprocess


@contextmanager
def owned_test_directory(
        base: pathlib.Path,
        powershell_executable: pathlib.Path) -> Iterator[pathlib.Path]:
    if not powershell_executable.is_absolute() or not powershell_executable.is_file():
        raise ValueError("PowerShell 必须为配置阶段解析的现有绝对路径")
    repository = pathlib.Path(__file__).resolve().parent.parent
    command = [
        str(powershell_executable), "-NoProfile", "-ExecutionPolicy", "Bypass",
        "-File", str(repository / "scripts" / "invoke_path_safety.ps1"),
        # 保留调用者的路径，不能先 resolve 而隐藏原路径上的 junction。
        "-BasePath", str(base), "-RepositoryRoot", str(repository),
    ]
    created = subprocess.run(
        command + ["-Action", "New"], check=True, capture_output=True,
        text=True, encoding="utf-8", errors="replace")
    root, owner_id = created.stdout.strip().split("\t")
    try:
        yield pathlib.Path(root)
    finally:
        subprocess.run(
            command + ["-Action", "Remove", "-RootPath", root,
                       "-OwnerId", owner_id], check=True, capture_output=True,
            text=True, encoding="utf-8", errors="replace")
