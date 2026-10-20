// ===========================================================================
// NOTE: windows.h's winnt.h defines an enum constant literally named
// "TokenType" (part of _TOKEN_INFORMATION_CLASS, used by the Win32 security
// token APIs). That collides with a type name, not just another constant,
// so no #include ordering can fix it — one of the two names has to change.
// Khan's own lexer token-kind enum was renamed to TokenKind (see token.h)
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
#ifdef _WIN32

static Value winhttp_request(Interpreter *interp,
                              const char *method,
                              const char *url,
                              const char *body,
                              Value headers) {
    (void)interp;

    const char *host_start = url;
    if      (strncmp(url, "https://", 8) == 0) host_start = url + 8;
    else if (strncmp(url, "http://",  7) == 0) host_start = url + 7;

    const char *slash = strchr(host_start, '/');
    char *host;
    const char *path;
    if (slash) {
        int hlen = (int)(slash - host_start);
        host = malloc(hlen + 1);
        strncpy(host, host_start, hlen);
        host[hlen] = '\0';
        path = slash;
    } else {
        host = strdup(host_start);
        path = "/";
    }

    BOOL use_https = (strncmp(url, "https", 5) == 0);
    INTERNET_PORT port = use_https ? INTERNET_DEFAULT_HTTPS_PORT
                                   : INTERNET_DEFAULT_HTTP_PORT;

    HINTERNET hSession = WinHttpOpen(L"Khan/1.1",
                                      WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                      NULL, NULL, 0);
    if (!hSession) {
        free(host);
        return make_response(0, "WinHttpOpen failed", 0);
    }

    int wlen = MultiByteToWideChar(CP_UTF8, 0, host, -1, NULL, 0);
    wchar_t *whost = malloc(wlen * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, host, -1, whost, wlen);

    int plen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    wchar_t *wpath = malloc(plen * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, plen);

    int mlen = MultiByteToWideChar(CP_UTF8, 0, method, -1, NULL, 0);
    wchar_t *wmethod = malloc(mlen * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, method, -1, wmethod, mlen);

    HINTERNET hConnect = WinHttpConnect(hSession, whost, port, 0);
    Value result = make_response(0, "Connection failed", 0);

    if (hConnect) {
        HINTERNET hReq = WinHttpOpenRequest(hConnect, wmethod, wpath,
                                             NULL, NULL, NULL,
                                             use_https ? WINHTTP_FLAG_SECURE : 0);
        if (hReq) {
            // Add custom headers if provided
            if (headers.type == VAL_MAP) {
                for (int i = 0; i < AS_MAP_COUNT(headers); i++) {
                    char hbuf[1024];
                    const char *val = "";
                    Value *hv = AS_MAP_ENTRIES(headers)[i].value;
                    if (hv && hv->type == VAL_STRING) {
                        val = hv->as.string;
                    }
                    snprintf(hbuf, sizeof(hbuf), "%s: %s",
                             AS_MAP_ENTRIES(headers)[i].key, val);

                    int hwlen = MultiByteToWideChar(CP_UTF8, 0, hbuf, -1, NULL, 0);
                    wchar_t *wh = malloc(hwlen * sizeof(wchar_t));
                    MultiByteToWideChar(CP_UTF8, 0, hbuf, -1, wh, hwlen);
                    WinHttpAddRequestHeaders(hReq, wh, (DWORD)-1,
                                             WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
                    free(wh);
                }
            }

            DWORD blen = body ? (DWORD)strlen(body) : 0;
            if (WinHttpSendRequest(hReq, NULL, 0,
                                    (LPVOID)body, blen, blen, 0) &&
                WinHttpReceiveResponse(hReq, NULL)) {

                DWORD status_code = 0, ssz = sizeof(status_code);
                WinHttpQueryHeaders(hReq,
                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    NULL, &status_code, &ssz, NULL);

                char *rbuf = NULL;
                DWORD total = 0, bytes_read = 0, avail = 0;
                while (WinHttpQueryDataAvailable(hReq, &avail) && avail > 0) {
                    rbuf = realloc(rbuf, total + avail + 1);
                    WinHttpReadData(hReq, rbuf + total, avail, &bytes_read);
                    total += bytes_read;
                    rbuf[total] = '\0';
                }
                if (!rbuf) rbuf = strdup("");
                result = make_response((long)status_code, rbuf,
                                        status_code >= 200 && status_code < 300);
                free(rbuf);
            }
            WinHttpCloseHandle(hReq);
        }
        WinHttpCloseHandle(hConnect);
    }

    free(whost); free(wpath); free(wmethod);
    WinHttpCloseHandle(hSession);
    free(host);
    return result;
}

void fn_http_get(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!req_check(interp, "http_get", 1, argc)) { *result = value_nil(); return; }
    if (args[0].type != VAL_STRING) {
        fprintf(stderr, "Runtime error: http_get(url) expects a string\n");
        interp->had_runtime_error = 1; *result = value_nil(); return;
    }
    *result = winhttp_request(interp, "GET", args[0].as.string, NULL, value_nil());
}

