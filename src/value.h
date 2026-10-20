#ifndef KHAN_VM_VALUE_H
#define KHAN_VM_VALUE_H

/*
 * value.h — VM-specific re-exports of core types.
 */

#include <stddef.h> /* size_t, for str_length() */
#include "interpreter.h"

/* ── VM specific ── */
#define vm_val_nil    value_nil
#define vm_val_bool   value_bool
#define vm_val_number value_number
#define vm_val_string value_string

int  vm_is_truthy(Value v);
int  vm_values_equal(Value a, Value b);
void vm_print_value(Value v);
size_t str_length(const char *s);

#endif
