// ===========================================================================
// webi_lib.c — Native HTTP server backend for the webi Kosh package
//
// Provides:
//   http_serve(port, app)  — blocking server loop, calls Kosh's webi_handle()
//
// Also provides the v1.1 requests extensions:
//   http_get_h / http_post_h / http_put_h / http_put_json
//   http_patch / http_patch_json / http_delete_h / http_head
//
// Platform support:
//   POSIX (Linux/macOS) — raw BSD sockets + curl subprocess for outbound
//   Windows            — WinHTTP for outbound; Winsock for server
// ===========================================================================

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <winhttp.h>
#  include <wincrypt.h>
#  pragma comment(lib, "ws2_32.lib")
#  pragma comment(lib, "winhttp.lib")
#  pragma comment(lib, "advapi32.lib")
typedef SOCKET wb_sock_t;
#else
#  define _POSIX_C_SOURCE 200809L
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <sys/time.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <netdb.h>
typedef int wb_sock_t;
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#include <sys/stat.h>
#include <signal.h>
#ifndef _WIN32
#include <limits.h>
#endif

#include "webi_lib.h"
#include "vm.h"
#include "json_lib.h"

#ifndef S_ISREG
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif

// ---------------------------------------------------------------------------
// Security limits (v1.1.1; configurable since v0.016.3)
//   - max header bytes: caps how much we buffer while waiting for the
//     \r\n\r\n header terminator, so a client that never sends one can't
//     grow our buffer forever.
//   - max body bytes: caps the Content-Length we will accept.
//   - read timeout: longest single wait for data on an accepted connection.
//   - request timeout: longest the WHOLE request may take, so a client that
//     trickles one byte at a time (slowloris) can't hold the single-threaded
//     server for longer than this.
// ---------------------------------------------------------------------------
#define WB_DEFAULT_MAX_HEADER   (64 * 1024)          /* 64 KB */
#define WB_DEFAULT_MAX_BODY     (10 * 1024 * 1024)   /* 10 MB */
#define WB_DEFAULT_READ_TIMEOUT 30                   /* seconds per recv */
#define WB_DEFAULT_REQ_TIMEOUT  60                   /* seconds for the WHOLE request */
#define WB_MAX_STATIC_FILE      (64 * 1024 * 1024)   /* 64 MB cap on a single static file */

// Runtime limits. Defaults above; overridden at server start from the app
// map's "limits" key (see webi_limits() in webi/limits.kh), validated by
// wb_load_limits().
typedef struct {
    size_t max_header;        // bytes buffered while waiting for the header terminator
    size_t max_body;          // largest Content-Length accepted
    int    read_timeout_s;    // longest single wait for data from the client
    int    request_timeout_s; // longest the whole request (headers + body) may take
} wb_limits_t;
static wb_limits_t g_lim = { WB_DEFAULT_MAX_HEADER, WB_DEFAULT_MAX_BODY,
                             WB_DEFAULT_READ_TIMEOUT, WB_DEFAULT_REQ_TIMEOUT };

/* Bridge: call Kosh function by name from C */
extern Value kosh_call_fn(Interpreter *interp, Environment *env,
                          const char *fn_name, int argc, Value *argv);
/* Global env pointer set by http_serve so the accept loop can call Kosh */
static Environment *g_webi_env = NULL;

// ---------------------------------------------------------------------------
// wb_safe_call_webi_handle — call the Kosh webi_handle() bridge function
// and CONTAIN any runtime error to this one request.
//
// Previously a runtime error inside a route handler (a typo, a bad map
// access, anything) set interp->had_runtime_error, and the accept loop
// checked that flag and `break`-ed out — permanently stopping the server
// for every future client, not just the one that triggered it. There was
// no per-request fault isolation at all: one bad or malicious request
// could take the whole app down until it was manually restarted.
//
// This wrapper calls webi_handle() the same way, but if it set the error
// flag, it logs the failure, clears the flag, discards whatever partial
// result came back, and returns a plain 500 response instead — the
// server keeps running and keeps serving everyone else.
// ---------------------------------------------------------------------------
static Value wb_safe_call_webi_handle(Interpreter *interp, Environment *env,
                                       Value *call_args, int argc,
                                       const char *method, const char *path) {
    Value res_map;

    // Check if we are in VM mode (interp is actually a VM pointer)
    // We check a magic field or use the fact that g_webi_env is NULL for VM
    if (interp && !env) {
        res_map = vm_call_fn((VM*)interp, "webi_handle", argc, call_args);
        if (((VM*)interp)->had_runtime_error) {
            fprintf(stderr, "[webi] VM request handler error on %s %s\n", method ? method : "?", path ? path : "?");
            ((VM*)interp)->had_runtime_error = 0;
            value_free(res_map);
            res_map = value_map_empty();
            map_set(&res_map, "status",       value_number(500));
            map_set(&res_map, "body",         value_string("500 Internal Server Error (VM Error)"));
            map_set(&res_map, "content_type", value_string("text/plain"));
            map_set(&res_map, "headers",      value_map_empty());
        }
    } else {
        fprintf(stderr,
                "[webi] request handler error on %s %s — request failed, "
                "server continues running.\n",
                method ? method : "?", path ? path : "?");
        interp->had_runtime_error = 0;
        value_free(res_map);

        res_map = value_map_empty();
        map_set(&res_map, "status",       value_number(500));
        map_set(&res_map, "body",         value_string("500 Internal Server Error"));
        map_set(&res_map, "content_type", value_string("text/plain"));
        map_set(&res_map, "headers",      value_map_empty());
    }

    return res_map;
}

// ---------------------------------------------------------------------------
// Shared buffer helpers (same pattern as requests_lib.c)
// ---------------------------------------------------------------------------
static void wb_buf_append(char **buf, int *len, int *cap, const char *s, int slen) {
    while (*len + slen + 1 > *cap) {
        *cap = *cap ? *cap * 2 : 512;
        *buf = realloc(*buf, *cap);
    }
    memcpy(*buf + *len, s, slen);
    *len += slen;
    (*buf)[*len] = '\0';
}
static void wb_buf_str(char **buf, int *len, int *cap, const char *s) {
    wb_buf_append(buf, len, cap, s, (int)strlen(s));
}
static void wb_buf_char(char **buf, int *len, int *cap, char c) {
    wb_buf_append(buf, len, cap, &c, 1);
}

// ---------------------------------------------------------------------------
// Inline JSON encoder (mirrors requests_lib.c — no cross-module dependency)
// ---------------------------------------------------------------------------
static void wb_json_string(const char *s, char **buf, int *len, int *cap) {
    wb_buf_char(buf, len, cap, '"');
    for (const char *p = s; *p; p++) {
        if      (*p == '"')  { wb_buf_append(buf, len, cap, "\\\"", 2); }
        else if (*p == '\\') { wb_buf_append(buf, len, cap, "\\\\", 2); }
        else if (*p == '\n') { wb_buf_append(buf, len, cap, "\\n",  2); }
        else if (*p == '\r') { wb_buf_append(buf, len, cap, "\\r",  2); }
        else if (*p == '\t') { wb_buf_append(buf, len, cap, "\\t",  2); }
        else wb_buf_char(buf, len, cap, *p);
    }
    wb_buf_char(buf, len, cap, '"');
}

static void wb_json_encode(Value v, char **buf, int *len, int *cap) {
    char tmp[64];
    switch (v.type) {
        case VAL_NIL:
            wb_buf_append(buf, len, cap, "null", 4); break;
        case VAL_BOOL:
            if (v.as.boolean) wb_buf_append(buf, len, cap, "true", 4);
            else              wb_buf_append(buf, len, cap, "false", 5);
            break;
        case VAL_NUMBER:
            if (v.as.number == (long long)v.as.number)
                snprintf(tmp, sizeof(tmp), "%lld", (long long)v.as.number);
            else
                snprintf(tmp, sizeof(tmp), "%.17g", v.as.number);
            wb_buf_append(buf, len, cap, tmp, (int)strlen(tmp)); break;
        case VAL_STRING:
            wb_json_string(v.as.string, buf, len, cap); break;
        case VAL_ARRAY:
            wb_buf_char(buf, len, cap, '[');
            for (int i = 0; i < AS_ARRAY_COUNT(v); i++) {
                if (i) wb_buf_append(buf, len, cap, ", ", 2);
                wb_json_encode(AS_ARRAY_ITEMS(v)[i], buf, len, cap);
            }
            wb_buf_char(buf, len, cap, ']'); break;
        case VAL_MAP:
            wb_buf_char(buf, len, cap, '{');
            for (int i = 0; i < AS_MAP_COUNT(v); i++) {
                if (i) wb_buf_append(buf, len, cap, ", ", 2);
                wb_json_string(AS_MAP_ENTRIES(v)[i].key, buf, len, cap);
                wb_buf_append(buf, len, cap, ": ", 2);
                wb_json_encode(*AS_MAP_ENTRIES(v)[i].value, buf, len, cap);
            }
            wb_buf_char(buf, len, cap, '}'); break;
        default:
            wb_buf_append(buf, len, cap, "null", 4); break;
    }
}

// ---------------------------------------------------------------------------
// Arg checking helpers
// ---------------------------------------------------------------------------
static int wb_check(Interpreter *interp, const char *fn, int min, int actual) {
    if (actual < min) {
        fprintf(stderr, "Runtime error: %s() expects at least %d argument(s), got %d\n",
                fn, min, actual);
        if (interp) interp->had_runtime_error = 1;
        return 0;
    }
    return 1;
}

static int wb_str_arg(Interpreter *interp, const char *fn, int idx,
                       Value v, const char **out) {
    if (v.type != VAL_STRING) {
        fprintf(stderr, "Runtime error: %s() argument %d must be a string\n", fn, idx+1);
        interp->had_runtime_error = 1;
        return 0;
    }
    *out = v.as.string;
    return 1;
}

// ===========================================================================
// SHARED HTTP PARSING / RESPONSE HELPERS (portable — used by both the POSIX
// and Windows server loops below). These only use recv() with an int-like fd
// (works for both POSIX fd and Winsock SOCKET-as-int) and plain string/buffer
// manipulation, so they compile and run unchanged on every platform.
// ===========================================================================

