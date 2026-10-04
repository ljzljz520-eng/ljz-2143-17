"""
validator.py - 策略校验（与 src/core/policy.c 同一套规则的参考实现）

C 客户端与服务必须给出一致结论；tests/ 中有跨语言对拍测试。
"""
from __future__ import annotations

import json
import hashlib
from dataclasses import dataclass, field

RESERVED = {
    ("Q", frozenset({"Ctrl"})): "Ctrl+Q is reserved for reliable quit",
    ("Q", frozenset({"Meta"})): "Meta+Q / Cmd+Q is reserved for reliable quit",
    ("F4", frozenset({"Alt"})): "Alt+F4 is reserved for window manager close",
}

PRESENTATION_COMMANDS = {
    "open_settings", "move_left", "move_right", "move_up", "move_down",
    "toggle_fullscreen", "restore_background", "confirm_business",
}
REQUIRED_COMMANDS = PRESENTATION_COMMANDS  # 全部必需

MOD_ORDER = ("Ctrl", "Alt", "Shift", "Meta")
MOD_BIT = {"Ctrl": "Ctrl", "Alt": "Alt", "Shift": "Shift", "Meta": "Meta"}


def chord_parts(chord: str):
    parts = chord.split("+")
    mods = []
    for p in parts[:-1]:
        if p not in MOD_BIT:
            return None
        mods.append(p)
    return parts[-1], mods


def reserved_reason(chord: str):
    parsed = chord_parts(chord)
    if not parsed:
        return None
    key, mods = parsed
    reason = RESERVED.get((key, frozenset(mods)))
    return reason


@dataclass
class Issue:
    code: str
    detail: str


@dataclass
class ValidationResult:
    ok: bool = True
    issues: list[Issue] = field(default_factory=list)
    conflicts: list[dict] = field(default_factory=list)
    reserved: list[str] = field(default_factory=list)

    def add(self, code: str, detail: str):
        self.ok = False
        self.issues.append(Issue(code, detail))

    def to_dict(self):
        return {
            "ok": self.ok,
            "issues": [{"code": i.code, "detail": i.detail} for i in self.issues],
            "conflicts": self.conflicts,
            "reserved": self.reserved,
        }


INT_RANGES = {
    "move_initial_delay_ms": (10, 1000),
    "move_repeat_ms": (10, 500),
    "move_max_hold_ms": (100, 10000),
    "bounds_w": (100, 10000),
    "bounds_h": (100, 10000),
}
MAX_BINDINGS = 32


def canonical_json(payload) -> str:
    """与 C 端 json_canonical / json.dumps 字节一致。"""
    return json.dumps(payload, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=False)


def default_payload(generation: int) -> dict:
    return {
        "generation": generation,
        "bounds_w": 1280,
        "bounds_h": 720,
        "move_initial_delay_ms": 350,
        "move_repeat_ms": 60,
        "move_max_hold_ms": 3000,
        "bindings": [
            {"chord": "Esc", "command": "open_settings"},
            {"chord": "F11", "command": "toggle_fullscreen"},
            {"chord": "Ctrl+R", "command": "restore_background"},
            {"chord": "ArrowLeft", "command": "move_left"},
            {"chord": "ArrowRight", "command": "move_right"},
            {"chord": "ArrowUp", "command": "move_up"},
            {"chord": "ArrowDown", "command": "move_down"},
            {"chord": "Enter", "command": "confirm_business"},
        ],
    }


