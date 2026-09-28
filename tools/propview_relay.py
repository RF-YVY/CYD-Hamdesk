"""Expose two APRS PropView API reads to a CYD via a Tailscale-connected host.

The CYD talks to this relay on local Wi-Fi. The host running this script uses
its own Tailscale client to reach PropView's tailnet address. The relay accepts
only /api/status and /api/propagation and requires the CYD's device token.
"""

import argparse
import getpass
import hmac
import json
import os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib import error, parse, request

ALLOWED_PATHS = frozenset(("/api/status", "/api/propagation"))
MAX_UPSTREAM_RESPONSE = 2 * 1024 * 1024


def project(path: str, upstream_body: bytes) -> bytes:
    """Keep only fields the CYD renders, making responses small and predictable."""
    source = json.loads(upstream_body)
    if not isinstance(source, dict):
        raise ValueError("upstream did not return an object")
    if path == "/api/status":
        keys = ("station", "rf_connected", "aprs_is_connected")
        result = {key: source.get(key) for key in keys}
    else:
        keys = ("my_score", "my_level", "my_stations_1h", "score", "level", "regional_stations_1h")
        result = {key: source.get(key) for key in keys}
        event = source.get("event") or {}
        result["event"] = {"state": event.get("state", "normal")}
    return json.dumps(result, separators=(",", ":")).encode("utf-8")


def make_handler(upstream: str, token: str):
    """Return a request handler bound to one upstream and one access token."""

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self) -> None:
            if self.path == "/health":
                self._reply(200, b'{"status":"ok"}')
                return
            if parse.urlsplit(self.path).path not in ALLOWED_PATHS or "?" in self.path:
                self._reply(404, b'{"error":"not found"}')
                return
            supplied = self.headers.get("X-HamDesk-Token", "")
            if not hmac.compare_digest(supplied, token):
                self._reply(403, b'{"error":"forbidden"}')
                return
            try:
                req = request.Request(upstream + self.path, headers={"Accept": "application/json"})
                with request.urlopen(req, timeout=5) as response:
                    if response.status != 200:
                        self._reply(502, b'{"error":"upstream status"}')
                        return
                    body = response.read(MAX_UPSTREAM_RESPONSE + 1)
                if len(body) > MAX_UPSTREAM_RESPONSE:
                    self._reply(502, b'{"error":"upstream response too large"}')
                    return
                body = project(self.path, body)
            except (error.URLError, TimeoutError, ValueError, OSError) as exc:
                print(f"PropView fetch failed for {self.path}: {exc}")
                self._reply(502, b'{"error":"upstream unavailable"}')
                return
            self._reply(200, body)

        def _reply(self, status: int, body: bytes) -> None:
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, format: str, *args) -> None:
            print(f"{self.client_address[0]} - {format % args}")

    return Handler


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--upstream", required=True, help="PropView tailnet URL, e.g. http://100.x.y.z:14501")
    parser.add_argument("--bind", default="0.0.0.0", help="LAN address to listen on")
    parser.add_argument("--port", type=int, default=18401, help="LAN TCP port (default 18401)")
    args = parser.parse_args()
    parsed = parse.urlsplit(args.upstream)
    if parsed.scheme not in {"http", "https"} or not parsed.netloc or parsed.path not in {"", "/"} or parsed.query or parsed.fragment:
        parser.error("--upstream must be an http(s) origin with no path or query")
    token = os.getenv("HAMDESK_TOKEN") or getpass.getpass("CYD Mesh bridge token: ")
    if len(token) < 16:
        parser.error("HAMDESK_TOKEN must be the device token from the CYD setup page")
    server = ThreadingHTTPServer((args.bind, args.port), make_handler(args.upstream.rstrip("/"), token))
    print(f"Serving PropView on {args.bind}:{args.port}; upstream {args.upstream}")
    try:
        server.serve_forever(poll_interval=0.5)
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
