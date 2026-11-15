#!/usr/bin/env python3
"""Regression tests for webi's native HTTP layer (src/webi_lib.c).

Starts a real `kosh` webi server on a free port and attacks it over raw sockets:
response-header injection, over-long headers/cookies, binary static files,
request smuggling shapes (duplicate/odd Content-Length, Transfer-Encoding),
malformed request lines and headers, truncated bodies, and clients that
disconnect mid-response.  Every check here failed before v0.016.2.

  python3 tests/webi_http/test_http_hardening.py          (KOSH=path/to/kosh to override)
"""
import os, re, shutil, socket, subprocess, sys, tempfile, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
EXE = ".exe" if os.name == "nt" else ""
KOSH = os.environ.get("KOSH", os.path.join(ROOT, "kosh" + EXE))

passed = failed = 0
def check(name, ok, detail=""):
    global passed, failed
    if ok: passed += 1; print("  \u2713  PASS  " + name)
    else:
        failed += 1; print("  \u2717  FAIL  " + name)
        if detail: print("       -> " + str(detail)[:400])

APP = '''import "%(webi)s"

fn h_hdr(req):
    let r = res_text("ok")
    return res_with_header(r, "X-Custom", req["body_json"]["v"])

fn h_cookie(req):
    let r = res_text("ok")
    r = res_with_cookie(r, req["body_json"]["n"], req["body_json"]["v"])
    return res_with_header(r, "X-After", "after")

fn h_scookie(req):
    let r = res_text("ok")
    r = res_with_secure_cookie(r, req["body_json"]["n"], req["body_json"]["v"], {"max_age": 60})
    return res_with_header(r, "X-After", "after")

fn h_rawhdr(req):
    let r = res_text("ok")
    r["headers"]["X-Raw"] = "a\\r\\nSet-Cookie: pwned=1"
    r["headers"]["X-Fine"] = "fine"
    r["cookies"] = push(r["cookies"], "bad=1\\r\\nX-Evil: 1")
    return r

fn h_clen(req):
    let r = res_text("hello")
    r["headers"]["Content-Length"] = "999"
    r["headers"]["Transfer-Encoding"] = "chunked"
    r["headers"]["Connection"] = "keep-alive"
    return r

fn h_429(req):
    return res_status(429, "slow down")

fn h_len(req):
    return res_text("body-len=" + str(len(req["body"])) + " body=" + req["body"])

fn h_hello(req):
    return res_text("hello")

let app = webi_app()
app = route(app, "POST", "/hdr", h_hdr)
app = route(app, "POST", "/cookie", h_cookie)
app = route(app, "POST", "/scookie", h_scookie)
app = route(app, "GET", "/rawhdr", h_rawhdr)
app = route(app, "GET", "/clen", h_clen)
app = route(app, "GET", "/429", h_429)
app = route(app, "POST", "/len", h_len)
app = route(app, "GET", "/", h_hello)
app = serve_static(app, "/static", "%(static)s")
%(extra)s
%(run)s
'''

def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p

