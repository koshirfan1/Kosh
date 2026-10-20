#ifndef KHAN_WEBI_LIB_H
#define KHAN_WEBI_LIB_H

#include "interpreter.h"

struct VM;

// ===========================================================================
// webi_lib — native HTTP server backend for the webi Khan package
//
// Registers:
//   http_serve(port, app)
//       Starts a blocking HTTP server on the given port.
//       For each incoming request it calls webi_handle(app, ...) in Khan-space
//       and writes the returned response map back to the socket.
//
// Additional natives registered here (v1.1 requests extensions):
//   http_get_h(url, headers)            GET with custom headers
//   http_post_h(url, body, headers)     POST with custom headers
//   http_put_h(url, body, headers)      PUT with custom headers
//   http_put_json(url, data)            PUT with JSON body
//   http_patch(url, body)               PATCH request
//   http_patch_json(url, data)          PATCH with JSON body
//   http_delete_h(url, headers)         DELETE with custom headers
//   http_head(url)                      HEAD request
// ===========================================================================
void webi_register_all(Environment *env);

/* Export function pointers for VM registration */
void fn_http_get_h(Value *result, Interpreter *interp, int argc, Value *args);
void fn_http_post_h(Value *result, Interpreter *interp, int argc, Value *args);
void fn_http_put_h(Value *result, Interpreter *interp, int argc, Value *args);
void fn_http_put_json(Value *result, Interpreter *interp, int argc, Value *args);
void fn_http_patch(Value *result, Interpreter *interp, int argc, Value *args);
void fn_http_patch_json(Value *result, Interpreter *interp, int argc, Value *args);
void fn_http_delete_h(Value *result, Interpreter *interp, int argc, Value *args);
void fn_http_head(Value *result, Interpreter *interp, int argc, Value *args);
void fn_http_serve(Value *result, Interpreter *interp, int argc, Value *args);
void fn_secure_token(Value *result, Interpreter *interp, int argc, Value *args);
void fn_rate_limit_check(Value *result, Interpreter *interp, int argc, Value *args);
void fn_webi_mime_type(Value *result, Interpreter *interp, int argc, Value *args);
void fn_webi_safe_static_path(Value *result, Interpreter *interp, int argc, Value *args);
void fn_webi_resolve_path(Value *result, Interpreter *interp, int argc, Value *args);
void fn_webi_script_dir(Value *result, Interpreter *interp, int argc, Value *args);

#endif
