"""通过既有 GsiReceiver 的回环 HTTP 合成载荷验证上下文；不操作游戏配置。"""
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


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def run(exe):
    port, gsi_port = free_port(), free_port()
    base = f"http://127.0.0.1:{port}"
    with tempfile.TemporaryDirectory(prefix="lineup-gsi-http-") as root:
        root = pathlib.Path(root)
        config = root / "gsi.ini"
        config.write_text(f"[gsi]\nenabled=true\nbind_address=127.0.0.1\nport={gsi_port}\nttl_ms=1500\n", encoding="utf-8")
        log = open(root / "service.log", "w", encoding="utf-8")
        process = subprocess.Popen([str(exe), "--synthetic", "--data", str(root / "data"),
                                    "--port", str(port), "--gsi-mode", "direct", "--gsi-config", str(config), "--max-seconds", "45"], stdout=log, stderr=log)
        def request(path, body=None, origin=base):
            req = urllib.request.Request(origin + path, data=json.dumps(body).encode() if body is not None else None,
                                         headers={"Content-Type": "application/json"})
            try:
                with urllib.request.urlopen(req, timeout=3) as r:
                    data = r.read()
                    return r.status, json.loads(data) if data else None
            except urllib.error.HTTPError as e:
                return e.code, json.loads(e.read())

        def state(): return request("/api/state")[1]
        def wait(predicate, seconds=5):
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                s = state()
                if predicate(s): return s
                time.sleep(.04)
            raise AssertionError(s)
        def command(action, **kwargs):
            s = state()
            status, result = request("/api/command", dict(epoch=s["epoch"], revision=s["revision"], request_id=str(uuid.uuid4()), action=action, **kwargs))
            assert status == 200, (status, result)
            return result
        def payload(map_name="de_dust2", team="T", observed=False):
            return {"provider": {"appid": 730, "steamid": "76561198000000000", "timestamp": int(time.time())},
                    "map": {"name": map_name, "phase": "live"}, "round": {"phase": "live"},
                    "player": {"steamid": "76561198000000001" if observed else "76561198000000000",
                               "activity": "playing", "team": team, "state": {"health": 100},
                               "weapons": {"weapon_0": {"name": "weapon_smokegrenade", "type": "Grenade", "state": "active"}}}}
        def ingest(body):
            assert request("/gsi", body, f"http://127.0.0.1:{gsi_port}")[0] == 200
        try:
            for _ in range(100):
                try:
                    s = state()
                    break
                except (OSError, urllib.error.URLError):
                    assert process.poll() is None
                    time.sleep(.04)
            assert s["context"]["team"] == "UNKNOWN"
            # 测试只用合成源创建配方，随后交由真实接收链的合成载荷筛选。
            command("context", mode="manual", map="de_dust2", team="T")
            command("start")
            command("mode", value="capture")
            command("capture", map="de_dust2", region="fixture")
            s = wait(lambda s: len(s["recipes"]) == 1 and not s["busy"])
            recipe = s["recipes"][0]["id"]
            command("update", items=[dict(id=recipe, name="fixture", target="center", grenade="smoke", team="T")])
            wait(lambda s: not s["busy"] and not s["recipes"][0]["draft"])
            command("mode", value="browse")
            command("context", mode="auto")
            ingest(payload())
            s = wait(lambda s: s["context"]["team"] == "T")
            assert s["context"]["map"] == "de_dust2" and s["context"]["identity_confirmed"]
            command("lock", id=recipe)
            ingest(payload(team="CT"))
            s = wait(lambda s: s["context"]["team"] == "CT" and not s["locked_id"])
            assert s.get("lock_reason")
            # 通用记录兼容两边；换地图仍撤销锁定。
            command("update", items=[dict(id=recipe, team="ANY")])
            wait(lambda s: not s["busy"] and s["recipes"][0]["team"] == "ANY")
            command("lock", id=recipe)
            ingest(payload(map_name="de_inferno", team="CT"))
            s = wait(lambda s: s["context"]["map"] == "de_inferno" and not s["locked_id"])
            # 观战对象并非provider本地身份，不能继承阵营。
            ingest(payload(map_name="de_inferno", team="T", observed=True))
            s = wait(lambda s: s["context"]["team"] == "UNKNOWN")
            assert not s["context"]["identity_confirmed"]
            command("context", mode="manual", map="de_dust2", team="CT")
            s = state()
            assert s["context"]["mode"] == "manual" and s["context"]["team"] == "CT"
            command("lock", id=recipe)
            # 人工模式不会因未知GSI伪装成自动，也不静默换配方。
            assert state()["locked_id"] == recipe
            command("context", mode="auto")
            assert not state()["locked_id"]
            time.sleep(1.1)  # 新provider秒，避免同秒已见状态被原接收器判重复。
            ingest(payload())
            wait(lambda s: s["context"]["team"] == "T")
            s = wait(lambda s: s["context"]["team"] == "UNKNOWN", seconds=3)
            assert s["context"]["map"] == ""
            print("GSI HTTP: auto map/team, grenade context, side/map invalidation, observer, manual/auto, stale: passed")
        finally:
            process.terminate()
            process.wait(timeout=6)
            log.close()
        # 同一端口被现有owner占用时不能抢占或使网页启动失败。
        with socket.socket() as occupied:
            occupied.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
            occupied.bind(("127.0.0.1", gsi_port))
            occupied.listen(1)
            process = subprocess.Popen([str(exe), "--data", str(root / "conflict"), "--port", str(port),
                                        "--gsi-mode", "direct", "--gsi-config", str(config), "--max-seconds", "15"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                for _ in range(100):
                    try:
                        s = state()
                        break
                    except (OSError, urllib.error.URLError): time.sleep(.04)
                assert s["context"]["team"] == "UNKNOWN"
                assert s["context"].get("reason")
                command("context", mode="manual", map="de_dust2", team="T")
                assert state()["context"]["team"] == "T"
                assert occupied.getsockname()[1] == gsi_port
                print("GSI port conflict: original owner retained, manual mode usable: passed")
            finally:
                process.terminate()
                process.wait(timeout=6)


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--exe", type=pathlib.Path, required=True)
    run(p.parse_args().exe.resolve())
