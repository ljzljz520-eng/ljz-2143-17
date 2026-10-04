"""跨语言一致性 + HTTP 端到端测试（标准库 unittest，零依赖）。"""
import json
import os
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.request
import urllib.error

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from service.db import DB
from service.validator import (canonical_json, default_payload, digest_payload,
                               parse_envelope, validate_payload)
from service.simulator import replay


def http(method, url, body=None, raw=None, ctype="application/json"):
    data = None
    headers = {}
    if raw is not None:
        data = raw
        headers["Content-Type"] = "application/octet-stream"
    elif body is not None:
        data = json.dumps(body, ensure_ascii=False).encode()
        headers["Content-Type"] = ctype
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req) as r:
            return r.status, json.loads(r.read().decode())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read().decode())


class ValidatorTests(unittest.TestCase):
    def test_default_payload_valid(self):
        r = validate_payload(default_payload(1))
        self.assertTrue(r.ok, r.to_dict())

    def test_reserved(self):
        for chord in ("Ctrl+Q", "Meta+Q", "Alt+F4"):
            p = default_payload(2)
            p["bindings"].append({"chord": chord, "command": "move_up"})
            r = validate_payload(p)
            self.assertFalse(r.ok)
            self.assertIn("RESERVED", [i.code for i in r.issues])

    def test_conflicts(self):
        p = default_payload(3)
        p["bindings"][0]["chord"] = "F11"  # 与 toggle 同和弦
        r = validate_payload(p)
        self.assertFalse(r.ok)
        codes = {i.code for i in r.issues}
        self.assertTrue({"DUP_CHORD", "DUP_COMMAND"} & codes)

    def test_ranges_and_missing(self):
        p = default_payload(4)
        p["move_repeat_ms"] = 99999
        r = validate_payload(p)
        self.assertFalse(r.ok)
        p = default_payload(5)
        p["bindings"] = p["bindings"][:4]
        r = validate_payload(p)
        self.assertIn("MISSING", [i.code for i in r.issues])

    def test_envelope_digest(self):
        payload = default_payload(7)
        env = {"generation": 7, "device_id": "d", "issued_at": "t",
               "payload": payload, "digest": digest_payload(payload)}
        r = parse_envelope(env)
        self.assertTrue(r.ok, [i.detail for i in r.issues])
        env["payload"]["bounds_w"] = 800
        r = parse_envelope(env)
        self.assertFalse(r.ok)
        self.assertIn("DIGEST", [i.code for i in r.issues])


class SimulatorTests(unittest.TestCase):
    def test_replay_blocks_business_confirm(self):
        sim = replay(
            [{"t_ms": 10, "type": "key", "key": "Enter", "down": True}],
            default_payload(1), origin="replay")
        self.assertTrue(all(c.blocked for c in sim.blocked))
        self.assertEqual(sim.blocked[0].name, "confirm_business")
        executed = [c.name for c in sim.commands if not c.blocked]
        self.assertNotIn("confirm_business", executed)

    def test_ime_gate(self):
        events = [
            {"t_ms": 0, "type": "key", "key": "Esc", "down": True},
            {"t_ms": 5, "type": "composition", "text": "hao"},
            {"t_ms": 10, "type": "key", "key": "F11", "down": True,
             "composition": True},
            {"t_ms": 20, "type": "key", "key": "Esc", "down": True,
             "composition": True},
            {"t_ms": 30, "type": "key", "key": "Q", "down": True,
             "mods": ["Ctrl"], "composition": True},
        ]
        sim = replay(events, default_payload(1), origin="human")
        names = [c.name for c in sim.commands]
        self.assertNotIn("toggle_fullscreen", names)
        self.assertNotIn("close_panel", names)
        self.assertIn("quit", names)

    def test_f11_edge(self):
        events = [{"t_ms": t, "type": "key", "key": "F11",
                   "down": True, "repeat": t > 0} for t in range(0, 300, 30)]
        sim = replay(events, default_payload(1), origin="human")
        self.assertEqual(
            len([c for c in sim.commands if c.name == "toggle_fullscreen"]), 1)
        self.assertTrue(sim.fullscreen)

    def test_bounded_repeat(self):
        sim = replay([], default_payload(1), origin="human")
        sim.key_event("ArrowRight", True, t=0)
        for t in range(16, 5000, 16):
            sim.tick(t)
        n1 = len([c for c in sim.commands if c.name == "move_right"])
        sim.tick(7000)
        n2 = len([c for c in sim.commands if c.name == "move_right"])
        self.assertEqual(n1, n2)
        self.assertGreater(n1, 1)
        self.assertLessEqual(n1, 45)

    def test_focus_lost_no_stuck(self):
        sim = replay([], default_payload(1), origin="human")
        sim.key_event("ArrowDown", True, t=0)
        sim.focus_lost()
        for t in range(16, 5000, 16):
            sim.tick(t)
        # 仅首步；失焦后无连发
        self.assertEqual(
            len([c for c in sim.commands if c.name == "move_down"]), 1)

    def test_activation_boundary(self):
        sim = replay([], default_payload(1), origin="human")
        sim.key_event("ArrowRight", True, t=0)
        self.assertEqual(len(sim.held), 1)
        released = sim.commit_policy(default_payload(2), t=100)
        self.assertEqual(released, ["ArrowRight"])
        self.assertEqual(len(sim.held), 0)
        self.assertEqual(sim.mods, set())

    def test_viewport_clamp(self):
        p = default_payload(1)
        p["bounds_w"] = 2
        p["bounds_h"] = 2
        sim = replay([], p, origin="human")
        sim.key_event("ArrowRight", True, t=0)
        sim.key_event("ArrowRight", False, t=1)
        sim.key_event("ArrowRight", True, t=2)
        sim.key_event("ArrowRight", False, t=3)
        sim.key_event("ArrowRight", True, t=4)
        self.assertLessEqual(sim.viewport.x, 2)


