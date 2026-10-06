"""仅回环合成样本的生产 HTTP 集成测试；不访问 NDI 或真实游戏。"""
import argparse
import json
import pathlib
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request
import uuid


def run(exe):
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    base = f"http://127.0.0.1:{port}"
    with tempfile.TemporaryDirectory(prefix="xen-lineup-http-") as directory:
        log = open(pathlib.Path(directory) / "service.log", "w", encoding="utf-8")
        process = subprocess.Popen([str(exe), "--synthetic", "--data", directory,
                                    "--port", str(port), "--max-seconds", "50"],
                                   stdout=log, stderr=subprocess.STDOUT)
        checks = []

        def request(path, body=None):
            data = json.dumps(body).encode() if body is not None else None
            req = urllib.request.Request(base + path, data=data,
                                         headers={"Content-Type": "application/json"})
            try:
                with urllib.request.urlopen(req, timeout=4) as response:
                    return response.status, response.read()
            except urllib.error.HTTPError as error:
                return error.code, error.read()

        def state():
            status, data = request("/api/state")
            assert status == 200
            return json.loads(data)

        def wait(predicate, seconds=10):
            end = time.monotonic() + seconds
            last = None
            while time.monotonic() < end:
                last = state()
                if predicate(last):
                    return last
                time.sleep(.08)
            raise AssertionError(f"state timeout: {last}")

        def command(action, **kwargs):
            s = state()
            body = dict(epoch=s["epoch"], revision=s["revision"], request_id=str(uuid.uuid4()),
                        action=action, **kwargs)
            status, data = request("/api/command", body)
            assert status == 200, (status, data)
            return body, json.loads(data)

        try:
            for _ in range(100):
                try:
                    state()
                    break
                except (OSError, urllib.error.URLError):
                    assert process.poll() is None, "service exited"
                    time.sleep(.05)
            else:
                raise AssertionError("service startup timeout")
            assert request("/")[0] == 200
            assert request("/app.js")[0] == 200
            assert request("/../../config.ini")[0] == 404
            checks.append("静态页面、目录穿越拒绝")
            command("start")
            command("mode", value="capture")
            body, _ = command("capture", map="合成测试地图", region="合成区域", standpoint_id="",
                              stance="站姿", instructions="测试夹具，非真实站位")
            assert request("/api/command", body)[0] == 200
            s = wait(lambda s: not s.get("busy") and len(s["recipes"]) == 1)
            time.sleep(.2)
            assert len(state()["recipes"]) == 1
            checks.append("重复采集幂等、下一新帧、草稿保存")
            recipe = s["recipes"][0]
            status, raw = request(recipe["reference_url"])
            assert status == 200 and raw.startswith(b"\x89PNG")
            assert request(recipe["standpoint_url"])[0] == 200
            checks.append("冻结原图和共用站位图可读取")
            command("update", items=[dict(id=recipe["id"], name="合成配方", target="测试点",
                                         grenade="烟雾弹", notes="SYNTHETIC", validation="unverified")])
            wait(lambda s: not s.get("busy") and not s["recipes"][0]["draft"])
            command("mode", value="browse")
            old, _ = command("lock", id=recipe["id"])
            s = wait(lambda s: s.get("preview", {}).get("status") == "valid")
            preview_url = s["preview"]["url"]
            assert s["preview"]["recipe_id"] == recipe["id"]
            status, marked = request(preview_url)
            assert status == 200 and marked.startswith(b"\xff\xd8")
            assert request(recipe["reference_url"])[1] == raw
            checks.append("锁定配方同帧标记、原图不变")
            command("cancel")
            assert not state()["locked_id"]
            assert request(preview_url)[0] == 410
            assert request("/api/command", old)[0] == 200
            assert not state()["locked_id"], "duplicate command replayed lock"
            stale = dict(old, request_id=str(uuid.uuid4()))
            assert request("/api/command", stale)[0] == 409
            stale["epoch"] = "prior-service"
            assert request("/api/command", stale)[0] == 409
            checks.append("取消撤标、重复锁定不重放、旧版本/会话拒绝")
            command("stop")
            assert request("/api/preview?frame_id=old")[0] == 410
            assert request("/api/command", {"action": "capture"})[0] == 400
            checks.append("停止撤标、畸形命令拒绝")
            # TCP 分片是正常边界；重复 Content-Length 和 Transfer-Encoding 必须拒绝。
            def raw_request(payload, split=False):
                with socket.create_connection(("127.0.0.1", port), timeout=4) as sock:
                    if split:
                        for p in [payload[:7], payload[7:22], payload[22:]]:
                            sock.sendall(p)
                            time.sleep(.01)
                    else:
                        sock.sendall(payload)
                    data = b""
                    while True:
                        part = sock.recv(65536)
                        if not part:
                            return data
                        data += part
            good = b"GET /api/state HTTP/1.1\r\nHost: localhost\r\n\r\n"
            assert raw_request(good, True).startswith(b"HTTP/1.1 200")
            for bad in [b"Content-Length: 0\r\nContent-Length: 0\r\n", b"Transfer-Encoding: chunked\r\n",
                        b"Transfer-Encoding : chunked\r\n", b"Invalid Header: value\r\n", b"Header: \x01\r\n"]:
                payload = b"GET /api/state HTTP/1.1\r\nHost: localhost\r\n" + bad + b"\r\n"
                assert raw_request(payload).startswith(b"HTTP/1.1 400")
            checks.append("HTTP分段读取、歧义framing拒绝")
            command("export")
            wait(lambda s: not s.get("busy"))
            assert list(pathlib.Path(directory).glob("exports/*/catalog.json"))
            checks.append("证据导出")
            print(json.dumps({"fixture": "synthetic only", "passed": checks, "count": len(checks)}, ensure_ascii=False, indent=2))
        finally:
            process.terminate()
            process.wait(timeout=8)
            log.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True, type=pathlib.Path)
    run(parser.parse_args().exe.resolve())