// ---------------------------------------------------------------------------
// Internal: parse the first line of an HTTP request
// Sets method, path, query (all must point to caller-allocated char[512])
// ---------------------------------------------------------------------------
static void wb_parse_request_line(const char *raw,
                                    char *method, char *path, char *query) {
    method[0] = path[0] = query[0] = '\0';
    sscanf(raw, "%511s %511s", method, path);

    // Split path into path + query string
    char *q = strchr(path, '?');
    if (q) {
        strncpy(query, q + 1, 511); query[511] = '\0';
        *q = '\0';
    }
}

// ---------------------------------------------------------------------------
// Low-level send helpers
// ---------------------------------------------------------------------------
static int wb_streq_ci(const char *a, const char *b);   /* defined with the MIME table below */

static const char *wb_reason(long status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 414: return "URI Too Long";
        case 415: return "Unsupported Media Type";
        case 422: return "Unprocessable Entity";
        case 426: return "Upgrade Required";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        default:  return "OK";
    }
}

// Send every byte, retrying on partial writes. A client that vanishes
// mid-response must never kill the server: MSG_NOSIGNAL where it exists, and
// the POSIX server loop also ignores SIGPIPE.
static void wb_send_all(wb_sock_t fd, const char *buf, size_t len) {
#ifdef MSG_NOSIGNAL
    const int flags = MSG_NOSIGNAL;
#else
    const int flags = 0;
#endif
    size_t off = 0;
    while (off < len) {
        size_t chunk = len - off;
        if (chunk > (1u << 20)) chunk = (1u << 20);
        long n = (long)send(fd, buf + off, (int)chunk, flags);
        if (n <= 0) return;
        off += (size_t)n;
    }
}

static void wb_send_error(wb_sock_t fd, int status) {
    char msg[320];
    const char *reason = wb_reason(status);
    int blen = (int)strlen(reason) + 1;
    int n = snprintf(msg, sizeof(msg),
        "HTTP/1.1 %d %s\r\nContent-Type: text/plain; charset=utf-8\r\n"
        "Content-Length: %d\r\nConnection: close\r\n\r\n%s\n",
        status, reason, blen, reason);
    if (n > 0) wb_send_all(fd, msg, (size_t)n);
}

// ---------------------------------------------------------------------------
// Time, socket timeouts, and configurable limits
// ---------------------------------------------------------------------------
static unsigned long long wb_now_ms(void) {
#ifdef _WIN32
    return (unsigned long long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ULL + (unsigned long long)(ts.tv_nsec / 1000000L);
#endif
}

static void wb_set_timeouts_ms(wb_sock_t fd, unsigned long long recv_ms, unsigned long long send_ms) {
#ifdef _WIN32
    DWORD r = (DWORD)recv_ms, w = (DWORD)send_ms;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&r, sizeof(r));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&w, sizeof(w));
#else
    struct timeval r, w;
    r.tv_sec = (time_t)(recv_ms / 1000); r.tv_usec = (suseconds_t)((recv_ms % 1000) * 1000);
    w.tv_sec = (time_t)(send_ms / 1000); w.tv_usec = (suseconds_t)((send_ms % 1000) * 1000);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &r, sizeof(r));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &w, sizeof(w));
#endif
}

// Called before every recv() of a request: returns 1 if the whole-request
// deadline has already passed; otherwise arms the socket's receive timeout to
// min(read timeout, time left), so a blocked recv wakes up in time.
static int wb_arm_timeout(wb_sock_t fd, unsigned long long start_ms) {
    unsigned long long total = (unsigned long long)g_lim.request_timeout_s * 1000ULL;
    unsigned long long used  = wb_now_ms() - start_ms;
    if (used >= total) return 1;
    unsigned long long left = total - used;
    unsigned long long per  = (unsigned long long)g_lim.read_timeout_s * 1000ULL;
    wb_set_timeouts_ms(fd, left < per ? (left < 50 ? 50 : left) : per, total);
    return 0;
}

// Read one numeric option from the app's "limits" map. Absent = keep the
// default. Present but not a number in [lo, hi] = a configuration error: the
// server refuses to start rather than silently ignoring a typo.
static int wb_limit_opt(Value *lim, const char *key, double lo, double hi, double *out) {
    Value *v = lim ? map_get(lim, key) : NULL;
    if (!v) return 1;
    if (v->type != VAL_NUMBER || v->as.number < lo || v->as.number > hi) {
        fprintf(stderr, "[webi] limits: '%s' must be a number between %.0f and %.0f\n", key, lo, hi);
        return 0;
    }
    *out = v->as.number;
    return 1;
}

static int wb_load_limits(Value *app) {
    wb_limits_t l = { WB_DEFAULT_MAX_HEADER, WB_DEFAULT_MAX_BODY,
                      WB_DEFAULT_READ_TIMEOUT, WB_DEFAULT_REQ_TIMEOUT };
    Value *lim = map_get(app, "limits");
    if (lim && lim->type != VAL_MAP) lim = NULL;
    double v;
    v = (double)l.max_header;   if (!wb_limit_opt(lim, "max_header_bytes", 1024, 1048576, &v)) return 0; l.max_header = (size_t)v;
    v = (double)l.max_body;     if (!wb_limit_opt(lim, "max_body_bytes", 0, 1073741824.0, &v)) return 0;  l.max_body = (size_t)v;
    v = l.read_timeout_s;       if (!wb_limit_opt(lim, "read_timeout_seconds", 1, 600, &v)) return 0;     l.read_timeout_s = (int)v;
    v = l.request_timeout_s;    if (!wb_limit_opt(lim, "request_timeout_seconds", 1, 3600, &v)) return 0; l.request_timeout_s = (int)v;
    if (l.read_timeout_s > l.request_timeout_s) l.read_timeout_s = l.request_timeout_s;
    g_lim = l;
    return 1;
}

// app["host"] -> IPv4 address to bind. "" / "0.0.0.0" / "*" = every interface.
// (Before 0.016.3 the host was ignored and the server always bound every
// interface, even for webi_run_host(app, "127.0.0.1", port).)
static int wb_parse_host(Value *app, struct in_addr *out) {
    Value *h = map_get(app, "host");
    const char *host = (h && h->type == VAL_STRING) ? h->as.string : "";
    if (!*host || strcmp(host, "0.0.0.0") == 0 || strcmp(host, "*") == 0) { out->s_addr = htonl(INADDR_ANY); return 1; }
    if (strcmp(host, "localhost") == 0) { out->s_addr = htonl(INADDR_LOOPBACK); return 1; }
    if (inet_pton(AF_INET, host, out) == 1) return 1;
    fprintf(stderr, "[webi] cannot bind to host '%s': use an IPv4 address, 'localhost', or '0.0.0.0' for every interface\n", host);
    return 0;
}

// ---------------------------------------------------------------------------
// Strict HTTP/1.x request reader
//
// Reads one request and validates it BEFORE any of it reaches Kosh code.
// Anything ambiguous is refused, because ambiguity is what request-smuggling
// and header-injection attacks are made of:
//   * request line must be exactly "METHOD target HTTP/1.0|1.1"; target must
//     start with "/" (or be "*") and fit the 512-byte buffers below
//   * header names must be RFC 7230 tokens; no obs-fold, no NUL, no control
//     characters, no bare CR/LF; at most WB_MAX_HEADER_LINES lines
//   * Content-Length: at most one, digits only, <= g_lim.max_body; the body
//     must arrive in full (a short body is an error, not a silent truncation)
//   * Transfer-Encoding is not implemented: 501 (400 if Content-Length is
//     also present, which is the classic smuggling shape)
//   * a request body may not contain a NUL byte (Kosh request bodies are
//     text; truncating at a NUL would let the checker and the app disagree)
// Returns 0 on success; an HTTP status (400, 408, 413, 414, 431, 501) the
// caller should answer with; or -1 if the client sent nothing and left.
// ---------------------------------------------------------------------------
#define WB_MAX_HEADER_LINES 100

typedef struct {
    char  *raw;       // request line + headers (+ any body bytes read with them), NUL-terminated
    size_t rawlen;
    char  *headers;   // "Key: Value\r\nKey2: Value2" (request line excluded)
    char  *body;      // exactly Content-Length bytes, NUL-terminated
} wb_request_t;

