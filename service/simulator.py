"""
simulator.py - 核心分发器的 Python 参考实现（用于网页"实验重放"）

与 src/core/dispatcher.c 保持同一语义：
  - 三层路由（模态 / IME 组合态输入框 / 展示层）
  - F11 等边沿命令只在首次 keydown 触发，重复事件被吞
  - 方向键忽略 OS repeat，由时间参数产生有界连发
  - 重放来源(origin=replay/remote)的 confirm_business 一律拦截
  - 激活边界：新策略提交时把仍按住的键按旧策略合成抬起
"""
from __future__ import annotations

from dataclasses import dataclass, field

from .validator import canonical_json, chord_parts  # noqa: F401

EDGE_ONLY = {
    "toggle_fullscreen", "restore_background",
    "confirm_business", "open_settings", "close_panel", "quit",
}
MOVEMENT = {"move_left", "move_right", "move_up", "move_down"}
MODS = ("Shift", "Ctrl", "Alt", "Meta")
MOD_KEYS = {"ShiftLeft", "ShiftRight", "ControlLeft", "ControlRight",
            "AltLeft", "AltRight", "MetaLeft", "MetaRight"}
MOD_OF = {"ShiftLeft": "Shift", "ShiftRight": "Shift",
          "ControlLeft": "Ctrl", "ControlRight": "Ctrl",
          "AltLeft": "Alt", "AltRight": "Alt",
          "MetaLeft": "Meta", "MetaRight": "Meta"}


def chord_str(key, mods):
    order = ("Ctrl", "Alt", "Shift", "Meta")
    return "".join(f"{m}+" for m in order if m in mods) + key


@dataclass
class Viewport:
    w: int = 1280
    h: int = 720
    x: int = 0
    y: int = 0

    def move(self, cmd, steps=1):
        dx = dy = 0
        if cmd == "move_left": dx = -steps
        elif cmd == "move_right": dx = steps
        elif cmd == "move_up": dy = -steps
        elif cmd == "move_down": dy = steps
        nx = min(max(self.x + dx, 0), self.w)
        ny = min(max(self.y + dy, 0), self.h)
        moved = (nx != self.x or ny != self.y)
        self.x, self.y = nx, ny
        return moved


@dataclass
class OutCommand:
    name: str
    origin: str
    t_ms: int
    arg: int = 0
    blocked: bool = False
    note: str = ""


