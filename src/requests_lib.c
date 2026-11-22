// ===========================================================================
// NOTE: windows.h's winnt.h defines an enum constant literally named
// "TokenType" (part of _TOKEN_INFORMATION_CLASS, used by the Win32 security
// token APIs). That collides with a type name, not just another constant,
// so no #include ordering can fix it — one of the two names has to change.
// Kosh's own lexer token-kind enum was renamed to TokenKind (see token.h)
// specifically to avoid this clash permanently, regardless of include order.
// ===========================================================================
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <winhttp.h>
#endif

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "requests_lib.h"

// ---------------------------------------------------------------------------
// Helper: build result map
// ---------------------------------------------------------------------------
static Value make_response(long status, const char *body, int success) {
    Value m = value_map_empty();
    map_set(&m, "status",  value_number((double)status));
    map_set(&m, "body",    value_string(body ? body : ""));
    map_set(&m, "success", value_bool(success));
    return m;
}

// ---------------------------------------------------------------------------
// Helper: check args
// ---------------------------------------------------------------------------
static int req_check(Interpreter *interp, const char *fn,
                     int expected, int actual) {
    if (actual < expected) {
        fprintf(stderr, "Runtime error: %s() expects %d argument(s), got %d\n",
                fn, expected, actual);
        if (interp) interp->had_runtime_error = 1;
        return 0;
    }
    return 1;
}

// ===========================================================================
// Inline JSON encoder (shared by both platforms for http_post_json)
// Kept here to avoid a cross-module dependency on json_lib.c
// ===========================================================================
static void req_buf_append(char **buf, int *len, int *cap, const char *s, int slen) {
    while (*len + slen + 1 > *cap) {
        *cap = *cap ? *cap * 2 : 256;
        *buf = realloc(*buf, *cap);
    }
    memcpy(*buf + *len, s, slen);
    *len += slen;
    (*buf)[*len] = '\0';
}
static void req_buf_char(char **buf, int *len, int *cap, char c) {
    req_buf_append(buf, len, cap, &c, 1);
}
static void req_json_string(const char *s, char **buf, int *len, int *cap) {
    req_buf_char(buf, len, cap, '"');
    for (const char *p = s; *p; p++) {
        if      (*p == '"')  { req_buf_append(buf, len, cap, "\\\"", 2); }
        else if (*p == '\\') { req_buf_append(buf, len, cap, "\\\\", 2); }
        else if (*p == '\n') { req_buf_append(buf, len, cap, "\\n",  2); }
        else if (*p == '\r') { req_buf_append(buf, len, cap, "\\r",  2); }
        else if (*p == '\t') { req_buf_append(buf, len, cap, "\\t",  2); }
        else req_buf_char(buf, len, cap, *p);
    }
    req_buf_char(buf, len, cap, '"');
}
static void req_json_encode(Value v, char **buf, int *len, int *cap) {
    char tmp[64];
    switch (v.type) {
        case VAL_NIL:
            req_buf_append(buf, len, cap, "null", 4); break;
        case VAL_BOOL:
            if (v.as.boolean) req_buf_append(buf, len, cap, "true", 4);
            else              req_buf_append(buf, len, cap, "false", 5);
            break;
        case VAL_NUMBER:
            if (v.as.number == (long long)v.as.number)
                snprintf(tmp, sizeof(tmp), "%lld", (long long)v.as.number);
            else
                snprintf(tmp, sizeof(tmp), "%.17g", v.as.number);
            req_buf_append(buf, len, cap, tmp, (int)strlen(tmp)); break;
        case VAL_STRING:
            req_json_string(v.as.string, buf, len, cap); break;
        case VAL_ARRAY:
            req_buf_char(buf, len, cap, '[');
            for (int i = 0; i < AS_ARRAY_COUNT(v); i++) {
                if (i) req_buf_append(buf, len, cap, ", ", 2);
                req_json_encode(AS_ARRAY_ITEMS(v)[i], buf, len, cap);
            }
            req_buf_char(buf, len, cap, ']'); break;
        case VAL_MAP:
            req_buf_char(buf, len, cap, '{');
            for (int i = 0; i < AS_MAP_COUNT(v); i++) {
                if (i) req_buf_append(buf, len, cap, ", ", 2);
                req_json_string(AS_MAP_ENTRIES(v)[i].key, buf, len, cap);
                req_buf_append(buf, len, cap, ": ", 2);
                req_json_encode(*AS_MAP_ENTRIES(v)[i].value, buf, len, cap);
            }
            req_buf_char(buf, len, cap, '}'); break;
        default:
            req_buf_append(buf, len, cap, "null", 4); break;
    }
}

// ===========================================================================
// PLATFORM: Windows — WinHTTP
// ===========================================================================
// ===========================================================================
// Hardened HTTP client core (used by http_get/post/put/delete/post_json/request
// here, and by http_get_h/post_h/put_h/... in webi_lib.c)
//
// What changed in 0.018.1, and why (each of these was reproduced first):
//   * Only http:// and https:// URLs are accepted. curl used to fetch file:///etc/..., ftp://,
//     gopher:// ... for any URL it was handed (a local-file read when the URL came from a user).
//   * Nothing is passed to curl on its command line. The URL, headers and body go in on
//     curl's STDIN as a config file, so (a) API keys no longer show up in `ps`, (b) a URL that
//     starts with "-" is just a URL, never an option, (c) a body starting with "@" is sent
//     as text: with -d, curl uploaded the named LOCAL FILE instead.
//   * No shell is involved any more (fork + exec, not popen).
//   * Real timeouts (connect + whole transfer) and a cap on the response size.
//   * The HTTP status is read from a per-request random marker that a server cannot guess
//     or forge (it used to be a fixed string, so a server could say "200" for a 500).
//   * Header lines and the method are validated (no CR/LF/control characters, token names);
//     curl's URL globbing ([1-3], {a,b}) is switched off; ~/.curlrc is ignored.
//   * Failures carry an "error" message in the result map instead of just status 0.
// ===========================================================================