static int wb_is_tchar(unsigned char c) {
    if (c == 0) return 0;
    if (isalnum(c)) return 1;
    return strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

static int wb_name_is(const char *s, size_t len, const char *want) {
    size_t wl = strlen(want);
    if (len != wl) return 0;
    for (size_t i = 0; i < len; i++)
        if (tolower((unsigned char)s[i]) != want[i]) return 0;
    return 1;
}

static long wb_find_header_end(const char *buf, size_t len, size_t from) {
    for (size_t i = from; i + 3 < len; i++)
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n')
            return (long)i;
    return -1;
}

// Validate buf[0..hend) (the request line plus headers, without the final
// CRLFCRLF). Returns 0 or an HTTP status; *clen gets the Content-Length.
static int wb_validate_head(const char *buf, size_t hend, size_t *clen) {
    *clen = 0;
    if (memchr(buf, '\0', hend)) return 400;

    // ---- request line ----
    size_t e = 0;
    while (e < hend && !(buf[e] == '\r' && e + 1 < hend && buf[e + 1] == '\n')) e++;
    const char *sp1 = memchr(buf, ' ', e);
    if (!sp1) return 400;
    const char *sp2 = memchr(sp1 + 1, ' ', e - (size_t)(sp1 + 1 - buf));
    if (!sp2) return 400;
    size_t mlen = (size_t)(sp1 - buf);
    if (mlen == 0 || mlen > 16) return 400;
    for (size_t i = 0; i < mlen; i++) if (!wb_is_tchar((unsigned char)buf[i])) return 400;
    size_t tlen = (size_t)(sp2 - (sp1 + 1));
    if (tlen == 0) return 400;
    if (tlen > 511) return 414;
    const char *target = sp1 + 1;
    if (!(target[0] == '/' || (tlen == 1 && target[0] == '*'))) return 400;
    for (size_t i = 0; i < tlen; i++) {
        unsigned char c = (unsigned char)target[i];
        if (c <= 0x20 || c == 0x7f) return 400;
    }
    const char *ver = sp2 + 1;
    size_t vlen = e - (size_t)(ver - buf);
    if (!((vlen == 8) && (memcmp(ver, "HTTP/1.1", 8) == 0 || memcmp(ver, "HTTP/1.0", 8) == 0))) return 400;

    // ---- header lines ----
    int lines = 0, cl_seen = 0, te_seen = 0;
    size_t pos = e + 2;
    while (e < hend && pos <= hend) {
        size_t le = pos;
        while (le < hend && !(buf[le] == '\r' && le + 1 < hend && buf[le + 1] == '\n')) le++;
        size_t llen = le - pos;
        if (llen == 0) return 400;
        if (++lines > WB_MAX_HEADER_LINES) return 431;
        const char *line = buf + pos;
        if (line[0] == ' ' || line[0] == '\t') return 400;      // obsolete line folding
        const char *colon = memchr(line, ':', llen);
        if (!colon || colon == line) return 400;
        size_t nlen = (size_t)(colon - line);
        for (size_t i = 0; i < nlen; i++) if (!wb_is_tchar((unsigned char)line[i])) return 400;
        const char *val = colon + 1;
        size_t vl = llen - nlen - 1;
        for (size_t i = 0; i < vl; i++) {
            unsigned char c = (unsigned char)val[i];
            if ((c < 0x20 && c != '\t') || c == 0x7f) return 400;  // includes bare CR / LF
        }
        if (wb_name_is(line, nlen, "content-length")) {
            if (++cl_seen > 1) return 400;
            while (vl && (*val == ' ' || *val == '\t')) { val++; vl--; }
            while (vl && (val[vl - 1] == ' ' || val[vl - 1] == '\t')) vl--;
            if (vl == 0 || vl > 18) return 400;
            unsigned long long n = 0;
            for (size_t i = 0; i < vl; i++) {
                if (val[i] < '0' || val[i] > '9') return 400;
                n = n * 10 + (unsigned)(val[i] - '0');
            }
            if (n > (unsigned long long)g_lim.max_body) return 413;
            *clen = (size_t)n;
        } else if (wb_name_is(line, nlen, "transfer-encoding")) {
            te_seen = 1;
        }
        pos = le + 2;
        if (le >= hend) break;
    }
    if (te_seen) return cl_seen ? 400 : 501;
    return 0;
}

static int wb_timed_out(void) {
#ifdef _WIN32
    return WSAGetLastError() == WSAETIMEDOUT;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

static int wb_read_request_strict(wb_sock_t fd, wb_request_t *out) {
    memset(out, 0, sizeof(*out));
    size_t cap = 8192, len = 0, scan = 0;
    char *buf = malloc(cap);
    if (!buf) return 500;
    char tmp[4096];
    long hend = -1;
    unsigned long long t_start = wb_now_ms();

    for (;;) {
        int expired = wb_arm_timeout(fd, t_start);
        long n = expired ? -1 : (long)recv(fd, tmp, (int)sizeof(tmp), 0);
        if (n <= 0) {
            int timed_out = expired || ((n < 0) && wb_timed_out());
            free(buf);
            if (len == 0 && !timed_out) return -1;              // connected and left
            return timed_out ? 408 : 400;                        // stalled / cut off mid-headers
        }
        if (len + (size_t)n + 1 > cap) {
            while (len + (size_t)n + 1 > cap) cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); return 500; }
            buf = nb;
        }
        memcpy(buf + len, tmp, (size_t)n);
        size_t before = len;
        len += (size_t)n;
        buf[len] = '\0';
        hend = wb_find_header_end(buf, len, before >= 3 ? before - 3 : 0);
        if (hend >= 0) break;
        scan = len;
        if (len > (size_t)g_lim.max_header) {
            fprintf(stderr, "[webi] request headers exceeded %zu bytes — rejecting\n", g_lim.max_header);
            free(buf);
            return 431;
        }
    }
    (void)scan;
    if ((size_t)hend > (size_t)g_lim.max_header) { free(buf); return 431; }

    size_t clen = 0;
    int st = wb_validate_head(buf, (size_t)hend, &clen);
    if (st != 0) { free(buf); return st; }

    // ---- body: exactly Content-Length bytes, all of them ----
    size_t start = (size_t)hend + 4;
    size_t have = len - start;
    // Allocate as the bytes arrive, not "Content-Length" up front: a client
    // claiming 1 GB must actually send it before it costs us 1 GB.
    size_t got = have < clen ? have : clen;
    size_t bcap = clen < 65536 ? clen : 65536;
    if (bcap < got) bcap = got;
    char *body = malloc(bcap + 1);
    if (!body) { free(buf); return 500; }
    if (got) memcpy(body, buf + start, got);
    while (got < clen) {
        if (got == bcap) {
            size_t ncap = bcap * 2 > clen ? clen : bcap * 2;
            char *nb = realloc(body, ncap + 1);
            if (!nb) { free(body); free(buf); return 500; }
            body = nb; bcap = ncap;
        }
        int expired = wb_arm_timeout(fd, t_start);
        size_t want = bcap - got; if (want > 65536) want = 65536;
        long n = expired ? -1 : (long)recv(fd, body + got, (int)want, 0);
        if (n <= 0) {
            int timed_out = expired || ((n < 0) && wb_timed_out());
            free(body); free(buf);
            return timed_out ? 408 : 400;
        }
        got += (size_t)n;
    }
    body[clen] = '\0';
    if (clen && memchr(body, '\0', clen)) {
        fprintf(stderr, "[webi] request body contains a NUL byte — rejecting (binary request bodies are not supported)\n");
        free(body); free(buf);
        return 400;
    }

    // headers string: everything after the request line
    const char *first_crlf = strstr(buf, "\r\n");
    char *headers;
    if (!first_crlf || (size_t)(first_crlf - buf) >= (size_t)hend) {
        headers = strdup("");
    } else {
        const char *hs = first_crlf + 2;
        size_t hl = (size_t)hend - (size_t)(hs - buf);
        headers = malloc(hl + 1);
        if (headers) { memcpy(headers, hs, hl); headers[hl] = '\0'; }
    }
    if (!headers) { free(body); free(buf); return 500; }

    out->raw = buf; out->rawlen = len;
    out->headers = headers;
    out->body = body;
    return 0;
}

// ---------------------------------------------------------------------------
// Binary-safe file read for responses (used for static files)
// ---------------------------------------------------------------------------
static char *wb_read_file_bytes(const char *path, size_t *outlen) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return NULL;
    if (st.st_size < 0 || (unsigned long long)st.st_size > (unsigned long long)WB_MAX_STATIC_FILE) return NULL;
    size_t size = (size_t)st.st_size;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *data = malloc(size + 1);
    if (!data) { fclose(f); return NULL; }
    size_t got = size ? fread(data, 1, size, f) : 0;
    fclose(f);
    if (got != size) { free(data); return NULL; }
    data[size] = '\0';
    *outlen = size;
    return data;
}

// ---------------------------------------------------------------------------
// Response builder
//
// Builds the whole HTTP response (returns a malloc'd buffer, *out_len bytes —
// it may contain NUL bytes, so never use strlen on it).
//   * no fixed-size line buffers: long headers/cookies are written in full
//   * a header or cookie containing CR, LF, NUL or another control character,
//     or a header whose name is not a valid token, is DROPPED (and logged)
//     rather than being sent: that is what stops response-header injection
//   * content-length / transfer-encoding / connection cannot be overridden
//     from the headers map
//   * the body length comes from the string's own length, not strlen
//   * res["body_file"] (set by serve_static) is read here as raw bytes, so
//     PNG/JPG/PDF/... are sent byte-for-byte
// ---------------------------------------------------------------------------
static int wb_header_name_ok(const char *s) {
    if (!*s) return 0;
    for (; *s; s++) if (!wb_is_tchar((unsigned char)*s)) return 0;
    return 1;
}
static int wb_header_value_ok(const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c < 0x20 && c != '\t') || c == 0x7f) return 0;
    }
    return 1;
}

