"""journal_codec.py - 解析 C 端 ISLOG 二进制日志（与 src/core/journal.c 对应）"""
from __future__ import annotations

import struct
import zlib

from dataclasses import dataclass

MAGIC = b"ISLOG"
HEADER_SIZE = 116

R_INPUT = 1
R_COMMAND = 2
R_SYSTEM = 3
R_ACTIVATION = 4
R_REPLAY_BLOCKED = 5

SYS_NAMES = {1: "focus_lost", 2: "focus_gained",
             3: "display_lost", 4: "display_restored"}

NK_NAMES = [
    "Unknown", "Esc",
    "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
]
# NK 枚举表必须与 C 端 key.h 严格一致（顺序即数值）
NK_TABLE = [
    "Unknown", "Esc",
    "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
    "Grave", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0",
    "Minus", "Equal", "Backspace",
    "Tab", "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P",
    "BracketLeft", "BracketRight", "Backslash",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", "Semicolon", "Quote", "Enter",
    "Z", "X", "C", "V", "B", "N", "M", "Comma", "Period", "Slash",
    "Space", "ArrowLeft", "ArrowRight", "ArrowUp", "ArrowDown",
    "ShiftLeft", "ShiftRight", "ControlLeft", "ControlRight",
    "AltLeft", "AltRight", "MetaLeft", "MetaRight",
    "Home", "End", "PageUp", "PageDown", "Insert", "Delete",
    "CapsLock", "PrintScreen", "ScrollLock", "Pause",
]


@dataclass
class ParseResult:
    ok: bool
    error: str = ""
    device_id: str = ""
    generation: int = 0
    started_at: str = ""
    events: list = None
    corrupt_at_frame: int | None = None

    def __post_init__(self):
        if self.events is None:
            self.events = []


def _u32(b, o): return struct.unpack_from("<I", b, o)[0]
def _u16(b, o): return struct.unpack_from("<H", b, o)[0]
def _u64(b, o): return struct.unpack_from("<Q", b, o)[0]


def parse(data: bytes) -> ParseResult:
    if len(data) < HEADER_SIZE or data[:5] != MAGIC:
        return ParseResult(False, "bad header")
    version = data[5]
    if version != 1:
        return ParseResult(False, f"unsupported version {version}")
    device_id = data[6:70].split(b"\x00", 1)[0].decode("utf-8", "replace")
    generation = _u64(data, 70)
    started_at = data[78:110].split(b"\x00", 1)[0].decode("utf-8", "replace")

    off = HEADER_SIZE
    idx = 0
    events = []
    while off < len(data):
        if off + 4 > len(data):
            return ParseResult(False, "truncated length", device_id,
                               generation, started_at, events, idx)
        frame_len = _u32(data, off)
        off += 4
        if frame_len < 1 or off + frame_len + 4 > len(data):
            return ParseResult(False, "truncated frame", device_id,
                               generation, started_at, events, idx)
        payload = data[off:off + frame_len]
        crc_stored = _u32(data, off + frame_len)
        crc_calc = zlib.crc32(payload) & 0xFFFFFFFF
        if crc_stored != crc_calc:
            return ParseResult(False, "crc mismatch", device_id,
                               generation, started_at, events, idx)
        off += frame_len + 4
        rtype = payload[0]
        body = payload[1:]
        rel_ms = _u32(body, 0)
        ev = {"frame": idx, "t_ms": rel_ms, "record": rtype}
        if rtype == R_INPUT and len(body) >= 84:
            origin = body[4]
            key_idx = _u16(body, 5)
            mods_bits = _u32(body, 7)
            flags = body[11]
            key = NK_TABLE[key_idx] if key_idx < len(NK_TABLE) else "Unknown"
            mod_names = []
            if mods_bits & 1: mod_names.append("Shift")
            if mods_bits & 2: mod_names.append("Ctrl")
            if mods_bits & 4: mod_names.append("Alt")
            if mods_bits & 8: mod_names.append("Meta")
            text = body[20:84].split(b"\x00", 1)[0].decode("utf-8", "replace")
            ev.update({
                "type": "key",
                "origin": ("human", "replay", "remote")[min(origin, 2)],
                "key": key, "mods": mod_names,
                "down": bool(flags & 1),
                "repeat": bool(flags & 2),
                "composition": bool(flags & 4),
                "text": text,
            })
            events.append(ev)
        elif rtype == R_SYSTEM and len(body) >= 5:
            ev["type"] = SYS_NAMES.get(body[4], "unknown_sys")
            events.append(ev)
        elif rtype == R_ACTIVATION and len(body) >= 24:
            ev.update({"type": "activation",
                       "old_gen": _u64(body, 4),
                       "new_gen": _u64(body, 12),
                       "released_keys": _u32(body, 20)})
        elif rtype == R_COMMAND and len(body) >= 57:
            origin = body[4]
            arg = struct.unpack_from("<i", body, 5)[0]
            name = body[9:57].split(b"\x00", 1)[0].decode()
            ev.update({"type": "command_log",
                       "origin": ("human", "replay", "remote")[min(origin, 2)],
                       "command": name, "arg": arg})
        idx += 1
    return ParseResult(True, "", device_id, generation, started_at, events)