def validate_payload(payload) -> ValidationResult:
    r = ValidationResult()
    if not isinstance(payload, dict):
        r.add("SCHEMA", "payload must be an object")
        return r

    gen = payload.get("generation")
    if not isinstance(gen, int) or isinstance(gen, bool) or gen < 1:
        r.add("SCHEMA", "generation must be a positive integer")

    for key, (lo, hi) in INT_RANGES.items():
        v = payload.get(key)
        if not isinstance(v, int) or isinstance(v, bool) or not (lo <= v <= hi):
            r.add("SCHEMA", f"{key} must be integer in [{lo},{hi}]")
    if isinstance(payload.get("move_max_hold_ms"), int) and \
       isinstance(payload.get("move_initial_delay_ms"), int) and \
       payload["move_max_hold_ms"] < payload["move_initial_delay_ms"]:
        r.add("SCHEMA", "move_max_hold_ms must be >= move_initial_delay_ms")

    bindings = payload.get("bindings")
    if not isinstance(bindings, list):
        r.add("SCHEMA", "bindings must be an array")
        return r
    if len(bindings) > MAX_BINDINGS:
        r.add("SCHEMA", f"too many bindings (max {MAX_BINDINGS})")

    norm = []
    for i, b in enumerate(bindings):
        where = f"bindings[{i}]"
        if not isinstance(b, dict):
            r.add("SCHEMA", f"{where} must be an object")
            continue
        if set(b.keys()) - {"chord", "command"}:
            r.add("SCHEMA", f"{where} has unknown field")
            continue
        chord, command = b.get("chord"), b.get("command")
        if not isinstance(chord, str) or not isinstance(command, str):
            r.add("SCHEMA", f"{where} requires string chord and command")
            continue
        if chord_parts(chord) is None:
            r.add("SCHEMA", f"{where}: unknown chord '{chord}'")
            continue
        key, mods = chord_parts(chord)
        if key in {"ShiftLeft", "ShiftRight", "ControlLeft", "ControlRight",
                   "AltLeft", "AltRight", "MetaLeft", "MetaRight"}:
            r.add("SCHEMA", f"{where}: cannot bind a modifier key alone")
            continue
        why = reserved_reason(chord)
        if why:
            r.add("RESERVED", f"{where}: {why}")
            r.reserved.append(chord)
            continue
        if command not in PRESENTATION_COMMANDS:
            r.add("NOT_ALLOWED", f"{where}: command '{command}' is not remappable")
            continue
        norm.append((chord, command))

    seen_chord = {}
    seen_cmd = {}
    for chord, command in norm:
        if chord in seen_chord:
            r.add("DUP_CHORD",
                  f"chord '{chord}' maps to both '{seen_chord[chord]}' and '{command}'")
            r.conflicts.append({"a": chord, "cmd_a": seen_chord[chord],
                                "b": chord, "cmd_b": command})
        if command in seen_cmd:
            r.add("DUP_COMMAND",
                  f"command '{command}' is bound to both '{seen_cmd[command]}' and '{chord}'")
            r.conflicts.append({"a": seen_cmd[command], "cmd_a": command,
                                "b": chord, "cmd_b": command})
        seen_chord[chord] = command
        seen_cmd[command] = chord

    for required in REQUIRED_COMMANDS:
        if required not in seen_cmd:
            r.add("MISSING", f"missing required binding for '{required}'")

    return r


def digest_payload(payload) -> str:
    return hashlib.sha256(canonical_json(payload).encode("utf-8")).hexdigest()


def parse_envelope(envelope) -> ValidationResult:
    """信封解析 + 摘要校验。成功时 r.payload 为 payload 对象。"""
    r = ValidationResult()
    if not isinstance(envelope, dict):
        r.add("SCHEMA", "envelope must be an object")
        return r
    if set(envelope.keys()) - {"generation", "device_id", "issued_at",
                               "payload", "digest"}:
        r.add("SCHEMA", "envelope has unknown field")
    gen = envelope.get("generation")
    if not isinstance(gen, int) or isinstance(gen, bool) or gen < 1:
        r.add("SCHEMA", "envelope.generation must be positive integer")
    if not isinstance(envelope.get("device_id"), str) or not envelope["device_id"]:
        r.add("SCHEMA", "bad envelope.device_id")
    if not isinstance(envelope.get("issued_at"), str) or not envelope["issued_at"]:
        r.add("SCHEMA", "bad envelope.issued_at")
    digest = envelope.get("digest")
    if not isinstance(digest, str) or len(digest) != 64:
        r.add("SCHEMA", "envelope.digest must be 64 hex chars")
    payload = envelope.get("payload")
    if not isinstance(payload, dict):
        r.add("SCHEMA", "envelope.payload must be an object")
        return r
    if isinstance(digest, str) and digest.lower() != digest_payload(payload):
        r.add("DIGEST", "payload digest mismatch: tampered or truncated")
    if isinstance(gen, int) and isinstance(payload.get("generation"), int) and \
            gen != payload["generation"]:
        r.add("SCHEMA", "envelope.generation must equal payload.generation")
    if r.ok:
        r.payload = payload  # type: ignore[attr-defined]
    # 即使摘要失败也继续做 schema 校验，便于网页同时显示所有问题
    inner = validate_payload(payload)
    if not inner.ok:
        r.ok = False
        r.issues.extend(inner.issues)
        r.conflicts.extend(inner.conflicts)
        r.reserved.extend(x for x in inner.reserved if x not in r.reserved)
    return r