#include <ctype.h>
#ifdef _WIN32
#  define strncasecmp _strnicmp
#else
#  include <strings.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <poll.h>
#  include <signal.h>
#  include <errno.h>
#  include <time.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <sys/stat.h>
#endif

// ---- settings --------------------------------------------------------------
static struct { int timeout_s, connect_s; size_t max_resp, max_req; } g_http =
    { 60, 10, 32u * 1024u * 1024u, 32u * 1024u * 1024u };

#define KH_HTTP_MAX_URL      8192
#define KH_HTTP_MAX_HEADERS  100
#define KH_HTTP_MAX_HDR_LEN  8192
#define KH_HTTP_INLINE_BODY  (64 * 1024)   /* larger bodies go through a private temp file */

// ---- small growable buffer -------------------------------------------------
typedef struct { char *p; size_t len, cap; } kh_buf;

static int kh_buf_add(kh_buf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 256;
        while (nc < b->len + n + 1) nc *= 2;
        char *np = realloc(b->p, nc);
        if (!np) return 0;
        b->p = np; b->cap = nc;
    }
    if (n) memcpy(b->p + b->len, s, n);
    b->len += n; b->p[b->len] = '\0';
    return 1;
}
static int kh_buf_str(kh_buf *b, const char *s) { return kh_buf_add(b, s, strlen(s)); }
static void kh_buf_free(kh_buf *b) { free(b->p); b->p = NULL; b->len = b->cap = 0; }

// ---- validation ------------------------------------------------------------
static int kh_is_tchar(unsigned char c) {
    if (c == 0) return 0;
    if (isalnum(c)) return 1;
    return strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

static int kh_valid_method(const char *m) {
    size_t n = m ? strlen(m) : 0;
    if (n == 0 || n > 16) return 0;
    for (size_t i = 0; i < n; i++) if (m[i] < 'A' || m[i] > 'Z') return 0;
    return 1;
}

static void kh_set_err(char *err, size_t el, const char *msg) {
    if (err && el) { strncpy(err, msg, el - 1); err[el - 1] = '\0'; }
}

// http:// or https:// only, no whitespace/control characters/backslashes, a host present.
static int kh_valid_url(const char *url, char *err, size_t el) {
    if (!url) { kh_set_err(err, el, "no URL given"); return 0; }
    size_t n = strlen(url);
    if (n > KH_HTTP_MAX_URL) { kh_set_err(err, el, "URL is longer than 8192 bytes"); return 0; }
    size_t off = 0;
    if (strncasecmp(url, "https://", 8) == 0) off = 8;
    else if (strncasecmp(url, "http://", 7) == 0) off = 7;
    else { kh_set_err(err, el, "only http:// and https:// URLs are allowed"); return 0; }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)url[i];
        if (c <= 0x20 || c == 0x7f || c == '\\') {
            kh_set_err(err, el, "the URL contains a space, control character or backslash (percent-encode it with url_encode)");
            return 0;
        }
    }
    if (url[off] == '\0' || url[off] == '/' || url[off] == '?' || url[off] == '#' || url[off] == ':') {
        kh_set_err(err, el, "the URL has no host name");
        return 0;
    }
    return 1;
}

static int kh_name_is(const char *line, size_t nlen, const char *want) {
    if (strlen(want) != nlen) return 0;
    for (size_t i = 0; i < nlen; i++) if (tolower((unsigned char)line[i]) != want[i]) return 0;
    return 1;
}

// "Name: value": a token name, a value with no control characters (tab is fine), and
// not one of the headers that frame the request itself.
static int kh_valid_header(const char *line, char *err, size_t el) {
    const char *colon = strchr(line, ':');
    if (!colon || colon == line) { kh_set_err(err, el, "a header must look like 'Name: value'"); return 0; }
    size_t nlen = (size_t)(colon - line);
    for (size_t i = 0; i < nlen; i++)
        if (!kh_is_tchar((unsigned char)line[i])) { kh_set_err(err, el, "invalid character in a header name"); return 0; }
    size_t vlen = strlen(colon + 1);
    if (vlen > KH_HTTP_MAX_HDR_LEN) { kh_set_err(err, el, "a header value is longer than 8192 bytes"); return 0; }
    for (const char *p = colon + 1; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if ((c < 0x20 && c != '\t') || c == 0x7f) {
            kh_set_err(err, el, "a header value may not contain CR, LF or other control characters");
            return 0;
        }
    }
    if (kh_name_is(line, nlen, "content-length") || kh_name_is(line, nlen, "transfer-encoding") ||
        kh_name_is(line, nlen, "connection")) {
        kh_set_err(err, el, "Content-Length, Transfer-Encoding and Connection are set by the client and cannot be overridden");
        return 0;
    }
    return 1;
}

// ---- header list -------------------------------------------------------------
typedef struct { char **v; int n; } kh_hdrs;