void fn_http_post(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!req_check(interp, "http_post", 2, argc)) { *result = value_nil(); return; }
    if (args[0].type != VAL_STRING || args[1].type != VAL_STRING) {
        fprintf(stderr, "Runtime error: http_post(url, body) - both must be strings\n");
        interp->had_runtime_error = 1; *result = value_nil(); return;
    }
    Value h = value_map_empty();
    map_set(&h, "Content-Type", value_string("application/x-www-form-urlencoded"));
    *result = winhttp_request(interp, "POST", args[0].as.string, args[1].as.string, h);
    value_free(h);
}

void fn_http_put(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!req_check(interp, "http_put", 2, argc)) { *result = value_nil(); return; }
    if (args[0].type != VAL_STRING || args[1].type != VAL_STRING) {
        fprintf(stderr, "Runtime error: http_put(url, body) - both must be strings\n");
        interp->had_runtime_error = 1; *result = value_nil(); return;
    }
    Value h = value_map_empty();
    map_set(&h, "Content-Type", value_string("application/x-www-form-urlencoded"));
    *result = winhttp_request(interp, "PUT", args[0].as.string, args[1].as.string, h);
    value_free(h);
}

void fn_http_delete(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!req_check(interp, "http_delete", 1, argc)) { *result = value_nil(); return; }
    if (args[0].type != VAL_STRING) {
        fprintf(stderr, "Runtime error: http_delete(url) expects a string\n");
        interp->had_runtime_error = 1; *result = value_nil(); return;
    }
    *result = winhttp_request(interp, "DELETE", args[0].as.string, NULL, value_nil());
}

void fn_http_post_json(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!req_check(interp, "http_post_json", 2, argc)) { *result = value_nil(); return; }
    if (args[0].type != VAL_STRING) {
        fprintf(stderr, "Runtime error: http_post_json(url, data) - url must be a string\n");
        interp->had_runtime_error = 1; *result = value_nil(); return;
    }
    char *json_body = NULL; int jlen = 0, jcap = 0;
    req_json_encode(args[1], &json_body, &jlen, &jcap);
    Value h = value_map_empty();
    map_set(&h, "Content-Type", value_string("application/json"));
    *result = winhttp_request(interp, "POST", args[0].as.string,
                               json_body ? json_body : "{}", h);
    value_free(h);
    free(json_body);
}

void fn_http_request(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!req_check(interp, "http_request", 2, argc)) { *result = value_nil(); return; }
    if (args[0].type != VAL_STRING || args[1].type != VAL_STRING) {
        fprintf(stderr, "Runtime error: http_request(method, url, ...) - method and url must be strings\n");
        interp->had_runtime_error = 1; *result = value_nil(); return;
    }
    const char *method = args[0].as.string;
    const char *url = args[1].as.string;
    const char *body = (argc > 2 && args[2].type == VAL_STRING) ? args[2].as.string : NULL;
    Value headers = (argc > 3 && args[3].type == VAL_MAP) ? args[3] : value_nil();

    *result = winhttp_request(interp, method, url, body, headers);
}

// ===========================================================================
// PLATFORM: POSIX — curl subprocess
// ===========================================================================
#else  // !_WIN32

static char *run_curl(const char **args, int nargs) {
    // Build shell command: curl -s <args...>
    size_t cmd_len = 16;
    for (int i = 0; i < nargs; i++)
        cmd_len += strlen(args[i]) * 2 + 4;

    char *cmd = malloc(cmd_len);
    strcpy(cmd, "curl -s");
    for (int i = 0; i < nargs; i++) {
        strcat(cmd, " '");
        char *p = cmd + strlen(cmd);
        for (const char *q = args[i]; *q; q++) {
            if (*q == '\'') {
                *p++ = '\''; *p++ = '\\'; *p++ = '\''; *p++ = '\'';
            } else {
                *p++ = *q;
            }
        }
        *p++ = '\''; *p = '\0';
    }

    FILE *fp = popen(cmd, "r");
    free(cmd);
    if (!fp) return strdup("");

    char *out = NULL;
    size_t out_len = 0, cap = 0;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (out_len + n + 1 > cap) {
            cap = cap ? (cap + n) * 2 : 4096;
            out = realloc(out, cap);
        }
        memcpy(out + out_len, buf, n);
        out_len += n;
    }
    pclose(fp);
    if (!out) return strdup("");
    out[out_len] = '\0';
    return out;
}

