#ifndef KOSH_REQUESTS_LIB_H
#define KOSH_REQUESTS_LIB_H

#include "interpreter.h"

// Registers HTTP request functions:
//   http_get(url)                    -> response map
//   http_post(url, body)             -> response map
//   http_post_json(url, map)         -> response map  (auto-encodes map to JSON)
//   http_put(url, body)              -> response map
//   http_delete(url)                 -> response map
//   http_request(method, url, body, headers) -> response map
//
// Response map keys: "status" (number), "body" (string), "success" (bool), and "error"
// (string, only when the request failed or was refused: bad URL, timeout, ...).
//   http_configure(options)          -> map of timeouts / size limits now in force
//   url_encode(text), url_decode(text, plus_is_space)
void requests_register_all(Environment *env);

/* Export function pointers for VM registration */
void fn_http_get(Value *result, Interpreter *interp, int argc, Value *args);
void fn_http_post(Value *result, Interpreter *interp, int argc, Value *args);
void fn_http_post_json(Value *result, Interpreter *interp, int argc, Value *args);
void fn_http_put(Value *result, Interpreter *interp, int argc, Value *args);
void fn_http_delete(Value *result, Interpreter *interp, int argc, Value *args);
void fn_http_request(Value *result, Interpreter *interp, int argc, Value *args);
void fn_http_configure(Value *result, Interpreter *interp, int argc, Value *args);
void fn_url_encode(Value *result, Interpreter *interp, int argc, Value *args);
void fn_url_decode(Value *result, Interpreter *interp, int argc, Value *args);

/* The hardened HTTP core, for webi_lib.c's http_get_h / http_post_h / ...:
 * method, url, optional body, optional first header ("Name: value"), and any further
 * headers as one "A: 1\r\nB: 2" string. Returns the usual {status, body, success[, error]} map. */
Value kh_http_call_strings(const char *method, const char *url, const char *body,
                           const char *ctype, const char *header_string);

#endif