static char *wb_build_http_response(Value *res_map, size_t *out_len) {
    Value *status_v = map_get(res_map, "status");
    Value *body_v   = map_get(res_map, "body");
    Value *ct_v     = map_get(res_map, "content_type");
    Value *hdrs_v   = map_get(res_map, "headers");
    Value *file_v   = map_get(res_map, "body_file");

    long status = status_v ? (long)status_v->as.number : 200;
    if (status < 100 || status > 599) status = 500;
    const char *body = (body_v && body_v->type == VAL_STRING) ? body_v->as.string : "";
    size_t body_len  = (body_v && body_v->type == VAL_STRING) ? str_length(body_v->as.string) : 0;
    const char *ct   = (ct_v && ct_v->type == VAL_STRING) ? ct_v->as.string : "text/plain; charset=utf-8";
    if (!wb_header_value_ok(ct)) {
        fprintf(stderr, "[webi] dropped an unsafe Content-Type (control characters)\n");
        ct = "text/plain; charset=utf-8";
    }

    char *file_data = NULL;
    if (file_v && file_v->type == VAL_STRING) {
        size_t flen = 0;
        file_data = wb_read_file_bytes(file_v->as.string, &flen);
        if (file_data) {
            body = file_data; body_len = flen;
        } else {
            fprintf(stderr, "[webi] static file '%s' could not be read — sending 500\n", file_v->as.string);
            status = 500;
            body = "500 Internal Server Error"; body_len = strlen(body);
            ct = "text/plain; charset=utf-8";
        }
    }
    if (body_len > (size_t)INT_MAX / 4) {
        status = 500; body = "500 Internal Server Error"; body_len = strlen(body);
        ct = "text/plain; charset=utf-8";
    }

    char *out = NULL; int olen = 0, ocap = 0;
    char num[64];

    snprintf(num, sizeof(num), "HTTP/1.1 %ld ", status);
    wb_buf_str(&out, &olen, &ocap, num);
    wb_buf_str(&out, &olen, &ocap, wb_reason(status));
    wb_buf_str(&out, &olen, &ocap, "\r\n");
    wb_buf_str(&out, &olen, &ocap, "Server: webi/1.3.0 (Kosh)\r\n");
    wb_buf_str(&out, &olen, &ocap, "Connection: close\r\n");
    wb_buf_str(&out, &olen, &ocap, "Content-Type: ");
    wb_buf_str(&out, &olen, &ocap, ct);
    wb_buf_str(&out, &olen, &ocap, "\r\n");
    snprintf(num, sizeof(num), "Content-Length: %zu\r\n", body_len);
    wb_buf_str(&out, &olen, &ocap, num);

    if (hdrs_v && hdrs_v->type == VAL_MAP) {
        for (int i = 0; i < AS_MAP_COUNT(*hdrs_v); i++) {
            const char *hk = AS_MAP_ENTRIES(*hdrs_v)[i].key;
            Value *hv_ptr = AS_MAP_ENTRIES(*hdrs_v)[i].value;
            if (!hv_ptr || hv_ptr->type != VAL_STRING) continue;
            const char *hv = hv_ptr->as.string;
            if (!wb_header_name_ok(hk) || !wb_header_value_ok(hv) ||
                wb_streq_ci(hk, "Content-Length") || wb_streq_ci(hk, "Transfer-Encoding") ||
                wb_streq_ci(hk, "Connection")) {
                fprintf(stderr, "[webi] dropped response header '%.40s' (invalid name/value or reserved)\n", hk);
                continue;
            }
            wb_buf_str(&out, &olen, &ocap, hk);
            wb_buf_str(&out, &olen, &ocap, ": ");
            wb_buf_str(&out, &olen, &ocap, hv);
            wb_buf_str(&out, &olen, &ocap, "\r\n");
        }
    }

    Value *cookies_v = map_get(res_map, "cookies");
    if (cookies_v && cookies_v->type == VAL_ARRAY) {
        for (int i = 0; i < AS_ARRAY_COUNT(*cookies_v); i++) {
            Value cv = AS_ARRAY_ITEMS(*cookies_v)[i];
            if (cv.type != VAL_STRING) continue;
            if (!wb_header_value_ok(cv.as.string) || !strchr(cv.as.string, '=')) {
                fprintf(stderr, "[webi] dropped a Set-Cookie value (control characters or no name=value)\n");
                continue;
            }
            wb_buf_str(&out, &olen, &ocap, "Set-Cookie: ");
            wb_buf_str(&out, &olen, &ocap, cv.as.string);
            wb_buf_str(&out, &olen, &ocap, "\r\n");
        }
    }

    wb_buf_str(&out, &olen, &ocap, "\r\n");
    if (body_len > 0) wb_buf_append(&out, &olen, &ocap, body, (int)body_len);
    free(file_data);
    *out_len = (size_t)olen;
    return out;
}


// ===========================================================================
// POSIX OUTBOUND REQUESTS (curl subprocess, same strategy as requests_lib.c)
// ===========================================================================
#ifndef _WIN32

static char *wb_run_curl(const char **args, int nargs) {
    size_t cmd_len = 16;
    for (int i = 0; i < nargs; i++)
        cmd_len += strlen(args[i]) * 4 + 4;   /* worst case: every char is a quote -> 4 bytes ('\'') */
    char *cmd = malloc(cmd_len);
    strcpy(cmd, "curl -s");
    for (int i = 0; i < nargs; i++) {
        strcat(cmd, " '");
        char *p = cmd + strlen(cmd);
        for (const char *q = args[i]; *q; q++) {
            if (*q == '\'') { *p++ = '\''; *p++ = '\\'; *p++ = '\''; *p++ = '\''; }
            else { *p++ = *q; }
        }
        *p++ = '\''; *p = '\0';
    }
    FILE *fp = popen(cmd, "r");
    free(cmd);
    if (!fp) return strdup("");
    char *out = NULL; size_t out_len = 0, cap = 0;
    char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (out_len + n + 1 > cap) { cap = cap ? (cap + n) * 2 : 4096; out = realloc(out, cap); }
        memcpy(out + out_len, buf, n); out_len += n;
    }
    pclose(fp);
    if (!out) return strdup("");
    out[out_len] = '\0';
    return out;
}

static Value wb_curl_request(const char *method, const char *url,
                               const char *body, const char *ctype,
                               const char *extra_headers) {
#define WB_MAX_ARGS 32
    const char *args[WB_MAX_ARGS];
    int n = 0;
    args[n++] = "-w"; args[n++] = "\n__WB_STATUS__%{http_code}";
    if (method && strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0) {
        args[n++] = "-X"; args[n++] = method;
    }
    if (strcmp(method, "HEAD") == 0) {
        args[n++] = "-I";
    }
    if (body) { args[n++] = "-d"; args[n++] = body; }
    if (ctype) { args[n++] = "-H"; args[n++] = ctype; }
    // Extra headers: split on \r\n and add each as -H
    if (extra_headers && strlen(extra_headers) > 0) {
        // We pass a single combined header string; curl handles multiple -H flags
        // Make a mutable copy and split on \r\n
        char *hcopy = strdup(extra_headers);
        char *line = strtok(hcopy, "\r\n");
        while (line && n < WB_MAX_ARGS - 2) {
            if (strlen(line) > 3) { args[n++] = "-H"; args[n++] = line; }
            line = strtok(NULL, "\r\n");
        }
        // Note: hcopy leaks here for simplicity; fix with arena allocator later
    }
    args[n++] = url;

    char *raw = wb_run_curl(args, n);
    const char *marker = strstr(raw, "\n__WB_STATUS__");
    long status = 0; char *body_str;
    if (marker) {
        int blen = (int)(marker - raw);
        body_str = malloc(blen + 1);
        memcpy(body_str, raw, blen); body_str[blen] = '\0';
        status = atol(marker + (int)strlen("\n__WB_STATUS__"));
    } else {
        body_str = strdup(raw);
    }
    free(raw);

    Value m = value_map_empty();
    map_set(&m, "status",  value_number((double)status));
    map_set(&m, "body",    value_string(body_str));
    map_set(&m, "success", value_bool(status >= 200 && status < 300));
    free(body_str);
    return m;
}

// ---------------------------------------------------------------------------
// v1.1 Extended request natives — POSIX
// ---------------------------------------------------------------------------

void fn_http_get_h(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!wb_check(interp, "http_get_h", 2, argc)) { *result = value_nil(); return; }
    const char *url, *headers;
    if (!wb_str_arg(interp, "http_get_h", 0, args[0], &url)) { *result = value_nil(); return; }
    if (!wb_str_arg(interp, "http_get_h", 1, args[1], &headers)) { *result = value_nil(); return; }
    *result = wb_curl_request("GET", url, NULL, NULL, headers);
}

void fn_http_post_h(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!wb_check(interp, "http_post_h", 3, argc)) { *result = value_nil(); return; }
    const char *url, *body, *headers;
    if (!wb_str_arg(interp, "http_post_h", 0, args[0], &url))     { *result = value_nil(); return; }
    if (!wb_str_arg(interp, "http_post_h", 1, args[1], &body))    { *result = value_nil(); return; }
    if (!wb_str_arg(interp, "http_post_h", 2, args[2], &headers)) { *result = value_nil(); return; }
    *result = wb_curl_request("POST", url, body, NULL, headers);
}

void fn_http_put_h(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!wb_check(interp, "http_put_h", 3, argc)) { *result = value_nil(); return; }
    const char *url, *body, *headers;
    if (!wb_str_arg(interp, "http_put_h", 0, args[0], &url))     { *result = value_nil(); return; }
    if (!wb_str_arg(interp, "http_put_h", 1, args[1], &body))    { *result = value_nil(); return; }
    if (!wb_str_arg(interp, "http_put_h", 2, args[2], &headers)) { *result = value_nil(); return; }
    *result = wb_curl_request("PUT", url, body, NULL, headers);
}

void fn_http_put_json(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!wb_check(interp, "http_put_json", 2, argc)) { *result = value_nil(); return; }
    const char *url;
    if (!wb_str_arg(interp, "http_put_json", 0, args[0], &url)) { *result = value_nil(); return; }
    char *json_body = NULL; int jlen = 0, jcap = 0;
    wb_json_encode(args[1], &json_body, &jlen, &jcap);
    *result = wb_curl_request("PUT", url, json_body ? json_body : "{}", "Content-Type: application/json", NULL);
    free(json_body);
}

void fn_http_patch(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!wb_check(interp, "http_patch", 2, argc)) { *result = value_nil(); return; }
    const char *url, *body;
    if (!wb_str_arg(interp, "http_patch", 0, args[0], &url))  { *result = value_nil(); return; }
    if (!wb_str_arg(interp, "http_patch", 1, args[1], &body)) { *result = value_nil(); return; }
    *result = wb_curl_request("PATCH", url, body, "Content-Type: application/x-www-form-urlencoded", NULL);
}

void fn_http_patch_json(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!wb_check(interp, "http_patch_json", 2, argc)) { *result = value_nil(); return; }
    const char *url;
    if (!wb_str_arg(interp, "http_patch_json", 0, args[0], &url)) { *result = value_nil(); return; }
    char *json_body = NULL; int jlen = 0, jcap = 0;
    wb_json_encode(args[1], &json_body, &jlen, &jcap);
    *result = wb_curl_request("PATCH", url, json_body ? json_body : "{}", "Content-Type: application/json", NULL);
    free(json_body);
}

void fn_http_delete_h(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!wb_check(interp, "http_delete_h", 2, argc)) { *result = value_nil(); return; }
    const char *url, *headers;
    if (!wb_str_arg(interp, "http_delete_h", 0, args[0], &url))     { *result = value_nil(); return; }
    if (!wb_str_arg(interp, "http_delete_h", 1, args[1], &headers)) { *result = value_nil(); return; }
    *result = wb_curl_request("DELETE", url, NULL, NULL, headers);
}

void fn_http_head(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!wb_check(interp, "http_head", 1, argc)) { *result = value_nil(); return; }
    const char *url;
    if (!wb_str_arg(interp, "http_head", 0, args[0], &url)) { *result = value_nil(); return; }
    *result = wb_curl_request("HEAD", url, NULL, NULL, NULL);
}

// ===========================================================================
// HTTP SERVER — POSIX (raw sockets, single-threaded, HTTP/1.1)
// ===========================================================================