@dataclass
class Simulator:
    policy: dict
    log_events: list | None = None  # 已解析归一化输入 [{t,type,...}]
    origin: str = "replay"
    commands: list[OutCommand] = field(default_factory=list)
    blocked: list[OutCommand] = field(default_factory=list)
    panel_open: bool = False
    composing: bool = False
    mods: set = field(default_factory=set)
    held: set = field(default_factory=set)
    armed_edge: set = field(default_factory=set)
    repeat_state: dict = field(default_factory=dict)
    viewport: Viewport = field(default_factory=Viewport)
    fullscreen: bool = False

    def __post_init__(self):
        self.viewport = Viewport(self.policy["bounds_w"],
                                 self.policy["bounds_h"])
        self._bind = {}
        for b in self.policy["bindings"]:
            self._bind[b["chord"]] = b["command"]

    def lookup(self, key, mods):
        return self._bind.get(chord_str(key, mods))

    def emit(self, name, t, arg=0):
        c = OutCommand(name, self.origin, t, arg)
        if name == "confirm_business" and self.origin != "human":
            c.blocked = True
            c.note = "replay/remote cannot forge on-site business confirmation"
            self.blocked.append(c)
            self.commands.append(c)
            return False
        self.commands.append(c)
        if name in MOVEMENT:
            self.viewport.move(name, arg)
        elif name == "toggle_fullscreen":
            self.fullscreen = not self.fullscreen
        return True

    def hard_quit(self, key, mods):
        return ((key == "Q" and mods == {"Ctrl"}) or
                (key == "Q" and mods == {"Meta"}) or
                (key == "F4" and mods == {"Alt"}))

    def key_event(self, key, down, repeat=False, composition=False,
                  mods=None, t=0):
        m = set(mods) if mods is not None else set(self.mods)
        if key in MOD_KEYS:
            mm = MOD_OF[key]
            if down: self.mods.add(mm)
            else: self.mods.discard(mm)
            if down: self.held.add(key)
            else: self.discard_held(key)
            return
        if down: self.held.add(key)
        else: self.discard_held(key)

        if self.hard_quit(key, m):
            if down and not repeat:
                self.emit("quit", t)
                self.commands[-1].note = "hard-reserved; replay keeps host alive"
            return

        if self.panel_open:
            if composition:
                if not self.hard_quit(key, m):
                    return  # IME 组合态：快捷键被门控
            if key == "Esc" and not composition:
                if down and not repeat:
                    self.panel_open = False
                    self.composing = False
                    self.emit("close_panel", t)
                return
            # 模态：展示快捷键不穿透
            return

        if not down:
            self.armed_edge.discard(key)
            self.repeat_state.pop(key, None)
            return

        cmd = self.lookup(key, m)
        if not cmd:
            return

        if cmd in MOVEMENT:
            if repeat:
                return  # OS 重复忽略，连发由 tick 生成
            self.emit(cmd, t, 1)
            self.repeat_state[key] = {
                "cmd": cmd, "start": t, "last": t,
                "deadline": t + self.policy["move_max_hold_ms"], "count": 0}
            return

        if cmd in EDGE_ONLY:
            if repeat:
                return  # 全屏/恢复/确认：吞掉重复，绝不来回触发
            if key in self.armed_edge:
                return
            self.armed_edge.add(key)
            self.emit(cmd, t)
            if cmd == "open_settings":
                self.panel_open = True
            return

    def discard_held(self, key):
        self.held.discard(key)

    def text_event(self, text, t=0):
        if self.panel_open and not self.composing:
            c = OutCommand("text_insert", self.origin, t)
            # 文本事件在重放中只做记录，不产生业务确认
            self.commands.append(c)

    def composition_event(self, text):
        self.composing = bool(text)

    def focus_lost(self):
        self.held.clear()
        self.mods.clear()
        self.armed_edge.clear()
        self.repeat_state.clear()

    def display_lost(self):
        pass  # 网页模拟器无真实显示；记录语义见命令序列

    def tick(self, now_ms):
        """每次帧 tick 最多补发一个到期移动（与 C repeat.c 同语义，
        由调用方以帧频率驱动）；到达 max_hold 自动结束。"""
        for key, st in list(self.repeat_state.items()):
            if now_ms >= st["deadline"]:
                del self.repeat_state[key]
                continue
            if st["count"] == 0:
                nxt = st["start"] + self.policy["move_initial_delay_ms"]
            else:
                nxt = st["last"] + self.policy["move_repeat_ms"]
            if now_ms >= nxt:
                ok = self.emit(st["cmd"], now_ms, 1)
                if not ok:
                    del self.repeat_state[key]
                    continue
                st["count"] += 1
                st["last"] = now_ms

    def commit_policy(self, new_policy, t=0):
        """激活边界：合成旧键抬起、清空修饰与边沿 ARM，再原子替换。"""
        released = sorted(self.held)
        self.held.clear()
        self.mods.clear()
        self.armed_edge.clear()
        self.repeat_state.clear()
        self.policy = new_policy
        self._bind = {b["chord"]: b["command"] for b in new_policy["bindings"]}
        return released


def replay(events, policy, origin="replay"):
    """events: [{t_ms, type, key, down, repeat, composition, mods, text, sys}]
    返回 Simulator（commands/blocked/viewport 供网页展示）。"""
    sim = Simulator(policy=policy, origin=origin)
    last_t = 0
    for ev in events:
        t = ev.get("t_ms", last_t)
        last_t = t
        sim.tick(t)
        typ = ev["type"]
        if typ == "key":
            sim.key_event(ev["key"], ev["down"], ev.get("repeat", False),
                          ev.get("composition", False),
                          ev.get("mods"), t)
        elif typ == "text":
            sim.text_event(ev.get("text", ""), t)
        elif typ == "composition":
            sim.composition_event(ev.get("text", ""))
        elif typ == "focus_lost":
            sim.focus_lost()
        elif typ == "display_lost":
            sim.display_lost()
    sim.tick(last_t + 1)
    return sim
