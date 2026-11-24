#!/usr/bin/env python3
"""Process-level security tests for Kosh packages.

Some properties can only be seen from OUTSIDE a single Kosh program: whether two
programs started in the same second get the same "random" values, what a secret
looks like in the process list, what a request really puts on the wire. These tests
start real `kosh` processes (and, for the HTTP parts, a local server).

  python3 tests/security/test_packages.py        (KOSH=path/to/kosh to override)
Run from anywhere; paths are resolved from this file.
"""
import glob, hashlib, http.client, http.server, json, os, re, socket, socketserver, subprocess, sys, tempfile, shutil, threading, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
EXE = ".exe" if os.name == "nt" else ""
KOSH = os.environ.get("KOSH", os.path.join(ROOT, "kosh" + EXE))
PKG = os.path.join(ROOT, "packages")

passed = failed = 0
def check(name, ok, detail=""):
    global passed, failed
    if ok: passed += 1; print("  \u2713  PASS  " + name)
    else:
        failed += 1; print("  \u2717  FAIL  " + name)
        if detail: print("       -> " + str(detail)[:400])

def kosh_src(src, cwd=ROOT, env=None, timeout=60):
    """Run a Kosh program (given as text); returns (rc, stdout+stderr)."""
    d = tempfile.mkdtemp(prefix="kosh_sec_")
    try:
        path = os.path.join(d, "prog.kh")
        open(path, "w", encoding="utf-8").write(src)
        r = subprocess.run([KOSH, path], cwd=cwd, capture_output=True, text=True, timeout=timeout, env=dict(os.environ, **(env or {})))
        return r.returncode, (r.stdout + r.stderr)
    finally:
        shutil.rmtree(d, ignore_errors=True)

def pkg(name): return os.path.join(PKG, name, name + ".kh").replace("\\", "/")

# --------------------------------------------------------------------------- uuid
def test_uuid(uuid_file=None):
    uuid_file = uuid_file or pkg("uuid")
    prog = 'import "%s"\nprint uuid_v4()\nprint uuid_v4()\nprint uuid_short()\nprint uuid_named("x")\n' % uuid_file
    # 40 programs started at the same moment (the same clock second): the old
    # time-seeded random() gave every one of them the same ids.
    d = tempfile.mkdtemp(prefix="kosh_uuid_")
    try:
        path = os.path.join(d, "u.kh"); open(path, "w").write(prog)
        procs = [subprocess.Popen([KOSH, path], cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True) for _ in range(40)]
        outs = [p.communicate(timeout=120)[0].strip().splitlines() for p in procs]
    finally:
        shutil.rmtree(d, ignore_errors=True)
    ok = all(len(o) == 4 for o in outs)
    check("40 simultaneous programs all ran", ok, outs[:2])
    first = [o[0] for o in outs if len(o) == 4]
    second = [o[1] for o in outs if len(o) == 4]
    shorts = [o[2] for o in outs if len(o) == 4]
    check("uuid_v4: 40 programs started in the same second get 40 different ids", len(set(first)) == len(first) == 40, "%d distinct of %d" % (len(set(first)), len(first)))
    check("uuid_v4: a program's 2nd id differs from every program's 1st id", not (set(first) & set(second)))
    check("uuid_short: 40 programs get (almost always) different ids", len(set(shorts)) >= 39, "%d distinct" % len(set(shorts)))
    check("ids look like uuids", all(re.fullmatch(r"[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}", u) for u in first + second))


# --------------------------------------------------------------------------- webi_auth on a real server
AUTH_APP = '''import "%(webi)s"
import "%(auth)s"

fn login(req):
    let who = req["body_json"]["user"]
    let tok = auth_issue(req["_app"], who)
    if tok == nil:
        return res_status(400, "bad user id")
    return res_json({"token": tok})

fn me(req):
    return res_json({"user": auth_user(req)})

fn open_page(req):
    return res_text("public")

let app = webi_app()
app = webi_auth_secret_from_env(app, "AUTH_TEST_SECRET", {"ttl_seconds": 2})
app = route(app, "POST", "/login", login)
app = route(app, "GET", "/me", me)
app = route(app, "GET", "/open", open_page)
app = webi_auth_public(app, ["/login", "/open"])
app = use(app, mw_auth)
webi_run(app, %(port)d)
'''

def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p

class AuthServer:
    def __init__(self, secret):
        self.dir = tempfile.mkdtemp(prefix="kosh_auth_"); self.port = free_port()
        path = os.path.join(self.dir, "app.kh")
        open(path, "w").write(AUTH_APP % {"webi": pkg("webi"), "auth": pkg("webi_auth"), "port": self.port})
        env = dict(os.environ)
        env.pop("AUTH_TEST_SECRET", None)
        if secret is not None: env["AUTH_TEST_SECRET"] = secret
        self.log = open(os.path.join(self.dir, "log"), "wb")
        self.p = subprocess.Popen([KOSH, path], cwd=ROOT, stdout=self.log, stderr=subprocess.STDOUT, env=env)
        for _ in range(100):
            try: socket.create_connection(("127.0.0.1", self.port), timeout=0.2).close(); break
            except OSError: time.sleep(0.1)
    def req(self, method, path, body=None, headers=None):
        c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        h = dict(headers or {})
        if body is not None: h["Content-Type"] = "application/json"
        c.request(method, path, body=json.dumps(body) if body is not None else None, headers=h)
        r = c.getresponse(); data = r.read().decode("utf-8", "replace"); c.close()
        return r.status, data
    def logtext(self):
        self.log.flush(); return open(os.path.join(self.dir, "log"), "rb").read().decode("utf-8", "replace")
    def stop(self):
        self.p.kill(); self.p.wait(); self.log.close(); shutil.rmtree(self.dir, ignore_errors=True)