// ---------------------------------------------------------------------------
// fn_http_serve — the main server loop
// Signature (Kosh): http_serve(port, app)
//
// The server:
//   1. Binds to 0.0.0.0:port
//   2. Accepts connections in a loop
//   3. Parses each HTTP request
//   4. Calls Kosh's webi_handle(app, method, path, query, headers, body)
//   5. Builds the HTTP response from the returned map
//   6. Sends it back and closes the connection
// ---------------------------------------------------------------------------
void fn_http_serve(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!wb_check(interp, "http_serve", 2, argc)) { *result = value_nil(); return; }

    if (args[0].type != VAL_NUMBER) {
        fprintf(stderr, "Runtime error: http_serve(port, app) — port must be a number\n");
        interp->had_runtime_error = 1; *result = value_nil(); return;
    }
    int port = (int)args[0].as.number;
    Value app = value_copy(args[1]);   // save the app map

    // Look up webi_handle in the current environment
    // We'll call it directly through the interpreter

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("[webi] socket()");
        interp->had_runtime_error = 1; *result = value_nil(); return;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (!wb_load_limits(&app)) { close(server_fd); value_free(app); interp->had_runtime_error = 1; *result = value_nil(); return; }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    if (!wb_parse_host(&app, &addr.sin_addr)) { close(server_fd); value_free(app); interp->had_runtime_error = 1; *result = value_nil(); return; }
    addr.sin_port        = htons((uint16_t)port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[webi] bind()");
        close(server_fd);
        interp->had_runtime_error = 1; *result = value_nil(); return;
    }
    if (listen(server_fd, 64) < 0) {
        perror("[webi] listen()");
        close(server_fd);
        interp->had_runtime_error = 1; *result = value_nil(); return;
    }

    signal(SIGPIPE, SIG_IGN);       /* a client that disconnects mid-response must not kill the server */
    g_webi_env = interp->base_env;  /* store env for accept loop */
    printf("[webi] Listening on port %d (%s)\n", port, addr.sin_addr.s_addr == htonl(INADDR_ANY) ? "all interfaces" : "bound address only");
    fflush(stdout);

    // Accept loop
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("[webi] accept()");
            break;
        }

        // Timeouts so a client that connects and stalls (or stops reading our
        // response) can't hold this single-threaded server open forever.
        wb_set_timeouts_ms(client_fd, (unsigned long long)g_lim.read_timeout_s * 1000ULL,
                           (unsigned long long)g_lim.request_timeout_s * 1000ULL);

        // Read and strictly validate the request (see wb_read_request_strict)
        wb_request_t rq;
        int rq_status = wb_read_request_strict(client_fd, &rq);
        if (rq_status != 0) {
            if (rq_status > 0) wb_send_error(client_fd, rq_status);
            close(client_fd);
            continue;
        }
        char *raw = rq.raw;
        char *headers_str = rq.headers;
        char *body_str    = rq.body;
        char method[512], path[512], query[512];
        wb_parse_request_line(raw, method, path, query);
        char  client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));

        // Call webi_handle(app, method, path, query, headers, body) in Kosh
        // We need to look up the function from the interpreter environment.
        // We pass it via a synthetic AstNode call using interpreter_execute.
        // Simpler approach: look up "webi_handle" as a global native or Kosh fn
        // and call its underlying C NativeFn / Kosh fn body directly.

        // Build args array for webi_handle
        Value call_args[7];
        call_args[0] = value_copy(app);
        call_args[1] = value_string(method);
        call_args[2] = value_string(path);
        call_args[3] = value_string(query);
        call_args[4] = value_string(headers_str);
        call_args[5] = value_string(body_str);
        call_args[6] = value_string(client_ip);

        /* Call Kosh webi_handle(app, method, path, query, headers, body, ip) */
        Value res_map;
        // In VM mode, g_webi_env is NULL, but we have the VM pointer in 'interp'
        if (g_webi_env || interp) {
            res_map = wb_safe_call_webi_handle(interp, g_webi_env, call_args, 7, method, path);
        } else {
            res_map = value_map_empty();
            map_set(&res_map, "status",       value_number(500));
            map_set(&res_map, "body",         value_string("[webi] env not set"));
            map_set(&res_map, "content_type", value_string("text/plain"));
            map_set(&res_map, "headers",      value_map_empty());
        }
        size_t http_len = 0;
        char *http_response = wb_build_http_response(&res_map, &http_len);
        wb_send_all(client_fd, http_response, http_len);

        // Cleanup
        // NOTE: call_args are NOT freed here — kosh_call_fn() binds them as
        // parameters in the callee's environment, and env_free() (called
        // internally by kosh_call_fn after the function returns) already
        // owns and frees each parameter Value. Freeing them again here
        // would be a double-free.
        free(http_response);
        free(raw); free(headers_str); free(body_str);
        value_free(res_map);
        close(client_fd);

        // A runtime error inside a single request is now contained and
        // logged by wb_safe_call_webi_handle() above — the server keeps
        // accepting new connections instead of shutting down.
    }

    close(server_fd);
    value_free(app);
    *result = value_nil();
}

#endif // !_WIN32

// ===========================================================================
// WINDOWS — WinHTTP outbound + Winsock server
// ===========================================================================
#ifdef _WIN32