static void kh_hdrs_free(kh_hdrs *h) {
    for (int i = 0; i < h->n; i++) free(h->v[i]);
    free(h->v); h->v = NULL; h->n = 0;
}
static int kh_hdrs_add(kh_hdrs *h, const char *line, char *err, size_t el) {
    if (h->n >= KH_HTTP_MAX_HEADERS) { kh_set_err(err, el, "too many headers (limit 100)"); return 0; }
    if (!kh_valid_header(line, err, el)) return 0;
    char **nv = realloc(h->v, sizeof(char *) * (size_t)(h->n + 1));
    if (!nv) { kh_set_err(err, el, "out of memory"); return 0; }
    h->v = nv;
    h->v[h->n] = strdup(line);
    if (!h->v[h->n]) { kh_set_err(err, el, "out of memory"); return 0; }
    h->n++;
    return 1;
}
static int kh_hdrs_has(const kh_hdrs *h, const char *lname) {
    for (int i = 0; i < h->n; i++) {
        const char *c = strchr(h->v[i], ':');
        if (c && kh_name_is(h->v[i], (size_t)(c - h->v[i]), lname)) return 1;
    }
    return 0;
}
// "A: 1\r\nB: 2" (or \n separated) -> list. Blank lines are skipped.
static int kh_hdrs_from_string(kh_hdrs *h, const char *s, char *err, size_t el) {
    if (!s) return 1;
    const char *p = s;
    while (*p) {
        const char *e = p;
        while (*e && *e != '\n') e++;
        size_t len = (size_t)(e - p);
        if (len && p[len - 1] == '\r') len--;
        size_t a = 0;
        while (a < len && (p[a] == ' ' || p[a] == '\t')) a++;
        if (a < len) {
            char *line = malloc(len + 1);
            if (!line) { kh_set_err(err, el, "out of memory"); return 0; }
            memcpy(line, p, len); line[len] = '\0';
            int ok = kh_hdrs_add(h, line, err, el);
            free(line);
            if (!ok) return 0;
        }
        p = *e ? e + 1 : e;
    }
    return 1;
}
static int kh_hdrs_from_map(kh_hdrs *h, Value headers, char *err, size_t el) {
    if (headers.type != VAL_MAP) return 1;
    for (int i = 0; i < AS_MAP_COUNT(headers); i++) {
        const char *k = AS_MAP_ENTRIES(headers)[i].key;
        Value *v = AS_MAP_ENTRIES(headers)[i].value;
        char num[64]; const char *val = "";
        if (v && v->type == VAL_STRING) val = v->as.string;
        else if (v && v->type == VAL_NUMBER) { snprintf(num, sizeof(num), "%.15g", v->as.number); val = num; }
        else if (v && v->type == VAL_BOOL) val = v->as.boolean ? "true" : "false";
        size_t need = strlen(k) + strlen(val) + 3;
        char *line = malloc(need);
        if (!line) { kh_set_err(err, el, "out of memory"); return 0; }
        snprintf(line, need, "%s: %s", k, val);
        int ok = kh_hdrs_add(h, line, err, el);
        free(line);
        if (!ok) return 0;
    }
    return 1;
}

// ---- request / result ------------------------------------------------------
typedef struct {
    const char *method, *url, *body;
    size_t body_len;
    const kh_hdrs *hdrs;
} kh_http_req;

typedef struct {
    long status;
    char *body;          // malloc'd, NUL terminated
    char error[300];
} kh_http_res;

static Value kh_result_value(long status, const char *body, const char *error) {
    Value m = make_response(status, body, status >= 200 && status < 300);
    if (error && *error) map_set(&m, "error", value_string(error));
    return m;
}

static void kh_http_perform(const kh_http_req *rq, kh_http_res *rs);   // platform specific, below

static Value kh_http_call(const char *method, const char *url, const char *body, kh_hdrs *hdrs) {
    char err[300] = "";
    if (!kh_valid_method(method)) return kh_result_value(0, "", "invalid HTTP method (use upper-case letters, e.g. GET)");
    if (!kh_valid_url(url, err, sizeof(err))) return kh_result_value(0, "", err);
    size_t blen = body ? strlen(body) : 0;
    if (strcmp(method, "HEAD") == 0) { body = NULL; blen = 0; }
    if (blen > g_http.max_req) return kh_result_value(0, "", "request body is larger than max_request_bytes (see http_configure)");
    if (body && !kh_hdrs_has(hdrs, "content-type")) {
        if (!kh_hdrs_add(hdrs, "Content-Type: application/x-www-form-urlencoded", err, sizeof(err)))
            return kh_result_value(0, "", err);
    }
    kh_http_req rq = { method, url, body, blen, hdrs };
    kh_http_res rs; memset(&rs, 0, sizeof(rs));
    kh_http_perform(&rq, &rs);
    Value v = kh_result_value(rs.status, rs.body ? rs.body : "", rs.error[0] ? rs.error : NULL);
    free(rs.body);
    return v;
}

// For webi_lib.c: headers as one "A: 1\r\nB: 2" string, plus an optional first header.
Value kh_http_call_strings(const char *method, const char *url, const char *body,
                           const char *ctype, const char *header_string) {
    char err[300] = "";
    kh_hdrs h = { NULL, 0 };
    Value out;
    if ((ctype && !kh_hdrs_add(&h, ctype, err, sizeof(err))) ||
        !kh_hdrs_from_string(&h, header_string, err, sizeof(err))) {
        out = kh_result_value(0, "", err);
    } else {
        out = kh_http_call(method, url, body, &h);
    }
    kh_hdrs_free(&h);
    return out;
}

