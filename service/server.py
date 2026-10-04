"""
server.py - 零第三方依赖的 HTTP 服务

提供：
  * 网页配置台静态资源（service/web）
  * 策略校验 / 发布 / 代次查询（服务接口检查保留键与冲突）
  * 设备轮询拉取 + 回执上报（关系库记录代次与设备回执）
  * 真人业务确认上报（拒绝非 human 来源）
  * 日志上传与实验重放（重放只能复现实验输入，不能伪造业务确认）
"""
from __future__ import annotations

import json
import os
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from service.db import DB
from service.validator import (canonical_json, default_payload, digest_payload,
                               parse_envelope, validate_payload)
from service.simulator import replay as sim_replay
from service import journal_codec

WEB_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "web")

_HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_DB = os.path.join(_HERE, "data", "strategy.db")
DEFAULT_DEVICE = "dev-001"

DB_LOCK = threading.Lock()


class Api:
    def __init__(self, db: DB):
        self.db = db

    def route(self, method, path, query, body):
        p = path.strip("/").split("/") if path != "/" else []
        # /api/validate
        if method == "POST" and p == ["api", "validate"]:
            payload = body
            res = validate_payload(payload)
            return 200, res.to_dict()
        if method == "GET" and p == ["api", "default-payload"]:
            gen = int(query.get("generation", ["1"])[0])
            return 200, default_payload(gen)
        if method == "GET" and p == ["api", "devices"]:
            rows = self.db.conn.execute(
                "SELECT device_id,name,created_at,last_seen_at FROM devices"
            ).fetchall()
            return 200, [dict(r) for r in rows]
        if method == "POST" and p == ["api", "devices"]:
            did = str(body.get("device_id", "")).strip()
            name = str(body.get("name", ""))
            if not did:
                return 400, {"error": "device_id required"}
            with DB_LOCK:
                self.db.upsert_device(did, name)
            return 201, {"ok": True, "device_id": did}

        # /api/devices/{id}...
        if len(p) >= 3 and p[:2] == ["api", "devices"]:
            did = p[2]
            with DB_LOCK:
                self.db.upsert_device(did)
            if len(p) == 3 and method == "GET":
                env = self.db.envelope_for(did)
                return 200, {"device_id": did, "envelope": env}
            if p[3:] == ["publish"] and method == "POST":
                with DB_LOCK:
                    info, res = self.db.publish(did, body)
                if not info:
                    return 409, res.to_dict()
                return 201, {"ok": True, **info,
                             "envelope": self.db.envelope_for(did)}
            if p[3:] == ["policy"] and method == "GET":
                # C 客户端轮询
                env = self.db.envelope_for(did)
                if not env:
                    return 404, {"error": "no policy published"}
                return 200, env
            if p[3:] == ["receipts"] and method == "GET":
                return 200, self.db.receipts(did)
            if p[3:] == ["receipts"] and method == "POST":
                gen = body.get("generation")
                status = body.get("status")
                if not isinstance(gen, int) or status not in ("applied", "rejected"):
                    return 400, {"error": "generation(int) and status required"}
                with DB_LOCK:
                    self.db.record_receipt(
                        did, gen, status, str(body.get("reason", "")),
                        int(body.get("released_keys", 0)))
                return 201, {"ok": True}
            if p[3:] == ["generations"] and method == "GET":
                return 200, self.db.list_generations(did)
            if p[3:] == ["confirmations"] and method == "GET":
                return 200, self.db.confirmations(did)
            if p[3:] == ["confirmations"] and method == "POST":
                # 真人业务确认的唯一入口。
                if body.get("origin", "human") != "human":
                    return 403, {
                        "error": "only on-site human origin can confirm business; "
                                 "replay/remote is forbidden"}
                gen = body.get("generation")
                if not isinstance(gen, int):
                    return 400, {"error": "generation(int) required"}
                with DB_LOCK:
                    cid = self.db.record_human_confirmation(
                        did, gen, str(body.get("detail", "")))
                return 201, {"ok": True, "id": cid}

        # /api/generations/{n}
        if len(p) == 3 and p[:2] == ["api", "generations"] and method == "GET":
            row = self.db.get_generation(int(p[2]))
            if not row:
                return 404, {"error": "not found"}
            row["payload_obj"] = json.loads(row["payload"])
            return 200, row

        # /api/replay ...
        if method == "POST" and p == ["api", "replay", "journal"]:
            raw = body if isinstance(body, (bytes, bytearray)) else None
            # 二进制上传：content-type 为 application/octet-stream 时直接转发
            return self._replay_journal(getattr(self, "_raw_body", b""), body)
        if method == "POST" and p == ["api", "replay", "events"]:
            return self._replay_events(body)
        if method == "GET" and p == ["api", "replay", "sessions"]:
            return 200, self.db.replay_sessions()

        return 404, {"error": f"no route: {method} {path}"}

    def _resolve_policy(self, qbody):
        gen = qbody.get("generation")
        payload = qbody.get("payload")
        if payload is not None:
            res = validate_payload(payload)
            if not res.ok:
                return None, (400, res.to_dict())
            return payload, None
        if isinstance(gen, int):
            row = self.db.get_generation(gen)
            if row:
                return json.loads(row["payload"]), None
        # 无指定 -> 默认策略
        return default_payload(1), None

    def _replay_events(self, body):
        events = body.get("events")
        if not isinstance(events, list):
            return 400, {"error": "events[] required"}
        policy, err = self._resolve_policy(body)
        if err:
            return err
        origin = body.get("origin", "replay")
        if origin not in ("replay", "remote"):
            origin = "replay"
        sim = sim_replay(events, policy, origin=origin)
        cmds = [{"name": c.name, "origin": c.origin, "t_ms": c.t_ms,
                 "arg": c.arg, "blocked": c.blocked, "note": c.note}
                for c in sim.commands]
        blocked = [{"name": c.name, "origin": c.origin, "t_ms": c.t_ms,
                    "note": c.note} for c in sim.blocked]
        with DB_LOCK:
            sid = self.db.save_replay_session(
                str(body.get("device_id", "lab")),
                body.get("generation"), "ok", len(sim.blocked), cmds,
                "web event replay")
            self.db.save_replay_blocked(sid, blocked)
        return 200, {
            "session_id": sid,
            "commands": cmds,
            "blocked": blocked,
            "viewport": {"x": sim.viewport.x, "y": sim.viewport.y},
            "fullscreen": sim.fullscreen,
            "panel_open": sim.panel_open,
            "policy_generation": policy["generation"],
        }

    def _replay_journal(self, raw, json_body):
        parsed = journal_codec.parse(raw)
        if not parsed.ok:
            with DB_LOCK:
                sid = self.db.save_replay_session(
                    parsed.device_id, parsed.generation, "corrupt", 0, [],
                    parsed.error)
            return 400, {"ok": False, "error": parsed.error,
                         "corrupt_at_frame": parsed.corrupt_at_frame,
                         "session_id": sid}
        # 只重放 key / focus / display 事件；R_COMMAND 审计帧不参与
        events = []
        for ev in parsed.events:
            if ev["type"] == "key":
                events.append({
                    "t_ms": ev["t_ms"], "type": "key", "key": ev["key"],
                    "down": ev["down"], "repeat": ev["repeat"],
                    "composition": ev["composition"], "mods": ev["mods"]})
            elif ev["type"] in ("focus_lost", "focus_gained",
                                "display_lost", "display_restored"):
                events.append({"t_ms": ev["t_ms"], "type": ev["type"]})
        body = dict(json_body or {})
        policy, err = self._resolve_policy(body)
        if err:
            return err
        sim = sim_replay(events, policy, origin="replay")
        cmds = [{"name": c.name, "origin": c.origin, "t_ms": c.t_ms,
                 "arg": c.arg, "blocked": c.blocked, "note": c.note}
                for c in sim.commands]
        blocked = [{"name": c.name, "origin": c.origin, "t_ms": c.t_ms,
                    "note": c.note} for c in sim.blocked]
        with DB_LOCK:
            sid = self.db.save_replay_session(
                parsed.device_id, body.get("generation", parsed.generation),
                "ok", len(sim.blocked), cmds, "device journal replay")
            self.db.save_replay_blocked(sid, blocked)
        return 200, {
            "session_id": sid,
            "device_id": parsed.device_id,
            "journal_generation": parsed.generation,
            "commands": cmds,
            "blocked": blocked,
            "viewport": {"x": sim.viewport.x, "y": sim.viewport.y},
            "input_frame_count": len(events)}


