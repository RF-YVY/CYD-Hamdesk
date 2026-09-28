import json
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib import error, request

from tools.propview_relay import make_handler, project


class Upstream(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/api/status":
            body = {"station": "K5YVY-1", "rf_connected": True, "aprs_is_connected": True, "private": "do not forward"}
        elif self.path == "/api/propagation":
            body = {"my_score": 32.0, "score": 21.4, "event": {"state": "normal", "transitions": [1, 2, 3]}}
        else:
            self.send_error(404)
            return
        payload = json.dumps(body).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, *args):
        pass


class RelayTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.upstream = ThreadingHTTPServer(("127.0.0.1", 0), Upstream)
        cls.relay = ThreadingHTTPServer(
            ("127.0.0.1", 0),
            make_handler(f"http://127.0.0.1:{cls.upstream.server_port}", "test-token-123456789"),
        )
        for server in (cls.upstream, cls.relay):
            threading.Thread(target=server.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        for server in (cls.relay, cls.upstream):
            server.shutdown()
            server.server_close()

    def get(self, path, token="test-token-123456789"):
        req = request.Request(
            f"http://127.0.0.1:{self.relay.server_port}{path}",
            headers={"X-HamDesk-Token": token},
        )
        with request.urlopen(req, timeout=3) as response:
            return response.status, json.load(response)

    def test_only_required_status_fields_are_forwarded(self):
        status, body = self.get("/api/status")
        self.assertEqual(status, 200)
        self.assertEqual(body["station"], "K5YVY-1")
        self.assertNotIn("private", body)

    def test_propagation_event_is_compact(self):
        status, body = self.get("/api/propagation")
        self.assertEqual(status, 200)
        self.assertEqual(body["event"], {"state": "normal"})

    def test_bad_token_and_other_paths_are_rejected(self):
        with self.assertRaises(error.HTTPError) as result:
            self.get("/api/status", "wrong")
        self.assertEqual(result.exception.code, 403)
        with self.assertRaises(error.HTTPError) as result:
            self.get("/api/stations/rf")
        self.assertEqual(result.exception.code, 404)

    def test_projection_rejects_non_object(self):
        with self.assertRaises(ValueError):
            project("/api/status", b"[]")


if __name__ == "__main__":
    unittest.main()