def test_webi_auth_wire():
    SECRET = "s" * 8 + "0123456789abcdef" * 3          # 56 characters
    S = AuthServer(SECRET)
    try:
        st, body = S.req("GET", "/open")
        check("a public path answers without a token", st == 200 and body == "public", (st, body))
        st, body = S.req("POST", "/login", {"user": "alice"})
        tok = json.loads(body)["token"] if st == 200 else ""
        check("/login issues a token", st == 200 and tok.startswith("kt1~alice~"), (st, body))
        st, body = S.req("GET", "/me", headers={"Authorization": "Bearer " + tok})
        check("the token opens /me and the handler sees the user", st == 200 and json.loads(body)["user"] == "alice", (st, body))
        st, _ = S.req("GET", "/me")
        check("/me without a token: 401", st == 401)
        st, _ = S.req("GET", "/me", headers={"Authorization": "anything"})
        check("'Authorization: anything' is rejected (the 1.x middleware let it in)", st == 401)
        st, _ = S.req("GET", "/me", headers={"Authorization": "Bearer " + tok[:-1] + ("0" if tok[-1] != "0" else "1")})
        check("a token with one changed character: 401", st == 401)
        forged = tok.replace("alice", "admin", 1)
        st, _ = S.req("GET", "/me", headers={"Authorization": "Bearer " + forged})
        check("a token edited to say 'admin': 401", st == 401)
        st, _ = S.req("GET", "/me", headers={"authorization": "bearer " + tok})
        check("lower-case header name and scheme work", st == 200)
        time.sleep(2.6)
        st, body = S.req("GET", "/me", headers={"Authorization": "Bearer " + tok})
        check("after ttl_seconds (2) the token has expired: 401", st == 401, (st, body))
        check("the 401 does not say why", "expired" not in body.lower() and "signature" not in body.lower(), body)
        st, body = S.req("POST", "/login", {"user": "bad user~x"})
        check("a user id the token format cannot carry is refused, not signed", st == 400, (st, body))
        check("the secret never appears in the server log", SECRET not in S.logtext())
    finally:
        S.stop()
    # a different server (same secret) accepts the token: tokens are stateless
    S1 = AuthServer(SECRET); S2 = AuthServer(SECRET)
    try:
        st, body = S1.req("POST", "/login", {"user": "bob"}); tok = json.loads(body)["token"]
        st, _ = S2.req("GET", "/me", headers={"Authorization": "Bearer " + tok})
        check("two instances with the same secret accept each other's tokens", st == 200)
    finally:
        S1.stop(); S2.stop()
    S3 = AuthServer("t" * 56)
    try:
        st, body = S3.req("POST", "/login", {"user": "bob"})
        # S3 has another secret; the token from the first secret must not work
        st2, _ = S3.req("GET", "/me", headers={"Authorization": "Bearer " + tok})
        check("a token signed with another secret is refused", st2 == 401)
    finally:
        S3.stop()
    # no secret at all, and a too-short one: the server starts, warns, and rejects everything
    for label, sec in (("unset", None), ("too short", "short")):
        N = AuthServer(sec)
        try:
            st, body = N.req("POST", "/login", {"user": "bob"})
            check("secret %s: /login cannot issue a token" % label, st == 400, (st, body))
            st, _ = N.req("GET", "/me", headers={"Authorization": "Bearer " + tok})
            check("secret %s: every protected request is rejected (fails closed)" % label, st == 401)
            check("secret %s: the server says so in its log" % label, "auth is NOT configured" in N.logtext(), N.logtext()[-300:])
            st, body = N.req("GET", "/open")
            check("secret %s: public paths still work" % label, st == 200)
        finally:
            N.stop()