static Value kh_http_call_map(const char *method, const char *url, const char *body, Value headers) {
    char err[300] = "";
    kh_hdrs h = { NULL, 0 };
    Value out;
    if (!kh_hdrs_from_map(&h, headers, err, sizeof(err))) out = kh_result_value(0, "", err);
    else out = kh_http_call(method, url, body, &h);
    kh_hdrs_free(&h);
    return out;
}

#ifdef _WIN32
// ===========================================================================
// Windows: WinHTTP
// (not exercised by the Linux test-suite; compile-checked only. WinHTTP only
// speaks http/https, never follows redirects here, verifies certificates, and
// has no command line, so the curl-specific protections do not apply.)
// ===========================================================================
static wchar_t *kh_utf8_to_w(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = malloc((size_t)n * sizeof(wchar_t));
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static void kh_http_perform(const kh_http_req *rq, kh_http_res *rs) {
    wchar_t *wurl = kh_utf8_to_w(rq->url), *wmethod = kh_utf8_to_w(rq->method);
    wchar_t *whost = NULL, *wpath = NULL;
    HINTERNET hs = NULL, hc = NULL, hr = NULL;
    ULONGLONG t0 = GetTickCount64(), limit_ms = (ULONGLONG)g_http.timeout_s * 1000ULL;
    char *buf = NULL; size_t total = 0, cap = 0;

    if (!wurl || !wmethod) { kh_set_err(rs->error, sizeof(rs->error), "out of memory"); goto done; }

    URL_COMPONENTS uc; memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.dwHostNameLength = (DWORD)-1; uc.dwUrlPathLength = (DWORD)-1; uc.dwExtraInfoLength = (DWORD)-1;
    uc.dwSchemeLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc) || !uc.lpszHostName || uc.dwHostNameLength == 0) {
        kh_set_err(rs->error, sizeof(rs->error), "could not parse the URL"); goto done;
    }
    if (uc.nScheme != INTERNET_SCHEME_HTTP && uc.nScheme != INTERNET_SCHEME_HTTPS) {
        kh_set_err(rs->error, sizeof(rs->error), "only http:// and https:// URLs are allowed"); goto done;
    }
    whost = malloc(((size_t)uc.dwHostNameLength + 1) * sizeof(wchar_t));
    wpath = malloc(((size_t)uc.dwUrlPathLength + (size_t)uc.dwExtraInfoLength + 2) * sizeof(wchar_t));
    if (!whost || !wpath) { kh_set_err(rs->error, sizeof(rs->error), "out of memory"); goto done; }
    memcpy(whost, uc.lpszHostName, (size_t)uc.dwHostNameLength * sizeof(wchar_t));
    whost[uc.dwHostNameLength] = L'\0';
    size_t pl = 0;
    if (uc.dwUrlPathLength) { memcpy(wpath, uc.lpszUrlPath, (size_t)uc.dwUrlPathLength * sizeof(wchar_t)); pl = uc.dwUrlPathLength; }
    else { wpath[0] = L'/'; pl = 1; }
    if (uc.dwExtraInfoLength) { memcpy(wpath + pl, uc.lpszExtraInfo, (size_t)uc.dwExtraInfoLength * sizeof(wchar_t)); pl += uc.dwExtraInfoLength; }
    wpath[pl] = L'\0';

    hs = WinHttpOpen(L"Kosh/1.2", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hs) { kh_set_err(rs->error, sizeof(rs->error), "WinHttpOpen failed"); goto done; }
    WinHttpSetTimeouts(hs, g_http.connect_s * 1000, g_http.connect_s * 1000, g_http.timeout_s * 1000, g_http.timeout_s * 1000);
    hc = WinHttpConnect(hs, whost, uc.nPort, 0);
    if (!hc) { kh_set_err(rs->error, sizeof(rs->error), "could not connect"); goto done; }
    hr = WinHttpOpenRequest(hc, wmethod, wpath, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                            uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!hr) { kh_set_err(rs->error, sizeof(rs->error), "WinHttpOpenRequest failed"); goto done; }
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(hr, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));

    for (int i = 0; i < rq->hdrs->n; i++) {
        wchar_t *wh = kh_utf8_to_w(rq->hdrs->v[i]);
        if (wh) { WinHttpAddRequestHeaders(hr, wh, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE); free(wh); }
    }
    DWORD blen = (DWORD)rq->body_len;
    if (!WinHttpSendRequest(hr, WINHTTP_NO_ADDITIONAL_HEADERS, 0, (LPVOID)rq->body, blen, blen, 0) ||
        !WinHttpReceiveResponse(hr, NULL)) {
        DWORD ec = GetLastError();
        if (ec == ERROR_WINHTTP_TIMEOUT) kh_set_err(rs->error, sizeof(rs->error), "timed out");
        else snprintf(rs->error, sizeof(rs->error), "request failed (WinHTTP error %lu)", (unsigned long)ec);
        goto done;
    }
    DWORD code = 0, csz = sizeof(code);
    WinHttpQueryHeaders(hr, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &csz, WINHTTP_NO_HEADER_INDEX);
    rs->status = (long)code;
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(hr, &avail) && avail > 0) {
        if (GetTickCount64() - t0 > limit_ms) { kh_set_err(rs->error, sizeof(rs->error), "timed out"); break; }
        if (total + avail > g_http.max_resp) { kh_set_err(rs->error, sizeof(rs->error), "response is larger than max_response_bytes (see http_configure)"); break; }
        if (total + avail + 1 > cap) {
            size_t nc = cap ? cap * 2 : 8192;
            while (nc < total + avail + 1) nc *= 2;
            char *nb = realloc(buf, nc);
            if (!nb) { kh_set_err(rs->error, sizeof(rs->error), "out of memory"); break; }
            buf = nb; cap = nc;
        }
        DWORD got = 0;
        if (!WinHttpReadData(hr, buf + total, avail, &got) || got == 0) break;
        total += got;
    }