static Value wh_request(const char *method, const char *url,
                          const char *body, const char *ctype,
                          const char *extra_headers) {
    const char *hs = url;
    if      (strncmp(url, "https://", 8) == 0) hs = url + 8;
    else if (strncmp(url, "http://",  7) == 0) hs = url + 7;
    const char *slash = strchr(hs, '/');
    char *host; const char *path;
    if (slash) {
        int hl = (int)(slash - hs);
        host = malloc(hl + 1); strncpy(host, hs, hl); host[hl] = '\0';
        path = slash;
    } else { host = strdup(hs); path = "/"; }

    BOOL use_https = (strncmp(url, "https", 5) == 0);
    INTERNET_PORT port = use_https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;

    HINTERNET hSess = WinHttpOpen(L"Kosh-webi/1.1",
                                   WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    Value result = value_map_empty();
    map_set(&result, "status",  value_number(0));
    map_set(&result, "body",    value_string(""));
    map_set(&result, "success", value_bool(0));
    if (!hSess) { free(host); return result; }

    int wl = MultiByteToWideChar(CP_UTF8, 0, host, -1, NULL, 0);
    wchar_t *whost = malloc(wl * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, host, -1, whost, wl);

    int pl = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    wchar_t *wpath = malloc(pl * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, pl);

    int ml = MultiByteToWideChar(CP_UTF8, 0, method, -1, NULL, 0);
    wchar_t *wmethod = malloc(ml * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, method, -1, wmethod, ml);

    HINTERNET hConn = WinHttpConnect(hSess, whost, port, 0);
    if (hConn) {
        HINTERNET hReq = WinHttpOpenRequest(hConn, wmethod, wpath, NULL, NULL, NULL,
                                             use_https ? WINHTTP_FLAG_SECURE : 0);
        if (hReq) {
            if (ctype) {
                int cl = MultiByteToWideChar(CP_UTF8, 0, ctype, -1, NULL, 0);
                wchar_t *wct = malloc(cl * sizeof(wchar_t));
                MultiByteToWideChar(CP_UTF8, 0, ctype, -1, wct, cl);
                WinHttpAddRequestHeaders(hReq, wct, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
                free(wct);
            }
            if (extra_headers) {
                int el = MultiByteToWideChar(CP_UTF8, 0, extra_headers, -1, NULL, 0);
                wchar_t *weh = malloc(el * sizeof(wchar_t));
                MultiByteToWideChar(CP_UTF8, 0, extra_headers, -1, weh, el);
                WinHttpAddRequestHeaders(hReq, weh, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
                free(weh);
            }
            DWORD blen = body ? (DWORD)strlen(body) : 0;
            if (WinHttpSendRequest(hReq, NULL, 0, (LPVOID)body, blen, blen, 0) &&
                WinHttpReceiveResponse(hReq, NULL)) {
                DWORD sc = 0, ssz = sizeof(sc);
                WinHttpQueryHeaders(hReq,
                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    NULL, &sc, &ssz, NULL);
                char *rbuf = NULL; DWORD tot = 0, br = 0, av = 0;
                while (WinHttpQueryDataAvailable(hReq, &av) && av > 0) {
                    rbuf = realloc(rbuf, tot + av + 1);
                    WinHttpReadData(hReq, rbuf + tot, av, &br);
                    tot += br; rbuf[tot] = '\0';
                }
                if (!rbuf) rbuf = strdup("");
                map_set(&result, "status",  value_number((double)sc));
                map_set(&result, "body",    value_string(rbuf));
                map_set(&result, "success", value_bool(sc >= 200 && sc < 300));
                free(rbuf);
            }
            WinHttpCloseHandle(hReq);
        }
        WinHttpCloseHandle(hConn);
    }
    free(whost); free(wpath); free(wmethod);
    WinHttpCloseHandle(hSess); free(host);
    return result;
}

void fn_http_get_h(Value *r, Interpreter *i, int argc, Value *args) {
    if (!wb_check(i, "http_get_h", 2, argc)) { *r = value_nil(); return; }
    const char *url, *h;
    if (!wb_str_arg(i, "http_get_h", 0, args[0], &url)) { *r = value_nil(); return; }
    if (!wb_str_arg(i, "http_get_h", 1, args[1], &h))   { *r = value_nil(); return; }
    *r = wh_request("GET", url, NULL, NULL, h);
}
void fn_http_post_h(Value *r, Interpreter *i, int argc, Value *args) {
    if (!wb_check(i, "http_post_h", 3, argc)) { *r = value_nil(); return; }
    const char *url, *body, *h;
    if (!wb_str_arg(i, "http_post_h", 0, args[0], &url))  { *r = value_nil(); return; }
    if (!wb_str_arg(i, "http_post_h", 1, args[1], &body)) { *r = value_nil(); return; }
    if (!wb_str_arg(i, "http_post_h", 2, args[2], &h))    { *r = value_nil(); return; }
    *r = wh_request("POST", url, body, NULL, h);
}
void fn_http_put_h(Value *r, Interpreter *i, int argc, Value *args) {
    if (!wb_check(i, "http_put_h", 3, argc)) { *r = value_nil(); return; }
    const char *url, *body, *h;
    if (!wb_str_arg(i, "http_put_h", 0, args[0], &url))  { *r = value_nil(); return; }
    if (!wb_str_arg(i, "http_put_h", 1, args[1], &body)) { *r = value_nil(); return; }
    if (!wb_str_arg(i, "http_put_h", 2, args[2], &h))    { *r = value_nil(); return; }
    *r = wh_request("PUT", url, body, NULL, h);
}
void fn_http_put_json(Value *r, Interpreter *i, int argc, Value *args) {
    if (!wb_check(i, "http_put_json", 2, argc)) { *r = value_nil(); return; }
    const char *url;
    if (!wb_str_arg(i, "http_put_json", 0, args[0], &url)) { *r = value_nil(); return; }
    char *j = NULL; int jl = 0, jc = 0;
    wb_json_encode(args[1], &j, &jl, &jc);
    *r = wh_request("PUT", url, j ? j : "{}", "Content-Type: application/json", NULL);
    free(j);
}
void fn_http_patch(Value *r, Interpreter *i, int argc, Value *args) {
    if (!wb_check(i, "http_patch", 2, argc)) { *r = value_nil(); return; }
    const char *url, *body;
    if (!wb_str_arg(i, "http_patch", 0, args[0], &url))  { *r = value_nil(); return; }
    if (!wb_str_arg(i, "http_patch", 1, args[1], &body)) { *r = value_nil(); return; }
    *r = wh_request("PATCH", url, body, "Content-Type: application/x-www-form-urlencoded", NULL);
}
void fn_http_patch_json(Value *r, Interpreter *i, int argc, Value *args) {
    if (!wb_check(i, "http_patch_json", 2, argc)) { *r = value_nil(); return; }
    const char *url;
    if (!wb_str_arg(i, "http_patch_json", 0, args[0], &url)) { *r = value_nil(); return; }
    char *j = NULL; int jl = 0, jc = 0;
    wb_json_encode(args[1], &j, &jl, &jc);
    *r = wh_request("PATCH", url, j ? j : "{}", "Content-Type: application/json", NULL);
    free(j);
}
void fn_http_delete_h(Value *r, Interpreter *i, int argc, Value *args) {
    if (!wb_check(i, "http_delete_h", 2, argc)) { *r = value_nil(); return; }
    const char *url, *h;
    if (!wb_str_arg(i, "http_delete_h", 0, args[0], &url)) { *r = value_nil(); return; }
    if (!wb_str_arg(i, "http_delete_h", 1, args[1], &h))   { *r = value_nil(); return; }
    *r = wh_request("DELETE", url, NULL, NULL, h);
}
void fn_http_head(Value *r, Interpreter *i, int argc, Value *args) {
    if (!wb_check(i, "http_head", 1, argc)) { *r = value_nil(); return; }
    const char *url;
    if (!wb_str_arg(i, "http_head", 0, args[0], &url)) { *r = value_nil(); return; }
    *r = wh_request("HEAD", url, NULL, NULL, NULL);
}

// Windows server — Winsock + real webi_handle call
void fn_http_serve(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!wb_check(interp, "http_serve", 2, argc)) { *result = value_nil(); return; }
    if (args[0].type != VAL_NUMBER) { *result = value_nil(); return; }
    int port = (int)args[0].as.number;
    Value app = value_copy(args[1]);

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv == INVALID_SOCKET) {
        fprintf(stderr, "[webi] socket() failed: %d\n", WSAGetLastError());
        WSACleanup(); *result = value_nil(); return;
    }
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

    if (!wb_load_limits(&app)) { closesocket(srv); WSACleanup(); value_free(app); *result = value_nil(); return; }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    if (!wb_parse_host(&app, &addr.sin_addr)) { closesocket(srv); WSACleanup(); value_free(app); *result = value_nil(); return; }
    addr.sin_port = htons((u_short)port);
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        fprintf(stderr, "[webi] bind() failed: %d\n", WSAGetLastError());
        closesocket(srv); WSACleanup(); value_free(app); *result = value_nil(); return;
    }
    listen(srv, 64);

    if (interp) g_webi_env = interp->base_env;
    printf("[webi] Listening on port %d (%s)\n", port, addr.sin_addr.s_addr == htonl(INADDR_ANY) ? "all interfaces" : "bound address only");
    fflush(stdout);

    while (1) {
        struct sockaddr_in client_addr;
        int client_len = sizeof(client_addr);
        SOCKET cli = accept(srv, (struct sockaddr *)&client_addr, &client_len);
        if (cli == INVALID_SOCKET) continue;

        // Timeouts so a client that connects and stalls (or stops reading our
        // response) can't hold this single-threaded server open forever.
        wb_set_timeouts_ms(cli, (unsigned long long)g_lim.read_timeout_s * 1000ULL,
                           (unsigned long long)g_lim.request_timeout_s * 1000ULL);

        // Read and strictly validate the request (see wb_read_request_strict)
        wb_request_t rq;
        int rq_status = wb_read_request_strict(cli, &rq);
        if (rq_status != 0) {
            if (rq_status > 0) wb_send_error(cli, rq_status);
            closesocket(cli);
            continue;
        }
        char *raw = rq.raw;
        char *headers_str = rq.headers;
        char *body_str    = rq.body;
        char method[512], path[512], query[512];
        wb_parse_request_line(raw, method, path, query);

        char client_ip[INET6_ADDRSTRLEN] = "";
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));

        Value call_args[7];
        call_args[0] = value_copy(app);
        call_args[1] = value_string(method);
        call_args[2] = value_string(path);
        call_args[3] = value_string(query);
        call_args[4] = value_string(headers_str ? headers_str : "");
        call_args[5] = value_string(body_str    ? body_str    : "");
        call_args[6] = value_string(client_ip);

        Value res_map;
        // In VM mode, g_webi_env is NULL, but we have the VM pointer in 'interp'
        if (g_webi_env || interp) {
            res_map = wb_safe_call_webi_handle(interp, g_webi_env, call_args, 7, method, path);
        } else {
            res_map = value_map_empty();
            map_set(&res_map, "status",       value_number(500));
            map_set(&res_map, "body",         value_string("[webi] env not set"));
            map_set(&res_map, "content_type", value_string("text/plain"));
            map_set(&res_map, "headers",      value_map_empty());
        }

        size_t http_len = 0;
        char *http_response = wb_build_http_response(&res_map, &http_len);
        wb_send_all(cli, http_response, http_len);

        // NOTE: call_args are NOT freed here — see POSIX loop comment above;
        // kosh_call_fn's internal env_free already owns/frees each param.
        free(http_response);
        free(raw);
        if (headers_str) free(headers_str);
        if (body_str)    free(body_str);
        value_free(res_map);
        closesocket(cli);

        // A runtime error inside a single request is now contained and
        // logged by wb_safe_call_webi_handle() above — the server keeps
        // accepting new connections instead of shutting down.
    }
    closesocket(srv);
    WSACleanup();
    value_free(app);
    *result = value_nil();
}
#endif // _WIN32

// ===========================================================================
// Registration
// ===========================================================================
// ===========================================================================
// Static file serving support (Phase 3): MIME type table + a
// traversal-safe path resolver shared by serve_static()/render_file() on
// the Kosh side (routing.kh/template.kh). Every untrusted path segment —
// the part of the URL after a serve_static mount prefix, or a template
// path passed to render_file() — goes through _webi_safe_static_path()
// below before anything ever touches the filesystem. That's the one
// choke point responsible for blocking path traversal; nothing upstream
// of it is trusted to have already sanitized the input.
// ===========================================================================

// Minimal percent-decoder: turns "%XX" into the corresponding byte.
// Deliberately leaves '+' alone — that's form-encoding, not URL-path
// encoding, and decoding it to a space here would be wrong. This exists
// specifically so a traversal attempt smuggled in encoded, e.g.
// "%2e%2e%2fetc%2fpasswd", gets turned into "../etc/passwd" *before* the
// realpath-based containment check runs below — checking the raw encoded
// string would miss it entirely, since "%2e%2e" doesn't look like ".."
// until it's decoded.
static char *wb_url_decode(const char *s) {
    size_t len = strlen(s);
    char *out = malloc(len + 1);
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '%' && i + 2 < len &&
            isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            char hex[3] = { s[i + 1], s[i + 2], '\0' };
            int byte = (int)strtol(hex, NULL, 16);
            // A decoded NUL would silently truncate every C-string
            // operation downstream (path join, comparisons, ...) and
            // could let an attacker hide a bogus suffix after it — treat
            // it as the literal two characters instead of decoding it.
            if (byte == 0) {
                out[j++] = s[i];
            } else {
                out[j++] = (char)byte;
                i += 2;
            }
        } else {
            out[j++] = s[i];
        }
    }
    out[j] = '\0';
    return out;
}

typedef struct { const char *ext; const char *mime; } wb_mime_entry_t;

static const wb_mime_entry_t WB_MIME_TABLE[] = {
    { ".html",  "text/html; charset=utf-8" },
    { ".htm",   "text/html; charset=utf-8" },
    { ".css",   "text/css; charset=utf-8" },
    { ".js",    "application/javascript; charset=utf-8" },
    { ".mjs",   "application/javascript; charset=utf-8" },
    { ".json",  "application/json" },
    { ".txt",   "text/plain; charset=utf-8" },
    { ".xml",   "application/xml" },
    { ".csv",   "text/csv; charset=utf-8" },
    { ".svg",   "image/svg+xml" },
    { ".png",   "image/png" },
    { ".jpg",   "image/jpeg" },
    { ".jpeg",  "image/jpeg" },
    { ".gif",   "image/gif" },
    { ".ico",   "image/x-icon" },
    { ".webp",  "image/webp" },
    { ".woff",  "font/woff" },
    { ".woff2", "font/woff2" },
    { ".ttf",   "font/ttf" },
    { ".pdf",   "application/pdf" },
    { ".mp4",   "video/mp4" },
    { ".mp3",   "audio/mpeg" },
    { ".wasm",  "application/wasm" },
};
#define WB_MIME_TABLE_LEN ((int)(sizeof(WB_MIME_TABLE) / sizeof(WB_MIME_TABLE[0])))

