"""从固定官方发布包安装最小中文模型，校验后逐个原子替换。"""

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import tarfile
import tempfile
import urllib.request

MODEL = "sherpa-onnx-kws-zipformer-wenetspeech-3.3M-2024-01-01"
URL = "https://github.com/k2-fsa/sherpa-onnx/releases/download/kws-models/" + MODEL + ".tar.bz2"
SHA256 = "b2f7c89690dc8ce4c6ed6afeab7cd800c36ad1421fb6b6302b4a4b194cf7f35f"
FILES = [part + "-epoch-12-avg-2-chunk-16-left-64.int8.onnx" for part in ("encoder", "decoder", "joiner")]
FILES += ["tokens.txt", "README.md"]


def install(destination, archive=None):
    destination.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="xen-voice-model-") as scratch:
        scratch = Path(scratch)
        if archive is None:
            archive = scratch / "model.tar.bz2"
            print("下载固定版本中文模型……", flush=True)
            request = urllib.request.Request(URL, headers={"User-Agent": "Xen-Voice-Setup"})
            with urllib.request.urlopen(request, timeout=60) as source, archive.open("wb") as output:
                shutil.copyfileobj(source, output)
        with archive.open("rb") as source:
            digest = hashlib.file_digest(source, "sha256").hexdigest()
        if digest != SHA256:
            raise ValueError("模型下载包 SHA-256 不匹配，未安装")
        with tarfile.open(archive, "r:bz2") as package:
            # 只读固定成员名，不提取路径、链接、样本或其他权重。
            for name in FILES:
                member = package.getmember(MODEL + "/" + name)
                if not member.isfile():
                    raise ValueError("模型成员不是普通文件")
                source = package.extractfile(member)
                with source, (scratch / name).open("wb") as output:
                    shutil.copyfileobj(source, output)
        for name in FILES:
            target = destination / name
            temporary = destination / (name + ".installing")
            shutil.copyfile(scratch / name, temporary)
            os.replace(temporary, target)
    print("中文模型安装完成：" + str(destination))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="安装固定版本本地中文模型")
    parser.add_argument("--destination", type=Path, default=Path(__file__).resolve().parent / "model")
    parser.add_argument("--archive", type=Path, help="已下载的同一官方压缩包；仍校验 SHA-256")
    args = parser.parse_args()
    install(args.destination, args.archive)
