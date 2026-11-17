#!/usr/bin/env python3
"""Process-level security tests for Kosh packages.

Some properties can only be seen from OUTSIDE a single Kosh program: whether two
programs started in the same second get the same "random" values, what a secret
looks like in the process list, what a request really puts on the wire. These tests
start real `kosh` processes (and, for the HTTP parts, a local server).

  python3 tests/security/test_packages.py        (KOSH=path/to/kosh to override)
Run from anywhere; paths are resolved from this file.
"""
import http.client, json, os, re, socket, subprocess, sys, tempfile, shutil, time

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

def run():
    test_uuid()
    test_webi_auth_wire()
    print("\n  %d/%d passed   |   %d failed" % (passed, passed + failed, failed))
    return 1 if failed else 0

if __name__ == "__main__":
    if not os.path.exists(KOSH): sys.exit("kosh binary not found: %s (build it with `make`)" % KOSH)
    sys.exit(run())
