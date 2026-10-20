#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "json_lib.h"

// ===========================================================================
// JSON ENCODE
// ===========================================================================

// Forward declaration for recursive encoding
static void json_encode_value(Value v, char **buf, int *len, int *cap);

static void buf_append(char **buf, int *len, int *cap, const char *s, int slen) {
    while (*len + slen + 1 > *cap) {
        *cap = *cap ? *cap * 2 : 256;
        *buf = realloc(*buf, *cap);
    }
    memcpy(*buf + *len, s, slen);
    *len += slen;
    (*buf)[*len] = '\0';
}

static void buf_char(char **buf, int *len, int *cap, char c) {
    buf_append(buf, len, cap, &c, 1);
}

static void json_encode_string(const char *s, char **buf, int *len, int *cap) {
    buf_char(buf, len, cap, '"');
    for (const char *p = s; *p; p++) {
        switch (*p) {
            case '"':  buf_append(buf, len, cap, "\\\"", 2); break;
            case '\\': buf_append(buf, len, cap, "\\\\", 2); break;
            case '\n': buf_append(buf, len, cap, "\\n",  2); break;
            case '\r': buf_append(buf, len, cap, "\\r",  2); break;
            case '\t': buf_append(buf, len, cap, "\\t",  2); break;
            default:
                if ((unsigned char)*p < 0x20) {
                    char esc[8];
                    snprintf(esc, sizeof(esc), "\\u%04x", (unsigned char)*p);
                    buf_append(buf, len, cap, esc, 6);
                } else {
                    buf_char(buf, len, cap, *p);
                }
        }
    }
    buf_char(buf, len, cap, '"');
}

static void json_encode_value(Value v, char **buf, int *len, int *cap) {
    char tmp[64];
    switch (v.type) {
        case VAL_NIL:
            buf_append(buf, len, cap, "null", 4);
            break;
        case VAL_BOOL:
            if (v.as.boolean) buf_append(buf, len, cap, "true", 4);
            else              buf_append(buf, len, cap, "false", 5);
            break;
        case VAL_NUMBER: {
            // Format: if it's an integer value, no decimal point
            double d = v.as.number;
            if (d == (long long)d && d >= -1e15 && d <= 1e15)
                snprintf(tmp, sizeof(tmp), "%lld", (long long)d);
            else
                snprintf(tmp, sizeof(tmp), "%.17g", d);
            buf_append(buf, len, cap, tmp, (int)strlen(tmp));
            break;
        }
        case VAL_STRING:
            json_encode_string(v.as.string, buf, len, cap);
            break;
        case VAL_ARRAY:
            buf_char(buf, len, cap, '[');
            for (int i = 0; i < AS_ARRAY_COUNT(v); i++) {
                if (i > 0) buf_append(buf, len, cap, ", ", 2);
                json_encode_value(AS_ARRAY_ITEMS(v)[i], buf, len, cap);
            }
            buf_char(buf, len, cap, ']');
            break;
        case VAL_MAP:
            buf_char(buf, len, cap, '{');
            for (int i = 0; i < AS_MAP_COUNT(v); i++) {
                if (i > 0) buf_append(buf, len, cap, ", ", 2);
                json_encode_string(AS_MAP_ENTRIES(v)[i].key, buf, len, cap);
                buf_append(buf, len, cap, ": ", 2);
                json_encode_value(*AS_MAP_ENTRIES(v)[i].value, buf, len, cap);
            }
            buf_char(buf, len, cap, '}');
            break;
        default:
            buf_append(buf, len, cap, "null", 4);
            break;
    }
}

void fn_json_encode(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    if (argc != 1) {
        fprintf(stderr, "Runtime error: json_encode() expects 1 argument\n");
        if (interp) interp->had_runtime_error = 1;
        *result = value_nil();
        return;
    }
    char *buf = NULL;
    int len = 0, cap = 0;
    json_encode_value(args[0], &buf, &len, &cap);
    *result = value_string(buf ? buf : "null");
    free(buf);
}

// ===========================================================================
// JSON DECODE  (recursive-descent parser over a C string)
// ===========================================================================

typedef struct {
    const char *src;
    int pos;
    int len;
    int error;
} JsonParser;

static void jp_skip_ws(JsonParser *jp) {
    while (jp->pos < jp->len && isspace((unsigned char)jp->src[jp->pos]))
        jp->pos++;
}

static Value jp_parse_value(JsonParser *jp);

