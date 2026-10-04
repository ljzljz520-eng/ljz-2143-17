#!/usr/bin/env python3
"""SQLite-backed management service for the C input policy client."""

from __future__ import annotations

import hashlib
import json
import os
import sqlite3
import threading
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

ROOT = Path(__file__).resolve().parent.parent
WEB_ROOT = ROOT / "web"
DB_PATH = os.environ.get("POLICY_DB", str(ROOT / "data" / "policies.sqlite3"))
DEFAULT_LISTEN = ("0.0.0.0", int(os.environ.get("POLICY_PORT", "8090")))
MAX_BODY = 256 * 1024

CANONICAL_ROLES = {
    "Escape": ("modal.cancel", False),
    "F11": ("fullscreen.toggle", False),
    "ArrowLeft": ("move.left", False),
    "ArrowRight": ("move.right", False),
    "ArrowUp": ("move.up", False),
    "ArrowDown": ("move.down", False),
}
RESTORE_KEYS = {"KeyR", "KeyB", "Backspace"}
ROLE_ALIASES = {
    "move.left": {"KeyA"},
    "move.right": {"KeyD"},
    "move.up": {"KeyW"},
    "move.down": {"KeyS"},
}
MOD_LSHIFT = 0x01
MOD_LCTRL = 0x04
MOD_RCTRL = 0x08
VALID_TRUST = {"operational", "experiment"}


def now_iso() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def connect_db(path: str | None = None) -> sqlite3.Connection:
    target = path or DB_PATH
    Path(target).parent.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(target, check_same_thread=False)
    conn.row_factory = sqlite3.Row
    return conn


def init_db(conn: sqlite3.Connection) -> None:
    conn.execute("PRAGMA journal_mode=WAL")
    conn.executescript(
        """
        CREATE TABLE IF NOT EXISTS policy_generations (
            generation INTEGER PRIMARY KEY,
            payload_json TEXT NOT NULL,
            checksum_sha256 TEXT NOT NULL,
            envelope_json TEXT NOT NULL,
            status TEXT NOT NULL CHECK(status IN ('draft','active','superseded')),
            created_at TEXT NOT NULL
        );
        CREATE TABLE IF NOT EXISTS devices (
            device_id TEXT PRIMARY KEY,
            last_seen_generation INTEGER NOT NULL DEFAULT 0,
            last_seen_at TEXT,
            platform TEXT,
            app_version TEXT
        );
        CREATE TABLE IF NOT EXISTS device_receipts (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            device_id TEXT NOT NULL,
            generation INTEGER NOT NULL,
            status TEXT NOT NULL CHECK(status IN ('staged','applied','rejected')),
            reason TEXT NOT NULL DEFAULT '',
            checksum_sha256 TEXT,
            received_at TEXT NOT NULL
        );
        CREATE TABLE IF NOT EXISTS replay_sessions (
            session_id TEXT PRIMARY KEY,
            trust_level TEXT NOT NULL CHECK(trust_level IN ('operational','experiment')),
            log_sha256 TEXT NOT NULL,
            log_text TEXT NOT NULL,
            business_confirmations INTEGER NOT NULL,
            accepted INTEGER NOT NULL,
            rejection_reason TEXT NOT NULL DEFAULT '',
            created_at TEXT NOT NULL
        );
        """
    )
    conn.commit()


def default_policy_payload() -> dict:
    bindings = [
        {"key": key, "modifiers": 0, "command": command, "alias": False}
        for key, (command, _) in CANONICAL_ROLES.items()
    ]
    bindings.append(
        {"key": "KeyR", "modifiers": 0, "command": "background.restore", "alias": False}
    )
    return {
        "generation": 1,
        "scene_width": 1280,
        "scene_height": 720,
        "repeat": {"initial_ms": 180, "interval_ms": 45, "step_pixels": 8},
        "bindings": bindings,
    }


def canonical_json(value: dict) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode()


def make_envelope(payload: dict) -> dict:
    encoded = canonical_json(payload)
    return {
        "version": 1,
        "generation": int(payload["generation"]),
        "payload": encoded.decode(),
        "sha256": hashlib.sha256(encoded).hexdigest(),
    }