class CrossLanguageTests(unittest.TestCase):
    """C 默认 payload 与 canonical/sha256 必须与 Python 一致。"""

    def setUp(self):
        self.bin = os.path.join(ROOT, "tests", "test_core")

    @unittest.skipUnless(os.path.exists(os.path.join(ROOT, "tests", "test_core")),
                         "C tests not built")
    def test_c_tests_pass(self):
        r = subprocess.run([self.bin], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


class HttpE2ETests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp()
        cls.db = os.path.join(cls.tmp, "e2e.db")
        env = dict(os.environ)
        cls.proc = subprocess.Popen(
            [sys.executable, "-m", "service.server",
             "--db", cls.db, "--port", "8099", "--seed-device", "dev-e2e"],
            cwd=ROOT, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        for _ in range(50):
            try:
                http("GET", "http://127.0.0.1:8099/api/devices")
                break
            except Exception:
                time.sleep(0.1)

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait(timeout=5)

    def test_01_index(self):
        with urllib.request.urlopen(
                "http://127.0.0.1:8099/") as r:
            self.assertIn("text/html", r.headers["Content-Type"])
            self.assertIn(b"input-strategy", r.read())

    def test_02_validate_and_publish(self):
        code, body = http("POST", "http://127.0.0.1:8099/api/validate",
                          default_payload(99))
        self.assertEqual((code, body["ok"]), (200, True))
        bad = default_payload(2)
        bad["bindings"].append({"chord": "Ctrl+Q", "command": "move_up"})
        code, body = http("POST", "http://127.0.0.1:8099/api/validate", bad)
        self.assertEqual((code, body["ok"]), (200, False))
        self.assertIn("RESERVED", [i["code"] for i in body["issues"]])

        code, body = http(
            "POST",
            "http://127.0.0.1:8099/api/devices/dev-e2e/publish",
            default_payload(1))
        self.assertEqual(code, 201, body)
        self.assertIn("envelope", body)

    def test_03_poll_and_receipt(self):
        code, env = http(
            "GET", "http://127.0.0.1:8099/api/devices/dev-e2e/policy")
        self.assertEqual(code, 200)
        self.assertTrue(parse_envelope(env).ok)
        code, body = http(
            "POST",
            "http://127.0.0.1:8099/api/devices/dev-e2e/receipts",
            {"generation": env["generation"], "status": "applied",
             "released_keys": 0})
        self.assertEqual(code, 201, body)

    def test_04_replay_safety_over_http(self):
        events = [
            {"t_ms": 100, "type": "key", "key": "Enter", "down": True},
            {"t_ms": 200, "type": "key", "key": "F11", "down": True},
        ]
        code, body = http(
            "POST", "http://127.0.0.1:8099/api/replay/events",
            {"device_id": "dev-e2e", "events": events})
        self.assertEqual(code, 200, body)
        self.assertEqual(body["blocked"][0]["name"], "confirm_business")
        names = [c["name"] for c in body["commands"] if not c["blocked"]]
        self.assertNotIn("confirm_business", names)

    def test_05_human_only_confirmation(self):
        code, body = http(
            "POST",
            "http://127.0.0.1:8099/api/devices/dev-e2e/confirmations",
            {"generation": 1, "origin": "replay"})
        self.assertEqual(code, 403)
        code, body = http(
            "POST",
            "http://127.0.0.1:8099/api/devices/dev-e2e/confirmations",
            {"generation": 1, "origin": "human", "detail": "on-site"})
        self.assertEqual(code, 201, body)

    def test_06_db_isolation(self):
        db = DB(self.db)
        confs = db.confirmations("dev-e2e")
        self.assertEqual(len(confs), 1)
        self.assertEqual(confs[0]["origin"], "human")
        sessions = db.replay_sessions()
        self.assertTrue(any(s["blocked_business"] >= 1 for s in sessions))
        db.close()


if __name__ == "__main__":
    unittest.main(verbosity=2)