static Value jp_parse_string(JsonParser *jp) {
    jp->pos++; // skip opening "
    char *out = malloc(jp->len + 1);
    int olen = 0;
    while (jp->pos < jp->len && jp->src[jp->pos] != '"') {
        if (jp->src[jp->pos] == '\\' && jp->pos + 1 < jp->len) {
            jp->pos++;
            char c = jp->src[jp->pos];
            switch (c) {
                case '"':  out[olen++] = '"';  break;
                case '\\': out[olen++] = '\\'; break;
                case '/':  out[olen++] = '/';  break;
                case 'n':  out[olen++] = '\n'; break;
                case 'r':  out[olen++] = '\r'; break;
                case 't':  out[olen++] = '\t'; break;
                case 'b':  out[olen++] = '\b'; break;
                case 'f':  out[olen++] = '\f'; break;
                case 'u': {
                    // Parse 4 hex digits (basic BMP only)
                    if (jp->pos + 4 < jp->len) {
                        char hex[5] = {
                            jp->src[jp->pos+1], jp->src[jp->pos+2],
                            jp->src[jp->pos+3], jp->src[jp->pos+4], 0
                        };
                        unsigned int cp = (unsigned int)strtol(hex, NULL, 16);
                        jp->pos += 4;
                        // Encode as UTF-8
                        if (cp < 0x80) {
                            out[olen++] = (char)cp;
                        } else if (cp < 0x800) {
                            out[olen++] = (char)(0xC0 | (cp >> 6));
                            out[olen++] = (char)(0x80 | (cp & 0x3F));
                        } else {
                            out[olen++] = (char)(0xE0 | (cp >> 12));
                            out[olen++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                            out[olen++] = (char)(0x80 | (cp & 0x3F));
                        }
                    }
                    break;
                }
                default: out[olen++] = c; break;
            }
        } else {
            out[olen++] = jp->src[jp->pos];
        }
        jp->pos++;
    }
    if (jp->pos < jp->len) jp->pos++; // skip closing "
    out[olen] = '\0';
    Value v = value_string(out);
    free(out);
    return v;
}

static Value jp_parse_number(JsonParser *jp) {
    char *end;
    double d = strtod(jp->src + jp->pos, &end);
    jp->pos = (int)(end - jp->src);
    return value_number(d);
}

static Value jp_parse_array(JsonParser *jp) {
    jp->pos++; // skip '['
    jp_skip_ws(jp);
    if (jp->pos < jp->len && jp->src[jp->pos] == ']') {
        jp->pos++;
        return value_array(NULL, 0);
    }
    Value *items = NULL;
    int count = 0, capacity = 0;
    while (jp->pos < jp->len) {
        jp_skip_ws(jp);
        Value elem = jp_parse_value(jp);
        if (count >= capacity) {
            capacity = capacity ? capacity * 2 : 4;
            items = realloc(items, sizeof(Value) * capacity);
        }
        items[count++] = elem;
        jp_skip_ws(jp);
        if (jp->pos < jp->len && jp->src[jp->pos] == ',') { jp->pos++; continue; }
        if (jp->pos < jp->len && jp->src[jp->pos] == ']') { jp->pos++; break; }
        jp->error = 1; break;
    }
    return value_array(items, count);
}

static Value jp_parse_object(JsonParser *jp) {
    jp->pos++; // skip '{'
    jp_skip_ws(jp);
    Value map = value_map_empty();
    if (jp->pos < jp->len && jp->src[jp->pos] == '}') {
        jp->pos++;
        return map;
    }
    while (jp->pos < jp->len) {
        jp_skip_ws(jp);
        if (jp->src[jp->pos] != '"') { jp->error = 1; break; }
        Value key_v = jp_parse_string(jp);
        const char *key = key_v.as.string;
        jp_skip_ws(jp);
        if (jp->pos < jp->len && jp->src[jp->pos] == ':') jp->pos++;
        jp_skip_ws(jp);
        Value val = jp_parse_value(jp);
        map_set(&map, key, val);
        value_free(key_v);

        jp_skip_ws(jp);
        if (jp->pos < jp->len && jp->src[jp->pos] == ',') { jp->pos++; continue; }
        if (jp->pos < jp->len && jp->src[jp->pos] == '}') { jp->pos++; break; }
        jp->error = 1; break;
    }
    return map;
}

static Value jp_parse_value(JsonParser *jp) {
    jp_skip_ws(jp);
    if (jp->pos >= jp->len) return value_nil();

    char c = jp->src[jp->pos];
    if (c == '"') return jp_parse_string(jp);
    if (c == '[') return jp_parse_array(jp);
    if (c == '{') return jp_parse_object(jp);
    if (c == 't' && strncmp(jp->src + jp->pos, "true", 4) == 0) {
        jp->pos += 4; return value_bool(1);
    }
    if (c == 'f' && strncmp(jp->src + jp->pos, "false", 5) == 0) {
        jp->pos += 5; return value_bool(0);
    }
    if (c == 'n' && strncmp(jp->src + jp->pos, "null", 4) == 0) {
        jp->pos += 4; return value_nil();
    }
    if (c == '-' || isdigit((unsigned char)c)) return jp_parse_number(jp);

    jp->error = 1;
    return value_nil();
}

void fn_json_decode(Value *result, Interpreter *interp, int argc, Value *args) {
    if (argc != 1 || args[0].type != VAL_STRING) {
        fprintf(stderr, "Runtime error: json_decode() expects 1 string argument\n");
        if (interp) interp->had_runtime_error = 1;
        *result = value_nil();
        return;
    }
    const char *src = args[0].as.string;
    JsonParser jp = { src, 0, (int)strlen(src), 0 };
    *result = jp_parse_value(&jp);
    if (jp.error) {
        value_free(*result);
        *result = value_nil();
    }
}

// ===========================================================================
// Registration
// ===========================================================================
void json_register_all(Environment *env) {
    env_define(env, "json_encode", value_native("json_encode", fn_json_encode));
    env_define(env, "json_decode", value_native("json_decode", fn_json_decode));
}
