#include "vm.h"
#include "khan_stdlib.h"
#include "json_lib.h"
#include "datetime_lib.h"
#include "requests_lib.h"
#include "webi_lib.h"
#include "sqlite_lib.h"

void vm_register_builtins(VM *vm) {
    // Standard Library
    vm_global_set_native(vm, "len",        fn_len);
    vm_global_set_native(vm, "str",        fn_str);
    vm_global_set_native(vm, "num",        fn_num);
    vm_global_set_native(vm, "type",       fn_type);
    vm_global_set_native(vm, "push",       fn_push);
    vm_global_set_native(vm, "range",      fn_range);
    vm_global_set_native(vm, "keys",       fn_keys);
    vm_global_set_native(vm, "has",        fn_has);
    vm_global_set_native(vm, "abs",        fn_abs);
    vm_global_set_native(vm, "sqrt",       fn_sqrt);
    vm_global_set_native(vm, "floor",      fn_floor);
    vm_global_set_native(vm, "ceil",       fn_ceil);
    vm_global_set_native(vm, "round",      fn_round);
    vm_global_set_native(vm, "pow",        fn_pow);
    vm_global_set_native(vm, "min",        fn_min);
    vm_global_set_native(vm, "max",        fn_max);
    vm_global_set_native(vm, "random",     fn_random);
    vm_global_set_native(vm, "clock",      fn_clock);
    vm_global_set_native(vm, "gc_collect", fn_gc_collect);
    vm_global_set_native(vm, "upper",      fn_upper);
    vm_global_set_native(vm, "lower",      fn_lower);
    vm_global_set_native(vm, "trim",       fn_trim);
    vm_global_set_native(vm, "contains",   fn_contains);
    vm_global_set_native(vm, "substring",  fn_substring);
    vm_global_set_native(vm, "split",      fn_split);
    vm_global_set_native(vm, "str_replace",fn_str_replace);
    vm_global_set_native(vm, "input",      fn_input);
    vm_global_set_native(vm, "read_file",  fn_read_file);
    vm_global_set_native(vm, "write_file", fn_write_file);
    vm_global_set_native(vm, "file_exists",fn_file_exists);
    vm_global_set_native(vm, "exit",       fn_exit);
    vm_global_set_native(vm, "sleep",      fn_sleep);
    vm_global_set_native(vm, "base64_decode_to_file", fn_base64_decode_to_file);
    vm_global_set_native(vm, "remove_file", fn_remove_file);
}

void json_register_all_vm(VM *vm) {
    vm_global_set_native(vm, "json_encode", fn_json_encode);
    vm_global_set_native(vm, "json_decode", fn_json_decode);
}

void datetime_register_all_vm(VM *vm) {
    vm_global_set_native(vm, "now",         fn_now);
    vm_global_set_native(vm, "utcnow",      fn_utcnow);
    vm_global_set_native(vm, "timestamp",   fn_timestamp);
    vm_global_set_native(vm, "date_format", fn_date_format);
    vm_global_set_native(vm, "date_parse",  fn_date_parse);
    vm_global_set_native(vm, "date_diff",   fn_date_diff);
    vm_global_set_native(vm, "date_add",    fn_date_add);
}

void requests_register_all_vm(VM *vm) {
    vm_global_set_native(vm, "http_get",       fn_http_get);
    vm_global_set_native(vm, "http_post",      fn_http_post);
    vm_global_set_native(vm, "http_post_json", fn_http_post_json);
    vm_global_set_native(vm, "http_put",       fn_http_put);
    vm_global_set_native(vm, "http_delete",    fn_http_delete);
    vm_global_set_native(vm, "http_request",   fn_http_request);
}

void webi_register_all_vm(VM *vm) {
    vm_global_set_native(vm, "http_get_h",      fn_http_get_h);
    vm_global_set_native(vm, "http_post_h",     fn_http_post_h);
    vm_global_set_native(vm, "http_put_h",      fn_http_put_h);
    vm_global_set_native(vm, "http_put_json",   fn_http_put_json);
    vm_global_set_native(vm, "http_patch",      fn_http_patch);
    vm_global_set_native(vm, "http_patch_json", fn_http_patch_json);
    vm_global_set_native(vm, "http_delete_h",   fn_http_delete_h);
    vm_global_set_native(vm, "http_head",       fn_http_head);
    vm_global_set_native(vm, "http_serve",      fn_http_serve);
    vm_global_set_native(vm, "secure_token",    fn_secure_token);
    vm_global_set_native(vm, "rate_limit_check",fn_rate_limit_check);
    vm_global_set_native(vm, "_webi_mime_type",        fn_webi_mime_type);
    vm_global_set_native(vm, "_webi_safe_static_path", fn_webi_safe_static_path);
    vm_global_set_native(vm, "_webi_resolve_path",     fn_webi_resolve_path);
    vm_global_set_native(vm, "_webi_script_dir",       fn_webi_script_dir);
}
