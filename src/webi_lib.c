// ===========================================================================
// webi_lib.c — Native HTTP server backend for the webi Khan package
//
// Provides:
//   http_serve(port, app)  — blocking server loop, calls Khan's webi_handle()
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
typedef int wb_sock_t;
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#include <sys/stat.h>
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
// Security limits (v1.1.1)
//   - WB_MAX_HEADER_SIZE: caps how much we'll buffer while waiting for the
//     \r\n\r\n header terminator, so a client that never sends one can't
//     grow our buffer forever.
//   - WB_MAX_BODY_SIZE: caps how large a request body we'll allocate for,
//     regardless of what Content-Length claims.
//   - WB_SOCKET_TIMEOUT_SEC: read timeout on an accepted connection, so a
//     client that connects and then sends data slowly (or not at all)
//     can't hold a connection open indefinitely (slowloris-style).
// ---------------------------------------------------------------------------
#define WB_MAX_HEADER_SIZE   (64 * 1024)          /* 64 KB */
#define WB_MAX_BODY_SIZE     (10 * 1024 * 1024)   /* 10 MB */
#define WB_SOCKET_TIMEOUT_SEC 30

/* Bridge: call Khan function by name from C */
extern Value khan_call_fn(Interpreter *interp, Environment *env,
                          const char *fn_name, int argc, Value *argv);
/* Global env pointer set by http_serve so the accept loop can call Khan */
static Environment *g_webi_env = NULL;

// ---------------------------------------------------------------------------
// wb_safe_call_webi_handle — call the Khan webi_handle() bridge function
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
// Internal: read a full HTTP request from a socket fd
// Returns malloc'd raw request string, caller frees.
// ---------------------------------------------------------------------------
static char *wb_read_request(wb_sock_t fd) {
    char *buf = NULL;
    int len = 0, cap = 0;
    char tmp[4096];
    ssize_t n;

    // Read until we have headers (double CRLF), or give up if the client
    // sends more than WB_MAX_HEADER_SIZE without ever completing them.
    while ((n = recv(fd, tmp, sizeof(tmp), 0)) > 0) {
        if (len + n + 1 > cap) {
            cap = cap ? (cap + (int)n) * 2 : 8192;
            buf = realloc(buf, cap);
        }
        memcpy(buf + len, tmp, n);
        len += n;
        buf[len] = '\0';

        // Check if we have the header terminator
        if (strstr(buf, "\r\n\r\n")) break;

        if (len > WB_MAX_HEADER_SIZE) {
            fprintf(stderr, "[webi] request headers exceeded %d bytes — dropping connection\n",
                    WB_MAX_HEADER_SIZE);
            free(buf);
            return strdup("");
        }
    }
    if (!buf) buf = strdup("");
    return buf;
}

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
// Internal: extract all headers from raw request into a single string
// "Key: Value\r\nKey2: Value2\r\n..."
// ---------------------------------------------------------------------------
static char *wb_extract_headers(const char *raw) {
    const char *start = strstr(raw, "\r\n");
    if (!start) return strdup("");
    start += 2; // skip first line
    const char *end = strstr(start, "\r\n\r\n");
    if (!end) end = start + strlen(start);
    int len = (int)(end - start);
    char *h = malloc(len + 1);
    memcpy(h, start, len);
    h[len] = '\0';
    return h;
}

// ---------------------------------------------------------------------------
// Internal: extract Content-Length from headers, read body from socket
// ---------------------------------------------------------------------------
static char *wb_read_body(wb_sock_t fd, const char *raw) {
    const char *cl = strstr(raw, "Content-Length:");
    if (!cl) cl = strstr(raw, "content-length:");
    if (!cl) return strdup("");

    long content_length = 0;
    sscanf(cl + 15, "%ld", &content_length);
    if (content_length <= 0) return strdup("");

    // Reject absurd/hostile Content-Length values instead of trusting the
    // client and allocating whatever they ask for.
    if (content_length > WB_MAX_BODY_SIZE) {
        fprintf(stderr, "[webi] request body Content-Length %ld exceeds %d byte limit — rejecting\n",
                content_length, WB_MAX_BODY_SIZE);
        return NULL;
    }

    // Body starts after \r\n\r\n
    const char *body_start = strstr(raw, "\r\n\r\n");
    if (body_start) {
        body_start += 4;
        int already = (int)strlen(body_start);
        if (already >= content_length) {
            char *b = malloc(content_length + 1);
            memcpy(b, body_start, content_length);
            b[content_length] = '\0';
            return b;
        }
    }

    // Need to read more from socket
    char *body = malloc(content_length + 1);
    int received = 0;
    if (body_start) {
        int already = (int)strlen(body_start);
        if (already > 0) {
            memcpy(body, body_start, already);
            received = already;
        }
    }
    while (received < content_length) {
        ssize_t n = recv(fd, body + received, content_length - received, 0);
        if (n <= 0) break;
        received += n;
    }
    body[received] = '\0';
    return body;
}