done:
    if (buf) buf[total] = '\0';
    rs->body = buf ? buf : strdup("");
    if (hr) WinHttpCloseHandle(hr);
    if (hc) WinHttpCloseHandle(hc);
    if (hs) WinHttpCloseHandle(hs);
    free(wurl); free(wmethod); free(whost); free(wpath);
    if (rs->status == 0 && !rs->error[0]) kh_set_err(rs->error, sizeof(rs->error), "no response");
}

#else  // !_WIN32
// ===========================================================================
// POSIX: curl in a child process, configured over its stdin
// ===========================================================================
static int kh_random_hex(char *out, size_t nbytes) {      // out gets nbytes*2 hex chars + NUL
    unsigned char r[32];
    if (nbytes > sizeof(r)) nbytes = sizeof(r);
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return 0;
    size_t got = 0;
    while (got < nbytes) { ssize_t n = read(fd, r + got, nbytes - got); if (n <= 0) { close(fd); return 0; } got += (size_t)n; }
    close(fd);
    for (size_t i = 0; i < nbytes; i++) snprintf(out + i * 2, 3, "%02x", r[i]);
    return 1;
}

// key = "value" with the escapes curl's config parser understands
static int kh_cfg_str(kh_buf *b, const char *key, const char *val, size_t vlen) {
    if (!kh_buf_str(b, key) || !kh_buf_str(b, " = \"")) return 0;
    for (size_t i = 0; i < vlen; i++) {
        char c = val[i];
        int ok;
        switch (c) {
            case '\\': ok = kh_buf_str(b, "\\\\"); break;
            case '"':  ok = kh_buf_str(b, "\\\""); break;
            case '\n': ok = kh_buf_str(b, "\\n");  break;
            case '\r': ok = kh_buf_str(b, "\\r");  break;
            case '\t': ok = kh_buf_str(b, "\\t");  break;
            case '\v': ok = kh_buf_str(b, "\\v");  break;
            default:   ok = kh_buf_add(b, &c, 1);   break;
        }
        if (!ok) return 0;
    }
    return kh_buf_str(b, "\"\n");
}
static int kh_cfg_num(kh_buf *b, const char *key, long v) {
    char t[64]; snprintf(t, sizeof(t), "%s = %ld\n", key, v);
    return kh_buf_str(b, t);
}

static void kh_set_nonblock(int fd) { int fl = fcntl(fd, F_GETFL, 0); if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK); }

