#!/usr/bin/env python3
"""Mock IMS locate service for bench-testing the drawer-lights firmware. Stdlib only.

Serves the contract the board consumes (with or without the /api/v1 prefix):
  GET  /locate/latest                  newest event; 204 if none yet (as the real IMS does)
  GET  /locate/next?after=<seq>&wait=S the first event with seq > after; 204 after S seconds
  POST /mock/emit  {"drawers":[...], "ttlMs": 15000}   inject an event

Every endpoint needs "Authorization: Bearer $MOCK_TOKEN". The token is read from the
environment and is never printed. Generate a throwaway certificate with:
  openssl req -x509 -newkey rsa:2048 -nodes -days 30 -subj /CN=ims.siot.solutions \
      -addext subjectAltName=DNS:ims.siot.solutions,IP:<this-host-ip> \
      -keyout mock.key -out mock.crt
"""
import argparse
import hmac
import json
import os
import ssl
import sys
import threading
import time
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

MAX_WAIT_S = 60
DEFAULT_TTL_MS = 15000
DRAWER_KEYS = {"zone", "room", "state", "cells"}
STATES = {"green", "red"}
MAX_BODY = 8192


class EventStore:
    """Append-only event list plus a condition variable for long-polling."""

    def __init__(self):
        self._cv = threading.Condition()
        self._events = []

    def emit(self, drawers, ttl_ms):
        with self._cv:
            seq = (self._events[-1]["seq"] if self._events else 0) + 1
            ev = {"seq": seq, "ts": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                  "ttlMs": ttl_ms, "drawers": drawers}
            self._events.append(ev)
            self._cv.notify_all()
            return ev

    def latest(self):
        """Newest event, or None before the first emit (the real IMS answers 204 then)."""
        with self._cv:
            return self._events[-1] if self._events else None

    def next_after(self, after, wait_s):
        """First event with seq > after, waiting up to wait_s seconds; None on timeout."""
        deadline = time.monotonic() + wait_s
        with self._cv:
            while True:
                for ev in self._events:
                    if ev["seq"] > after:
                        return ev
                left = deadline - time.monotonic()
                if left <= 0:
                    return None
                self._cv.wait(left)


class ValidationError(Exception):
    pass


def validate_emit(body):
    if not isinstance(body, dict) or not isinstance(body.get("drawers"), list):
        raise ValidationError('expected {"drawers": [...]}')
    extra = set(body) - {"drawers", "ttlMs"}
    if extra:
        raise ValidationError("unknown field(s): " + ", ".join(sorted(extra)))
    ttl = body.get("ttlMs", DEFAULT_TTL_MS)
    if isinstance(ttl, bool) or not isinstance(ttl, int) or not 100 <= ttl <= 3600000:
        raise ValidationError("ttlMs must be an integer 100..3600000")
    drawers = []
    for d in body["drawers"]:
        if not isinstance(d, dict):
            raise ValidationError("each drawer must be an object")
        # The real feed carries no product names or ids; refuse to emit anything else.
        extra = set(d) - DRAWER_KEYS
        if extra:
            raise ValidationError("drawer has unknown field(s): " + ", ".join(sorted(extra)))
        if not isinstance(d.get("zone"), str) or not d["zone"]:
            raise ValidationError("drawer.zone must be a non-empty string")
        if d.get("state") not in STATES:
            raise ValidationError("drawer.state must be green or red")
        if "cells" in d and not (isinstance(d["cells"], list)
                                 and all(isinstance(c, str) for c in d["cells"])):
            raise ValidationError("drawer.cells must be a list of strings")
        if "room" in d and not isinstance(d["room"], str):
            raise ValidationError("drawer.room must be a string")
        drawers.append(d)
    return drawers, ttl


def make_handler(store, token):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"
        server_version = "mock-ims"

        def log_message(self, fmt, *args):  # status line only; never the Authorization header
            sys.stderr.write("%s %s\n" % (self.command, urlparse(self.path).path))

        def _send(self, code, obj=None):
            body = b"" if obj is None else json.dumps(obj).encode()
            self.send_response(code)
            if obj is not None:
                self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def _authorised(self):
            got = self.headers.get("Authorization", "")
            want = "Bearer " + token
            if hmac.compare_digest(got.encode(), want.encode()):
                return True
            self._send(401, {"error": "bad or missing bearer token"})
            return False

        @staticmethod
        def _route(path):
            return path[len("/api/v1"):] if path.startswith("/api/v1/") else path

        def do_GET(self):
            if not self._authorised():
                return
            url = urlparse(self.path)
            route = self._route(url.path)
            if route == "/locate/latest":
                ev = store.latest()
                return self._send(204) if ev is None else self._send(200, ev)
            if route == "/locate/next":
                q = parse_qs(url.query)
                try:
                    after = int(q.get("after", ["0"])[0])
                    wait = min(max(float(q.get("wait", ["25"])[0]), 0), MAX_WAIT_S)
                except ValueError:
                    return self._send(400, {"error": "after must be an integer, wait a number"})
                ev = store.next_after(after, wait)
                return self._send(204) if ev is None else self._send(200, ev)
            self._send(404, {"error": "not found"})

        def do_POST(self):
            if not self._authorised():
                return
            if self._route(urlparse(self.path).path) != "/mock/emit":
                return self._send(404, {"error": "not found"})
            try:
                n = int(self.headers.get("Content-Length", "0"))
            except ValueError:
                n = -1
            if not 0 < n <= MAX_BODY:
                return self._send(400, {"error": "body must be 1..%d bytes" % MAX_BODY})
            try:
                drawers, ttl = validate_emit(json.loads(self.rfile.read(n)))
            except (ValueError, ValidationError) as e:
                return self._send(400, {"error": str(e)})
            self._send(200, store.emit(drawers, ttl))

    return Handler


def serve(host, port, token, cert=None, key=None):
    store = EventStore()
    httpd = ThreadingHTTPServer((host, port), make_handler(store, token))
    httpd.daemon_threads = True
    if cert:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.minimum_version = ssl.TLSVersion.TLSv1_2
        ctx.load_cert_chain(cert, key)
        httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)
    return httpd, store


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    p.add_argument("--host", default="0.0.0.0")
    p.add_argument("--port", type=int, default=8443)
    p.add_argument("--cert", help="PEM certificate (enables HTTPS together with --key)")
    p.add_argument("--key", help="PEM private key")
    args = p.parse_args(argv)
    token = os.environ.get("MOCK_TOKEN", "")
    if len(token) < 16:
        print("error: set MOCK_TOKEN (at least 16 characters) in the environment", file=sys.stderr)
        return 1
    if bool(args.cert) != bool(args.key):
        print("error: --cert and --key must be given together", file=sys.stderr)
        return 1
    if not args.cert:
        print("warning: serving plain HTTP (no --cert/--key); the board needs HTTPS", file=sys.stderr)
    httpd, _ = serve(args.host, args.port, token, args.cert, args.key)
    print("mock IMS on %s://%s:%d" % ("https" if args.cert else "http", args.host, args.port),
          file=sys.stderr)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