class Server:
    def __init__(self, extra="", run=None, env=None, wait=True):
        self.dir = tempfile.mkdtemp(prefix="webi_hard_")
        self.static = os.path.join(self.dir, "public"); os.makedirs(self.static)
        self.files = {
            "pic.png": bytes([0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0,0,0,0x0d,0x49,0x48,0x44,0x52,0,0,0,1,0xff,0xd8,0xff,0,0,7]) * 25,
            "zero.bin": b"\0" * 1000,
            "big.bin": bytes(range(256)) * 12000,          # ~3 MB, full of NULs
            "empty.png": b"",
            "page.html": b"<h1>hi</h1>\n",
            "data.json": b'{"a": 1}',
        }
        for n, d in self.files.items(): open(os.path.join(self.static, n), "wb").write(d)
        self.port = free_port()
        app = os.path.join(self.dir, "app.kh")
        webi = os.path.join(ROOT, "packages", "webi", "webi.kh").replace("\\", "/")
        run = (run or "webi_run(app, %(port)d)") % {"port": self.port}
        open(app, "w").write(APP % {"webi": webi, "static": self.static.replace("\\", "/"), "port": self.port, "extra": extra, "run": run})
        self.log = open(os.path.join(self.dir, "server.log"), "wb")
        self.p = subprocess.Popen([KOSH, app], stdout=self.log, stderr=subprocess.STDOUT, cwd=self.dir, env=dict(os.environ, **(env or {})))
        for _ in range(100 if wait else 0):
            try: socket.create_connection(("127.0.0.1", self.port), timeout=0.2).close(); break
            except OSError: time.sleep(0.1)
    def alive(self):
        if self.p.poll() is not None: return False
        try: r = self.get("/")
        except OSError: return False                 # connection refused: the server died
        return r["status"] == 200 and r["body"] == b"hello"
    def raw(self, data, pieces=None, shut=True, read=True):
        s = socket.create_connection(("127.0.0.1", self.port), timeout=10)
        if pieces:
            for piece in pieces: s.sendall(piece); time.sleep(0.05)
        else: s.sendall(data)
        if shut:
            try: s.shutdown(socket.SHUT_WR)       # "I am done sending": a short body is now a real EOF, not a stall
            except OSError: pass
        out = b""
        if read:
            try:
                while True:
                    c = s.recv(1 << 20)
                    if not c: break
                    out += c
            except OSError: pass
        s.close(); return out
    @staticmethod
    def parse(resp):
        head, _, body = resp.partition(b"\r\n\r\n")
        lines = head.split(b"\r\n")
        try: status = int(lines[0].split()[1])
        except Exception: status = 0
        hdrs = [l for l in lines[1:]]
        return {"status": status, "line": lines[0], "headers": hdrs, "body": body, "head": head}
    def get(self, path, extra=b""):
        return self.parse(self.raw(b"GET " + path.encode() + b" HTTP/1.1\r\nHost: x\r\n" + extra + b"Connection: close\r\n\r\n"))
    def post(self, path, body, ctype=b"application/json", extra=b""):
        return self.parse(self.raw(b"POST " + path.encode() + b" HTTP/1.1\r\nHost: x\r\nContent-Type: " + ctype +
                                   b"\r\nContent-Length: " + str(len(body)).encode() + b"\r\n" + extra + b"Connection: close\r\n\r\n" + body))
    def status_of(self, request_bytes): return self.parse(self.raw(request_bytes))["status"]
    def logtext(self):
        self.log.flush(); return open(os.path.join(self.dir, "server.log"), "rb").read().decode("utf-8", "replace")
    def stop(self):
        self.p.kill(); self.p.wait(); self.log.close(); shutil.rmtree(self.dir, ignore_errors=True)

def has_header(r, name):
    return any(h.lower().startswith(name.lower().encode() + b":") for h in r["headers"])

def trickle(port, first, per_byte_s, total_s):
    """Send `first` one byte at a time, like a slowloris client; return (response, seconds taken)."""
    s = socket.create_connection(("127.0.0.1", port), timeout=10)
    t0 = time.time(); out = b""
    try:
        for b in first:
            if time.time() - t0 > total_s: break
            s.sendall(bytes([b])); time.sleep(per_byte_s)
            s.settimeout(0.01)
            try:
                c = s.recv(65536)
                if c: out += c; break
            except OSError: pass
        s.settimeout(10)
        if not out:
            try: out = s.recv(65536)
            except OSError: pass
    except OSError: pass
    s.close(); return out, time.time() - t0

def linux_loopback_aliases():
    return sys.platform.startswith("linux")

