#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include "vision_lib.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#pragma GCC diagnostic pop

/* ===========================================================================
 * Image handle table
 *
 * Khan-level "images" are maps holding an integer __vision_id that indexes
 * into this table. The actual pixel buffer never touches the Khan value
 * system (see vision_lib.h for why).
 * ========================================================================= */

#define VISION_MAX_IMAGES 256

typedef struct {
    int in_use;
    unsigned char *data;
    int width;
    int height;
    int channels;
} VisionImage;

static VisionImage g_vision_images[VISION_MAX_IMAGES];

static int vision_alloc_slot(void) {
    for (int i = 0; i < VISION_MAX_IMAGES; i++) {
        if (!g_vision_images[i].in_use) return i;
    }
    return -1;
}

static Value vision_make_handle(int slot) {
    VisionImage *img = &g_vision_images[slot];
    Value m = value_map_empty();
    map_set(&m, "__vision_id", value_number(slot));
    map_set(&m, "width",       value_number(img->width));
    map_set(&m, "height",      value_number(img->height));
    map_set(&m, "channels",    value_number(img->channels));
    return m;
}

static int vision_get_slot(Value img_map) {
    if (img_map.type != VAL_MAP) return -1;
    Value *idv = map_get(&img_map, "__vision_id");
    if (!idv || idv->type != VAL_NUMBER) return -1;
    int slot = (int)idv->as.number;
    if (slot < 0 || slot >= VISION_MAX_IMAGES) return -1;
    if (!g_vision_images[slot].in_use) return -1;
    return slot;
}

static int vision_ends_with_ci(const char *s, const char *suffix) {
    size_t ls = strlen(s), lf = strlen(suffix);
    if (lf > ls) return 0;
    for (size_t i = 0; i < lf; i++) {
        if (tolower((unsigned char)s[ls - lf + i]) != tolower((unsigned char)suffix[i])) return 0;
    }
    return 1;
}

static int vision_clamp_byte(double v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (int)(v + 0.5);
}

/* ===========================================================================
 * Cross-file accessors — used by vision_cv.c and vision_cascade.c, which
 * need to read/create image buffers without duplicating this registry.
 * ========================================================================= */

int vision_internal_get_slot(Value img_map) {
    return vision_get_slot(img_map);
}

unsigned char *vision_internal_data(int slot) {
    if (slot < 0 || slot >= VISION_MAX_IMAGES || !g_vision_images[slot].in_use) return NULL;
    return g_vision_images[slot].data;
}

int vision_internal_width(int slot) {
    if (slot < 0 || slot >= VISION_MAX_IMAGES || !g_vision_images[slot].in_use) return 0;
    return g_vision_images[slot].width;
}

int vision_internal_height(int slot) {
    if (slot < 0 || slot >= VISION_MAX_IMAGES || !g_vision_images[slot].in_use) return 0;
    return g_vision_images[slot].height;
}

int vision_internal_channels(int slot) {
    if (slot < 0 || slot >= VISION_MAX_IMAGES || !g_vision_images[slot].in_use) return 0;
    return g_vision_images[slot].channels;
}

Value vision_internal_wrap(unsigned char *data, int w, int h, int c) {
    int slot = vision_alloc_slot();
    if (slot < 0) { free(data); return value_nil(); }
    g_vision_images[slot].in_use   = 1;
    g_vision_images[slot].data     = data;
    g_vision_images[slot].width    = w;
    g_vision_images[slot].height   = h;
    g_vision_images[slot].channels = c;
    return vision_make_handle(slot);
}

/* ===========================================================================
 * Native functions
 * ========================================================================= */

void fn_vision_load(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 1 || args[0].type != VAL_STRING) return;

    int slot = vision_alloc_slot();
    if (slot < 0) return;

    int w, h, c;
    unsigned char *data = stbi_load(args[0].as.string, &w, &h, &c, 0);
    if (!data) return;

    g_vision_images[slot].in_use   = 1;
    g_vision_images[slot].data     = data;
    g_vision_images[slot].width    = w;
    g_vision_images[slot].height   = h;
    g_vision_images[slot].channels = c;
    *result = vision_make_handle(slot);
}