static void kh_http_perform(const kh_http_req *rq, kh_http_res *rs) {
    char marker[48], hex[17];
    if (!kh_random_hex(hex, 8)) { kh_set_err(rs->error, sizeof(rs->error), "no secure random source available"); rs->body = strdup(""); return; }
    snprintf(marker, sizeof(marker), "__KH_%s__", hex);

    signal(SIGPIPE, SIG_IGN);

    // ---- build the curl config ----
    kh_buf cfg = { NULL, 0, 0 };
    char tmpname[512] = "";
    int ok = 1;
    ok = ok && kh_cfg_str(&cfg, "url", rq->url, strlen(rq->url));
    if (strcmp(rq->method, "HEAD") == 0) ok = ok && kh_buf_str(&cfg, "head\n");
    else ok = ok && kh_cfg_str(&cfg, "request", rq->method, strlen(rq->method));
    for (int i = 0; ok && i < rq->hdrs->n; i++) ok = kh_cfg_str(&cfg, "header", rq->hdrs->v[i], strlen(rq->hdrs->v[i]));
    if (ok && rq->body) {
        if (rq->body_len <= KH_HTTP_INLINE_BODY) {
            ok = kh_cfg_str(&cfg, "data-raw", rq->body, rq->body_len);
        } else {                                   // big body: a private (0600) temp file we create and delete
            const char *td = getenv("TMPDIR"); if (!td || !*td) td = "/tmp";
            snprintf(tmpname, sizeof(tmpname), "%s/kosh_body_XXXXXX", td);
            int fd = mkstemp(tmpname);
            if (fd < 0) { kh_set_err(rs->error, sizeof(rs->error), "could not create a temporary file for the request body"); kh_buf_free(&cfg); rs->body = strdup(""); tmpname[0] = '\0'; return; }
            size_t off = 0;
            while (off < rq->body_len) { ssize_t n = write(fd, rq->body + off, rq->body_len - off); if (n <= 0) break; off += (size_t)n; }
            close(fd);
            if (off != rq->body_len) { unlink(tmpname); kh_set_err(rs->error, sizeof(rs->error), "could not write the request body to a temporary file"); kh_buf_free(&cfg); rs->body = strdup(""); return; }
            char ref[560]; snprintf(ref, sizeof(ref), "@%s", tmpname);
            ok = kh_cfg_str(&cfg, "data-binary", ref, strlen(ref));
        }
    }
    ok = ok && kh_cfg_num(&cfg, "connect-timeout", g_http.connect_s)
            && kh_cfg_num(&cfg, "max-time", g_http.timeout_s)
            && kh_cfg_num(&cfg, "max-filesize", (long)g_http.max_resp)
            && kh_cfg_str(&cfg, "proto", "=http,https", 11)
            && kh_cfg_str(&cfg, "proto-redir", "=http,https", 11)
            && kh_buf_str(&cfg, "tlsv1.2\nsilent\nshow-error\ngloboff\n");
    if (ok) {
        char wo[96]; snprintf(wo, sizeof(wo), "\n%s%%{http_code}", marker);
        ok = kh_cfg_str(&cfg, "write-out", wo, strlen(wo));
    }
    if (!ok) { kh_set_err(rs->error, sizeof(rs->error), "out of memory"); kh_buf_free(&cfg); if (tmpname[0]) unlink(tmpname); rs->body = strdup(""); return; }

    // ---- start curl: no shell, config on stdin ----
    int pin[2], pout[2], perr[2];
    if (pipe(pin) != 0) { kh_set_err(rs->error, sizeof(rs->error), "pipe() failed"); goto fail_early; }
    if (pipe(pout) != 0) { close(pin[0]); close(pin[1]); kh_set_err(rs->error, sizeof(rs->error), "pipe() failed"); goto fail_early; }
    if (pipe(perr) != 0) { close(pin[0]); close(pin[1]); close(pout[0]); close(pout[1]); kh_set_err(rs->error, sizeof(rs->error), "pipe() failed"); goto fail_early; }
    pid_t pid = fork();
    if (pid < 0) {
        close(pin[0]); close(pin[1]); close(pout[0]); close(pout[1]); close(perr[0]); close(perr[1]);
        kh_set_err(rs->error, sizeof(rs->error), "fork() failed"); goto fail_early;
    }
    if (pid == 0) {
        dup2(pin[0], 0); dup2(pout[1], 1); dup2(perr[1], 2);
        long maxfd = sysconf(_SC_OPEN_MAX); if (maxfd < 0 || maxfd > 4096) maxfd = 4096;
        for (int fd = 3; fd < maxfd; fd++) close(fd);
        execlp("curl", "curl", "-q", "--config", "-", (char *)NULL);
        const char *m = "curl: not found";
        if (write(2, m, strlen(m)) < 0) { /* nothing to do */ }
        _exit(127);
    }
    close(pin[0]); close(pout[1]); close(perr[1]);
    kh_set_nonblock(pin[1]); kh_set_nonblock(pout[0]); kh_set_nonblock(perr[0]);

    kh_buf out = { NULL, 0, 0 }, errb = { NULL, 0, 0 };
    size_t woff = 0, out_cap = g_http.max_resp + 256;
    int out_open = 1, err_open = 1, in_open = 1, killed = 0;
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    long deadline_ms = ((long)g_http.timeout_s + 5) * 1000L;

    while (out_open || err_open) {
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (long)((now.tv_sec - t0.tv_sec) * 1000L + (now.tv_nsec - t0.tv_nsec) / 1000000L);
        if (elapsed >= deadline_ms) { kill(pid, SIGKILL); killed = 1; snprintf(rs->error, sizeof(rs->error), "timed out (the request was stopped after %d seconds)", g_http.timeout_s); break; }
        struct pollfd fds[3]; int nf = 0, ix_in = -1, ix_out = -1, ix_err = -1;
        if (in_open)  { ix_in = nf;  fds[nf].fd = pin[1];  fds[nf].events = POLLOUT; fds[nf].revents = 0; nf++; }
        if (out_open) { ix_out = nf; fds[nf].fd = pout[0]; fds[nf].events = POLLIN;  fds[nf].revents = 0; nf++; }
        if (err_open) { ix_err = nf; fds[nf].fd = perr[0]; fds[nf].events = POLLIN;  fds[nf].revents = 0; nf++; }
        int wait_ms = (int)((deadline_ms - elapsed) < 1000 ? (deadline_ms - elapsed) : 1000);
        int pr = poll(fds, (nfds_t)nf, wait_ms);
        if (pr < 0 && errno != EINTR) break;
        if (pr <= 0) continue;
        if (ix_in >= 0 && (fds[ix_in].revents & (POLLOUT | POLLERR | POLLHUP))) {
            ssize_t n = write(pin[1], cfg.p + woff, cfg.len - woff);
            if (n > 0) woff += (size_t)n;
            if (woff >= cfg.len || (n < 0 && errno != EAGAIN && errno != EINTR)) { close(pin[1]); in_open = 0; }
        }
        if (ix_out >= 0 && (fds[ix_out].revents & (POLLIN | POLLHUP | POLLERR))) {
            char tmp[16384];
            ssize_t n = read(pout[0], tmp, sizeof(tmp));
            if (n > 0) {
                if (out.len + (size_t)n > out_cap) { kill(pid, SIGKILL); killed = 1; kh_set_err(rs->error, sizeof(rs->error), "response is larger than max_response_bytes (see http_configure)"); break; }
                if (!kh_buf_add(&out, tmp, (size_t)n)) { kill(pid, SIGKILL); killed = 1; kh_set_err(rs->error, sizeof(rs->error), "out of memory"); break; }
            } else if (n == 0 || (errno != EAGAIN && errno != EINTR)) { close(pout[0]); out_open = 0; }
        }
        if (ix_err >= 0 && (fds[ix_err].revents & (POLLIN | POLLHUP | POLLERR))) {
            char tmp[1024];
            ssize_t n = read(perr[0], tmp, sizeof(tmp));
            if (n > 0) { if (errb.len < 2048) kh_buf_add(&errb, tmp, (size_t)n); }
            else if (n == 0 || (errno != EAGAIN && errno != EINTR)) { close(perr[0]); err_open = 0; }
        }
    }
    if (in_open) close(pin[1]);
    if (out_open) close(pout[0]);
    if (err_open) close(perr[0]);
    int wst = 0;
    waitpid(pid, &wst, 0);
    kh_buf_free(&cfg);
    if (tmpname[0]) unlink(tmpname);

    // ---- read the status from OUR marker (the last one; a server cannot predict it) ----
    char needle[64]; snprintf(needle, sizeof(needle), "\n%s", marker);
    size_t nl = strlen(needle), pos = (size_t)-1;
    if (out.p && out.len >= nl + 1) {
        for (size_t i = out.len - nl; ; i--) {
            if (memcmp(out.p + i, needle, nl) == 0) { pos = i; break; }
            if (i == 0) break;
        }
    }
    int code = WIFEXITED(wst) ? WEXITSTATUS(wst) : -1;
    if (!killed && pos != (size_t)-1) {
        long st = 0;
        const char *d = out.p + pos + nl;
        while (*d >= '0' && *d <= '9' && st < 100000) { st = st * 10 + (*d - '0'); d++; }
        rs->status = st;
        char *b = malloc(pos + 1);
        if (b) { memcpy(b, out.p, pos); b[pos] = '\0'; rs->body = b; }
    }
    // If curl itself reports a failure, what we hold is NOT a complete response, even when an
    // HTTP status was printed: with --max-filesize curl stops early and still says "200",
    // which used to come back as a successful, silently truncated response.
    if (!killed && code != 0 && !rs->error[0]) {
        rs->status = 0;
        free(rs->body); rs->body = NULL;
        if (code == 127) kh_set_err(rs->error, sizeof(rs->error), "curl was not found: Kosh uses curl for HTTP requests; please install it");
        else if (code == 63) kh_set_err(rs->error, sizeof(rs->error), "response is larger than max_response_bytes (see http_configure)");
        else if (errb.p && errb.len) {
            size_t l = 0;
            while (l < errb.len && errb.p[l] != '\n' && l < sizeof(rs->error) - 1) l++;
            memcpy(rs->error, errb.p, l); rs->error[l] = '\0';
        } else snprintf(rs->error, sizeof(rs->error), "curl failed (exit code %d)", code);
    }
    if (!rs->body) rs->body = strdup("");
    if (rs->status == 0 && !rs->error[0]) kh_set_err(rs->error, sizeof(rs->error), "no response");
    kh_buf_free(&out); kh_buf_free(&errb);
    return;

fail_early:
    kh_buf_free(&cfg);
    if (tmpname[0]) unlink(tmpname);
    rs->body = strdup("");
}
#endif // !_WIN32