// ---------------------------------------------------------------------------
// Internal: build HTTP response string from a webi response map
// ---------------------------------------------------------------------------
static char *wb_build_http_response(Value *res_map) {
    Value *status_v = map_get(res_map, "status");
    Value *body_v   = map_get(res_map, "body");
    Value *ct_v     = map_get(res_map, "content_type");
    Value *hdrs_v   = map_get(res_map, "headers");

    long   status = status_v ? (long)status_v->as.number : 200;
    const char *body    = (body_v && body_v->type == VAL_STRING) ? body_v->as.string : "";
    const char *ct      = (ct_v  && ct_v->type  == VAL_STRING)  ? ct_v->as.string   : "text/plain; charset=utf-8";

    // Map status code to reason phrase
    const char *reason = "OK";
    if      (status == 200) reason = "OK";
    else if (status == 201) reason = "Created";
    else if (status == 204) reason = "No Content";
    else if (status == 301) reason = "Moved Permanently";
    else if (status == 302) reason = "Found";
    else if (status == 400) reason = "Bad Request";
    else if (status == 401) reason = "Unauthorized";
    else if (status == 403) reason = "Forbidden";
    else if (status == 404) reason = "Not Found";
    else if (status == 405) reason = "Method Not Allowed";
    else if (status == 500) reason = "Internal Server Error";

    int body_len = (int)strlen(body);

    char *out = NULL; int olen = 0, ocap = 0;
    char line[256];

    // Status line
    snprintf(line, sizeof(line), "HTTP/1.1 %ld %s\r\n", status, reason);
    wb_buf_str(&out, &olen, &ocap, line);

    // Standard headers
    wb_buf_str(&out, &olen, &ocap, "Server: webi/1.1.3 (Khan)\r\n");
    wb_buf_str(&out, &olen, &ocap, "Connection: close\r\n");
    snprintf(line, sizeof(line), "Content-Type: %s\r\n", ct);
    wb_buf_str(&out, &olen, &ocap, line);
    snprintf(line, sizeof(line), "Content-Length: %d\r\n", body_len);
    wb_buf_str(&out, &olen, &ocap, line);

    // Extra headers from res["headers"] map
    if (hdrs_v && hdrs_v->type == VAL_MAP) {
        for (int i = 0; i < AS_MAP_COUNT(*hdrs_v); i++) {
            const char *hk = AS_MAP_ENTRIES(*hdrs_v)[i].key;
            Value *hv_ptr = AS_MAP_ENTRIES(*hdrs_v)[i].value;
            if (hv_ptr && hv_ptr->type == VAL_STRING) {
                snprintf(line, sizeof(line), "%s: %s\r\n", hk, hv_ptr->as.string);
                wb_buf_str(&out, &olen, &ocap, line);
            }
        }
    }

    // Cookies from res["cookies"] array (v1.1.1)
    Value *cookies_v = map_get(res_map, "cookies");
    if (cookies_v && cookies_v->type == VAL_ARRAY) {
        for (int i = 0; i < AS_ARRAY_COUNT(*cookies_v); i++) {
            Value cv = AS_ARRAY_ITEMS(*cookies_v)[i];
            if (cv.type == VAL_STRING) {
                snprintf(line, sizeof(line), "Set-Cookie: %s\r\n", cv.as.string);
                wb_buf_str(&out, &olen, &ocap, line);
            }
        }
    }

    wb_buf_str(&out, &olen, &ocap, "\r\n");
    if (body_len > 0) wb_buf_str(&out, &olen, &ocap, body);

    return out;
}


// ===========================================================================
// POSIX OUTBOUND REQUESTS (curl subprocess, same strategy as requests_lib.c)
// ===========================================================================
#ifndef _WIN32

