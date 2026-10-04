"""db.py - 关系数据库：策略代次、设备回执、业务确认、重放会话/审计。

关键隔离设计：
  business_confirmations 只记录 ORIGIN=human 的真人确认；
  replay_sessions 与 replay_events 是独立实验表；
  replay 产生的 confirm_business 进入 replay_blocked_events，
  永远不会写入 business_confirmations。
"""
from __future__ import annotations

import json
import sqlite3
import time
from pathlib import Path

from .validator import canonical_json, digest_payload, validate_payload

SCHEMA = """
CREATE TABLE IF NOT EXISTS devices (
    device_id   TEXT PRIMARY KEY,
    name        TEXT NOT NULL DEFAULT '',
    created_at  TEXT NOT NULL,
    last_seen_at TEXT
);

CREATE TABLE IF NOT EXISTS policy_generations (
    generation  INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id   TEXT NOT NULL,
    payload     TEXT NOT NULL,
    digest      TEXT NOT NULL,
    issued_at   TEXT NOT NULL,
    published_by TEXT NOT NULL DEFAULT 'web',
    superseded  INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS device_receipts (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id   TEXT NOT NULL,
    generation  INTEGER NOT NULL,
    status      TEXT NOT NULL,            -- applied | rejected
    reason      TEXT NOT NULL DEFAULT '',
    released_keys INTEGER NOT NULL DEFAULT 0,
    received_at TEXT NOT NULL,
    UNIQUE(device_id, generation)
);

-- 仅真人现场确认。重放/远程永远不会进入此表。
CREATE TABLE IF NOT EXISTS business_confirmations (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id   TEXT NOT NULL,
    generation  INTEGER NOT NULL,
    confirmed_at TEXT NOT NULL,
    origin      TEXT NOT NULL CHECK (origin = 'human'),
    detail      TEXT NOT NULL DEFAULT ''
);

-- 实验重放会话（与正式业务数据分离）
CREATE TABLE IF NOT EXISTS replay_sessions (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id   TEXT NOT NULL DEFAULT '',
    generation  INTEGER,
    started_at  TEXT NOT NULL,
    result      TEXT NOT NULL,           -- ok | corrupt
    blocked_business INTEGER NOT NULL DEFAULT 0,
    commands_json TEXT NOT NULL DEFAULT '[]',
    note        TEXT NOT NULL DEFAULT ''
);

CREATE TABLE IF NOT EXISTS replay_blocked_events (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id  INTEGER NOT NULL,
    command     TEXT NOT NULL,
    origin      TEXT NOT NULL,
    reason      TEXT NOT NULL,
    at_ms       INTEGER NOT NULL,
    FOREIGN KEY(session_id) REFERENCES replay_sessions(id)
);
"""


def now_iso():
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