// ===========================================================================
// Natives (same on every platform)
// ===========================================================================
static int req_str_arg(Interpreter *interp, const char *fn, Value v, int idx) {
    if (v.type != VAL_STRING) {
        fprintf(stderr, "Runtime error: %s() argument %d must be a string\n", fn, idx);
        if (interp) interp->had_runtime_error = 1;
        return 0;
    }
    return 1;
}

void fn_http_get(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!req_check(interp, "http_get", 1, argc) || !req_str_arg(interp, "http_get", args[0], 1)) { *result = value_nil(); return; }
    *result = kh_http_call_map("GET", args[0].as.string, NULL, value_nil());
}
void fn_http_post(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!req_check(interp, "http_post", 2, argc) || !req_str_arg(interp, "http_post", args[0], 1) || !req_str_arg(interp, "http_post", args[1], 2)) { *result = value_nil(); return; }
    *result = kh_http_call_strings("POST", args[0].as.string, args[1].as.string, "Content-Type: application/x-www-form-urlencoded", NULL);
}
void fn_http_put(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!req_check(interp, "http_put", 2, argc) || !req_str_arg(interp, "http_put", args[0], 1) || !req_str_arg(interp, "http_put", args[1], 2)) { *result = value_nil(); return; }
    *result = kh_http_call_strings("PUT", args[0].as.string, args[1].as.string, "Content-Type: application/x-www-form-urlencoded", NULL);
}
void fn_http_delete(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!req_check(interp, "http_delete", 1, argc) || !req_str_arg(interp, "http_delete", args[0], 1)) { *result = value_nil(); return; }
    *result = kh_http_call_map("DELETE", args[0].as.string, NULL, value_nil());
}
void fn_http_post_json(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!req_check(interp, "http_post_json", 2, argc) || !req_str_arg(interp, "http_post_json", args[0], 1)) { *result = value_nil(); return; }
    char *json_body = NULL; int jlen = 0, jcap = 0;
    req_json_encode(args[1], &json_body, &jlen, &jcap);
    *result = kh_http_call_strings("POST", args[0].as.string, json_body ? json_body : "{}", "Content-Type: application/json", NULL);
    free(json_body);
}
void fn_http_request(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!req_check(interp, "http_request", 2, argc) || !req_str_arg(interp, "http_request", args[0], 1) || !req_str_arg(interp, "http_request", args[1], 2)) { *result = value_nil(); return; }
    const char *body = (argc > 2 && args[2].type == VAL_STRING) ? args[2].as.string : NULL;
    Value headers = (argc > 3 && args[3].type == VAL_MAP) ? args[3] : value_nil();
    *result = kh_http_call_map(args[0].as.string, args[1].as.string, body, headers);
}