void fn_vision_new(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 3) return;
    if (args[0].type != VAL_NUMBER || args[1].type != VAL_NUMBER || args[2].type != VAL_NUMBER) return;

    int w = (int)args[0].as.number;
    int h = (int)args[1].as.number;
    int c = (int)args[2].as.number;
    if (w <= 0 || h <= 0 || c < 1 || c > 4) return;

    int slot = vision_alloc_slot();
    if (slot < 0) return;

    unsigned char *data = calloc((size_t)w * (size_t)h * (size_t)c, 1);
    if (!data) return;

    g_vision_images[slot].in_use   = 1;
    g_vision_images[slot].data     = data;
    g_vision_images[slot].width    = w;
    g_vision_images[slot].height   = h;
    g_vision_images[slot].channels = c;
    *result = vision_make_handle(slot);
}

void fn_vision_free(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 1) return;
    int slot = vision_get_slot(args[0]);
    if (slot < 0) return;
    free(g_vision_images[slot].data);
    g_vision_images[slot].data   = NULL;
    g_vision_images[slot].in_use = 0;
}

void fn_vision_save(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_bool(0);
    if (argc < 2) return;
    int slot = vision_get_slot(args[0]);
    if (slot < 0) return;
    if (args[1].type != VAL_STRING) return;

    VisionImage *img = &g_vision_images[slot];
    const char *path = args[1].as.string;
    int ok;

    if (vision_ends_with_ci(path, ".bmp")) {
        ok = stbi_write_bmp(path, img->width, img->height, img->channels, img->data);
    } else if (vision_ends_with_ci(path, ".jpg") || vision_ends_with_ci(path, ".jpeg")) {
        ok = stbi_write_jpg(path, img->width, img->height, img->channels, img->data, 90);
    } else if (vision_ends_with_ci(path, ".tga")) {
        ok = stbi_write_tga(path, img->width, img->height, img->channels, img->data);
    } else {
        /* default: PNG, regardless of extension (covers ".png" and anything unrecognized) */
        ok = stbi_write_png(path, img->width, img->height, img->channels, img->data,
                             img->width * img->channels);
    }
    *result = value_bool(ok != 0);
}

void fn_vision_get_pixel(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 3) return;
    int slot = vision_get_slot(args[0]);
    if (slot < 0) return;
    if (args[1].type != VAL_NUMBER || args[2].type != VAL_NUMBER) return;

    VisionImage *img = &g_vision_images[slot];
    int x = (int)args[1].as.number;
    int y = (int)args[2].as.number;
    if (x < 0 || x >= img->width || y < 0 || y >= img->height) return;

    unsigned char *p = img->data + ((size_t)y * img->width + x) * img->channels;
    Value m = value_map_empty();
    map_set(&m, "r", value_number(p[0]));
    map_set(&m, "g", value_number(img->channels > 1 ? p[1] : p[0]));
    map_set(&m, "b", value_number(img->channels > 2 ? p[2] : p[0]));
    if (img->channels > 3) map_set(&m, "a", value_number(p[3]));
    *result = m;
}

void fn_vision_set_pixel(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_bool(0);
    if (argc < 6) return;
    int slot = vision_get_slot(args[0]);
    if (slot < 0) return;
    if (args[1].type != VAL_NUMBER || args[2].type != VAL_NUMBER ||
        args[3].type != VAL_NUMBER || args[4].type != VAL_NUMBER || args[5].type != VAL_NUMBER) return;

    VisionImage *img = &g_vision_images[slot];
    int x = (int)args[1].as.number;
    int y = (int)args[2].as.number;
    if (x < 0 || x >= img->width || y < 0 || y >= img->height) return;

    unsigned char *p = img->data + ((size_t)y * img->width + x) * img->channels;
    p[0] = (unsigned char)vision_clamp_byte(args[3].as.number);
    if (img->channels > 1) p[1] = (unsigned char)vision_clamp_byte(args[4].as.number);
    if (img->channels > 2) p[2] = (unsigned char)vision_clamp_byte(args[5].as.number);
    *result = value_bool(1);
}