def validate_policy(payload: dict) -> None:
    if not isinstance(payload, dict):
        raise ValueError("policy must be a JSON object")
    generation = payload.get("generation")
    if not isinstance(generation, int) or isinstance(generation, bool) or generation < 1:
        raise ValueError("generation must be a positive integer")

    width = payload.get("scene_width")
    height = payload.get("scene_height")
    if not isinstance(width, int) or not isinstance(height, int) or width <= 72 or height <= 72:
        raise ValueError("scene dimensions must be integers larger than 72")

    repeat = payload.get("repeat")
    if not isinstance(repeat, dict):
        raise ValueError("repeat must be an object")
    initial = repeat.get("initial_ms")
    interval = repeat.get("interval_ms")
    step = repeat.get("step_pixels")
    if not all(isinstance(v, int) and not isinstance(v, bool) for v in (initial, interval, step)):
        raise ValueError("repeat timings and step must be integers")
    if not (50 <= initial <= 2000 and 10 <= interval <= 500 and initial >= interval):
        raise ValueError("repeat timing window rejected")
    if not 1 <= step <= 128:
        raise ValueError("step_pixels must be between 1 and 128")

    bindings = payload.get("bindings")
    if not isinstance(bindings, list) or not bindings:
        raise ValueError("bindings must be a non-empty list")
    if len(bindings) > 32:
        raise ValueError("too many bindings")

    seen_keys: set[tuple[str, int]] = set()
    seen_commands: set[str] = set()
    alias_commands: set[str] = set()
    restore_count = 0

    for raw in bindings:
        if not isinstance(raw, dict):
            raise ValueError("each binding must be an object")
        key = raw.get("key")
        command = raw.get("command")
        modifiers = raw.get("modifiers", 0)
        if not isinstance(key, str) or not isinstance(command, str):
            raise ValueError("binding key and command must be strings")
        if not isinstance(modifiers, int) or isinstance(modifiers, bool) or not 0 <= modifiers <= 0xFFFF:
            raise ValueError("binding modifiers are invalid")
        if (key, modifiers) in seen_keys:
            raise ValueError(f"duplicate physical key: {key}")
        seen_keys.add((key, modifiers))

        if command == "app.safe-quit" or (
            key == "KeyQ" and (modifiers & (MOD_LCTRL | MOD_RCTRL)) and (modifiers & MOD_LSHIFT)
        ):
            raise ValueError("Ctrl+Shift+Q is a non-configurable reserved chord")

        if key in CANONICAL_ROLES:
            expected, _ = CANONICAL_ROLES[key]
            if modifiers != 0 or command != expected:
                raise ValueError(f"{key} is a protected physical key and cannot be remapped")
            if command in seen_commands:
                raise ValueError(f"duplicate command mapping: {command}")
            seen_commands.add(command)
        elif command in {role[0] for role in CANONICAL_ROLES.values()}:
            if command not in ROLE_ALIASES or key not in ROLE_ALIASES[command] or modifiers != 0:
                raise ValueError(f"unsupported alias for protected role: {command}")
            if command in alias_commands:
                raise ValueError(f"duplicate alias command mapping: {command}")
            alias_commands.add(command)
        elif command == "background.restore":
            restore_count += 1
            if modifiers != 0 or key not in RESTORE_KEYS:
                raise ValueError("background restore accepts only KeyR, KeyB or Backspace without modifiers")
            if command in seen_commands:
                raise ValueError("duplicate command mapping: background.restore")
            seen_commands.add(command)
        else:
            raise ValueError(f"unknown policy command: {command}")

    for _, (required, _) in CANONICAL_ROLES.items():
        if required not in seen_commands:
            raise ValueError(f"required role missing: {required}")
    if restore_count != 1:
        raise ValueError("background restore must have exactly one binding")


def save_policy(conn: sqlite3.Connection, payload: dict) -> dict:
    validate_policy(payload)
    max_row = conn.execute("SELECT COALESCE(MAX(generation), 0) AS g FROM policy_generations").fetchone()
    next_generation = int(max_row["g"]) + 1
    payload = dict(payload)
    payload["generation"] = next_generation
    encoded = canonical_json(payload)
    checksum = hashlib.sha256(encoded).hexdigest()
    envelope = make_envelope(payload)
    conn.execute("UPDATE policy_generations SET status='superseded' WHERE status='active'")
    conn.execute(
        """INSERT INTO policy_generations
           (generation,payload_json,checksum_sha256,envelope_json,status,created_at)
           VALUES (?,?,?,?,?,?)""",
        (next_generation, encoded.decode(), checksum, json.dumps(envelope, separators=(",", ":")),
         "active", now_iso()),
    )
    conn.commit()
    return envelope


