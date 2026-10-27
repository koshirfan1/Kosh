/*
 * proof.js - the live self-check shown at /_webi/proof.
 *
 * It runs REAL checks against the server it was loaded from, in the
 * visitor's own browser, using the same client code applications use. It can
 * show that this server's channel behaves as documented right now. It cannot
 * show that the server is free of bugs, that the code has been reviewed
 * (it has not - see "external_review" in /_webi/proof.json), or that the
 * visitor's browser and network are not compromised.
 */
(function () {
  "use strict";
  var doc = document;
  var list = doc.getElementById("checks");
  var summary = doc.getElementById("summary");
  var facts = doc.getElementById("facts");
  var counts = { pass: 0, fail: 0, warn: 0, info: 0 };

  function add(status, name, detail) {
    counts[status]++;
    var li = doc.createElement("li");
    li.className = status;
    var tag = doc.createElement("span");
    tag.className = "tag";
    tag.textContent = status === "pass" ? "PASS" : status === "fail" ? "FAIL" : status === "warn" ? "WARN" : "INFO";
    var b = doc.createElement("strong");
    b.textContent = " " + name;
    li.appendChild(tag);
    li.appendChild(b);
    if (detail) {
      var d = doc.createElement("div");
      d.className = "detail";
      d.textContent = detail;
      li.appendChild(d);
    }
    list.appendChild(li);
  }
  function fact(k, v) {
    var dt = doc.createElement("dt"); dt.textContent = k;
    var dd = doc.createElement("dd"); dd.textContent = String(v);
    facts.appendChild(dt); facts.appendChild(dd);
  }
  function hex(b) { var s = ""; for (var i = 0; i < b.length; i++) s += (b[i] < 16 ? "0" : "") + b[i].toString(16); return s; }
  function rnd() { var b = new Uint8Array(12); crypto.getRandomValues(b); return hex(b); }

  async function rawSecure(packet) {
    var r = await fetch("/_webi/secure", { method: "POST", body: packet, cache: "no-store", credentials: "omit" });
    return { status: r.status, body: await r.text() };
  }

  async function run() {
    // 1. transport
    if (location.protocol === "https:") add("pass", "Page was loaded over HTTPS", "The channel is defense in depth on top of TLS; TLS provides the server authentication.");
    else add("warn", "Page was NOT loaded over HTTPS", "The Webi channel is designed to run inside TLS. Its handshake is not authenticated on its own, so on plain HTTP an active attacker can sit in the middle.");

    // 2. server facts
    var info = null;
    try {
      var ir = await fetch("/_webi/proof.json", { cache: "no-store" });
      info = await ir.json();
      fact("Protocol", info.protocol);
      fact("Key exchange", info.primitives.kex);
      fact("Key derivation", info.primitives.kdf);
      fact("Packet encryption", info.primitives.aead);
      fact("Replay window", info.replay_window + " packets per direction");
      fact("Plain requests refused (require_secure)", info.require_secure);
      fact("Browser shell (bootstrap)", info.bootstrap);
      fact("Crawler verification active", info.bot_verification);
      fact("Sessions open right now", info.sessions_active);
      var st = info.stats || {};
      for (var k in st) if (Object.prototype.hasOwnProperty.call(st, k)) fact("Counter: " + k, st[k]);
      fact("External cryptographic review", info.external_review ? "yes" : "NO - not reviewed");
      add("pass", "Server publishes its configuration", "Read from /_webi/proof.json");
    } catch (e) {
      add("fail", "Could not read /_webi/proof.json", String(e && e.message || e));
      return;
    }

    // 3. client integrity (consistency only)
    try {
      var cr = await fetch("/_webi/client.js", { cache: "no-store" });
      var text = await cr.text();
      var local = hex(Webi._internal.sha256(new TextEncoder().encode(text)));
      var hdr = cr.headers.get("x-webi-client-sha256");
      if (local === hdr && local === info.client_sha256) add("pass", "Client script matches the hash the server advertises", local);
      else add("fail", "Client script hash mismatch", "computed " + local + ", header " + hdr + ", proof.json " + info.client_sha256);
      add("info", "This hash check is a consistency check, not a trust anchor", "A server that served malicious code could advertise a matching hash. Pin the hash out of band (for example in a Subresource Integrity attribute) to rely on it.");
    } catch (e) {
      add("fail", "Could not verify the client script", String(e && e.message || e));
    }

    // 4. handshake
    var ch = null;
    try {
      ch = await Webi.connect("");
      add("pass", "X25519 handshake completed", "session " + ch.session.sid.slice(0, 8) + "... (fresh ephemeral key pair generated in this browser)");
    } catch (e) {
      add("fail", "Handshake failed", String(e && e.message || e));
      return;
    }

    // 5. authenticated round trip with a random nonce
    var nonce = rnd();
    var probe = "/.well-known/webi-proof";
    try {
      var r = await ch.request("GET", probe, { query: "n=" + nonce });
      var echoed = JSON.parse(r.body);
      if (r.status === 200 && echoed.nonce === nonce) add("pass", "Encrypted round trip with a random nonce", "The server echoed " + nonce.slice(0, 8) + "... inside an authenticated, encrypted response.");
      else add("fail", "Round trip returned unexpected data", "status " + r.status);
    } catch (e) {
      add("fail", "Encrypted round trip failed", String(e && e.message || e));
    }

    // 6. wire opacity
    var packet = ch.session.seal("webi-req", JSON.stringify({ method: "GET", path: probe, query: "n=" + nonce, headers: "", body: "" }));
    var leaks = packet.indexOf("well-known") >= 0 || packet.indexOf(nonce) >= 0 || packet.indexOf("GET") >= 0;
    if (!leaks) add("pass", "Request on the wire shows no path, query or method", packet.slice(0, 40) + "...");
    else add("fail", "Request packet contains plaintext", packet.slice(0, 80));

    // 7. replay
    try {
      var first = await rawSecure(packet);
      var again = await rawSecure(packet);
      if (first.status === 200 && again.status === 403) add("pass", "Replaying a captured request is rejected", "first delivery HTTP " + first.status + ", identical packet again HTTP " + again.status);
      else add("fail", "Replay check", "first " + first.status + ", second " + again.status);

      // 8. tamper
      var p2 = ch.session.seal("webi-req", JSON.stringify({ method: "GET", path: probe, query: "n=x", headers: "", body: "" }));
      var flipped = p2.slice(0, -1) + (p2.slice(-1) === "0" ? "1" : "0");
      var tam = await rawSecure(flipped);
      if (tam.status === 403) add("pass", "A packet with one flipped bit is rejected", "HTTP " + tam.status);
      else add("fail", "Tampered packet was not rejected", "HTTP " + tam.status);

      // 9. uniform failure
      if (again.body === tam.body) add("pass", "Replay and tamper failures are indistinguishable", "Both return the same body, so the endpoint does not reveal which check failed.");
      else add("fail", "Failure responses differ", "replay: " + again.body + " / tamper: " + tam.body);
    } catch (e) {
      add("fail", "Attack checks could not run", String(e && e.message || e));
    }

    // 10. plain access policy
    try {
      var pr = await fetch(probe + "?n=plain", { cache: "no-store", credentials: "omit", headers: { Accept: "application/json" } });
      if (info.require_secure) {
        if (pr.status === 426) add("pass", "A plain (non-channel) request to a protected path is refused", "HTTP 426");
        else add("fail", "require_secure is on but a plain request got HTTP " + pr.status, "");
      } else {
        add("info", "require_secure is off", "Plain requests are served normally; the channel is optional for clients.");
      }
    } catch (e) {
      add("fail", "Plain access check failed", String(e && e.message || e));
    }
  }

  run().then(function () {
    summary.className = counts.fail ? "bad" : counts.warn ? "meh" : "good";
    summary.textContent = counts.pass + " passed, " + counts.fail + " failed, " + counts.warn + " warnings";
    doc.documentElement.setAttribute("data-proof-done", counts.fail ? "fail" : "pass");
  }, function (e) {
    summary.className = "bad";
    summary.textContent = "The self-check crashed: " + (e && e.message || e);
    doc.documentElement.setAttribute("data-proof-done", "fail");
  });
})();