// http_configure(options = {}) -> map of the settings now in force
//   timeout_seconds          1 .. 3600        60   whole request, connect included
//   connect_timeout_seconds  1 .. 600         10
//   max_response_bytes       1024 .. 2^30     32 MB
//   max_request_bytes        0 .. 2^30        32 MB
// Applies to every http_* function (including http_get_h and friends) from now on.
// An unknown key or an out-of-range value is an error and nothing is changed.
void fn_http_configure(Value *result, Interpreter *interp, int argc, Value *args) {
    struct { const char *key; double lo, hi; } spec[4] = {
        { "timeout_seconds", 1, 3600 }, { "connect_timeout_seconds", 1, 600 },
        { "max_response_bytes", 1024, 1073741824.0 }, { "max_request_bytes", 0, 1073741824.0 } };
    double nv[4]; int set[4] = {0, 0, 0, 0};
    if (argc >= 1) {
        if (args[0].type != VAL_MAP) {
            fprintf(stderr, "Runtime error: http_configure(options) expects a map\n");
            interp->had_runtime_error = 1; *result = value_nil(); return;
        }
        for (int i = 0; i < AS_MAP_COUNT(args[0]); i++) {
            const char *k = AS_MAP_ENTRIES(args[0])[i].key;
            Value *v = AS_MAP_ENTRIES(args[0])[i].value;
            int found = -1;
            for (int s = 0; s < 4; s++) if (strcmp(k, spec[s].key) == 0) found = s;
            if (found < 0) {
                fprintf(stderr, "Runtime error: http_configure: unknown option '%s' (valid: timeout_seconds, connect_timeout_seconds, max_response_bytes, max_request_bytes)\n", k);
                interp->had_runtime_error = 1; *result = value_nil(); return;
            }
            if (!v || v->type != VAL_NUMBER || v->as.number < spec[found].lo || v->as.number > spec[found].hi) {
                fprintf(stderr, "Runtime error: http_configure: '%s' must be a number between %.0f and %.0f\n", k, spec[found].lo, spec[found].hi);
                interp->had_runtime_error = 1; *result = value_nil(); return;
            }
            nv[found] = v->as.number; set[found] = 1;
        }
    }
    if (set[0]) g_http.timeout_s = (int)nv[0];
    if (set[1]) g_http.connect_s = (int)nv[1];
    if (set[2]) g_http.max_resp = (size_t)nv[2];
    if (set[3]) g_http.max_req = (size_t)nv[3];
    if (g_http.connect_s > g_http.timeout_s) g_http.connect_s = g_http.timeout_s;
    Value m = value_map_empty();
    map_set(&m, "timeout_seconds",         value_number((double)g_http.timeout_s));
    map_set(&m, "connect_timeout_seconds", value_number((double)g_http.connect_s));
    map_set(&m, "max_response_bytes",      value_number((double)g_http.max_resp));
    map_set(&m, "max_request_bytes",       value_number((double)g_http.max_req));
    *result = m;
}

// url_encode(text) -> percent-encodes everything except A-Z a-z 0-9 - . _ ~  (RFC 3986)
void fn_url_encode(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!req_check(interp, "url_encode", 1, argc) || !req_str_arg(interp, "url_encode", args[0], 1)) { *result = value_nil(); return; }
    const char *s = args[0].as.string;
    size_t n = strlen(s);
    char *out = malloc(n * 3 + 1);
    if (!out) { *result = value_string(""); return; }
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') out[o++] = (char)c;
        else { snprintf(out + o, 4, "%%%02X", c); o += 3; }
    }
    out[o] = '\0';
    *result = value_string(out);
    free(out);
}

static int kh_hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// url_decode(text, plus_is_space = false) -> decodes %XX. A '%' not followed by two hex
// digits, and %00 (a NUL cannot live in a Kosh string), are left as written.
// With plus_is_space = true '+' becomes a space (query strings and HTML forms).
void fn_url_decode(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!req_check(interp, "url_decode", 1, argc) || !req_str_arg(interp, "url_decode", args[0], 1)) { *result = value_nil(); return; }
    int plus = (argc >= 2 && args[1].type == VAL_BOOL && args[1].as.boolean);
    const char *s = args[0].as.string;
    size_t n = strlen(s);
    char *out = malloc(n + 1);
    if (!out) { *result = value_string(""); return; }
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '%') {
            int a = (i + 1 < n) ? kh_hexval((unsigned char)s[i + 1]) : -1;
            int b = (i + 2 < n) ? kh_hexval((unsigned char)s[i + 2]) : -1;
            if (a >= 0 && b >= 0 && (a * 16 + b) != 0) { out[o++] = (char)(a * 16 + b); i += 2; continue; }
        }
        out[o++] = (plus && s[i] == '+') ? ' ' : s[i];
    }
    out[o] = '\0';
    *result = value_string(out);
    free(out);
}

// ===========================================================================
// Registration
// ===========================================================================
void requests_register_all(Environment *env) {
    env_define(env, "http_get",       value_native("http_get",       fn_http_get));
    env_define(env, "http_post",      value_native("http_post",      fn_http_post));
    env_define(env, "http_post_json", value_native("http_post_json", fn_http_post_json));
    env_define(env, "http_put",       value_native("http_put",       fn_http_put));
    env_define(env, "http_delete",    value_native("http_delete",    fn_http_delete));
    env_define(env, "http_request",   value_native("http_request",   fn_http_request));
    env_define(env, "http_configure", value_native("http_configure", fn_http_configure));
    env_define(env, "url_encode",     value_native("url_encode",     fn_url_encode));
    env_define(env, "url_decode",     value_native("url_decode",     fn_url_decode));
}