static Value curl_request(const char *method, const char *url,
                           const char *body, Value headers) {
#define MAX_CURL_ARGS 64
    const char *args[MAX_CURL_ARGS];
    int n = 0;

    args[n++] = "-w";
    args[n++] = "\n__KHAN_STATUS__%{http_code}";

    if (method && strcmp(method, "GET") != 0) {
        args[n++] = "-X";
        args[n++] = method;
    }
    if (body) {
        args[n++] = "-d";
        args[n++] = body;
    }

    // Add custom headers
    char **hstrings = NULL;
    int hcount = 0;
    if (headers.type == VAL_MAP && headers.as.obj) {
        int mcount = headers.as.obj->as.map.count;
        hstrings = malloc(sizeof(char *) * mcount);
        for (int i = 0; i < mcount; i++) {
            char hbuf[1024];
            const char *val = "";
            MapEntry *entry = &headers.as.obj->as.map.entries[i];
            if (entry->value->type == VAL_STRING) {
                val = entry->value->as.string;
            }
            snprintf(hbuf, sizeof(hbuf), "%s: %s",
                     entry->key, val);
            hstrings[hcount++] = strdup(hbuf);
            if (n < MAX_CURL_ARGS - 2) {
                args[n++] = "-H";
                args[n++] = hstrings[hcount - 1];
            }
        }
    }

    args[n++] = url;

    char *raw = run_curl(args, n);

    // Cleanup header strings
    for (int i = 0; i < hcount; i++) free(hstrings[i]);
    free(hstrings);

    const char *marker = strstr(raw, "\n__KHAN_STATUS__");
    long status = 0;
    char *body_str;
    if (marker) {
        int blen = (int)(marker - raw);
        body_str = malloc(blen + 1);
        memcpy(body_str, raw, blen);
        body_str[blen] = '\0';
        status = atol(marker + (int)strlen("\n__KHAN_STATUS__"));
    } else {
        body_str = strdup(raw);
    }
    free(raw);

    Value result = make_response(status, body_str, status >= 200 && status < 300);
    free(body_str);
    return result;
}

void fn_http_get(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!req_check(interp, "http_get", 1, argc)) { *result = value_nil(); return; }
    if (args[0].type != VAL_STRING) {
        fprintf(stderr, "Runtime error: http_get(url) expects a string\n");
        interp->had_runtime_error = 1; *result = value_nil(); return;
    }
    *result = curl_request("GET", args[0].as.string, NULL, value_nil());
}

void fn_http_post(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!req_check(interp, "http_post", 2, argc)) { *result = value_nil(); return; }
    if (args[0].type != VAL_STRING || args[1].type != VAL_STRING) {
        fprintf(stderr, "Runtime error: http_post(url, body) expects strings\n");
        interp->had_runtime_error = 1; *result = value_nil(); return;
    }
    Value h = value_map_empty();
    map_set(&h, "Content-Type", value_string("application/x-www-form-urlencoded"));
    *result = curl_request("POST", args[0].as.string, args[1].as.string, h);
    value_free(h);
}

void fn_http_put(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!req_check(interp, "http_put", 2, argc)) { *result = value_nil(); return; }
    if (args[0].type != VAL_STRING || args[1].type != VAL_STRING) {
        fprintf(stderr, "Runtime error: http_put(url, body) expects strings\n");
        interp->had_runtime_error = 1; *result = value_nil(); return;
    }
    Value h = value_map_empty();
    map_set(&h, "Content-Type", value_string("application/x-www-form-urlencoded"));
    *result = curl_request("PUT", args[0].as.string, args[1].as.string, h);
    value_free(h);
}

void fn_http_delete(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!req_check(interp, "http_delete", 1, argc)) { *result = value_nil(); return; }
    if (args[0].type != VAL_STRING) {
        fprintf(stderr, "Runtime error: http_delete(url) expects a string\n");
        interp->had_runtime_error = 1; *result = value_nil(); return;
    }
    *result = curl_request("DELETE", args[0].as.string, NULL, value_nil());
}

void fn_http_post_json(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!req_check(interp, "http_post_json", 2, argc)) { *result = value_nil(); return; }
    if (args[0].type != VAL_STRING) {
        fprintf(stderr, "Runtime error: http_post_json(url, data) - url must be a string\n");
        interp->had_runtime_error = 1; *result = value_nil(); return;
    }
    char *json_body = NULL; int jlen = 0, jcap = 0;
    req_json_encode(args[1], &json_body, &jlen, &jcap);
    Value h = value_map_empty();
    map_set(&h, "Content-Type", value_string("application/json"));
    *result = curl_request("POST", args[0].as.string,
                            json_body ? json_body : "{}", h);
    value_free(h);
    free(json_body);
}

void fn_http_request(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!req_check(interp, "http_request", 2, argc)) { *result = value_nil(); return; }
    if (args[0].type != VAL_STRING || args[1].type != VAL_STRING) {
        fprintf(stderr, "Runtime error: http_request(method, url, ...) - method and url must be strings\n");
        interp->had_runtime_error = 1; *result = value_nil(); return;
    }
    const char *method = args[0].as.string;
    const char *url = args[1].as.string;
    const char *body = (argc > 2 && args[2].type == VAL_STRING) ? args[2].as.string : NULL;
    Value headers = (argc > 3 && args[3].type == VAL_MAP) ? args[3] : value_nil();

    *result = curl_request(method, url, body, headers);
}

#endif // !_WIN32

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
}