static int wb_streq_ci(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

static const char *wb_mime_for_path(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    for (int i = 0; i < WB_MIME_TABLE_LEN; i++) {
        if (wb_streq_ci(dot, WB_MIME_TABLE[i].ext)) return WB_MIME_TABLE[i].mime;
    }
    return "application/octet-stream";
}

// Kosh: _webi_mime_type(path) -> string
void fn_webi_mime_type(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!wb_check(interp, "_webi_mime_type", 1, argc)) { *result = value_nil(); return; }
    const char *path;
    if (!wb_str_arg(interp, "_webi_mime_type", 0, args[0], &path)) { *result = value_nil(); return; }
    *result = value_string(wb_mime_for_path(path));
}

// Resolves `path` to a canonical, existence-verified absolute path.
// POSIX: realpath() resolves ".."/"." AND symlinks, and fails if the
// path doesn't exist — exactly the semantics we want for "does this
// really point where it looks like it points".
// Windows: GetFullPathNameA only does lexical "..'"/"." normalization —
// it doesn't resolve symlinks/junctions and doesn't require the path to
// exist, so callers here separately stat() the result afterward.
static int wb_realpath(const char *path, char *out, size_t out_cap) {
#ifdef _WIN32
    DWORD n = GetFullPathNameA(path, (DWORD)out_cap, out, NULL);
    return (n > 0 && n < out_cap);
#else
    char resolved[PATH_MAX];
    if (!realpath(path, resolved)) return 0;
    if (strlen(resolved) >= out_cap) return 0;
    strcpy(out, resolved);
    return 1;
#endif
}

// Kosh: _webi_script_dir() -> string
//
// Same fallback order as _webi_resolve_path, exposed on its own so
// callers that need "the directory of whichever .kh file is currently
// running" as a value (e.g. render_file()'s containment boundary) don't
// have to fake it by resolving ".".
void fn_webi_script_dir(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)args;
    if (!wb_check(interp, "_webi_script_dir", 0, argc)) { *result = value_nil(); return; }
    if (interp->current_import_dir[0] != '\0') {
        *result = value_string(interp->current_import_dir);
    } else if (interp->base_path) {
        *result = value_string(interp->base_path);
    } else {
        *result = value_string(".");
    }
}

// Kosh: _webi_resolve_path(path) -> string
//
// Directory-aware resolution for a file path an app author wrote in
// their own source (a template path for render_file(), a folder for
// serve_static()) — same fallback order Phase 2 already uses for
// `import`: an absolute path is left alone; otherwise resolve relative
// to whatever file is currently executing (current_import_dir) if we're
// inside one, else relative to the top-level script's own directory
// (base_path), so it works the same regardless of what directory `kosh`
// happens to be invoked from. Falls back to the path unchanged (resolved
// relative to the process's current directory) if neither is set.
void fn_webi_resolve_path(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!wb_check(interp, "_webi_resolve_path", 1, argc)) { *result = value_nil(); return; }
    const char *path;
    if (!wb_str_arg(interp, "_webi_resolve_path", 0, args[0], &path)) { *result = value_nil(); return; }

    // Already absolute (POSIX "/..." or Windows "C:\..." / "\\...") — use as-is.
    if (path[0] == '/' || path[0] == '\\' || (strlen(path) >= 2 && path[1] == ':')) {
        *result = value_string(path);
        return;
    }

    char joined[2048];
    if (interp->current_import_dir[0] != '\0') {
        snprintf(joined, sizeof(joined), "%s/%s", interp->current_import_dir, path);
    } else if (interp->base_path) {
        snprintf(joined, sizeof(joined), "%s/%s", interp->base_path, path);
    } else {
        snprintf(joined, sizeof(joined), "%s", path);
    }
    *result = value_string(joined);
}


//
// `folder` is the trusted mount root (a path the app author wrote in
// their own source). `rel` is untrusted — either the tail of a URL after
// a serve_static() mount prefix, or a template filename passed to
// render_file(). Returns the real, existence-verified absolute path to
// serve if (and only if) it resolves to somewhere inside `folder`;
// returns nil for anything else — traversal attempts (encoded or not),
// absolute-path/drive-letter escapes, symlink escapes (POSIX), missing
// files, or directories.
void fn_webi_safe_static_path(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!wb_check(interp, "_webi_safe_static_path", 2, argc)) { *result = value_nil(); return; }
    const char *folder, *rel;
    if (!wb_str_arg(interp, "_webi_safe_static_path", 0, args[0], &folder)) { *result = value_nil(); return; }
    if (!wb_str_arg(interp, "_webi_safe_static_path", 1, args[1], &rel))    { *result = value_nil(); return; }

    char *decoded = wb_url_decode(rel);

    // Reject anything that looks like it's trying to escape the "relative
    // to folder" contract outright, rather than relying solely on the
    // containment check after the join — an absolute path or a Windows
    // drive letter has no business being "the rest of a mounted URL".
    if (decoded[0] == '/' || decoded[0] == '\\' ||
        (strlen(decoded) >= 2 && decoded[1] == ':')) {
        free(decoded);
        *result = value_nil();
        return;
    }

    char candidate[2048];
    int wrote = snprintf(candidate, sizeof(candidate), "%s/%s", folder, decoded);
    free(decoded);
    if (wrote < 0 || (size_t)wrote >= sizeof(candidate)) { *result = value_nil(); return; }

    char folder_real[2048], candidate_real[2048];
    if (!wb_realpath(folder, folder_real, sizeof(folder_real))) {
        // The mounted folder itself doesn't exist — an app config bug,
        // not a request to serve.
        *result = value_nil();
        return;
    }
    if (!wb_realpath(candidate, candidate_real, sizeof(candidate_real))) {
        // Doesn't exist (POSIX) or path malformed (Windows) — 404 either way.
        *result = value_nil();
        return;
    }

    // Containment check: candidate_real must equal folder_real, or sit
    // strictly inside it (folder_real + separator + ...). Comparing the
    // prefix alone without also requiring that separator would wrongly
    // let a sibling folder like "/mnt/static-evil" pass a check meant for
    // "/mnt/static".
    size_t flen = strlen(folder_real);
#ifdef _WIN32
    int contained = _strnicmp(candidate_real, folder_real, flen) == 0 &&
#else
    int contained = strncmp(candidate_real, folder_real, flen) == 0 &&
#endif
                    (candidate_real[flen] == '\0' ||
                     candidate_real[flen] == '/'  ||
                     candidate_real[flen] == '\\');
    if (!contained) {
        fprintf(stderr,
                "[webi] serve_static: rejected path traversal attempt ('%s' resolves outside '%s')\n",
                rel, folder);
        *result = value_nil();
        return;
    }

    struct stat st;
    if (stat(candidate_real, &st) != 0 || !S_ISREG(st.st_mode)) {
        // Missing, or a directory — this feature serves files, not
        // directory listings.
        *result = value_nil();
        return;
    }

    *result = value_string(candidate_real);
}

// ===========================================================================
// secure_token — cryptographically-random hex token (v1.1.1 security)
//
// Kosh's existing random() is seeded rand() — fine for game logic, but
// predictable enough that it must never be used for session IDs, CSRF
// tokens, password reset links, or anything else where an attacker being
// able to guess the value matters. This pulls from the OS's actual CSPRNG
// instead (/dev/urandom on POSIX, CryptGenRandom on Windows).
// ===========================================================================
static int wb_fill_random_bytes(unsigned char *buf, int n) {
#ifdef _WIN32
    HCRYPTPROV hProv = 0;
    if (!CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        return 0;
    }
    BOOL ok = CryptGenRandom(hProv, (DWORD)n, buf);
    CryptReleaseContext(hProv, 0);
    return ok ? 1 : 0;
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) return 0;
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    return got == (size_t)n;
#endif
}