def run_phase2():
    CONF = ('app = webi_limits(app, {"max_body_bytes": 1000, "max_header_bytes": 2048, "read_timeout_seconds": 2, "request_timeout_seconds": 3})\n'
            'app = webi_production(app, {})')
    S = Server(extra=CONF, run='webi_run_host(app, "127.0.0.1", %(port)d)')
    try:
        check("server with limits + production + host=127.0.0.1 starts", S.alive())
        check("startup line says it is bound to one address only", "bound address only" in S.logtext(), S.logtext())

        # ---------------------------------------------------- configurable limits
        P = b"POST /len HTTP/1.1\r\nHost: x\r\n"
        check("body within max_body_bytes (500) is accepted", S.parse(S.raw(P + b"Content-Length: 500\r\nConnection: close\r\n\r\n" + b"a" * 500))["body"].startswith(b"body-len=500"))
        check("Content-Length above max_body_bytes (1001 > 1000) -> 413", S.status_of(P + b"Content-Length: 1001\r\n\r\n") == 413)
        check("exactly max_body_bytes (1000) is accepted", S.parse(S.raw(P + b"Content-Length: 1000\r\nConnection: close\r\n\r\n" + b"a" * 1000))["status"] == 200)
        check("headers above max_header_bytes (2048) -> 431", S.status_of(b"GET / HTTP/1.1\r\nX-Big: " + b"a" * 3000 + b"\r\n\r\n") == 431)
        check("headers within max_header_bytes are accepted", S.status_of(b"GET / HTTP/1.1\r\nHost: x\r\nX-Big: " + b"a" * 1500 + b"\r\nConnection: close\r\n\r\n") == 200)

        # ---------------------------------------------------- timeouts (read 2 s, whole request 3 s)
        s = socket.create_connection(("127.0.0.1", S.port), timeout=10); t0 = time.time(); got = s.recv(1000); dt = time.time() - t0; s.close()
        check("an idle connection (sends nothing) is closed by the read timeout (~2 s) with 408", got.startswith(b"HTTP/1.1 408") and 1.5 < dt < 2.8, "%r after %.2fs (read timeout is 2 s; the whole-request limit is 3 s)" % (got[:30], dt))
        hdr = b"GET / HTTP/1.1\r\nHost: x\r\nX-Pad: " + b"a" * 200 + b"\r\n\r\n"
        out, dt = trickle(S.port, hdr, 0.25, 12)
        check("headers trickled one byte at a time are cut off at the whole-request limit (3 s), not after minutes",
              out.startswith(b"HTTP/1.1 408") and dt < 6.5, "%r after %.1fs" % (out[:30], dt))
        # fast trickle: data keeps arriving, so only the whole-request deadline (3 s) can stop it
        fast = b"GET / HTTP/1.1\r\nHost: x\r\nX-Pad: " + b"a" * 400 + b"\r\nConnection: close\r\n\r\n"     # ~440 bytes at 12 ms = ~5.3 s
        out, dt = trickle(S.port, fast, 0.012, 15)
        check("headers that keep arriving (one byte every 12 ms) are still cut off at 3 s with 408", out.startswith(b"HTTP/1.1 408") and dt < 4.2, "%r after %.1fs" % (out[:30], dt))
        s = socket.create_connection(("127.0.0.1", S.port), timeout=15)
        s.sendall(b"POST /len HTTP/1.1\r\nHost: x\r\nContent-Length: 900\r\nConnection: close\r\n\r\n")
        t0 = time.time(); out = b""
        try:
            for i in range(900):
                s.sendall(b"x"); time.sleep(0.012)
                s.settimeout(0.001)
                try:
                    c = s.recv(65536)
                    if c: out += c; break
                except OSError: pass
                s.settimeout(15)
        except OSError: pass
        s.settimeout(5)
        if not out:
            try: out = s.recv(65536)
            except OSError: pass
        dt = time.time() - t0; s.close()
        check("a body that keeps arriving (900 bytes at 12 ms = ~11 s) is cut off at 3 s with 408", out.startswith(b"HTTP/1.1 408") and dt < 4.2, "%r after %.1fs" % (out[:30], dt))
        body_hdr = b"POST /len HTTP/1.1\r\nHost: x\r\nContent-Length: 100\r\n\r\n"
        s = socket.create_connection(("127.0.0.1", S.port), timeout=10); s.sendall(body_hdr)
        t0 = time.time(); out = b""
        try:
            for i in range(60):
                s.sendall(b"x"); time.sleep(0.4)
                s.settimeout(0.01)
                try:
                    c = s.recv(65536)
                    if c: out += c; break
                except OSError: pass
                s.settimeout(10)
        except OSError: pass
        dt = time.time() - t0; s.close()
        check("a body trickled slowly is cut off at the whole-request limit with 408", out.startswith(b"HTTP/1.1 408") and dt < 6.5, "%r after %.1fs" % (out[:30], dt))
        check("server still answers normal requests after the slow clients", S.alive())

        # ---------------------------------------------------- production mode over the wire
        r = S.get("/")
        check("production: X-Content-Type-Options on a normal response", b"X-Content-Type-Options: nosniff" in r["head"], r["head"])
        check("production: X-Frame-Options and Referrer-Policy", b"X-Frame-Options: DENY" in r["head"] and b"Referrer-Policy: strict-origin-when-cross-origin" in r["head"])
        check("production: no HSTS over plain HTTP", not has_header(r, "Strict-Transport-Security"))
        r = S.get("/", extra=b"X-Forwarded-Proto: https\r\n")
        check("production: HSTS when a TLS proxy marks the request https", b"Strict-Transport-Security: max-age=15552000" in r["head"], r["head"])
        check("production: security headers on a 404", b"X-Content-Type-Options: nosniff" in S.get("/nope")["head"])
        check("production: security headers on static files", b"X-Content-Type-Options: nosniff" in S.get("/static/page.html")["head"])
        r = S.post("/cookie", b'{"n": "sid", "v": "abc"}')
        check("production: a plain cookie is sent Secure; HttpOnly; SameSite=Lax", b"Set-Cookie: sid=abc; Path=/; Secure; HttpOnly; SameSite=Lax" in r["head"], r["head"])
        r = S.post("/scookie", b'{"n": "sid", "v": "abc"}')
        check("production: a secure cookie keeps its flags without duplicates", r["head"].count(b"HttpOnly") == 1 and r["head"].count(b"Secure") == 1 and b"SameSite=Lax" in r["head"], r["head"])
        check("production: a lower-cased request header is matched case-insensitively", S.status_of(b"GET / HTTP/1.1\r\nhost: x\r\nx-forwarded-proto: https\r\nconnection: close\r\n\r\n") == 200)

        # ---------------------------------------------------- host binding
        if linux_loopback_aliases():
            try:
                socket.create_connection(("127.0.0.2", S.port), timeout=2).close(); reachable = True
            except OSError: reachable = False
            check("webi_run_host(app, '127.0.0.1', ...) is NOT reachable on another local address (127.0.0.2)", not reachable)
    finally:
        S.stop()

    # default host: every interface
    S = Server()
    try:
        if linux_loopback_aliases():
            try:
                socket.create_connection(("127.0.0.2", S.port), timeout=2).close(); reachable = True
            except OSError: reachable = False
            check("the default host still listens on every interface (reachable on 127.0.0.2)", reachable)
        check("startup line says all interfaces", "all interfaces" in S.logtext(), S.logtext())
        check("no security headers by default (existing apps unchanged)", not has_header(S.get("/"), "X-Content-Type-Options"))
        r = S.post("/cookie", b'{"n": "sid", "v": "abc"}')
        check("no cookie flags added by default", b"Set-Cookie: sid=abc; Path=/\r\n" in r["head"] + b"\r\n", r["head"])
    finally:
        S.stop()

    # bad configuration must stop the server from starting, with a message
    def refuses(name, extra, run, needle):
        X = Server(extra=extra, run=run, wait=False)
        try:
            try: X.p.wait(timeout=8); code = X.p.returncode
            except subprocess.TimeoutExpired: code = None
            txt = X.logtext()
            check(name, code is not None and needle in txt and "Listening on port" not in txt, "exit=%s log=%r" % (code, txt[-300:]))
        finally: X.stop()
    refuses("a host that is not an IPv4 address stops the server", "", 'webi_run_host(app, "not-an-ip", %(port)d)', "cannot bind to host")
    refuses("a negative limit stops the server", 'app = webi_limits(app, {"max_body_bytes": -5})', None, "max_body_bytes")
    refuses("a limit of the wrong type stops the server", 'app = webi_limits(app, {"request_timeout_seconds": "fast"})', None, "request_timeout_seconds")
    refuses("an out-of-range timeout stops the server", 'app = webi_limits(app, {"read_timeout_seconds": 100000})', None, "read_timeout_seconds")