static char *wb_run_curl(const char **args, int nargs) {
    size_t cmd_len = 16;
    for (int i = 0; i < nargs; i++)
        cmd_len += strlen(args[i]) * 2 + 4;
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
// Signature (Khan): http_serve(port, app)
//
// The server:
//   1. Binds to 0.0.0.0:port
//   2. Accepts connections in a loop
//   3. Parses each HTTP request
//   4. Calls Khan's webi_handle(app, method, path, query, headers, body)
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

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
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

    g_webi_env = interp->base_env;  /* store env for accept loop */
    printf("[webi] Listening on port %d\n", port);
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

        // Read timeout so a client that connects and stalls can't hold the
        // connection (and this single-threaded server) open forever.
        struct timeval wb_timeout;
        wb_timeout.tv_sec = WB_SOCKET_TIMEOUT_SEC;
        wb_timeout.tv_usec = 0;
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &wb_timeout, sizeof(wb_timeout));

        // Read request
        char *raw = wb_read_request(client_fd);
        char method[512], path[512], query[512];
        wb_parse_request_line(raw, method, path, query);
        char *headers_str = wb_extract_headers(raw);
        char *body_str    = wb_read_body(client_fd, raw);
        if (!body_str) {
            // Body exceeded WB_MAX_BODY_SIZE — reject without buffering it.
            const char *resp =
                "HTTP/1.1 413 Payload Too Large\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send(client_fd, resp, strlen(resp), 0);
            free(raw); free(headers_str);
            close(client_fd);
            continue;
        }
        char  client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));

        // Call webi_handle(app, method, path, query, headers, body) in Khan
        // We need to look up the function from the interpreter environment.
        // We pass it via a synthetic AstNode call using interpreter_execute.
        // Simpler approach: look up "webi_handle" as a global native or Khan fn
        // and call its underlying C NativeFn / Khan fn body directly.

        // Build args array for webi_handle
        Value call_args[7];
        call_args[0] = value_copy(app);
        call_args[1] = value_string(method);
        call_args[2] = value_string(path);
        call_args[3] = value_string(query);
        call_args[4] = value_string(headers_str);
        call_args[5] = value_string(body_str);
        call_args[6] = value_string(client_ip);

        /* Call Khan webi_handle(app, method, path, query, headers, body, ip) */
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
        char *http_response = wb_build_http_response(&res_map);
        send(client_fd, http_response, strlen(http_response), 0);

        // Cleanup
        // NOTE: call_args are NOT freed here — khan_call_fn() binds them as
        // parameters in the callee's environment, and env_free() (called
        // internally by khan_call_fn after the function returns) already
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

    HINTERNET hSess = WinHttpOpen(L"Khan-webi/1.1",
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

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((u_short)port);
    bind(srv, (struct sockaddr *)&addr, sizeof(addr));
    listen(srv, 64);

    if (interp) g_webi_env = interp->base_env;
    printf("[webi] Listening on port %d\n", port);
    fflush(stdout);

    while (1) {
        struct sockaddr_in client_addr;
        int client_len = sizeof(client_addr);
        SOCKET cli = accept(srv, (struct sockaddr *)&client_addr, &client_len);
        if (cli == INVALID_SOCKET) continue;

        // Read timeout so a client that connects and stalls can't hold the
        // connection (and this single-threaded server) open forever.
        DWORD wb_timeout_ms = WB_SOCKET_TIMEOUT_SEC * 1000;
        setsockopt(cli, SOL_SOCKET, SO_RCVTIMEO, (const char *)&wb_timeout_ms, sizeof(wb_timeout_ms));

        /* Read raw request */
        char *raw = NULL;
        int rlen = 0, rcap = 0;
        char rbuf[4096];
        int n;
        int header_too_big = 0;
        while ((n = recv(cli, rbuf, sizeof(rbuf), 0)) > 0) {
            if (rlen + n + 1 > rcap) {
                rcap = rcap ? rcap * 2 : 8192;
                raw = realloc(raw, rcap);
            }
            memcpy(raw + rlen, rbuf, n);
            rlen += n;
            raw[rlen] = '\0';
            if (strstr(raw, "\r\n\r\n")) break;
            if (rlen > WB_MAX_HEADER_SIZE) { header_too_big = 1; break; }
        }
        if (!raw) { closesocket(cli); continue; }
        if (header_too_big) {
            const char *resp =
                "HTTP/1.1 431 Request Header Fields Too Large\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send(cli, resp, (int)strlen(resp), 0);
            free(raw);
            closesocket(cli);
            continue;
        }

        char method[512], path[512], query[512];
        wb_parse_request_line(raw, method, path, query);
        char *headers_str = wb_extract_headers(raw);
        char *body_str    = wb_read_body(cli, raw);
        if (!body_str) {
            const char *resp =
                "HTTP/1.1 413 Payload Too Large\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send(cli, resp, (int)strlen(resp), 0);
            free(raw); free(headers_str);
            closesocket(cli);
            continue;
        }

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

        char *http_response = wb_build_http_response(&res_map);
        send(cli, http_response, (int)strlen(http_response), 0);

        // NOTE: call_args are NOT freed here — see POSIX loop comment above;
        // khan_call_fn's internal env_free already owns/frees each param.
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
// the Khan side (routing.kh/template.kh). Every untrusted path segment —
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

// Khan: _webi_mime_type(path) -> string
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

// Khan: _webi_script_dir() -> string
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

// Khan: _webi_resolve_path(path) -> string
//
// Directory-aware resolution for a file path an app author wrote in
// their own source (a template path for render_file(), a folder for
// serve_static()) — same fallback order Phase 2 already uses for
// `import`: an absolute path is left alone; otherwise resolve relative
// to whatever file is currently executing (current_import_dir) if we're
// inside one, else relative to the top-level script's own directory
// (base_path), so it works the same regardless of what directory `khan`
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
// Khan's existing random() is seeded rand() — fine for game logic, but
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

// Khan: secure_token(num_bytes = 32) -> hex string (num_bytes*2 chars)
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

// ===========================================================================
// rate_limit_check — simple in-memory, per-key sliding-window rate limiter
// (v1.1.1 security)
//
// State has to live here in C, not in a Khan-visible map, because the
// native server passes http_serve a *copy* of the app map on every single
// request (see call_args[0] = value_copy(app) above) — anything mutated
// on a Khan map during one request is gone by the next one. This table is
// a real process-lifetime C global, so counts actually persist across
// requests the way a rate limiter needs them to.
//
// Limitations (documented, not silent): fixed-size table (1024 keys) with
// no eviction — under a very large number of distinct keys it can fill up,
// at which point it fails OPEN (allows the request) rather than locking
// everyone out. Resets on server restart. Not shared across multiple
// server processes/machines. Good enough for a single-instance app;
// swap for a real store (Redis etc.) if you scale beyond that.
// ===========================================================================
#define WB_RATE_LIMIT_BUCKETS 1024
#define WB_RATE_LIMIT_KEY_LEN 128

typedef struct {
    char key[WB_RATE_LIMIT_KEY_LEN];
    int in_use;
    int count;
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

    for (int i = 0; i < WB_RATE_LIMIT_BUCKETS; i++) {
        int idx = (int)((h + (unsigned long)i) % WB_RATE_LIMIT_BUCKETS);
        wb_rate_entry_t *e = &g_rate_table[idx];

        if (e->in_use && strncmp(e->key, key, WB_RATE_LIMIT_KEY_LEN) == 0) {
            if ((long)(now - e->window_start) >= window_seconds) {
                e->window_start = now;
                e->count = 1;
                return 1;
            }
            if (e->count >= max_requests) return 0;
            e->count++;
            return 1;
        }
        if (!e->in_use) {
            e->in_use = 1;
            strncpy(e->key, key, WB_RATE_LIMIT_KEY_LEN - 1);
            e->key[WB_RATE_LIMIT_KEY_LEN - 1] = '\0';
            e->window_start = now;
            e->count = 1;
            return 1;
        }
    }
    // Table is completely full — fail open rather than block everyone.
    return 1;
}

// Khan: rate_limit_check(key, max_requests, window_seconds) -> bool
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

    // static file serving (v1.1.2 — Phase 3)
    env_define(env, "_webi_mime_type",         value_native("_webi_mime_type",         fn_webi_mime_type));
    env_define(env, "_webi_safe_static_path",  value_native("_webi_safe_static_path",  fn_webi_safe_static_path));
    env_define(env, "_webi_resolve_path",      value_native("_webi_resolve_path",      fn_webi_resolve_path));
    env_define(env, "_webi_script_dir",        value_native("_webi_script_dir",        fn_webi_script_dir));
}