def make_handler(api: DB):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):
            sys.stderr.write("[http] " + (fmt % args) + "\n")

        def _send(self, code, obj, ctype="application/json; charset=utf-8"):
            data = obj if isinstance(obj, bytes) else \
                json.dumps(obj, ensure_ascii=False).encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def _serve_static(self, rel):
            if rel == "":
                rel = "index.html"
            path = os.path.normpath(os.path.join(WEB_DIR, rel))
            if not path.startswith(WEB_DIR) or not os.path.isfile(path):
                self._send(404, {"error": "not found"})
                return
            ctype = {".html": "text/html; charset=utf-8",
                     ".js": "application/javascript; charset=utf-8",
                     ".css": "text/css; charset=utf-8",
                     ".svg": "image/svg+xml"}.get(
                         os.path.splitext(path)[1], "application/octet-stream")
            with open(path, "rb") as f:
                self._send(200, f.read(), ctype)

        def _read_body(self):
            n = int(self.headers.get("Content-Length", "0") or 0)
            return self.rfile.read(n) if n else b""

        def do_GET(self):
            u = urlparse(self.path)
            if u.path.startswith("/api/"):
                try:
                    code, obj = Api(api).route(
                        "GET", u.path, parse_qs(u.query), None)
                except Exception as exc:  # noqa: BLE001
                    code, obj = 500, {"error": str(exc)}
                self._send(code, obj)
            else:
                self._serve_static(u.path.lstrip("/"))

        def do_POST(self):
            u = urlparse(self.path)
            raw = self._read_body()
            ctype = self.headers.get("Content-Type", "")
            try:
                if u.path == "/api/replay/journal":
                    api_obj = Api(api)
                    api_obj._raw_body = raw
                    # JSON 部分（generation/payload 选择）通过查询字符串?meta=
                    meta = parse_qs(u.query).get("meta", ["{}"])[0]
                    code, obj = api_obj.route(
                        "POST", u.path, parse_qs(u.query),
                        json.loads(meta or "{}"))
                else:
                    body = json.loads(raw.decode("utf-8")) if raw else {}
                    code, obj = Api(api).route(
                        "POST", u.path, parse_qs(u.query), body)
            except json.JSONDecodeError as exc:
                code, obj = 400, {"error": f"invalid JSON body: {exc}"}
            except Exception as exc:  # noqa: BLE001
                code, obj = 500, {"error": str(exc)}
            self._send(code, obj)

    return Handler


def main(argv=None):
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default=DEFAULT_DB)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--seed-device", default=DEFAULT_DEVICE)
    args = ap.parse_args(argv)

    db = DB(args.db)
    db.upsert_device(args.seed_device, "主展示终端")
    # 首次启动发布出厂默认策略代次
    if db.latest_generation(args.seed_device) is None:
        db.publish(args.seed_device, default_payload(1), published_by="factory")
    srv = ThreadingHTTPServer((args.host, args.port), make_handler(db))
    print(f"strategy service on http://{args.host}:{args.port} "
          f"(db={args.db})", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        db.close()


if __name__ == "__main__":
    main()