def run():
    S = Server()
    try:
        check("server starts and answers", S.alive())

        # ---------------------------------------------------- response header / cookie injection
        r = S.post("/hdr", b'{"v": "a\\r\\nSet-Cookie: pwned=1"}')
        check("CR/LF in a header value is refused (res_with_header)", r["status"] == 200 and not has_header(r, "Set-Cookie") and not has_header(r, "X-Custom"), r["head"])
        check("... and the response is still well formed", r["body"] == b"ok" and b"Content-Length: 2" in r["head"], r)
        r = S.post("/hdr", b'{"v": "plain value"}')
        check("a normal custom header still works", has_header(r, "X-Custom") and b"X-Custom: plain value" in r["head"], r["head"])
        r = S.post("/cookie", b'{"n": "sid", "v": "abc\\r\\nSet-Cookie: pwned=1"}')
        check("CR/LF in a cookie value is refused", not has_header(r, "Set-Cookie") and has_header(r, "X-After") and r["body"] == b"ok", r["head"])
        r = S.post("/cookie", b'{"n": "sid", "v": "abc; Domain=evil.example"}')
        check("';' in a cookie value (attribute injection) is refused", not has_header(r, "Set-Cookie"), r["head"])
        r = S.post("/cookie", b'{"n": "si d", "v": "x"}')
        check("a cookie name with a space is refused", not has_header(r, "Set-Cookie"), r["head"])
        r = S.post("/cookie", b'{"n": "sid", "v": "token123"}')
        check("a normal cookie still works", b"Set-Cookie: sid=token123; Path=/" in r["head"], r["head"])
        r = S.post("/scookie", b'{"n": "sid", "v": "t\\nSet-Cookie: x=1"}')
        check("CR/LF in a secure cookie is refused", not has_header(r, "Set-Cookie") and r["body"] == b"ok", r["head"])
        r = S.post("/scookie", b'{"n": "sid", "v": "token123"}')
        check("a normal secure cookie still works", b"HttpOnly" in r["head"] and b"SameSite=Lax" in r["head"] and b"Max-Age=60" in r["head"], r["head"])

        r = S.get("/rawhdr")
        check("native layer drops a header with CR/LF even when Kosh code bypassed the helper",
              not has_header(r, "X-Raw") and not has_header(r, "Set-Cookie") and not has_header(r, "X-Evil") and has_header(r, "X-Fine"), r["head"])
        check("... and the body is intact", r["body"] == b"ok", r)
        r = S.get("/clen")
        check("handlers cannot override Content-Length / Transfer-Encoding / Connection",
              b"Content-Length: 5" in r["head"] and b"999" not in r["head"] and not has_header(r, "Transfer-Encoding") and b"keep-alive" not in r["head"] and r["body"] == b"hello", r)
        check("status 429 gets its own reason phrase", S.get("/429")["line"].startswith(b"HTTP/1.1 429 Too Many Requests"), S.get("/429")["line"])

        # ---------------------------------------------------- long headers and cookies (used to be cut at 255 bytes)
        for size in (300, 1000, 5000):
            val = ("A" * size).encode()
            r = S.post("/cookie", b'{"n": "sid", "v": "' + val + b'"}')
            check("a %d-byte cookie is sent in full, and the body is not corrupted" % size,
                  (b"Set-Cookie: sid=" + val) in r["head"] and has_header(r, "X-After") and r["body"] == b"ok" and b"Content-Length: 2" in r["head"], r["head"][-120:])
        r = S.post("/hdr", b'{"v": "' + b"B" * 4000 + b'"}')
        check("a 4000-byte header value is sent in full", (b"X-Custom: " + b"B" * 4000) in r["head"] and r["body"] == b"ok")

        # ---------------------------------------------------- binary static files
        for name, data in S.files.items():
            r = S.get("/static/" + name)
            check("static %s is byte-identical (%d bytes)" % (name, len(data)), r["status"] == 200 and r["body"] == data, "status %s, got %d bytes" % (r["status"], len(r["body"])))
            m = re.search(rb"Content-Length: (\d+)", r["head"])
            check("static %s Content-Length matches the file" % name, m is not None and int(m.group(1)) == len(data), r["head"])
        check("PNG gets image/png", b"Content-Type: image/png" in S.get("/static/pic.png")["head"])
        check("HTML keeps its text type", b"text/html" in S.get("/static/page.html")["head"])
        for bad in ("/static/../app.kh", "/static/%2e%2e%2fapp.kh", "/static/..%2fapp.kh", "/static/nope.png", "/static/"):
            check("static still refuses %s" % bad, S.get(bad)["status"] == 404, S.get(bad)["line"])

        # ---------------------------------------------------- a client that vanishes must not kill the server
        for i in range(6):          # ask for 3 MB, then hang up without reading: the server's later writes hit a dead socket
            s = socket.create_connection(("127.0.0.1", S.port)); s.sendall(b"GET /static/big.bin HTTP/1.1\r\nHost: x\r\n\r\n"); s.close()
        s = socket.create_connection(("127.0.0.1", S.port)); s.sendall(b"GET /static/big.bin HTTP/1.1\r\nHost: x\r\n\r\n"); s.recv(10)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, b"\x01\x00\x00\x00\x00\x00\x00\x00"); s.close()      # RST mid-response
        time.sleep(0.5)
        check("server survives clients that disconnect mid-response (no SIGPIPE crash)", S.alive())

        # ---------------------------------------------------- normal requests still fine
        body = b'{"k": "v"}'
        r = S.post("/len", body)
        check("POST with a body works", r["body"] == b"body-len=10 body=" + body, r["body"])
        req = b"POST /len HTTP/1.1\r\nHost: x\r\nContent-Length: 11\r\nConnection: close\r\n\r\n"
        r = S.parse(S.raw(None, pieces=[req[:20], req[20:], b"hello", b" world"]))
        check("headers and body split over several packets are reassembled", r["body"] == b"body-len=11 body=hello world", r["body"])
        r = S.parse(S.raw(None, pieces=[b"G", b"ET / HT", b"TP/1.1\r\nHo", b"st: x\r\n\r", b"\n"]))
        check("a request dribbled in tiny pieces works", r["status"] == 200 and r["body"] == b"hello")
        r = S.parse(S.raw(b"POST /len HTTP/1.1\r\nHost: x\r\nCONTENT-LENGTH: 5\r\nConnection: close\r\n\r\nhello"))
        check("header names are case-insensitive (CONTENT-LENGTH)", r["body"] == b"body-len=5 body=hello", r["body"])
        r = S.parse(S.raw(b"POST /len HTTP/1.1\r\nHost: x\r\nX-Content-Length: 3\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello"))
        check("X-Content-Length is not mistaken for Content-Length", r["body"] == b"body-len=5 body=hello", r["body"])
        r = S.parse(S.raw(b"POST /len HTTP/1.1\r\nHost: x\r\nContent-Length:   5  \r\nConnection: close\r\n\r\nhello"))
        check("spaces around the Content-Length value are fine", r["body"] == b"body-len=5 body=hello", r["body"])
        r = S.parse(S.raw(b"POST /len HTTP/1.0\r\nContent-Length: 2\r\n\r\nhi"))
        check("HTTP/1.0 request works", r["body"] == b"body-len=2 body=hi", r["body"])

        # ---------------------------------------------------- request smuggling shapes
        def refused(name, req, want):
            st = S.status_of(req)
            check(name + " -> %d" % want, st == want, "got %s" % st)
        P = b"POST /len HTTP/1.1\r\nHost: x\r\n"
        refused("duplicate Content-Length (5 and 12)", P + b"Content-Length: 5\r\nContent-Length: 12\r\n\r\nhello world!", 400)
        refused("duplicate identical Content-Length", P + b"Content-Length: 5\r\nContent-Length: 5\r\n\r\nhello", 400)
        refused("Content-Length with letters", P + b"Content-Length: 12abc\r\n\r\nhello world!", 400)
        refused("negative Content-Length", P + b"Content-Length: -1\r\n\r\n", 400)
        refused("'+5' Content-Length", P + b"Content-Length: +5\r\n\r\nhello", 400)
        refused("hex Content-Length", P + b"Content-Length: 0x10\r\n\r\n", 400)
        refused("empty Content-Length", P + b"Content-Length:\r\n\r\n", 400)
        refused("20-digit Content-Length", P + b"Content-Length: 99999999999999999999\r\n\r\n", 400)
        refused("Content-Length above the 10 MB limit", P + b"Content-Length: 20000000\r\n\r\n", 413)
        refused("Transfer-Encoding together with Content-Length", P + b"Transfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\nhello", 400)
        refused("Transfer-Encoding: chunked on its own (not implemented)", P + b"Transfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n", 501)
        refused("Transfer-Encoding with odd case", P + b"tRaNsFeR-eNcOdInG: chunked\r\n\r\n0\r\n\r\n", 501)

        # ---------------------------------------------------- malformed requests
        H = b"Host: x\r\n"
        refused("obsolete header folding", b"GET / HTTP/1.1\r\nX-A: 1\r\n  X-Smuggled: yes\r\n" + H + b"\r\n", 400)
        refused("header line without a colon", b"GET / HTTP/1.1\r\nNoColonHere\r\n" + H + b"\r\n", 400)
        refused("space before the colon in a header name", b"GET / HTTP/1.1\r\nX-A : 1\r\n" + H + b"\r\n", 400)
        refused("bare LF inside a header value", b"GET / HTTP/1.1\r\nX-A: a\nb: c\r\n" + H + b"\r\n", 400)
        refused("bare CR inside a header value", b"GET / HTTP/1.1\r\nX-A: a\rb\r\n" + H + b"\r\n", 400)
        refused("NUL byte in a header", b"GET / HTTP/1.1\r\nX-A: a\x00b\r\n" + H + b"\r\n", 400)
        refused("control character in a header", b"GET / HTTP/1.1\r\nX-A: a\x01b\r\n" + H + b"\r\n", 400)
        refused("empty header name", b"GET / HTTP/1.1\r\n: value\r\n" + H + b"\r\n", 400)
        refused("more than 100 header lines", b"GET / HTTP/1.1\r\n" + b"".join(b"X-%d: v\r\n" % i for i in range(150)) + b"\r\n", 431)
        refused("header section above 64 KB", b"GET / HTTP/1.1\r\nX-Big: " + b"a" * 70000 + b"\r\n\r\n", 431)
        refused("request line without a version", b"GET /\r\n" + H + b"\r\n", 400)
        refused("unsupported HTTP version", b"GET / HTTP/2.0\r\n" + H + b"\r\n", 400)
        refused("lower-case http version", b"GET / http/1.1\r\n" + H + b"\r\n", 400)
        refused("extra space in the request line", b"GET  / HTTP/1.1\r\n" + H + b"\r\n", 400)
        refused("target that does not start with '/'", b"GET foo HTTP/1.1\r\n" + H + b"\r\n", 400)
        refused("absolute-form target", b"GET http://x/ HTTP/1.1\r\n" + H + b"\r\n", 400)
        refused("URI longer than 511 bytes", b"GET /" + b"a" * 600 + b" HTTP/1.1\r\n" + H + b"\r\n", 414)
        refused("control character in the target", b"GET /a\x01b HTTP/1.1\r\n" + H + b"\r\n", 400)
        refused("NUL in a request body", P + b"Content-Length: 8\r\n\r\nab\x00cdefg", 400)
        refused("body shorter than Content-Length", P + b"Content-Length: 20\r\n\r\nshort", 400)
        check("a request with extra bytes after the body only uses Content-Length bytes",
              S.parse(S.raw(P + b"Content-Length: 5\r\nConnection: close\r\n\r\nhelloEXTRA"))["body"] == b"body-len=5 body=hello")
        check("connecting and leaving without sending anything is harmless", S.raw(b"", read=True) == b"")
        S.raw(b"GET / HTT", read=False)
        check("a half-sent request line followed by a disconnect is harmless", S.alive())

        check("server log has no crash", S.p.poll() is None)
        check("server is still healthy after all of the above", S.alive())
    finally:
        S.stop()
    run_phase2()
    print("\n  %d/%d passed   |   %d failed" % (passed, passed + failed, failed))
    return 1 if failed else 0

if __name__ == "__main__":
    if not os.path.exists(KOSH): sys.exit("kosh binary not found: %s (build it with `make`)" % KOSH)
    sys.exit(run())
