#!/usr/bin/env python3
"""离线检查 Git 跟踪的公开文档链接；不扫描本地 docs 或运行数据。"""

import argparse
import html
from html.parser import HTMLParser
from pathlib import Path
import posixpath
import re
import subprocess
import sys
import unicodedata
from urllib.parse import unquote, urlsplit

ROOT_DOCS = {
    "README.md", "LICENSE", "LICENSE.md", "CONTRIBUTING.md", "SUPPORT.md",
    "CODE_OF_CONDUCT.md", "SECURITY.md",
}


def public_document(path):
    return path in ROOT_DOCS or (
        path.lower().endswith(".md")
        and path.startswith(("assets/guide/", "assets/readme/", ".github/"))
    )


def prose(text):
    """屏蔽围栏、缩进代码和注释，保留行号用于诊断。"""
    text = re.sub(r"<!--.*?-->", lambda m: "\n" * m[0].count("\n"), text, flags=re.S)
    lines = []
    fence = None
    for line in text.splitlines(keepends=True):
        marker = re.match(r"^ {0,3}(`{3,}|~{3,})(.*)$", line)
        if fence:
            if marker and marker[1][0] == fence[0] and len(marker[1]) >= len(fence) and not marker[2].strip():
                fence = None
            lines.append("\n")
        elif marker:
            fence = marker[1]
            lines.append("\n")
        elif line.startswith(("    ", "\t")) and not line.lstrip().startswith("<"):
            lines.append("\n")
        else:
            lines.append(line)
    return "".join(lines)


def without_inline_code(text):
    return re.sub(r"(`+)(?!`)(.*?)\1(?!`)", lambda m: " " * len(m[0]), text, flags=re.S)


class HtmlLinks(HTMLParser):
    def __init__(self):
        super().__init__()
        self.links = []
        self.anchors = set()

    def handle_starttag(self, tag, attrs):
        for key, value in attrs:
            if value is None:
                continue
            if key in ("href", "src"):
                self.links.append((self.getpos()[0], value))
            if key == "id" or (tag == "a" and key == "name"):
                self.anchors.add(value)


def anchors(text):
    text = prose(text)
    parser = HtmlLinks()
    parser.feed(text)
    result = set(parser.anchors)
    lines = text.splitlines()
    for index, line in enumerate(lines):
        heading = re.match(r"^ {0,3}#{1,6}\s+(.+?)\s*#*\s*$", line)
        title = heading[1] if heading else None
        if title is None and index + 1 < len(lines) and line.strip() and re.fullmatch(r" {0,3}(?:=+|-+)\s*", lines[index + 1]):
            title = line.strip()
        if title is None:
            continue
        title = re.sub(r"!?\[([^\]]+)\]\([^)]*\)", r"\1", title)
        title = html.unescape(re.sub(r"<[^>]*>", "", title)).lower()
        slug = "".join(c for c in title if c in "-_ " or unicodedata.category(c)[0] in "LNM")
        slug = slug.replace(" ", "-")
        candidate, suffix = slug, 0
        while candidate in result:
            suffix += 1
            candidate = f"{slug}-{suffix}"
        result.add(candidate)
    return result


def links(text):
    text = without_inline_code(prose(text))
    parser = HtmlLinks()
    parser.feed(text)
    found = list(parser.links)
    definitions = {}
    definition_spans = []
    destination = r"(?:<([^>\n]+)>|([^\s]+?))(?:\s+[\"'].*?[\"'])?\s*"
    for match in re.finditer(r"^ {0,3}\[([^\]]+)\]:\s*" + destination + r"$", text, re.M):
        definitions[" ".join(match[1].lower().split())] = match[2] or match[3]
        definition_spans.append(match.span())
        found.append((text.count("\n", 0, match.start()) + 1, match[2] or match[3]))
    # 支持目标中的一层括号；公开文档避免复杂嵌套 Markdown。
    inline = r"!?\[[^\]\n]*\]\(\s*(<[^>\n]*>|(?:[^\s()\\]|\\.|\([^()]*\))*)(?:\s+[\"'][^\n]*?[\"'])?\s*\)"
    spans = list(definition_spans)
    for match in re.finditer(inline, text):
        target = match[1]
        if target.startswith("<"):
            target = target[1:-1]
        found.append((text.count("\n", 0, match.start()) + 1, re.sub(r"\\(.)", r"\1", target)))
        spans.append(match.span())
    for match in re.finditer(r"!?\[([^\]\n]+)\](?:\[([^\]\n]*)\])?", text):
        if any(start <= match.start() < end for start, end in spans):
            continue
        label = " ".join((match[2] or match[1]).lower().split())
        if label in definitions:
            found.append((text.count("\n", 0, match.start()) + 1, definitions[label]))
        elif match[2] is not None:
            found.append((text.count("\n", 0, match.start()) + 1, "!undefined-reference:" + label))
    return found


def check(root):
    raw = subprocess.check_output(["git", "-C", str(root), "ls-files", "-z"])
    tracked = set(raw.decode("utf-8").split("\0")) - {""}
    directories = {posixpath.dirname(p) for p in tracked}
    directories |= {"/".join(p.split("/")[:i]) for p in tracked for i in range(1, len(p.split("/")))}
    errors, count = [], 0
    cache = {}
    documents = sorted(p for p in tracked if public_document(p))
    for document in documents:
        source = root / document
        if not source.is_file():
            errors.append(f"{document}: 已跟踪文档不存在")
            continue
        for line, target in links(source.read_text(encoding="utf-8-sig")):
            if target.startswith("!undefined-reference:"):
                errors.append(f"{document}:{line}: 未定义引用 {target.split(':', 1)[1]}")
                continue
            parsed = urlsplit(html.unescape(target))
            if parsed.scheme or parsed.netloc:
                continue
            count += 1
            path = unquote(parsed.path)
            resolved = posixpath.normpath(posixpath.join(posixpath.dirname(document), path)) if path else document
            if path.startswith("/") or resolved == ".." or resolved.startswith("../") or "\\" in path:
                errors.append(f"{document}:{line}: 非仓库相对路径 {target}")
                continue
            if resolved not in tracked and resolved not in directories:
                errors.append(f"{document}:{line}: 目标未被 Git 跟踪 {target}")
                continue
            if not (root / resolved).exists():
                errors.append(f"{document}:{line}: 本地目标不存在 {target}")
                continue
            if parsed.fragment and resolved.lower().endswith(".md"):
                if resolved not in cache:
                    cache[resolved] = anchors((root / resolved).read_text(encoding="utf-8-sig"))
                if unquote(parsed.fragment) not in cache[resolved]:
                    errors.append(f"{document}:{line}: Markdown 锚点不存在 {target}")
    for error in errors:
        print(error, file=sys.stderr)
    print(f"公开文档检查：{len(documents)} 个文件，{count} 个本地链接，{len(errors)} 个错误")
    return 1 if errors else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1], help="待检查 Git 仓库根目录")
    args = parser.parse_args()
    try:
        return check(args.root.resolve())
    except (OSError, UnicodeError, ValueError, subprocess.CalledProcessError) as error:
        print(f"公开文档检查失败：{error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