void fn_vision_grayscale(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 1) return;
    int slot = vision_get_slot(args[0]);
    if (slot < 0) return;

    VisionImage *src = &g_vision_images[slot];
    int dst = vision_alloc_slot();
    if (dst < 0) return;

    size_t n = (size_t)src->width * src->height;
    unsigned char *data = malloc(n * src->channels);
    if (!data) return;

    for (size_t i = 0; i < n; i++) {
        unsigned char *sp = src->data + i * src->channels;
        unsigned char *dp = data + i * src->channels;
        double lum = (src->channels >= 3)
            ? (0.299 * sp[0] + 0.587 * sp[1] + 0.114 * sp[2])
            : sp[0];
        unsigned char g = (unsigned char)vision_clamp_byte(lum);
        dp[0] = g;
        if (src->channels > 1) dp[1] = g;
        if (src->channels > 2) dp[2] = g;
        if (src->channels > 3) dp[3] = sp[3];
    }

    g_vision_images[dst].in_use   = 1;
    g_vision_images[dst].data     = data;
    g_vision_images[dst].width    = src->width;
    g_vision_images[dst].height   = src->height;
    g_vision_images[dst].channels = src->channels;
    *result = vision_make_handle(dst);
}

void fn_vision_invert(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 1) return;
    int slot = vision_get_slot(args[0]);
    if (slot < 0) return;

    VisionImage *src = &g_vision_images[slot];
    int dst = vision_alloc_slot();
    if (dst < 0) return;

    size_t n = (size_t)src->width * src->height;
    unsigned char *data = malloc(n * src->channels);
    if (!data) return;

    int color_channels = src->channels >= 4 ? 3 : src->channels; /* leave alpha alone */
    for (size_t i = 0; i < n; i++) {
        unsigned char *sp = src->data + i * src->channels;
        unsigned char *dp = data + i * src->channels;
        for (int ch = 0; ch < src->channels; ch++) {
            dp[ch] = (ch < color_channels) ? (unsigned char)(255 - sp[ch]) : sp[ch];
        }
    }

    g_vision_images[dst].in_use   = 1;
    g_vision_images[dst].data     = data;
    g_vision_images[dst].width    = src->width;
    g_vision_images[dst].height   = src->height;
    g_vision_images[dst].channels = src->channels;
    *result = vision_make_handle(dst);
}

void fn_vision_crop(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 5) return;
    int slot = vision_get_slot(args[0]);
    if (slot < 0) return;
    for (int i = 1; i < 5; i++) if (args[i].type != VAL_NUMBER) return;

    VisionImage *src = &g_vision_images[slot];
    int x = (int)args[1].as.number;
    int y = (int)args[2].as.number;
    int w = (int)args[3].as.number;
    int h = (int)args[4].as.number;
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > src->width || y + h > src->height) return;

    int dst = vision_alloc_slot();
    if (dst < 0) return;
    unsigned char *data = malloc((size_t)w * h * src->channels);
    if (!data) return;

    for (int row = 0; row < h; row++) {
        memcpy(data + (size_t)row * w * src->channels,
               src->data + ((size_t)(y + row) * src->width + x) * src->channels,
               (size_t)w * src->channels);
    }

    g_vision_images[dst].in_use   = 1;
    g_vision_images[dst].data     = data;
    g_vision_images[dst].width    = w;
    g_vision_images[dst].height   = h;
    g_vision_images[dst].channels = src->channels;
    *result = vision_make_handle(dst);
}