// Kosh: secure_token(num_bytes = 32) -> hex string (num_bytes*2 chars)
void fn_secure_token(Value *result, Interpreter *interp, int argc, Value *args) {
    int nbytes = 32; /* 256 bits by default */
    if (argc >= 1) {
        if (args[0].type != VAL_NUMBER) {
            fprintf(stderr, "Runtime error: secure_token() argument must be a number\n");
            interp->had_runtime_error = 1; *result = value_nil(); return;
        }
        nbytes = (int)args[0].as.number;
        if (nbytes < 1)    nbytes = 1;
        if (nbytes > 1024) nbytes = 1024; /* sane upper bound */
    }

    unsigned char *buf = malloc((size_t)nbytes);
    if (!wb_fill_random_bytes(buf, nbytes)) {
        fprintf(stderr, "Runtime error: secure_token() could not read system randomness\n");
        free(buf);
        interp->had_runtime_error = 1; *result = value_nil(); return;
    }

    static const char hex_digits[] = "0123456789abcdef";
    char *out = malloc((size_t)nbytes * 2 + 1);
    for (int i = 0; i < nbytes; i++) {
        out[i * 2]     = hex_digits[(buf[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex_digits[buf[i] & 0xF];
    }
    out[nbytes * 2] = '\0';
    free(buf);

    *result = value_string(out);
    free(out);
}

// Kosh: env_var(name, default = nil) -> the process environment variable, as a string
void fn_env_var(Value *result, Interpreter *interp, int argc, Value *args) {
    if (argc < 1 || args[0].type != VAL_STRING) {
        fprintf(stderr, "Runtime error: env_var(name, default = nil) needs a string name\n");
        interp->had_runtime_error = 1; *result = value_nil(); return;
    }
    const char *v = getenv(args[0].as.string);
    if (v) { *result = value_string(v); return; }
    *result = (argc >= 2) ? value_copy(args[1]) : value_nil();
}

// ===========================================================================
// rate_limit_check — simple in-memory, per-key sliding-window rate limiter
// (v1.1.1 security)
//
// State has to live here in C, not in a Kosh-visible map, because the
// native server passes http_serve a *copy* of the app map on every single
// request (see call_args[0] = value_copy(app) above) — anything mutated
// on a Kosh map during one request is gone by the next one. This table is
// a real process-lifetime C global, so counts actually persist across
// requests the way a rate limiter needs them to.
//
// Limitations (documented, not silent): fixed-size table (4096 keys).
// Entries whose window has expired are reused, so ordinary traffic never
// fills the table. If more than 4096 DISTINCT keys are live at once (a
// flood from many addresses), the entry with the lowest count (oldest on a
// tie) is evicted to make room: keys that are already over the limit have
// the highest counts and stay blocked, while flood entries (count 1) are
// the ones that get dropped. The limiter never fails open for everyone.
// Resets on server restart. Not shared across multiple server
// processes/machines. Good enough for a single-instance app;
// swap for a real store (Redis etc.) if you scale beyond that.
// ===========================================================================
#define WB_RATE_LIMIT_BUCKETS 4096
#define WB_RATE_LIMIT_KEY_LEN 128

typedef struct {
    char key[WB_RATE_LIMIT_KEY_LEN];
    int in_use;
    int count;
    int window;            // window_seconds this entry was created with
    time_t window_start;
} wb_rate_entry_t;

static wb_rate_entry_t g_rate_table[WB_RATE_LIMIT_BUCKETS];

static unsigned long wb_hash_str(const char *s) {
    unsigned long h = 5381;
    int c;
    while ((c = (unsigned char)*s++)) h = ((h << 5) + h) + (unsigned long)c;
    return h;
}

// Returns 1 if this call is within the limit (and counts it), 0 if the
// caller identified by `key` has exceeded `max_requests` within the last
// `window_seconds`.
static int wb_rate_limit_check(const char *key, int max_requests, int window_seconds) {
    if (max_requests <= 0) max_requests = 1;
    if (window_seconds <= 0) window_seconds = 1;

    unsigned long h = wb_hash_str(key) % WB_RATE_LIMIT_BUCKETS;
    time_t now = time(NULL);

    int reuse = -1;          // first expired slot seen on the probe chain
    int empty = -1;          // first never-used slot (end of the chain)

    for (int i = 0; i < WB_RATE_LIMIT_BUCKETS; i++) {
        int idx = (int)((h + (unsigned long)i) % WB_RATE_LIMIT_BUCKETS);
        wb_rate_entry_t *e = &g_rate_table[idx];

        if (!e->in_use) { empty = idx; break; }

        if (strncmp(e->key, key, WB_RATE_LIMIT_KEY_LEN - 1) == 0) {
            if ((long)(now - e->window_start) >= window_seconds) {
                e->window_start = now;
                e->window = window_seconds;
                e->count = 1;
                return 1;
            }
            if (e->count >= max_requests) return 0;
            e->count++;
            return 1;
        }
        if (reuse < 0 && (long)(now - e->window_start) >= e->window) reuse = idx;
    }

    // Key not in the table: pick a slot for it. Entries are never removed
    // (that would break probe chains), only overwritten, so a chain stays
    // intact.
    int slot = -1;
    if (reuse >= 0)      slot = reuse;      // an expired entry
    else if (empty >= 0) slot = empty;      // a never-used slot
    else {
        // Table full of live entries: evict the least significant one.
        int best = 0;
        for (int i = 1; i < WB_RATE_LIMIT_BUCKETS; i++) {
            wb_rate_entry_t *c = &g_rate_table[i], *b2 = &g_rate_table[best];
            if (c->count < b2->count ||
                (c->count == b2->count && c->window_start < b2->window_start)) best = i;
        }
        slot = best;
    }
    wb_rate_entry_t *e = &g_rate_table[slot];
    e->in_use = 1;
    strncpy(e->key, key, WB_RATE_LIMIT_KEY_LEN - 1);
    e->key[WB_RATE_LIMIT_KEY_LEN - 1] = '\0';
    e->window = window_seconds;
    e->window_start = now;
    e->count = 1;
    return 1;
}

// Kosh: rate_limit_check(key, max_requests, window_seconds) -> bool
void fn_rate_limit_check(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!wb_check(interp, "rate_limit_check", 3, argc)) { *result = value_nil(); return; }
    const char *key;
    if (!wb_str_arg(interp, "rate_limit_check", 0, args[0], &key)) { *result = value_nil(); return; }
    if (args[1].type != VAL_NUMBER || args[2].type != VAL_NUMBER) {
        fprintf(stderr, "Runtime error: rate_limit_check(key, max_requests, window_seconds) — "
                        "max_requests and window_seconds must be numbers\n");
        interp->had_runtime_error = 1; *result = value_nil(); return;
    }
    int max_requests   = (int)args[1].as.number;
    int window_seconds = (int)args[2].as.number;

    int allowed = wb_rate_limit_check(key, max_requests, window_seconds);
    *result = value_bool(allowed);
}

// ===========================================================================
// DNS helpers for verifying search-engine crawlers (forward-confirmed
// reverse DNS). A claim in the User-Agent header proves nothing; the
// crawler's *address* has to reverse-resolve to a hostname in the search
// engine's domain, and that hostname has to resolve back to the same
// address. These two natives are the building blocks; the policy (which
// domains, caching, budgets) lives in packages/webi/bot.kh.
//
// NOTE: getnameinfo()/getaddrinfo() block. webi's server is single-threaded,
// so bot.kh only calls them for unverified crawler claims, behind a cache
// and a lookup budget.
// ===========================================================================

static void wb_net_init(void) {
#ifdef _WIN32
    static int done = 0;
    if (!done) { WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa); done = 1; }
#endif
}

// Parses a textual IPv4/IPv6 address. Returns 1 on success.
static int wb_parse_ip(const char *s, struct sockaddr_storage *ss, socklen_t *len) {
    if (!s || strlen(s) == 0 || strlen(s) > 45) return 0;
    memset(ss, 0, sizeof(*ss));
    struct sockaddr_in *a4 = (struct sockaddr_in *)ss;
    if (inet_pton(AF_INET, s, &a4->sin_addr) == 1) {
        a4->sin_family = AF_INET; *len = (socklen_t)sizeof(*a4); return 1;
    }
    struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)ss;
    memset(ss, 0, sizeof(*ss));
    if (inet_pton(AF_INET6, s, &a6->sin6_addr) == 1) {
        a6->sin6_family = AF_INET6; *len = (socklen_t)sizeof(*a6); return 1;
    }
    return 0;
}

// Hostnames are checked before they reach the resolver or come back to Kosh:
// letters, digits, '.', '-', '_' only, at most 253 chars.
static int wb_valid_hostname(const char *h) {
    size_t n = strlen(h);
    if (n == 0 || n > 253) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)h[i];
        if (!(isalnum(c) || c == '.' || c == '-' || c == '_')) return 0;
    }
    return 1;
}

// Kosh: dns_reverse(ip) -> hostname string | nil
void fn_dns_reverse(Value *result, Interpreter *interp, int argc, Value *args) {
    *result = value_nil();
    if (!wb_check(interp, "dns_reverse", 1, argc)) return;
    const char *ip;
    if (!wb_str_arg(interp, "dns_reverse", 0, args[0], &ip)) return;
    struct sockaddr_storage ss; socklen_t sl = 0;
    if (!wb_parse_ip(ip, &ss, &sl)) return;
    wb_net_init();
    char host[1025];   // NI_MAXHOST is not exposed under strict -std=c11
    if (getnameinfo((struct sockaddr *)&ss, sl, host, sizeof(host), NULL, 0, NI_NAMEREQD) != 0) return;
    if (!wb_valid_hostname(host)) return;
    *result = value_string(host);
}

// Kosh: dns_resolves_to(hostname, ip) -> bool
void fn_dns_resolves_to(Value *result, Interpreter *interp, int argc, Value *args) {
    *result = value_bool(0);
    if (!wb_check(interp, "dns_resolves_to", 2, argc)) return;
    const char *host, *ip;
    if (!wb_str_arg(interp, "dns_resolves_to", 0, args[0], &host)) return;
    if (!wb_str_arg(interp, "dns_resolves_to", 1, args[1], &ip)) return;
    if (!wb_valid_hostname(host)) return;
    struct sockaddr_storage want; socklen_t wl = 0;
    if (!wb_parse_ip(ip, &want, &wl)) return;
    wb_net_init();
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return;
    int found = 0;
    for (struct addrinfo *p = res; p && !found; p = p->ai_next) {
        if (p->ai_family == AF_INET && want.ss_family == AF_INET) {
            found = memcmp(&((struct sockaddr_in *)p->ai_addr)->sin_addr,
                           &((struct sockaddr_in *)&want)->sin_addr, 4) == 0;
        } else if (p->ai_family == AF_INET6 && want.ss_family == AF_INET6) {
            found = memcmp(&((struct sockaddr_in6 *)p->ai_addr)->sin6_addr,
                           &((struct sockaddr_in6 *)&want)->sin6_addr, 16) == 0;
        }
    }
    freeaddrinfo(res);
    *result = value_bool(found);
}

void webi_register_all(Environment *env) {
    // v1.1 extended request natives
    env_define(env, "http_get_h",      value_native("http_get_h",      fn_http_get_h));
    env_define(env, "http_post_h",     value_native("http_post_h",     fn_http_post_h));
    env_define(env, "http_put_h",      value_native("http_put_h",      fn_http_put_h));
    env_define(env, "http_put_json",   value_native("http_put_json",   fn_http_put_json));
    env_define(env, "http_patch",      value_native("http_patch",      fn_http_patch));
    env_define(env, "http_patch_json", value_native("http_patch_json", fn_http_patch_json));
    env_define(env, "http_delete_h",   value_native("http_delete_h",   fn_http_delete_h));
    env_define(env, "http_head",       value_native("http_head",       fn_http_head));

    // webi server
    env_define(env, "http_serve",      value_native("http_serve",      fn_http_serve));

    // security (v1.1.1)
    env_define(env, "secure_token",    value_native("secure_token",    fn_secure_token));
    env_define(env, "rate_limit_check",value_native("rate_limit_check",fn_rate_limit_check));
    env_define(env, "dns_reverse",     value_native("dns_reverse",     fn_dns_reverse));
    env_define(env, "dns_resolves_to", value_native("dns_resolves_to", fn_dns_resolves_to));

    // static file serving (v1.1.2 — Phase 3)
    env_define(env, "_webi_mime_type",         value_native("_webi_mime_type",         fn_webi_mime_type));
    env_define(env, "_webi_safe_static_path",  value_native("_webi_safe_static_path",  fn_webi_safe_static_path));
    env_define(env, "_webi_resolve_path",      value_native("_webi_resolve_path",      fn_webi_resolve_path));
    env_define(env, "_webi_script_dir",        value_native("_webi_script_dir",        fn_webi_script_dir));
}