#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <winhttp.h>

// NOTE: windows.h's "TokenType" enum constant once collided with Khan's own
// lexer token-kind type. That's now permanently avoided by naming Khan's
// type TokenKind instead (see token.h) — no include ordering required.
#include "net.h"

// ===========================================================================
// Helper: check argument count and types
// ===========================================================================
static int net_check_arg_count(Interpreter *interp, const char *name,
                                int expected, int actual) {
    if (actual != expected) {
        fprintf(stderr, "Runtime error: %s() expects %d argument(s), got %d\n",
                name, expected, actual);
        interp->had_runtime_error = 1;
        return 0;
    }
    return 1;
}

static int net_expect_string(Interpreter *interp, const char *name,
                              int index, Value v, const char **out) {
    if (v.type != VAL_STRING) {
        fprintf(stderr, "Runtime error: %s() argument %d must be a string\n",
                name, index + 1);
        interp->had_runtime_error = 1;
        return 0;
    }
    *out = v.as.string;
    return 1;
}

// ===========================================================================
// HTTP GET request
// ===========================================================================
// http_get(url) -> returns a map:
//   {"status": 200, "body": "...", "success": true}
// On error: {"status": 0, "body": "", "success": false}
static void fn_http_get(Value *result, Interpreter *interp, int argc, Value *args) {
    if (!net_check_arg_count(interp, "http_get", 1, argc)) {
        *result = value_nil();
        return;
    }

    const char *url;
    if (!net_expect_string(interp, "http_get", 0, args[0], &url)) {
        *result = value_nil();
        return;
    }

    // Parse URL to extract host and path
    const char *host_start = url;
    const char *path_start = "/";

    if (strncmp(url, "https://", 8) == 0) {
        host_start = url + 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        host_start = url + 7;
    }

    const char *slash = strchr(host_start, '/');

    char *host = NULL;
    if (slash) {
        int host_len = (int)(slash - host_start);
        host = malloc(host_len + 1);
        strncpy(host, host_start, host_len);
        host[host_len] = '\0';
        path_start = slash;
    } else {
        host = strdup(host_start);
        path_start = "/";
    }

    BOOL use_https = (strncmp(url, "https", 5) == 0);
    INTERNET_PORT port = use_https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;

    HINTERNET hSession = WinHttpOpen(L"Khan/1.0",
                                      WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                      NULL, NULL, 0);

    Value result_map = value_map_empty();

    if (hSession) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, host, -1, NULL, 0);
        wchar_t *whost = malloc(wlen * sizeof(wchar_t));
        MultiByteToWideChar(CP_UTF8, 0, host, -1, whost, wlen);

        int plen = MultiByteToWideChar(CP_UTF8, 0, path_start, -1, NULL, 0);
        wchar_t *wpath = malloc(plen * sizeof(wchar_t));
        MultiByteToWideChar(CP_UTF8, 0, path_start, -1, wpath, plen);

        HINTERNET hConnect = WinHttpConnect(hSession, whost, port, 0);

        if (hConnect) {
            HINTERNET hRequest = WinHttpOpenRequest(hConnect,
                                                     L"GET", wpath, NULL,
                                                     NULL, NULL,
                                                     use_https ? WINHTTP_FLAG_SECURE : 0);

            if (hRequest) {
                DWORD_PTR context = 0;
                if (WinHttpSendRequest(hRequest, NULL, 0, NULL, 0, 0, context)) {
                    if (WinHttpReceiveResponse(hRequest, NULL)) {
                        DWORD status_code = 0;
                        DWORD status_size = sizeof(status_code);
                        WinHttpQueryHeaders(hRequest,
                                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                            NULL, &status_code, &status_size, NULL);

                        // Read response body
                        char *body = NULL;
                        DWORD body_size = 0;
                        DWORD total_read = 0;
                        DWORD bytes_read = 0;

                        WinHttpQueryDataAvailable(hRequest, &body_size);
                        if (body_size > 0) {
                            body = malloc(body_size + 1);
                            while (WinHttpReadData(hRequest, body + total_read,
                                                    body_size - total_read, &bytes_read) &&
                                   bytes_read > 0) {
                                total_read += bytes_read;
                                if (total_read >= body_size) {
                                    body_size += 4096;
                                    body = realloc(body, body_size + 1);
                                }
                            }
                            body[total_read] = '\0';
                        } else {
                            body = strdup("");
                        }

                        map_set(&result_map, "status", value_number((double)status_code));
                        map_set(&result_map, "body", value_string(body ? body : ""));
                        map_set(&result_map, "success", value_bool(status_code >= 200 && status_code < 300));

                        if (body) free(body);
                    }
                }
                WinHttpCloseHandle(hRequest);
            }
            WinHttpCloseHandle(hConnect);
        }
        free(whost);
        free(wpath);
        WinHttpCloseHandle(hSession);
    }

    if (map_get(&result_map, "success") == NULL) {
        map_set(&result_map, "status", value_number(0));
        map_set(&result_map, "body", value_string(""));
        map_set(&result_map, "success", value_bool(0));
    }

    free(host);
    *result = result_map;
}

// ===========================================================================
// Registration
// ===========================================================================
void net_register_all(Environment *env) {
    env_define(env, "http_get", value_native("http_get", fn_http_get));
}