def active_envelope(conn: sqlite3.Connection) -> dict | None:
    row = conn.execute(
        "SELECT envelope_json FROM policy_generations WHERE status='active' ORDER BY generation DESC LIMIT 1"
    ).fetchone()
    return None if row is None else json.loads(row["envelope_json"])


def seed_default_policy(conn: sqlite3.Connection) -> dict:
    existing = active_envelope(conn)
    if existing is not None:
        return existing
    return save_policy(conn, default_policy_payload())


def validate_replay_log(log_text: str) -> tuple[bool, str, int]:
    if '"trust": "experiment"' not in log_text.splitlines()[0] and \
       '"trust":"experiment"' not in log_text.splitlines()[0]:
        return False, "replay requires an experiment-signed log manifest", 0
    if "business.confirm" in log_text or "operator_confirmed" in log_text:
        return False, "replay cannot synthesize or execute business confirmation", 0
    if log_text.count('"type":"key"') == 0 and log_text.count('"type": "key"') == 0:
        return False, "replay contains no experimental input events", 0
    return True, "", 0


class PolicyHandler(BaseHTTPRequestHandler):
    server_version = "InputPolicy/1.0"

    def log_message(self, fmt: str, *args) -> None:
        return

    @property
    def database(self) -> sqlite3.Connection:
        return self.server.database  # type: ignore[attr-defined]

    def send_json(self, status: int, value: dict) -> None:
        body = json.dumps(value, ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def read_json(self) -> dict:
        length = int(self.headers.get("Content-Length", "0"))
        if length <= 0 or length > MAX_BODY:
            raise ValueError("invalid request body length")
        raw = self.rfile.read(length)
        value = json.loads(raw.decode())
        if not isinstance(value, dict):
            raise ValueError("request body must be an object")
        return value

    def serve_static(self, path: str) -> None:
        if path == "/":
            file_path = WEB_ROOT / "index.html"
        else:
            relative = path.lstrip("/")
            file_path = (WEB_ROOT / relative).resolve()
            if WEB_ROOT.resolve() not in file_path.parents and file_path != WEB_ROOT.resolve():
                self.send_json(HTTPStatus.FORBIDDEN, {"error": "forbidden"})
                return
        if not file_path.is_file():
            self.send_json(HTTPStatus.NOT_FOUND, {"error": "not found"})
            return
        content = file_path.read_bytes()
        content_type = "text/html; charset=utf-8" if file_path.suffix == ".html" else "application/octet-stream"
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(content)))
        self.end_headers()
        self.wfile.write(content)

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        path = parsed.path
        try:
            if path == "/health":
                self.send_json(HTTPStatus.OK, {"ok": True, "service": "input-policy"})
            elif path == "/api/policy/current":
                envelope = active_envelope(self.database)
                if envelope is None:
                    self.send_json(HTTPStatus.NOT_FOUND, {"error": "no active policy"})
                else:
                    self.send_json(HTTPStatus.OK, envelope)
            elif path.startswith("/api/devices/") and path.endswith("/policy"):
                device_id = path.split("/")[3]
                envelope = active_envelope(self.database)
                self.database.execute(
                    """INSERT INTO devices(device_id,last_seen_at) VALUES(?,?)
                       ON CONFLICT(device_id) DO UPDATE SET last_seen_at=excluded.last_seen_at""",
                    (device_id, now_iso()),
                )
                self.database.commit()
                if envelope is None:
                    self.send_json(HTTPStatus.NOT_FOUND, {"error": "no active policy"})
                else:
                    self.send_json(HTTPStatus.OK, envelope)
            elif path == "/api/policies":
                rows = self.database.execute(
                    "SELECT generation,checksum_sha256,status,created_at FROM policy_generations ORDER BY generation"
                ).fetchall()
                self.send_json(HTTPStatus.OK, {"policies": [dict(r) for r in rows]})
            else:
                self.serve_static(path)
        except Exception as exc:  # defensive HTTP boundary
            self.send_json(HTTPStatus.INTERNAL_SERVER_ERROR, {"error": str(exc)})

    def do_POST(self) -> None:
        parsed = urlparse(self.path)
        path = parsed.path
        try:
            body = self.read_json()
            if path == "/api/policy/validate":
                validate_policy(body)
                self.send_json(HTTPStatus.OK, {"valid": True})
            elif path == "/api/policies":
                envelope = save_policy(self.database, body)
                self.send_json(HTTPStatus.CREATED, envelope)
            elif path.startswith("/api/devices/") and path.endswith("/receipts"):
                parts = path.strip("/").split("/")
                device_id = parts[2]
                generation = int(body.get("generation", -1))
                status = body.get("status")
                if status not in {"staged", "applied", "rejected"}:
                    raise ValueError("invalid receipt status")
                reason = str(body.get("reason", ""))[:500]
                checksum = str(body.get("checksum_sha256", ""))[:64]
                self.database.execute(
                    """INSERT INTO device_receipts
                       (device_id,generation,status,reason,checksum_sha256,received_at)
                       VALUES (?,?,?,?,?,?)""",
                    (device_id, generation, status, reason, checksum, now_iso()),
                )
                applied_generation = generation if status == "applied" else 0
                self.database.execute(
                    """INSERT INTO devices(device_id,last_seen_generation,last_seen_at,platform,app_version)
                       VALUES(?,?,?,?,?)
                       ON CONFLICT(device_id) DO UPDATE SET
                         last_seen_generation=CASE WHEN ?='applied' THEN excluded.last_seen_generation
                                                   ELSE devices.last_seen_generation END,
                         last_seen_at=excluded.last_seen_at,
                         platform=COALESCE(excluded.platform,devices.platform),
                         app_version=COALESCE(excluded.app_version,devices.app_version)""",
                    (device_id, applied_generation, now_iso(), body.get("platform"), body.get("app_version"), status),
                )
                self.database.commit()
                self.send_json(HTTPStatus.ACCEPTED, {"accepted": True})
            elif path == "/api/devices/check-in":
                device_id = str(body.get("device_id", ""))[:128]
                if not device_id:
                    raise ValueError("device_id is required")
                self.database.execute(
                    """INSERT INTO devices(device_id,last_seen_at,platform,app_version)
                       VALUES(?,?,?,?)
                       ON CONFLICT(device_id) DO UPDATE SET last_seen_at=excluded.last_seen_at,
                         platform=COALESCE(excluded.platform,devices.platform),
                         app_version=COALESCE(excluded.app_version,devices.app_version)""",
                    (device_id, now_iso(), body.get("platform"), body.get("app_version")),
                )
                self.database.commit()
                self.send_json(HTTPStatus.OK, {"device_id": device_id})
            elif path == "/api/replay-sessions":
                log_text = str(body.get("log", ""))
                trust = str(body.get("trust", "experiment"))
                if trust != "experiment":
                    raise ValueError("only experiment logs can be replayed")
                accepted, reason, _ = validate_replay_log(log_text)
                session_id = str(body.get("session_id") or hashlib.sha256(log_text.encode()).hexdigest()[:16])
                log_hash = hashlib.sha256(log_text.encode()).hexdigest()
                self.database.execute(
                    """INSERT OR REPLACE INTO replay_sessions
                       (session_id,trust_level,log_sha256,log_text,business_confirmations,
                        accepted,rejection_reason,created_at)
                       VALUES (?,?,?,?,?,?,?,?)""",
                    (session_id[:64], "experiment", log_hash, log_text[:200000], 0,
                     1 if accepted else 0, reason, now_iso()),
                )
                self.database.commit()
                self.send_json(HTTPStatus.OK if accepted else HTTPStatus.BAD_REQUEST,
                               {"accepted": accepted, "session_id": session_id, "reason": reason})
            else:
                self.send_json(HTTPStatus.NOT_FOUND, {"error": "not found"})
        except ValueError as exc:
            self.send_json(HTTPStatus.BAD_REQUEST, {"error": str(exc)})
        except json.JSONDecodeError:
            self.send_json(HTTPStatus.BAD_REQUEST, {"error": "malformed JSON"})
        except Exception as exc:
            self.send_json(HTTPStatus.INTERNAL_SERVER_ERROR, {"error": str(exc)})


def build_server(address=DEFAULT_LISTEN, db_path: str | None = None) -> ThreadingHTTPServer:
    ThreadingHTTPServer.allow_reuse_address = True
    server = ThreadingHTTPServer(address, PolicyHandler)
    server.database = connect_db(db_path)  # type: ignore[attr-defined]
    init_db(server.database)  # type: ignore[attr-defined]
    seed_default_policy(server.database)  # type: ignore[attr-defined]
    return server


def main() -> None:
    server = build_server()
    host, port = server.server_address
    print(f"input policy service listening on http://{host}:{port}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.database.close()  # type: ignore[attr-defined]
        server.server_close()


if __name__ == "__main__":
    main()