void fn_vision_resize(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 3) return;
    int slot = vision_get_slot(args[0]);
    if (slot < 0) return;
    if (args[1].type != VAL_NUMBER || args[2].type != VAL_NUMBER) return;

    VisionImage *src = &g_vision_images[slot];
    int nw = (int)args[1].as.number;
    int nh = (int)args[2].as.number;
    if (nw <= 0 || nh <= 0) return;

    int dst = vision_alloc_slot();
    if (dst < 0) return;
    unsigned char *data = malloc((size_t)nw * nh * src->channels);
    if (!data) return;

    /* Nearest-neighbor resize — simple and dependency-free. Good enough
       for thumbnails; not as smooth as bilinear/bicubic for upscaling. */
    for (int y = 0; y < nh; y++) {
        int sy = (int)((double)y * src->height / nh);
        if (sy >= src->height) sy = src->height - 1;
        for (int x = 0; x < nw; x++) {
            int sx = (int)((double)x * src->width / nw);
            if (sx >= src->width) sx = src->width - 1;
            memcpy(data + ((size_t)y * nw + x) * src->channels,
                   src->data + ((size_t)sy * src->width + sx) * src->channels,
                   src->channels);
        }
    }

    g_vision_images[dst].in_use   = 1;
    g_vision_images[dst].data     = data;
    g_vision_images[dst].width    = nw;
    g_vision_images[dst].height   = nh;
    g_vision_images[dst].channels = src->channels;
    *result = vision_make_handle(dst);
}

void fn_vision_flip_h(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 1) return;
    int slot = vision_get_slot(args[0]);
    if (slot < 0) return;

    VisionImage *src = &g_vision_images[slot];
    int dst = vision_alloc_slot();
    if (dst < 0) return;
    unsigned char *data = malloc((size_t)src->width * src->height * src->channels);
    if (!data) return;

    for (int y = 0; y < src->height; y++) {
        for (int x = 0; x < src->width; x++) {
            int mirrored_x = src->width - 1 - x;
            memcpy(data + ((size_t)y * src->width + x) * src->channels,
                   src->data + ((size_t)y * src->width + mirrored_x) * src->channels,
                   src->channels);
        }
    }

    g_vision_images[dst].in_use   = 1;
    g_vision_images[dst].data     = data;
    g_vision_images[dst].width    = src->width;
    g_vision_images[dst].height   = src->height;
    g_vision_images[dst].channels = src->channels;
    *result = vision_make_handle(dst);
}

void fn_vision_flip_v(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 1) return;
    int slot = vision_get_slot(args[0]);
    if (slot < 0) return;

    VisionImage *src = &g_vision_images[slot];
    int dst = vision_alloc_slot();
    if (dst < 0) return;
    unsigned char *data = malloc((size_t)src->width * src->height * src->channels);
    if (!data) return;

    size_t row_bytes = (size_t)src->width * src->channels;
    for (int y = 0; y < src->height; y++) {
        int mirrored_y = src->height - 1 - y;
        memcpy(data + (size_t)y * row_bytes,
               src->data + (size_t)mirrored_y * row_bytes,
               row_bytes);
    }

    g_vision_images[dst].in_use   = 1;
    g_vision_images[dst].data     = data;
    g_vision_images[dst].width    = src->width;
    g_vision_images[dst].height   = src->height;
    g_vision_images[dst].channels = src->channels;
    *result = vision_make_handle(dst);
}

void fn_vision_average_brightness(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 1) return;
    int slot = vision_get_slot(args[0]);
    if (slot < 0) return;

    VisionImage *img = &g_vision_images[slot];
    size_t n = (size_t)img->width * img->height;
    if (n == 0) { *result = value_number(0); return; }

    double sum = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char *p = img->data + i * img->channels;
        double lum = (img->channels >= 3) ? (0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]) : p[0];
        sum += lum;
    }
    *result = value_number(sum / (double)n);
}

/* Min/max/average luminosity in a single native pass — the Khan-level
   equivalent (looping vision_get_pixel per pixel) would mean millions
   of native calls + map allocations on a real photo. */
void fn_vision_brightness_stats(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 1) return;
    int slot = vision_get_slot(args[0]);
    if (slot < 0) return;

    VisionImage *img = &g_vision_images[slot];
    size_t n = (size_t)img->width * img->height;
    if (n == 0) return;

    double sum = 0, min_lum = 255.0, max_lum = 0.0;
    for (size_t i = 0; i < n; i++) {
        unsigned char *p = img->data + i * img->channels;
        double lum = (img->channels >= 3) ? (0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]) : p[0];
        if (lum < min_lum) min_lum = lum;
        if (lum > max_lum) max_lum = lum;
        sum += lum;
    }

    Value m = value_map_empty();
    map_set(&m, "min",     value_number(vision_clamp_byte(min_lum)));
    map_set(&m, "max",     value_number(vision_clamp_byte(max_lum)));
    map_set(&m, "average", value_number(sum / (double)n));
    *result = m;
}

