import http.client
import json
import os
import shutil
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import unittest

sys.path.insert(0, os.path.dirname(__file__))
import mock_ims as m  # noqa: E402

TOKEN = "unit-test-token-0123456789"
AUTH = {"Authorization": "Bearer " + TOKEN}


def start(cert=None, key=None):
    httpd, store = m.serve("127.0.0.1", 0, TOKEN, cert, key)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd, store


def call(httpd, method, path, body=None, headers=AUTH, tls=None, timeout=10):
    port = httpd.server_address[1]
    c = (http.client.HTTPSConnection("127.0.0.1", port, timeout=timeout, context=tls) if tls
         else http.client.HTTPConnection("127.0.0.1", port, timeout=timeout))
    c.request(method, path, body=None if body is None else json.dumps(body), headers=headers)
    r = c.getresponse()
    data = r.read()
    c.close()
    return r.status, (json.loads(data) if data else None), r


class Tests(unittest.TestCase):
    def setUp(self):
        self.httpd, self.store = start()

    def tearDown(self):
        self.httpd.shutdown()
        self.httpd.server_close()

    def emit(self, drawers, **kw):
        return call(self.httpd, "POST", "/mock/emit", dict(drawers=drawers, **kw))

    def test_latest_empty_is_seq0(self):
        st, ev, _ = call(self.httpd, "GET", "/locate/latest")
        self.assertEqual((st, ev["seq"], ev["drawers"]), (200, 0, []))

    def test_emit_then_latest_and_prefix(self):
        st, ev, _ = self.emit([{"zone": "B2", "room": "Cabinet B", "state": "green", "cells": ["2A-2D"]}])
        self.assertEqual((st, ev["seq"], ev["ttlMs"]), (200, 1, 15000))
        for path in ("/locate/latest", "/api/v1/locate/latest"):
            self.assertEqual(call(self.httpd, "GET", path)[1]["drawers"][0]["zone"], "B2")

    def test_next_returns_pending_event_immediately(self):
        self.emit([{"zone": "A4", "state": "red"}])
        t = time.monotonic()
        st, ev, _ = call(self.httpd, "GET", "/api/v1/locate/next?after=0&wait=25")
        self.assertEqual((st, ev["seq"]), (200, 1))
        self.assertLess(time.monotonic() - t, 1)

    def test_next_times_out_with_204(self):
        t = time.monotonic()
        st, body, _ = call(self.httpd, "GET", "/locate/next?after=0&wait=1")
        self.assertEqual((st, body), (204, None))
        self.assertGreaterEqual(time.monotonic() - t, 0.9)

    def test_long_poll_wakes_on_emit(self):
        threading.Timer(0.5, lambda: self.emit([{"zone": "A2", "state": "green"}])).start()
        t = time.monotonic()
        st, ev, _ = call(self.httpd, "GET", "/locate/next?after=0&wait=10")
        self.assertEqual((st, ev["seq"]), (200, 1))
        self.assertLess(time.monotonic() - t, 3)

    def test_next_skips_already_seen(self):
        self.emit([{"zone": "A1", "state": "green"}])
        self.emit([{"zone": "A2", "state": "red"}])
        self.assertEqual(call(self.httpd, "GET", "/locate/next?after=1&wait=1")[1]["seq"], 2)
        self.assertEqual(call(self.httpd, "GET", "/locate/next?after=2&wait=1")[0], 204)

    def test_empty_event_allowed(self):
        st, ev, _ = self.emit([])
        self.assertEqual((st, ev["drawers"]), (200, []))

    def test_auth_required_everywhere(self):
        for method, path in (("GET", "/locate/latest"), ("GET", "/locate/next?after=0&wait=0"),
                             ("POST", "/mock/emit")):
            for hdr in ({}, {"Authorization": "Bearer wrong"}, {"Authorization": TOKEN}):
                st, _, _ = call(self.httpd, method, path, {"drawers": []} if method == "POST" else None, hdr)
                self.assertEqual(st, 401, (method, path, hdr))
        self.assertEqual(self.store.latest()["seq"], 0)  # unauthorised emit changed nothing

    def test_emit_validation(self):
        bad = [
            {"drawers": "x"}, {}, {"drawers": [], "extra": 1}, {"drawers": [], "ttlMs": 5},
            {"drawers": [], "ttlMs": True}, {"drawers": ["A1"]},
            {"drawers": [{"zone": "A1", "state": "blue"}]},
            {"drawers": [{"zone": "", "state": "red"}]},
            {"drawers": [{"zone": "A1", "state": "red", "productName": "resistor"}]},
            {"drawers": [{"zone": "A1", "state": "red", "cells": "1A"}]},
        ]
        for b in bad:
            self.assertEqual(call(self.httpd, "POST", "/mock/emit", b)[0], 400, b)
        self.assertEqual(self.store.latest()["seq"], 0)

    def test_bad_query_and_routes(self):
        self.assertEqual(call(self.httpd, "GET", "/locate/next?after=abc")[0], 400)
        self.assertEqual(call(self.httpd, "GET", "/nope")[0], 404)
        self.assertEqual(call(self.httpd, "POST", "/locate/latest", {})[0], 404)

    def test_wait_is_capped(self):
        self.assertEqual(m.MAX_WAIT_S, 60)

    def test_main_requires_token(self):
        old = os.environ.pop("MOCK_TOKEN", None)
        try:
            self.assertEqual(m.main(["--port", "0"]), 1)
            os.environ["MOCK_TOKEN"] = "x" * 16
            self.assertEqual(m.main(["--cert", "only.crt"]), 1)
        finally:
            os.environ.pop("MOCK_TOKEN", None)
            if old is not None:
                os.environ["MOCK_TOKEN"] = old


@unittest.skipUnless(shutil.which("openssl"), "openssl not installed")
class TlsTest(unittest.TestCase):
    def test_https_with_self_signed_cert(self):
        with tempfile.TemporaryDirectory() as d:
            crt, key = os.path.join(d, "m.crt"), os.path.join(d, "m.key")
            subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                            "-subj", "/CN=localhost", "-addext", "subjectAltName=IP:127.0.0.1",
                            "-keyout", key, "-out", crt], check=True, capture_output=True)
            httpd, _ = start(crt, key)
            try:
                trusted = ssl.create_default_context(cafile=crt)
                st, ev, _ = call(httpd, "GET", "/locate/latest", tls=trusted)
                self.assertEqual((st, ev["seq"]), (200, 0))
                with self.assertRaises(ssl.SSLError):  # system CAs must not trust it
                    call(httpd, "GET", "/locate/latest", tls=ssl.create_default_context())
            finally:
                httpd.shutdown()
                httpd.server_close()


if __name__ == "__main__":
    unittest.main()