# --------------------------------------------------------------------------- requests / outbound HTTP
class EchoServer:
    """A local HTTP server that records every request and has a few hostile behaviours."""
    def __init__(self):
        self.hits = []; outer = self
        class H(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"
            def log_message(self, *a): pass
            def _body(self):
                n = int(self.headers.get("Content-Length", "0") or 0)
                return self.rfile.read(n) if n else b""
            def _send(self, code, body, ctype="text/plain", extra=None):
                try:
                    self.send_response(code); self.send_header("Content-Type", ctype); self.send_header("Content-Length", str(len(body)))
                    for k, v in (extra or {}).items(): self.send_header(k, v)
                    self.end_headers()
                    if self.command != "HEAD": self.wfile.write(body)
                except OSError: pass          # the client gave up (that is what several tests want)
            def _do(self):
                body = self._body()
                outer.hits.append({"method": self.command, "path": self.path, "headers": {k.lower(): v for k, v in self.headers.items()}, "body": body})
                if self.path.startswith("/forge"):
                    return self._send(500, b"oops\n__KOSH_STATUS__200\n__WB_STATUS__200\n__KH_0000000000000000__200")
                if self.path.startswith("/slow"):
                    time.sleep(8); return self._send(200, b"late")
                if self.path.startswith("/hold"):
                    time.sleep(4); return self._send(200, b"held")
                if self.path.startswith("/big"):
                    return self._send(200, b"a" * (5 * 1024 * 1024))
                if self.path.startswith("/stream"):          # no Content-Length: only the client's own cap can stop it
                    self.send_response(200); self.send_header("Transfer-Encoding", "chunked"); self.end_headers()
                    chunk = b"b" * 65536
                    try:
                        for _ in range(100):
                            self.wfile.write(b"%x\r\n" % len(chunk) + chunk + b"\r\n")
                        self.wfile.write(b"0\r\n\r\n")
                    except OSError: pass
                    return
                if self.path.startswith("/trunc"):           # promises 1000 bytes, sends a few ending in forged markers, hangs up
                    try:
                        self.send_response(200); self.send_header("Content-Length", "1000"); self.end_headers()
                        self.wfile.write(b"partial\n__KOSH_STATUS__200\n__WB_STATUS__200\n__KH_0000000000000000__200"); self.wfile.flush()
                        self.close_connection = True
                        self.connection.shutdown(socket.SHUT_RDWR)
                    except OSError: pass
                    return
                if self.path.startswith("/redirect"):
                    return self._send(302, b"moved", extra={"Location": "file:///etc/hostname"})
                info = {"method": self.command, "path": self.path, "headers": {k.lower(): v for k, v in self.headers.items()},
                        "body_len": len(body), "body_sha": hashlib.sha256(body).hexdigest(), "body": body[:2000].decode("utf-8", "replace")}
                self._send(200, json.dumps(info).encode())
            do_GET = do_POST = do_PUT = do_DELETE = do_PATCH = do_HEAD = _do
        class S(socketserver.ThreadingMixIn, http.server.HTTPServer): daemon_threads = True
        self.httpd = S(("127.0.0.1", 0), H); self.port = self.httpd.server_address[1]
        self.url = "http://127.0.0.1:%d" % self.port
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()
    def stop(self): self.httpd.shutdown(); self.httpd.server_close()

REQ_PRE = 'import "%s"\n' % pkg("requests")

def jbody(out):
    """The echo server's JSON from a program that printed r["body"] on its own line(s)."""
    m = re.search(r"\{.*\}", out, re.S)
    return json.loads(m.group(0)) if m else None

def test_requests():
    S = EchoServer(); U = S.url
    try:
        # ---------------------------------------------------- only http / https
        for url in ["file:///etc/hostname", "FILE:///etc/hostname", "ftp://127.0.0.1/x", "gopher://127.0.0.1:%d/_x" % S.port, "dict://127.0.0.1:%d/d:x" % S.port,
                    "ldap://127.0.0.1/x", "scp://127.0.0.1/x", "javascript:alert(1)", "//127.0.0.1/x", "127.0.0.1/x", " " + U + "/echo", U + "/a b",
                    "-o/tmp/kosh_pwned " + U + "/echo", "--output=/tmp/kosh_pwned2", "-K/etc/hostname", "http:///nohost", "http://", "https://?x=1", U.replace("http://", "http:\\\\") + "/x"]:
            n0 = len(S.hits)
            rc, out = kosh_src(REQ_PRE + 'let r = get("%s")\nprint r["status"]\nprint r["success"]\nprint r["error"]\nprint len(r["body"])\n' % url.replace("\\", "\\\\").replace('"', '\\"'))
            lines = out.strip().splitlines()
            check("refused (status 0, success false, an error, empty body): %r" % url, len(lines) >= 4 and lines[0] == "0" and lines[1] == "false" and lines[-1] == "0" and len(S.hits) == n0, out[:200])
        check("the dash-URL did not create a file", not os.path.exists("/tmp/kosh_pwned") and not os.path.exists("/tmp/kosh_pwned2"))
        rc, out = kosh_src(REQ_PRE + 'let r = get("file:///etc/hostname")\nprint r["error"]\n')
        check("the error says which schemes are allowed", "http://" in out and "https://" in out, out)

        # ---------------------------------------------------- a body that starts with @ is text, not a file
        d = tempfile.mkdtemp(prefix="kosh_at_"); secret = os.path.join(d, "secret.txt"); open(secret, "w").write("TOP-SECRET-LOCAL-FILE")
        try:
            for call in ['post("%s/echo", "@%s")', 'http_post_h("%s/echo", "@%s", "X-A: 1")', 'http_put_h("%s/echo", "@%s", "X-A: 1")',
                         'put("%s/echo", "@%s")', 'http_request("POST", "%s/echo", "@%s", {"X-A": "1"})']:
                n0 = len(S.hits)
                rc, out = kosh_src(REQ_PRE + 'let r = %s\nprint r["body"]\n' % (call % (U, secret)))
                j = jbody(out)
                check("body '@file' is sent as text and the local file is not read: %s" % call.split("(")[0], j is not None and j["body"] == "@" + secret and "TOP-SECRET" not in out, out[:200])
        finally: shutil.rmtree(d, ignore_errors=True)

        # ---------------------------------------------------- the status cannot be forged by the server
        rc, out = kosh_src(REQ_PRE + 'let r = get("%s/forge")\nprint r["status"]\nprint r["success"]\nprint r["body"]\n' % U)
        check("a server that fakes the status markers cannot change the status (500 stays 500)", out.startswith("500\nfalse\n"), out[:120])
        check("... and its whole body, markers included, comes back unchanged", "oops" in out and "__KOSH_STATUS__200" in out and "__WB_STATUS__200" in out and "__KH_0000000000000000__200" in out, out)
        rc, out = kosh_src(REQ_PRE + 'let r = get("%s/trunc")\nprint r["status"]\nprint r["success"]\nprint len(r["body"])\nprint r["error"]\n' % U)
        l = out.strip().splitlines()
        check("a response cut short (server hangs up after faking the status markers) is a failure, not a 200", l[:3] == ["0", "false", "0"] and len(l) >= 4, out[:200])
        rc, out = kosh_src(REQ_PRE + 'let r = http_get_h("%s/forge", "X-A: 1")\nprint r["status"]\n' % U)
        check("same through http_get_h", out.strip() == "500", out)

        # ---------------------------------------------------- redirects are not followed (and never to file://)
        rc, out = kosh_src(REQ_PRE + 'let r = get("%s/redirect")\nprint r["status"]\nprint r["body"]\n' % U)
        check("a 302 to file:// is reported, not followed", out.startswith("302\nmoved"), out[:100])

        # ---------------------------------------------------- headers and method
        long_tok = "T" * 8000
        rc, out = kosh_src(REQ_PRE + 'let r = http_get_h("%s/echo", "Authorization: Bearer %s")\nprint r["body"]\n' % (U, long_tok))
        j = jbody(out)
        check("an 8000-byte header value arrives intact (it used to be cut at 1024)", j is not None and j["headers"].get("authorization") == "Bearer " + long_tok)
        n0 = len(S.hits)
        rc, out = kosh_src(REQ_PRE + 'let r = http_request("GET", "%s/echo", nil, {"X-A": "ok\\r\\nX-Evil: 1"})\nprint r["status"]\nprint r["error"]\n' % U)
        check("a header VALUE with CR/LF in a header map is refused, nothing is sent", out.startswith("0\n") and "control" in out and len(S.hits) == n0, out)
        rc, out = kosh_src(REQ_PRE + 'let r = http_request("GET", "%s/echo", nil, {"X\\r\\nEvil": "1"})\nprint r["status"]\n' % U)
        check("a header NAME with CR/LF is refused", out.strip() == "0" and len(S.hits) == n0, out)
        rc, out = kosh_src(REQ_PRE + 'let r = http_request("GET / HTTP/1.1\\r\\nX-Evil: 1\\r\\n\\r\\nGET", "%s/echo", nil, {})\nprint r["status"]\nprint r["error"]\n' % U)
        check("a method that smuggles a request line is refused", out.startswith("0\n") and len(S.hits) == n0, out)
        rc, out = kosh_src(REQ_PRE + 'let r = http_get_h("%s/echo", "Content-Length: 5")\nprint r["status"]\n' % U)
        check("Content-Length cannot be set by the caller", out.strip() == "0" and len(S.hits) == n0, out)
        rc, out = kosh_src(REQ_PRE + 'let r = http_get_h("%s/echo", "@/etc/hostname")\nprint r["status"]\n' % U)
        check("a header line that is not 'Name: value' (curl would read a file for '@...') is refused", out.strip() == "0" and len(S.hits) == n0, out)
        for m in ("GET", "POST", "PUT", "DELETE", "PATCH"):
            rc, out = kosh_src(REQ_PRE + 'let r = http_request("%s", "%s/echo", "x=1", {"X-M": "%s"})\nprint r["body"]\n' % (m, U, m))
            j = jbody(out)
            check("http_request %s reaches the server with that method and headers" % m, j is not None and j["method"] == m and j["headers"].get("x-m") == m, out[:150])
        rc, out = kosh_src(REQ_PRE + 'let r = http_request("HEAD", "%s/echo", nil, {})\nprint r["status"]\n' % U)
        check("HEAD works", out.strip() == "200", out)
        rc, out = kosh_src(REQ_PRE + 'let r = http_request("get", "%s/echo", nil, {})\nprint r["status"]\nprint r["error"]\n' % U)
        check("a lower-case method is refused (methods are upper-case tokens)", out.startswith("0\n"), out)

        # ---------------------------------------------------- bodies survive intact (small, newlines/quotes, 200 KB, 3 MB)
        weird = 'line1\nline2 "quoted" back\\slash \ttab \u00e9\u0645 @x [1-3] {a,b}'
        wd = tempfile.mkdtemp(prefix="kosh_weird_"); wp = os.path.join(wd, "body.txt")
        open(wp, "w", encoding="utf-8").write(weird)
        rc, out = kosh_src(REQ_PRE + 'let b = read_file("%s")\nlet r = post("%s/echo", b)\nprint r["body"]\n' % (wp.replace("\\", "/"), U))
        j = jbody(out)
        check("a body with newlines, quotes, backslashes, tabs and non-ASCII arrives exactly", j is not None and j["body"] == weird, (out[:200], j and j["body"]))
        shutil.rmtree(wd, ignore_errors=True)
        before = set(glob.glob("/tmp/kosh_body_*"))
        for size in (200 * 1024, 3 * 1024 * 1024):
            prog = REQ_PRE + 'let s = "0123456789abcdef"\nwhile len(s) < %d:\n    s = s + s\nlet body = substring(s, 0, %d)\nlet r = post("%s/echo", body)\nprint r["status"]\nprint r["body"]\n' % (size, size, U)
            rc, out = kosh_src(prog)
            j = jbody(out)
            expect = hashlib.sha256((("0123456789abcdef" * (size // 16 + 1))[:size]).encode()).hexdigest()
            check("a %d KB body arrives intact (via a private temp file)" % (size // 1024), j is not None and j["body_len"] == size and j["body_sha"] == expect, out[:150])
        check("the temp files are gone afterwards", set(glob.glob("/tmp/kosh_body_*")) == before)
        rc, out = kosh_src(REQ_PRE + 'http_configure({"max_request_bytes": 1000})\nlet s = "0123456789abcdef"\nwhile len(s) < 2000:\n    s = s + s\nlet r = post("%s/echo", s)\nprint r["status"]\nprint r["error"]\n' % U)
        check("max_request_bytes is enforced", out.startswith("0\n") and "max_request_bytes" in out, out)

        # ---------------------------------------------------- URL globbing is off
        n0 = len(S.hits)
        rc, out = kosh_src(REQ_PRE + 'let r = get("%s/echo?x=[1-3]&y={a,b}")\nprint r["status"]\n' % U)
        check("[1-3] and {a,b} in a URL are sent as they are: ONE request", out.strip() == "200" and len(S.hits) == n0 + 1 and S.hits[-1]["path"] == "/echo?x=[1-3]&y={a,b}", (out, [h["path"] for h in S.hits[n0:]]))

        # ---------------------------------------------------- ~/.curlrc is ignored
        h = tempfile.mkdtemp(prefix="kosh_home_")
        try:
            open(os.path.join(h, ".curlrc"), "w").write('header = "X-From-Curlrc: yes"\nuser-agent = "curlrc-agent"\n')
            rc, out = kosh_src(REQ_PRE + 'let r = get("%s/echo")\nprint r["body"]\n' % U, env={"HOME": h, "CURL_HOME": h})
            j = jbody(out)
            check("settings in ~/.curlrc are not applied", j is not None and "x-from-curlrc" not in j["headers"] and j["headers"].get("user-agent") != "curlrc-agent", out[:200])
        finally: shutil.rmtree(h, ignore_errors=True)

        # ---------------------------------------------------- timeouts
        t0 = time.time()
        rc, out = kosh_src(REQ_PRE + 'http_configure({"timeout_seconds": 2})\nlet r = get("%s/slow")\nprint r["status"]\nprint r["success"]\nprint r["error"]\n' % U)
        dt = time.time() - t0
        check("timeout_seconds=2: a server that answers after 8 s is given up on within ~2 s, with an error", out.startswith("0\nfalse\n") and "timed out" in out.lower() and dt < 6, "%s after %.1fs" % (out[:120], dt))
        rc, out = kosh_src(REQ_PRE + 'let c = http_configure({"timeout_seconds": 5, "connect_timeout_seconds": 3})\nprint c["timeout_seconds"]\nprint c["connect_timeout_seconds"]\nprint c["max_response_bytes"]\nprint c["max_request_bytes"]\n')
        check("http_configure returns the settings in force", out.split() == ["5", "3", "33554432", "33554432"], out)
        t0 = time.time()
        rc, out = kosh_src(REQ_PRE + 'let r = get("http://127.0.0.1:1/")\nprint r["status"]\nprint r["error"]\n')
        check("a closed port fails fast with an error", out.startswith("0\n") and len(out.strip().splitlines()) >= 2 and time.time() - t0 < 5, out)
        rc, out = kosh_src(REQ_PRE + 'http_configure({"timeout_seconds": 5})\nlet r = get("http://no-such-host.invalid/")\nprint r["status"]\nprint r["error"]\n')
        check("an unknown host fails with an error", out.startswith("0\n") and len(out.strip().splitlines()) >= 2, out)
        for bad, label in (('{"timeout_seconds": 0}', "timeout 0"), ('{"timeout_seconds": "fast"}', "a string timeout"), ('{"nope": 1}', "an unknown option"), ('{"max_response_bytes": 10}', "a tiny response cap"), ('{"timeout_seconds": 99999}', "an enormous timeout")):
            rc, out = kosh_src(REQ_PRE + 'http_configure(%s)\nprint "after:" + str(http_configure({})["timeout_seconds"])\n' % bad)
            check("http_configure refuses %s (error, and the settings stay at the defaults)" % label, "Runtime error" in out and "after:60" in out, out[:200])

        # ---------------------------------------------------- response size cap
        rc, out = kosh_src(REQ_PRE + 'http_configure({"max_response_bytes": 1048576})\nlet r = get("%s/big")\nprint r["status"]\nprint len(r["body"])\nprint r["error"]\n' % U)
        l = out.strip().splitlines()
        check("max_response_bytes: a 5 MB response with a Content-Length is refused", l[0] == "0" and l[1] == "0" and len(l) >= 3, out[:200])
        rc, out = kosh_src(REQ_PRE + 'http_configure({"max_response_bytes": 1048576})\nlet r = get("%s/stream")\nprint r["status"]\nprint len(r["body"])\nprint r["error"]\n' % U)
        l = out.strip().splitlines()
        check("... and so is a 6 MB chunked stream with no Content-Length (the client's own cap stops it)", l[0] == "0" and l[1] == "0" and "larger" in out, out[:200])
        rc, out = kosh_src(REQ_PRE + 'let r = get("%s/big")\nprint r["status"]\nprint len(r["body"])\n' % U)
        check("with the default 32 MB cap a 5 MB response is fine", out.split() == ["200", str(5 * 1024 * 1024)], out[:100])

        # ---------------------------------------------------- secrets are not on any command line
        secret_key = "sk-TOPSECRET-%d" % os.getpid()
        d = tempfile.mkdtemp(prefix="kosh_ps_"); path = os.path.join(d, "p.kh")
        open(path, "w").write(REQ_PRE + 'let r = http_get_h("%s/hold?key=QUERYSECRET%d", "Authorization: Bearer %s")\nprint r["status"]\n' % (U, os.getpid(), secret_key))
        p = subprocess.Popen([KOSH, path], cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        time.sleep(1.5)
        ps = subprocess.run(["ps", "-eo", "args"], capture_output=True, text=True).stdout
        curl_lines = [l for l in ps.splitlines() if re.match(r"\s*curl\b", l)]
        check("while a request is in flight a curl process exists (so the next checks mean something)", len(curl_lines) >= 1, ps[-300:])
        check("the API key is not in any process's command line", secret_key not in ps)
        check("the URL (with its query string) is not in any command line either", ("QUERYSECRET%d" % os.getpid()) not in ps and "/hold" not in ps)
        tree = subprocess.run(["ps", "-eo", "pid,ppid,args"], capture_output=True, text=True).stdout.splitlines()[1:]
        procs = {}
        for l in tree:
            f = l.split(None, 2)
            if len(f) == 3: procs[f[0]] = (f[1], f[2])
        curls = [pid for pid, (pp, a) in procs.items() if re.match(r"curl\b", a)]
        parents = [procs.get(procs[c][0], ("", ""))[1] for c in curls]
        check("curl is a direct child of kosh (no shell in between)", curls and all(os.path.basename(KOSH) in par.split()[0] for par in parents if par), parents)
        check("curl was started with only '-q --config -' on its command line", curls and all(procs[c][1].strip() == "curl -q --config -" for c in curls), [procs[c][1] for c in curls])
        out = p.communicate(timeout=30)[0]
        check("... and the request still succeeded", out.strip() == "200", out)
        shutil.rmtree(d, ignore_errors=True)

        # ---------------------------------------------------- no leaks over many requests
        rc, out = kosh_src(REQ_PRE + 'let i = 0\nlet ok = 0\nwhile i < 400:\n    let r = get("%s/echo?i=" + str(i))\n    if r["status"] == 200:\n        ok = ok + 1\n    i = i + 1\nprint ok\n' % U, timeout=180)
        check("400 requests in a row all succeed (no leaked pipes/descriptors, no stuck children)", out.strip() == "400", out[-200:])
        defunct = subprocess.run(["ps", "-eo", "stat,args"], capture_output=True, text=True).stdout
        check("no zombie curl processes are left behind", not any(l.startswith("Z") and "curl" in l for l in defunct.splitlines()))

        # ---------------------------------------------------- curl missing
        empty = tempfile.mkdtemp(prefix="kosh_nopath_")
        try:
            rc, out = kosh_src(REQ_PRE + 'let r = get("%s/echo")\nprint r["status"]\nprint r["error"]\n' % U, env={"PATH": empty})
            check("without curl on PATH the error says so (and nothing hangs)", out.startswith("0\n") and "curl" in out.lower() and "install" in out.lower(), out)
        finally: shutil.rmtree(empty, ignore_errors=True)
    finally:
        S.stop()

# --------------------------------------------------------------------------- openai / dns / sessions / retries on the wire
class ScriptServer:
    """A local HTTP server whose answers are scripted per path prefix: a list of
    (status, body, delay_seconds); the last entry repeats once the list is used up."""
    def __init__(self):
        self.hits = []; self.scripts = {}; outer = self
        class H(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"
            def log_message(self, *a): pass
            def _do(self):
                n = int(self.headers.get("Content-Length", "0") or 0)
                body = self.rfile.read(n) if n else b""
                outer.hits.append({"method": self.command, "path": self.path, "headers": {k.lower(): v for k, v in self.headers.items()}, "body": body})
                entries = None
                for prefix, lst in outer.scripts.items():
                    if self.path.startswith(prefix): entries = lst; break
                if not entries: status, rbody, delay = 404, b"no script", 0
                else:
                    idx = sum(1 for h in outer.hits[:-1] if h["path"].startswith(prefix))
                    status, rbody, delay = entries[min(idx, len(entries) - 1)]
                if delay: time.sleep(delay)
                if isinstance(rbody, str): rbody = rbody.encode()
                try:
                    self.send_response(status); self.send_header("Content-Type", "application/json"); self.send_header("Content-Length", str(len(rbody))); self.end_headers(); self.wfile.write(rbody)
                except OSError: pass
            do_GET = do_POST = do_PUT = do_DELETE = _do
        class S(socketserver.ThreadingMixIn, http.server.HTTPServer): daemon_threads = True
        self.httpd = S(("127.0.0.1", 0), H); self.port = self.httpd.server_address[1]
        self.url = "http://127.0.0.1:%d" % self.port
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()
    def count(self, prefix): return sum(1 for h in self.hits if h["path"].startswith(prefix))
    def stop(self): self.httpd.shutdown(); self.httpd.server_close()

CHAT_OK = json.dumps({"choices": [{"message": {"role": "assistant", "content": "Hello from the fake API"}, "finish_reason": "stop"}], "usage": {"total_tokens": 7}})
OAI_PRE = 'import "%s"\n' % pkg("openai")
OAI_KEY = "sk-live-0123456789abcdefABCDEF"

def test_openai_wire():
    S = ScriptServer(); U = S.url + "/v1"
    mk = lambda extra="": 'let c = openai_client_ex("%s", "%s")\n%s' % (OAI_KEY, U, extra)
    try:
        S.scripts["/v1/chat"] = [(200, CHAT_OK, 0)]
        rc, out = kosh_src(OAI_PRE + mk('let r = openai_ask(c, "hi there")\nprint r\nprint c\n'))
        check("a normal call returns the reply", "Hello from the fake API" in out, out[:200])
        h = S.hits[-1]
        check("the key reaches the server, as a Bearer header", h["headers"].get("authorization") == "Bearer " + OAI_KEY, h["headers"])
        j = json.loads(h["body"]) if h["body"] else {}
        check("the request body carries the model and the user message, and not the key", j.get("model") == "gpt-4o-mini" and j["messages"][-1]["content"] == "hi there" and OAI_KEY not in h["body"].decode("utf-8", "replace"), j)
        check("printing the client afterwards does not show the key", OAI_KEY not in out and "sk-****CDEF" in out, out[-300:])

        # ---- timeout is enforced
        S.hits.clear(); S.scripts["/v1/chat"] = [(200, CHAT_OK, 6)]
        t0 = time.time()
        rc, out = kosh_src(OAI_PRE + mk('c = openai_set_timeout(c, 2000)\nlet r = openai_chat(c, [msg_user("x")])\nprint r["status"]\nprint r["message"]\nprint http_configure({})["timeout_seconds"]\n'))
        dt = time.time() - t0
        l = out.strip().splitlines()
        check("timeout 2000 ms: a server that needs 6 s is given up on within ~2 s", l[0] == "0" and "timed out" in out.lower() and dt < 5.5, "%s after %.1fs" % (out[:200], dt))
        check("a timeout is not retried (the request may already have been processed)", S.count("/v1/chat") == 1, S.count("/v1/chat"))
        check("the global HTTP timeout is put back afterwards (60)", l[-1] == "60", out)

        # ---- retries
        fast = 'c = openai_set_retry_backoff(c, 5, 20)\n'
        S.hits.clear(); S.scripts["/v1/chat"] = [(429, '{"error":{"message":"slow down"}}', 0), (429, "{}", 0), (200, CHAT_OK, 0)]
        rc, out = kosh_src(OAI_PRE + mk(fast + 'print openai_ask(c, "x")\n'))
        check("429, 429, 200: the third attempt succeeds", "Hello from the fake API" in out and S.count("/v1/chat") == 3, (out[:100], S.count("/v1/chat")))
        S.hits.clear(); S.scripts["/v1/chat"] = [(500, '{"error":{"message":"boom"}}', 0)]
        rc, out = kosh_src(OAI_PRE + mk(fast + 'c = openai_set_max_retries(c, 1000)\nlet r = openai_chat(c, [msg_user("x")])\nprint r["status"]\nprint r["message"]\n'))
        check("max_retries 1000 is capped: exactly 10 attempts, then the error comes back", S.count("/v1/chat") == 10 and out.startswith("500\nboom"), (S.count("/v1/chat"), out[:80]))
        S.hits.clear()
        rc, out = kosh_src(OAI_PRE + mk(fast + 'c = openai_set_max_retries(c, 1)\nlet r = openai_chat(c, [msg_user("x")])\nprint r["status"]\n'))
        check("max_retries 1: a single attempt", S.count("/v1/chat") == 1 and out.strip() == "500", (S.count("/v1/chat"), out))
        S.hits.clear(); S.scripts["/v1/chat"] = [(401, '{"error":{"message":"Incorrect API key provided","type":"invalid_request_error","code":"invalid_api_key"}}', 0)]
        rc, out = kosh_src(OAI_PRE + mk(fast + 'let r = openai_chat(c, [msg_user("x")])\nprint r["status"]\nprint r["message"]\nprint r["code"]\n'))
        check("a 401 is not retried and its message is passed on", S.count("/v1/chat") == 1 and out.split("\n")[0] == "401" and "Incorrect API key" in out and "invalid_api_key" in out, (S.count("/v1/chat"), out[:200]))
        S.hits.clear(); S.scripts["/v1/chat"] = [(500, "{}", 0)]
        t0 = time.time()
        rc, out = kosh_src(OAI_PRE + mk('c = openai_set_retry_backoff(c, 400, 400)\nc = openai_set_max_retries(c, 10)\nc = openai_set_max_total_seconds(c, 1)\nlet r = openai_chat(c, [msg_user("x")])\nprint r["status"]\n'))
        dt = time.time() - t0
        check("max_total_seconds=1 stops the retrying early (well under 10 attempts, well under 4 s)", 2 <= S.count("/v1/chat") <= 5 and dt < 4.5, "%d attempts in %.1fs" % (S.count("/v1/chat"), dt))
        S.hits.clear(); S.scripts["/v1/models"] = [(200, '{"data":[{"id":"gpt-a"},{"id":"gpt-b"}]}', 0)]
        rc, out = kosh_src(OAI_PRE + mk('let m = openai_list_models(c)\nprint len(m)\nprint m[0]\n'))
        check("openai_list_models works through the same path", out.split() == ["2", "gpt-a"] and S.hits[-1]["headers"].get("authorization") == "Bearer " + OAI_KEY, out)

        # ---- refusals send nothing
        S.hits.clear()
        for label, prog in (("a remote http:// base_url", 'let c = openai_client_ex("%s", "http://remote.example/v1")\nprint openai_ask(c, "x")\n' % OAI_KEY),
                            ("a user@host base_url", 'let c = openai_client_ex("%s", "https://api.openai.com@127.0.0.1:%d/v1")\nprint openai_ask(c, "x")\n' % (OAI_KEY, S.port)),
                            ("a key with a line break", 'let c = openai_client_ex("sk-aaaaaaaaaaaa\\r\\nX-Evil: 1", "%s")\nprint openai_ask(c, "x")\n' % U),
                            ("an organization with a line break", 'let c = openai_set_organization(openai_client_ex("%s", "%s"), "org\\r\\nX-Evil: 1")\nprint openai_ask(c, "x")\n' % (OAI_KEY, U)),
                            ("no key at all", 'let c = openai_client_ex("", "%s")\nprint openai_ask(c, "x")\n' % U)):
            n0 = len(S.hits)
            rc, out = kosh_src(OAI_PRE + prog)
            check("%s: nothing is sent, and no reply text comes back" % label, len(S.hits) == n0 and "Hello from" not in out, (len(S.hits) - n0, out[-200:]))
    finally:
        S.stop()

def test_dns_wire():
    S = ScriptServer(); R = S.url + "/resolve"
    pre = 'import "%s"\ndns_set_resolver("%s")\n' % (pkg("dns"), R)
    a_ok = json.dumps({"Status": 0, "Answer": [{"name": "example.com.", "type": 1, "TTL": 60, "data": "93.184.216.34"}]})
    try:
        S.scripts["/resolve"] = [(200, a_ok, 0)]
        rc, out = kosh_src(pre + 'print dns_lookup("example.com")\n')
        check("dns_lookup returns the A record", out.strip() == "93.184.216.34", out)
        check("the query is name=...&type=A on the configured resolver, asking for application/dns-json",
              S.hits[-1]["path"] == "/resolve?name=example.com&type=A" and S.hits[-1]["headers"].get("accept") == "application/dns-json", S.hits[-1])
        n0 = len(S.hits)
        for label, dom in (("a domain with &type=TXT", "example.com&type=TXT"), ("a domain with a space", "a b.example"), ("a domain with ?", "example.com?x=1"), ("a domain with a newline", "example.com\\nX: 1")):
            rc, out = kosh_src(pre + 'print dns_lookup("%s")\n' % dom)
            check("%s: refused (nil), nothing sent" % label, "nil" in out and len(S.hits) == n0, out[-120:])
        S.scripts["/resolve"] = [(200, json.dumps({"Status": 0, "Answer": [{"type": 5, "data": "alias.example.net."}, {"type": 1, "data": "10.0.0.7"}]}), 0)]
        rc, out = kosh_src(pre + 'print dns_lookup("www.example.com")\n')
        check("a CNAME in front of the A record is skipped (it used to be returned as if it were an IP)", out.strip() == "10.0.0.7", out)
        S.scripts["/resolve"] = [(200, json.dumps({"Status": 0, "Answer": [{"type": 5, "data": "alias.example.net."}]}), 0)]
        rc, out = kosh_src(pre + 'print dns_lookup("www.example.com")\n')
        check("only a CNAME and no A record: nil", out.strip() == "nil", out)
        S.scripts["/resolve"] = [(200, json.dumps({"Status": 3}), 0)]
        rc, out = kosh_src(pre + 'print dns_lookup("nonexistent.example")\n')
        check("NXDOMAIN (Status 3): nil", out.strip() == "nil", out)
        S.scripts["/resolve"] = [(200, "<html>not json</html>", 0)]
        rc, out = kosh_src(pre + 'print dns_lookup("example.com")\n')
        check("a non-JSON answer: nil", out.strip() == "nil", out)
        S.scripts["/resolve"] = [(500, "{}", 0)]
        rc, out = kosh_src(pre + 'print dns_lookup("example.com")\n')
        check("an HTTP 500 from the resolver: nil", out.strip() == "nil", out)
        S.scripts["/resolve"] = [(200, json.dumps({"Status": 0, "Answer": [{"type": 15, "data": "10 mail1.example.com."}, {"type": 15, "data": "20 mail2.example.com."}, {"type": 15, "data": 5}, "junk", {"type": 15}]}), 0)]
        rc, out = kosh_src(pre + 'let m = dns_query("example.com", "mx")\nprint len(m)\nprint m[0]\nprint m[1]\n')
        check("dns_query(MX) returns the MX strings and skips malformed entries", out.split("\n")[:3] == ["2", "10 mail1.example.com.", "20 mail2.example.com."], out)
        check("... asked for type=MX", S.hits[-1]["path"] == "/resolve?name=example.com&type=MX", S.hits[-1]["path"])
        S.scripts["/resolve"] = [(200, json.dumps({"Status": 0, "Answer": [{"type": 16, "data": "t%d" % i} for i in range(200)]}), 0)]
        rc, out = kosh_src(pre + 'print len(dns_query("example.com", "TXT"))\n')
        check("at most 50 answers are returned", out.strip() == "50", out)
        n0 = len(S.hits)
        rc, out = kosh_src(pre + 'print dns_query("example.com", "A&do=1")\nprint dns_query("example.com", "ANY")\n')
        check("an unknown / injected record type is refused, nothing sent", out.count("nil") == 2 and len(S.hits) == n0, out[-160:])
        rc, out = kosh_src(pre + 'print dns_set_resolver("http://dns.remote.example/resolve")\nprint dns_resolver()\n')
        ls = out.strip().splitlines()
        check("a plain-http remote resolver is refused and the old one stays", ls[-2] == "false" and ls[-1].endswith("/resolve") and "dns.remote.example" not in ls[-1], out)
    finally:
        S.stop()

def test_session_and_retry_wire():
    S = ScriptServer(); EVIL = ScriptServer(); U = S.url
    pre = 'import "%s"\n' % pkg("requests")
    try:
        S.scripts["/echo"] = [(200, '{"ok":true}', 0)]
        EVIL.scripts["/"] = [(200, "evil", 0)]
        rc, out = kosh_src(pre + 'let s = session("%s")\ns = session_set_header(s, "Authorization", "Bearer sk-SESSION-SECRET")\nlet r = session_get(s, "@127.0.0.1:%d/x")\nprint r["status"]\nprint r["error"]\n' % (U, EVIL.port))
        check("a session path that would change the host sends nothing anywhere", "\n0\n" in out and len(EVIL.hits) == 0 and len(S.hits) == 0, (out[:160], len(EVIL.hits), len(S.hits)))
        rc, out = kosh_src(pre + 'let s = session("%s")\ns = session_set_header(s, "Authorization", "Bearer sk-SESSION-SECRET")\nlet r = session_get(s, "/echo?a=1")\nprint r["status"]\n' % U)
        check("a normal session request carries the session header to the session host only", out.strip() == "200" and S.hits[-1]["headers"].get("authorization") == "Bearer sk-SESSION-SECRET" and len(EVIL.hits) == 0, out)
        rc, out = kosh_src(pre + 'let s = session("%s")\ns = session_set_header(s, "X-User", "bob\\r\\nX-Role: admin")\nlet r = session_get(s, "/echo")\nprint r["status"]\n' % U)
        check("a header value with CR/LF set on a session never turns into a second header", "x-role" not in S.hits[-1]["headers"], S.hits[-1]["headers"])
        rc, out = kosh_src(pre + 'let r = get("%s/echo?" + build_query({"q": "a b&admin=true"}))\nprint r["status"]\n' % U)
        check("build_query: the value arrives as ONE parameter (the server sees %26, not a new &admin=true)", S.hits[-1]["path"] == "/echo?q=a%20b%26admin%3Dtrue", S.hits[-1]["path"])

        # retries
        S.hits.clear(); S.scripts["/flaky"] = [(503, "{}", 0), (503, "{}", 0), (200, '{"done":true}', 0)]
        t0 = time.time()
        rc, out = kosh_src(pre + 'let r = get_retry("%s/flaky", 5)\nprint r["status"]\n' % U)
        check("get_retry: 503, 503, 200 -> success on the third try", out.strip() == "200" and S.count("/flaky") == 3 and time.time() - t0 < 6, (out, S.count("/flaky")))
        S.hits.clear(); S.scripts["/missing"] = [(404, "{}", 0)]
        rc, out = kosh_src(pre + 'let r = get_retry("%s/missing", 5)\nprint r["status"]\n' % U)
        check("get_retry does not retry a 404", out.strip() == "404" and S.count("/missing") == 1, (out, S.count("/missing")))
        S.hits.clear(); S.scripts["/down"] = [(503, "{}", 0)]
        rc, out = kosh_src(pre + 'let r = get_retry("%s/down", 1)\nprint r["status"]\n' % U)
        check("get_retry with attempts=1 tries once", out.strip() == "503" and S.count("/down") == 1, (out, S.count("/down")))
        S.hits.clear(); S.scripts["/pflaky"] = [(500, "{}", 0), (200, '{"done":true}', 0)]
        rc, out = kosh_src(pre + 'let r = post_retry("%s/pflaky", "a=1&b=2", 3)\nprint r["status"]\n' % U)
        check("post_retry sends the same body on the retry", out.strip() == "200" and [h["body"] for h in S.hits if h["path"].startswith("/pflaky")] == [b"a=1&b=2", b"a=1&b=2"], (out, [h["body"] for h in S.hits]))
        S.hits.clear()
        rc, out = kosh_src(pre + 'let r = get_retry("http://127.0.0.1:1/", 2)\nprint r["status"]\nprint r["error"]\n')
        check("get_retry on an unreachable port gives up with status 0 and an error", out.startswith("0\n") and len(out.strip().splitlines()) >= 2, out)
    finally:
        S.stop(); EVIL.stop()

# --------------------------------------------------------------------------- Phase B: files
def test_files():
    """fs sandbox vs symlinks, atomic write failure handling, dotenv permissions, orm path names."""
    posix = os.name != "nt"
    base = tempfile.mkdtemp(prefix="kosh_files_")
    try:
        root = os.path.join(base, "root"); os.mkdir(root)
        outside = os.path.join(base, "outside.txt"); open(outside, "w").write("TOPSECRET")
        os.mkdir(os.path.join(base, "other")); open(os.path.join(base, "other", "x.txt"), "w").write("other")
        b = base.replace("\\", "/")
        pre = 'import "%s"\n' % pkg("fs")
        if posix:
            os.symlink(outside, os.path.join(root, "link.txt"))
            os.symlink(os.path.join(base, "other"), os.path.join(root, "dirlink"))
            rc, out = kosh_src(pre + 'let sb = fs_sandbox("%s/root")\nprint fs_sandbox_read(sb, "link.txt")\nprint fs_sandbox_read(sb, "dirlink/x.txt")\nprint fs_sandbox_write(sb, "dirlink/new.txt", "z")\n' % b)
            check("sandbox refuses a symlink pointing outside (file and directory)", out.split() == ["nil", "nil", "nil"] and "TOPSECRET" not in out and not os.path.exists(os.path.join(base, "other", "new.txt")), out)
            os.symlink(root, os.path.join(base, "rootlink"))
            rc, out = kosh_src(pre + 'let sb = fs_sandbox("%s/rootlink")\nprint fs_sandbox_write(sb, "ok.txt", "hi")\nprint fs_sandbox_read(sb, "ok.txt")\n' % b)
            check("a symlinked sandbox root still works and is resolved", out.split() == ["2", "hi"] and os.path.exists(os.path.join(root, "ok.txt")), out)
        rc, out = kosh_src(pre + 'let sb = fs_sandbox("%s/root")\nprint fs_sandbox_read(sb, "../outside.txt")\nprint fs_sandbox_write(sb, "../escaped.txt", "x")\n' % b)
        check("sandbox refuses ../ read and write", out.split() == ["nil", "nil"] and not os.path.exists(os.path.join(base, "escaped.txt")), out)

        # atomic write: when the target cannot be replaced, the old data and the directory stay clean
        target = os.path.join(base, "adir"); os.mkdir(target)
        rc, out = kosh_src('print write_file_atomic("%s/adir", "x")\n' % b)
        check("write_file_atomic returns nil when the target is a directory, leaves no temp file", out.strip() == "nil" and os.listdir(base).count("adir") == 1 and not [f for f in os.listdir(base) if "kosh-tmp" in f], (out, os.listdir(base)))
        f = os.path.join(base, "keep.txt"); open(f, "w").write("OLD" * 1000)
        rc, out = kosh_src('print write_file_atomic("%s/keep.txt", "NEW")\n' % b)
        check("write_file_atomic replaces content and leaves no temp files", out.strip() == "3" and open(f).read() == "NEW" and not [x for x in os.listdir(base) if "kosh-tmp" in x], out)
        rc, out = kosh_src('let s = "%s"\nprint write_file_atomic("%s/big.txt", s + s)\n' % ("A" * 200000, b))
        check("large atomic write is complete", out.strip() == "400000" and os.path.getsize(os.path.join(base, "big.txt")) == 400000, out)

        # dotenv permissions
        if posix:
            env = os.path.join(base, ".env"); open(env, "w").write("K=v\n")
            pre2 = 'import "%s"\n' % pkg("dotenv")
            os.chmod(env, 0o644)
            rc, out = kosh_src(pre2 + 'print load_env_strict("%s/.env")\n' % b)
            check("load_env_strict refuses a world-readable .env", out.strip().endswith("nil") and "chmod 600" in out, out)
            os.chmod(env, 0o640)
            rc, out = kosh_src(pre2 + 'print load_env_strict("%s/.env")\n' % b)
            check("load_env_strict refuses a group-readable .env", out.strip().endswith("nil"), out)
            os.chmod(env, 0o600)
            rc, out = kosh_src(pre2 + 'print env_get(load_env_strict("%s/.env"), "K")\n' % b)
            check("load_env_strict accepts mode 600", out.strip() == "v", out)

        # orm table names cannot reach outside the working directory
        # (orm imports json_db by name, which resolves against ./packages, so run from ROOT)
        pwn = os.path.join(ROOT, "..", "pwn.json")
        rc, out = kosh_src('import "orm"\nlet m = orm_model("../pwn")\nlet m2 = orm_create(m, {"x": 1})\nprint m\nlet n = orm_model("%s/pwn")\nprint n\n' % b)
        check("orm refuses ../ and absolute table names; no file created", out.count("nil") == 2 and not os.path.exists(pwn) and not os.path.exists(os.path.join(base, "pwn.json")), out)
    finally:
        shutil.rmtree(base, ignore_errors=True)

# --------------------------------------------------------------------------- Phase C: sqlite locking
def test_sqlite_lock():
    """Another connection holds a write lock: Kosh waits for the busy timeout, then reports failure (not a hang, not a fake true)."""
    import sqlite3, time
    base = tempfile.mkdtemp(prefix="kosh_sqlite_")
    try:
        dbp = os.path.join(base, "l.db")
        c = sqlite3.connect(dbp, isolation_level=None, check_same_thread=False)
        c.execute("CREATE TABLE t (v)")
        c.execute("BEGIN EXCLUSIVE")          # hold the lock
        b = dbp.replace("\\", "/")
        pre = 'import "%s"\n' % pkg("sqlite")
        t0 = time.time()
        rc, out = kosh_src(pre + 'let db = db_open("%s")\ndb_busy_timeout(db, 0)\nprint db_exec(db, "INSERT INTO t VALUES (1)", [])\n' % b)
        fast = time.time() - t0
        check("busy timeout 0: fails at once with false and a 'locked' message", out.split()[0] == "false" and "locked" in out and fast < 3, (out, fast))
        t0 = time.time()
        rc, out = kosh_src(pre + 'let db = db_open("%s")\ndb_busy_timeout(db, 700)\nprint db_exec(db, "INSERT INTO t VALUES (1)", [])\n' % b)
        waited = time.time() - t0
        check("busy timeout 700 ms: waits about that long, then false", out.split()[0] == "false" and "locked" in out and waited >= 0.6, (out, waited))
        # the lock is released while Kosh is waiting -> the write succeeds (default timeout is 5 s)
        import threading
        threading.Timer(0.8, lambda: c.execute("COMMIT")).start()
        rc, out = kosh_src(pre + 'let db = db_open("%s")\nprint db_exec(db, "INSERT INTO t VALUES (7)", [])\n' % b)
        check("default busy timeout: waits for the other connection and then succeeds", out.split()[0] == "true", out)
        rows = sqlite3.connect(dbp).execute("SELECT v FROM t").fetchall()
        check("the waited-for row is really in the database", rows == [(7,)], rows)
    finally:
        shutil.rmtree(base, ignore_errors=True)

def run():
    test_uuid()
    test_webi_auth_wire()
    test_requests()
    test_openai_wire()
    test_dns_wire()
    test_session_and_retry_wire()
    test_files()
    test_sqlite_lock()
    print("\n  %d/%d passed   |   %d failed" % (passed, passed + failed, failed))
    return 1 if failed else 0

if __name__ == "__main__":
    if not os.path.exists(KOSH): sys.exit("kosh binary not found: %s (build it with `make`)" % KOSH)
    sys.exit(run())