/* Whether every pixel's R/G/B are within `tolerance` of each other. */
void fn_vision_is_grayscale(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 2) return;
    int slot = vision_get_slot(args[0]);
    if (slot < 0) return;
    if (args[1].type != VAL_NUMBER) return;

    VisionImage *img = &g_vision_images[slot];
    if (img->channels < 3) { *result = value_bool(1); return; }

    double tol = args[1].as.number;
    size_t n = (size_t)img->width * img->height;
    int is_gray = 1;
    for (size_t i = 0; i < n && is_gray; i++) {
        unsigned char *p = img->data + i * img->channels;
        if (fabs((double)p[0] - p[1]) > tol || fabs((double)p[1] - p[2]) > tol) {
            is_gray = 0;
        }
    }
    *result = value_bool(is_gray);
}

/* ===========================================================================
 * Registration
 * ========================================================================= */

void vision_register_all(Environment *env) {
    env_define(env, "vision_load",               value_native("vision_load",               fn_vision_load));
    env_define(env, "vision_new",                 value_native("vision_new",                fn_vision_new));
    env_define(env, "vision_free",                value_native("vision_free",               fn_vision_free));
    env_define(env, "vision_save",                value_native("vision_save",               fn_vision_save));
    env_define(env, "vision_get_pixel",           value_native("vision_get_pixel",          fn_vision_get_pixel));
    env_define(env, "vision_set_pixel",           value_native("vision_set_pixel",          fn_vision_set_pixel));
    env_define(env, "vision_grayscale",           value_native("vision_grayscale",          fn_vision_grayscale));
    env_define(env, "vision_invert",              value_native("vision_invert",             fn_vision_invert));
    env_define(env, "vision_crop",                value_native("vision_crop",               fn_vision_crop));
    env_define(env, "vision_resize",              value_native("vision_resize",             fn_vision_resize));
    env_define(env, "vision_flip_h",              value_native("vision_flip_h",             fn_vision_flip_h));
    env_define(env, "vision_flip_v",              value_native("vision_flip_v",             fn_vision_flip_v));
    env_define(env, "vision_average_brightness",  value_native("vision_average_brightness", fn_vision_average_brightness));
    env_define(env, "vision_brightness_stats",    value_native("vision_brightness_stats",   fn_vision_brightness_stats));
    env_define(env, "vision_is_grayscale",        value_native("vision_is_grayscale",       fn_vision_is_grayscale));
}

void vision_register_all_vm(VM *vm) {
    vm_global_set_native(vm, "vision_load",              fn_vision_load);
    vm_global_set_native(vm, "vision_new",                fn_vision_new);
    vm_global_set_native(vm, "vision_free",               fn_vision_free);
    vm_global_set_native(vm, "vision_save",               fn_vision_save);
    vm_global_set_native(vm, "vision_get_pixel",          fn_vision_get_pixel);
    vm_global_set_native(vm, "vision_set_pixel",          fn_vision_set_pixel);
    vm_global_set_native(vm, "vision_grayscale",          fn_vision_grayscale);
    vm_global_set_native(vm, "vision_invert",             fn_vision_invert);
    vm_global_set_native(vm, "vision_crop",               fn_vision_crop);
    vm_global_set_native(vm, "vision_resize",             fn_vision_resize);
    vm_global_set_native(vm, "vision_flip_h",             fn_vision_flip_h);
    vm_global_set_native(vm, "vision_flip_v",             fn_vision_flip_v);
    vm_global_set_native(vm, "vision_average_brightness", fn_vision_average_brightness);
    vm_global_set_native(vm, "vision_brightness_stats",   fn_vision_brightness_stats);
    vm_global_set_native(vm, "vision_is_grayscale",       fn_vision_is_grayscale);
}
