import hashlib
import tempfile
import threading
import unittest
import urllib.request
from http.server import ThreadingHTTPServer
from pathlib import Path

import sys
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "service"))

import policy_server as server_mod


class PolicyServiceTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        db = str(Path(self.tmp.name) / "test.sqlite3")
        self.httpd = server_mod.ThreadingHTTPServer(("127.0.0.1", 0), server_mod.PolicyHandler)
        self.httpd.database = server_mod.connect_db(db)
        server_mod.init_db(self.httpd.database)
        server_mod.seed_default_policy(self.httpd.database)
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self.thread.start()
        self.base = f"http://127.0.0.1:{self.httpd.server_address[1]}"

    def tearDown(self):
        self.httpd.shutdown()
        self.thread.join(timeout=2)
        self.httpd.database.close()
        self.httpd.server_close()
        self.tmp.cleanup()

    def request(self, method, path, value=None):
        data = None if value is None else __import__("json").dumps(value).encode()
        headers = {"Content-Type": "application/json"} if data else {}
        req = urllib.request.Request(self.base + path, data=data, headers=headers, method=method)
        try:
            with urllib.request.urlopen(req, timeout=3) as response:
                return response.status, __import__("json").loads(response.read())
        except Exception as exc:
            body = getattr(exc, "read", lambda: b"{}")()
            return exc.code, __import__("json").loads(body)

    def test_current_policy_receipt_and_generation(self):
        status, envelope = self.request("GET", "/api/devices/kiosk-1/policy")
        self.assertEqual(status, 200)
        self.assertEqual(envelope["generation"], 1)
        self.assertEqual(hashlib.sha256(envelope["payload"].encode()).hexdigest(),
                         envelope["sha256"])

        receipt = {"generation": 1, "status": "applied", "reason": "quiescent boundary"}
        status, result = self.request("POST", "/api/devices/kiosk-1/receipts", receipt)
        self.assertEqual(status, 202)
        self.assertTrue(result["accepted"])

        row = self.httpd.database.execute(
            "SELECT * FROM device_receipts WHERE device_id='kiosk-1'"
        ).fetchone()
        self.assertEqual(row["status"], "applied")

    def test_reserved_and_conflict_validation(self):
        payload = server_mod.default_policy_payload()
        payload["bindings"][0]["key"] = "F11"
        status, result = self.request("POST", "/api/policy/validate", payload)
        self.assertEqual(status, 400)
        self.assertIn("protected", result["error"])

        payload = server_mod.default_policy_payload()
        payload["bindings"][0]["command"] = "fullscreen.toggle"
        status, result = self.request("POST", "/api/policy/validate", payload)
        self.assertEqual(status, 400)
        self.assertIn("protected", result["error"])

        payload = server_mod.default_policy_payload()
        payload["bindings"].append(
            {"key": "KeyQ", "modifiers": 5, "command": "app.safe-quit", "alias": False}
        )
        status, result = self.request("POST", "/api/policy/validate", payload)
        self.assertEqual(status, 400)
        self.assertIn("reserved", result["error"])

    def test_alias_and_new_generation_accepted(self):
        payload = server_mod.default_policy_payload()
        payload["restoreKey" if False else "bindings"][-1]["key"] = "Backspace"
        payload["bindings"].append(
            {"key": "KeyA", "modifiers": 0, "command": "move.left", "alias": True}
        )
        status, envelope = self.request("POST", "/api/policies", payload)
        self.assertEqual(status, 201)
        self.assertEqual(envelope["generation"], 2)
        status, current = self.request("GET", "/api/policy/current")
        self.assertEqual(current["generation"], 2)

    def test_corrupt_envelope_is_descriptive(self):
        payload = {"generation": 1}
        status, result = self.request("POST", "/api/policy/validate", payload)
        self.assertEqual(status, 400)
        self.assertIn("dimensions", result["error"])

    def test_replay_rejects_operational_and_business_confirmation(self):
        manifest = '{"seq":0,"session":"s","trust":"experiment","hash":"x"}\n'
        status, result = self.request("POST", "/api/replay-sessions", {
            "trust": "experiment", "log": manifest
        })
        self.assertEqual(status, 400)
        self.assertIn("input events", result["reason"])

        operational = '{"seq":0,"trust":"operational","hash":"x"}\n'
        status, result = self.request("POST", "/api/replay-sessions", {
            "trust": "operational", "log": operational
        })
        self.assertEqual(status, 400)

        event = '{"seq":1,"type":"key","key":"Enter","business.confirm":true}\n'
        status, result = self.request("POST", "/api/replay-sessions", {
            "trust": "experiment", "log": manifest + event
        })
        self.assertEqual(status, 400)
        self.assertIn("business confirmation", result["reason"])


if __name__ == "__main__":
    unittest.main()