class DB:
    def __init__(self, path: str):
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        self.conn = sqlite3.connect(path, check_same_thread=False)
        self.conn.row_factory = sqlite3.Row
        self.conn.executescript(SCHEMA)
        self.conn.commit()

    def close(self):
        self.conn.close()

    def upsert_device(self, device_id, name=""):
        self.conn.execute(
            "INSERT INTO devices(device_id,name,created_at,last_seen_at) "
            "VALUES(?,?,?,?) ON CONFLICT(device_id) DO UPDATE SET "
            "last_seen_at=excluded.last_seen_at",
            (device_id, name, now_iso(), now_iso()))
        self.conn.commit()

    def publish(self, device_id, payload, published_by="web"):
        result = validate_payload(payload)
        if not result.ok:
            return None, result
        cur = self.conn.execute(
            "UPDATE policy_generations SET superseded=1 WHERE device_id=?",
            (device_id,))
        gen = (self.conn.execute(
            "SELECT COALESCE(MAX(generation),0)+1 AS g "
            "FROM policy_generations").fetchone())["g"]
        payload = dict(payload)
        payload["generation"] = gen  # 代次以数据库分配为准
        digest = digest_payload(payload)
        issued = now_iso()
        self.conn.execute(
            "INSERT INTO policy_generations"
            "(generation,device_id,payload,digest,issued_at,published_by) "
            "VALUES(?,?,?,?,?,?)",
            (gen, device_id, canonical_json(payload), digest, issued,
             published_by))
        self.conn.commit()
        return {"generation": gen, "digest": digest, "issued_at": issued}, result

    def latest_generation(self, device_id):
        row = self.conn.execute(
            "SELECT * FROM policy_generations WHERE device_id=? "
            "ORDER BY generation DESC LIMIT 1", (device_id,)).fetchone()
        return dict(row) if row else None

    def get_generation(self, generation):
        row = self.conn.execute(
            "SELECT * FROM policy_generations WHERE generation=?",
            (generation,)).fetchone()
        return dict(row) if row else None

    def list_generations(self, device_id, limit=50):
        rows = self.conn.execute(
            "SELECT generation,device_id,digest,issued_at,published_by,"
            "superseded FROM policy_generations WHERE device_id=? "
            "ORDER BY generation DESC LIMIT ?", (device_id, limit)).fetchall()
        return [dict(r) for r in rows]

    def envelope_for(self, device_id):
        row = self.latest_generation(device_id)
        if not row:
            return None
        return {
            "generation": row["generation"],
            "device_id": device_id,
            "issued_at": row["issued_at"],
            "payload": json.loads(row["payload"]),
            "digest": row["digest"],
        }

    def record_receipt(self, device_id, generation, status, reason="",
                       released_keys=0):
        self.conn.execute(
            "INSERT INTO device_receipts"
            "(device_id,generation,status,reason,released_keys,received_at) "
            "VALUES(?,?,?,?,?,?) ON CONFLICT(device_id,generation) DO UPDATE SET "
            "status=excluded.status,reason=excluded.reason,"
            "released_keys=excluded.released_keys,received_at=excluded.received_at",
            (device_id, generation, status, reason, released_keys, now_iso()))
        self.conn.execute(
            "UPDATE devices SET last_seen_at=? WHERE device_id=?",
            (now_iso(), device_id))
        self.conn.commit()

    def receipts(self, device_id, limit=50):
        rows = self.conn.execute(
            "SELECT * FROM device_receipts WHERE device_id=? "
            "ORDER BY id DESC LIMIT ?", (device_id, limit)).fetchall()
        return [dict(r) for r in rows]

    def record_human_confirmation(self, device_id, generation, detail=""):
        """唯一能写入业务确认的入口；非 human origin 由调用层拒绝。"""
        cur = self.conn.execute(
            "INSERT INTO business_confirmations"
            "(device_id,generation,confirmed_at,origin,detail) "
            "VALUES(?,?,?,?,?)",
            (device_id, generation, now_iso(), "human", detail))
        self.conn.commit()
        return cur.lastrowid

    def confirmations(self, device_id, limit=50):
        rows = self.conn.execute(
            "SELECT * FROM business_confirmations WHERE device_id=? "
            "ORDER BY id DESC LIMIT ?", (device_id, limit)).fetchall()
        return [dict(r) for r in rows]

    def save_replay_session(self, device_id, generation, result, blocked,
                            commands, note=""):
        cur = self.conn.execute(
            "INSERT INTO replay_sessions"
            "(device_id,generation,started_at,result,blocked_business,"
            "commands_json,note) VALUES(?,?,?,?,?,?,?)",
            (device_id, generation, now_iso(), result, blocked,
             json.dumps(commands, ensure_ascii=False), note))
        self.conn.commit()
        return cur.lastrowid

    def save_replay_blocked(self, session_id, blocked_events):
        for ev in blocked_events:
            self.conn.execute(
                "INSERT INTO replay_blocked_events"
                "(session_id,command,origin,reason,at_ms) "
                "VALUES(?,?,?,?,?)",
                (session_id, ev.get("name", "confirm_business"),
                 ev.get("origin", "replay"), ev.get("note", ""),
                 ev.get("t_ms", 0)))
        self.conn.commit()

    def replay_sessions(self, limit=50):
        rows = self.conn.execute(
            "SELECT * FROM replay_sessions ORDER BY id DESC LIMIT ?",
            (limit,)).fetchall()
        return [dict(r) for r in rows]
