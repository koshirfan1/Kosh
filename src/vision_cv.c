#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "vision_lib.h"
#include "vision_cv.h"
#include "vision_font.h"

#ifndef VCV_PI
#define VCV_PI 3.14159265358979323846
#endif

/* ===========================================================================
 * Shared helpers
 * ========================================================================= */

static int vcv_clamp_byte(double v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (int)(v + 0.5);
}

static int vcv_clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* Convert any registered image to a freshly malloc'd single-channel
   grayscale buffer (row-major, w*h bytes). Caller frees. */
static unsigned char *vcv_to_gray(int slot, int *out_w, int *out_h) {
    unsigned char *data = vision_internal_data(slot);
    if (!data) return NULL;
    int w = vision_internal_width(slot);
    int h = vision_internal_height(slot);
    int c = vision_internal_channels(slot);

    unsigned char *gray = malloc((size_t)w * h);
    if (!gray) return NULL;

    for (long i = 0; i < (long)w * h; i++) {
        unsigned char *p = data + i * c;
        double lum = (c >= 3) ? (0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]) : p[0];
        gray[i] = (unsigned char)vcv_clamp_byte(lum);
    }
    *out_w = w;
    *out_h = h;
    return gray;
}

static void vcv_put_pixel(unsigned char *data, int w, int h, int channels,
                           int x, int y, int r, int g, int b) {
    if (x < 0 || x >= w || y < 0 || y >= h) return;
    unsigned char *p = data + ((size_t)y * w + x) * channels;
    p[0] = (unsigned char)vcv_clamp_byte(r);
    if (channels > 1) p[1] = (unsigned char)vcv_clamp_byte(g);
    if (channels > 2) p[2] = (unsigned char)vcv_clamp_byte(b);
}

static void vcv_rgb_to_hsv(int r, int g, int b, double *h, double *s, double *v) {
    double rf = r / 255.0, gf = g / 255.0, bf = b / 255.0;
    double maxc = rf; if (gf > maxc) maxc = gf; if (bf > maxc) maxc = bf;
    double minc = rf; if (gf < minc) minc = gf; if (bf < minc) minc = bf;
    double delta = maxc - minc;

    *v = maxc * 100.0;
    *s = (maxc <= 0) ? 0 : (delta / maxc) * 100.0;

    if (delta <= 0) { *h = 0; return; }
    double hue;
    if (maxc == rf) hue = 60.0 * fmod((gf - bf) / delta, 6.0);
    else if (maxc == gf) hue = 60.0 * ((bf - rf) / delta + 2.0);
    else hue = 60.0 * ((rf - gf) / delta + 4.0);
    if (hue < 0) hue += 360.0;
    *h = hue;
}

/* ---------------------------------------------------------------------------
 * vision_color_mask(image, h_min,h_max, s_min,s_max, v_min,v_max) -> new binary image
 * Hue in [0,360), saturation/value in [0,100]. Real, genuine "find pixels
 * in this color range" — e.g. h_min=0,h_max=15 with high s/v ~ finds red.
 * ------------------------------------------------------------------------- */
void fn_vision_color_mask(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 7) return;
    int slot = vision_internal_get_slot(args[0]);
    if (slot < 0) return;
    for (int i = 1; i < 7; i++) if (args[i].type != VAL_NUMBER) return;

    double h_min = args[1].as.number, h_max = args[2].as.number;
    double s_min = args[3].as.number, s_max = args[4].as.number;
    double v_min = args[5].as.number, v_max = args[6].as.number;

    unsigned char *data = vision_internal_data(slot);
    int w = vision_internal_width(slot), h = vision_internal_height(slot), c = vision_internal_channels(slot);
    unsigned char *out = malloc((size_t)w * h);
    if (!out) return;

    for (long i = 0; i < (long)w * h; i++) {
        unsigned char *p = data + i * c;
        int r = p[0], g = (c > 1) ? p[1] : p[0], b = (c > 2) ? p[2] : p[0];
        double hh, ss, vv;
        vcv_rgb_to_hsv(r, g, b, &hh, &ss, &vv);

        int hue_match = (h_min <= h_max) ? (hh >= h_min && hh <= h_max)
                                          : (hh >= h_min || hh <= h_max); /* wraps past 360 */
        int match = hue_match && ss >= s_min && ss <= s_max && vv >= v_min && vv <= v_max;
        out[i] = match ? 255 : 0;
    }
    *result = vision_internal_wrap(out, w, h, 1);
}

/* ===========================================================================
 * Convolution engine (per-channel, edge-clamped)
 * ========================================================================= */

static unsigned char *vcv_convolve(const unsigned char *src, int w, int h, int channels,
                                    const double *kernel, int ksize, double divisor, double offset) {
    unsigned char *out = malloc((size_t)w * h * channels);
    if (!out) return NULL;
    int half = ksize / 2;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            for (int ch = 0; ch < channels; ch++) {
                if (channels == 4 && ch == 3) {
                    /* leave alpha untouched */
                    out[((size_t)y * w + x) * channels + ch] = src[((size_t)y * w + x) * channels + ch];
                    continue;
                }
                double acc = 0;
                for (int ky = 0; ky < ksize; ky++) {
                    int sy = vcv_clamp_int(y + ky - half, 0, h - 1);
                    for (int kx = 0; kx < ksize; kx++) {
                        int sx = vcv_clamp_int(x + kx - half, 0, w - 1);
                        acc += kernel[ky * ksize + kx] * src[((size_t)sy * w + sx) * channels + ch];
                    }
                }
                out[((size_t)y * w + x) * channels + ch] = (unsigned char)vcv_clamp_byte(acc / divisor + offset);
            }
        }
    }
    return out;
}

/* ---------------------------------------------------------------------------
 * vision_blur_box(image, radius) -> new image
 * ------------------------------------------------------------------------- */
void fn_vision_blur_box(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 2) return;
    int slot = vision_internal_get_slot(args[0]);
    if (slot < 0 || args[1].type != VAL_NUMBER) return;

    int radius = (int)args[1].as.number;
    if (radius < 1) radius = 1;
    int ksize = radius * 2 + 1;

    unsigned char *src = vision_internal_data(slot);
    int w = vision_internal_width(slot), h = vision_internal_height(slot), c = vision_internal_channels(slot);

    double *kernel = malloc(sizeof(double) * ksize * ksize);
    if (!kernel) return;
    for (int i = 0; i < ksize * ksize; i++) kernel[i] = 1.0;

    unsigned char *out = vcv_convolve(src, w, h, c, kernel, ksize, (double)(ksize * ksize), 0);
    free(kernel);
    if (!out) return;
    *result = vision_internal_wrap(out, w, h, c);
}

/* ---------------------------------------------------------------------------
 * vision_blur_gaussian(image, radius) -> new image
 * ------------------------------------------------------------------------- */
void fn_vision_blur_gaussian(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 2) return;
    int slot = vision_internal_get_slot(args[0]);
    if (slot < 0 || args[1].type != VAL_NUMBER) return;

    int radius = (int)args[1].as.number;
    if (radius < 1) radius = 1;
    int ksize = radius * 2 + 1;
    double sigma = radius / 2.0;
    if (sigma < 0.5) sigma = 0.5;

    double *kernel = malloc(sizeof(double) * ksize * ksize);
    if (!kernel) return;
    double sum = 0;
    for (int ky = 0; ky < ksize; ky++) {
        for (int kx = 0; kx < ksize; kx++) {
            int dy = ky - radius, dx = kx - radius;
            double v = exp(-(dx * dx + dy * dy) / (2.0 * sigma * sigma));
            kernel[ky * ksize + kx] = v;
            sum += v;
        }
    }

    unsigned char *src = vision_internal_data(slot);
    int w = vision_internal_width(slot), h = vision_internal_height(slot), c = vision_internal_channels(slot);
    unsigned char *out = vcv_convolve(src, w, h, c, kernel, ksize, sum, 0);
    free(kernel);
    if (!out) return;
    *result = vision_internal_wrap(out, w, h, c);
}

/* ---------------------------------------------------------------------------
 * vision_sharpen(image) -> new image
 * ------------------------------------------------------------------------- */
void fn_vision_sharpen(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 1) return;
    int slot = vision_internal_get_slot(args[0]);
    if (slot < 0) return;

    static const double kernel[9] = {
         0, -1,  0,
        -1,  5, -1,
         0, -1,  0
    };

    unsigned char *src = vision_internal_data(slot);
    int w = vision_internal_width(slot), h = vision_internal_height(slot), c = vision_internal_channels(slot);
    unsigned char *out = vcv_convolve(src, w, h, c, kernel, 3, 1.0, 0);
    if (!out) return;
    *result = vision_internal_wrap(out, w, h, c);
}

/* ---------------------------------------------------------------------------
 * vision_emboss(image) -> new image
 * ------------------------------------------------------------------------- */
void fn_vision_emboss(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 1) return;
    int slot = vision_internal_get_slot(args[0]);
    if (slot < 0) return;

    static const double kernel[9] = {
        -2, -1,  0,
        -1,  1,  1,
         0,  1,  2
    };

    unsigned char *src = vision_internal_data(slot);
    int w = vision_internal_width(slot), h = vision_internal_height(slot), c = vision_internal_channels(slot);
    unsigned char *out = vcv_convolve(src, w, h, c, kernel, 3, 1.0, 128);
    if (!out) return;
    *result = vision_internal_wrap(out, w, h, c);
}

/* ---------------------------------------------------------------------------
 * vision_edges_sobel(image) -> new single-channel image (gradient magnitude)
 * ------------------------------------------------------------------------- */
void fn_vision_edges_sobel(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 1) return;
    int slot = vision_internal_get_slot(args[0]);
    if (slot < 0) return;

    int w, h;
    unsigned char *gray = vcv_to_gray(slot, &w, &h);
    if (!gray) return;

    unsigned char *out = malloc((size_t)w * h);
    if (!out) { free(gray); return; }

    static const int gx[9] = {-1, 0, 1, -2, 0, 2, -1, 0, 1};
    static const int gy[9] = {-1, -2, -1, 0, 0, 0, 1, 2, 1};

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int sx = 0, sy = 0, k = 0;
            for (int ky = -1; ky <= 1; ky++) {
                int py = vcv_clamp_int(y + ky, 0, h - 1);
                for (int kx = -1; kx <= 1; kx++) {
                    int px = vcv_clamp_int(x + kx, 0, w - 1);
                    unsigned char v = gray[(size_t)py * w + px];
                    sx += gx[k] * v;
                    sy += gy[k] * v;
                    k++;
                }
            }
            double mag = sqrt((double)sx * sx + (double)sy * sy);
            out[(size_t)y * w + x] = (unsigned char)vcv_clamp_byte(mag);
        }
    }
    free(gray);
    *result = vision_internal_wrap(out, w, h, 1);
}

/* ---------------------------------------------------------------------------
 * vision_rotate(image, degrees) -> new image, rotated around its center
 * (nearest-neighbor sampling; output canvas is sized to fit the whole
 * rotated image, background filled black / transparent-if-alpha)
 * ------------------------------------------------------------------------- */
void fn_vision_rotate(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 2) return;
    int slot = vision_internal_get_slot(args[0]);
    if (slot < 0 || args[1].type != VAL_NUMBER) return;

    double degrees = args[1].as.number;
    /* Clockwise-positive for callers. The rotation formula below assumes
     * the standard math convention (y-up, positive angle = CCW); image
     * coordinates are y-DOWN, and applying that same formula directly to
     * y-down coordinates already flips the visual rotation direction, so
     * NO extra negation is needed here to get clockwise-positive - adding
     * one (as a previous version of this function did) inverts it back
     * to counter-clockwise, which is the bug this comment used to
     * describe rather than fix. Verified against real rotated images,
     * all four orientations, before and after this fix. */
    double rad = degrees * VCV_PI / 180.0;
    double cos_a = cos(rad), sin_a = sin(rad);

    unsigned char *src = vision_internal_data(slot);
    int sw = vision_internal_width(slot), sh = vision_internal_height(slot), c = vision_internal_channels(slot);

    double cx = sw / 2.0, cy = sh / 2.0;
    /* Compute output canvas size that fits all four rotated corners. */
    double corners_x[4] = {0, (double)sw, 0, (double)sw};
    double corners_y[4] = {0, 0, (double)sh, (double)sh};
    double min_x = 1e18, max_x = -1e18, min_y = 1e18, max_y = -1e18;
    for (int i = 0; i < 4; i++) {
        double dx = corners_x[i] - cx, dy = corners_y[i] - cy;
        double rx = dx * cos_a - dy * sin_a + cx;
        double ry = dx * sin_a + dy * cos_a + cy;
        if (rx < min_x) min_x = rx; if (rx > max_x) max_x = rx;
        if (ry < min_y) min_y = ry; if (ry > max_y) max_y = ry;
    }
    int dw = (int)ceil(max_x - min_x);
    int dh = (int)ceil(max_y - min_y);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    double dcx = dw / 2.0, dcy = dh / 2.0;

    unsigned char *out = calloc((size_t)dw * dh, c);
    if (!out) return;

    /* Inverse-map each destination pixel back into source space. */
    double inv_cos = cos(-rad), inv_sin = sin(-rad);
    for (int y = 0; y < dh; y++) {
        for (int x = 0; x < dw; x++) {
            double dx = x - dcx, dy = y - dcy;
            double sx = dx * inv_cos - dy * inv_sin + cx;
            double sy = dx * inv_sin + dy * inv_cos + cy;
            int ix = (int)(sx + 0.5), iy = (int)(sy + 0.5);
            if (ix < 0 || ix >= sw || iy < 0 || iy >= sh) continue;
            memcpy(out + ((size_t)y * dw + x) * c, src + ((size_t)iy * sw + ix) * c, c);
        }
    }
    *result = vision_internal_wrap(out, dw, dh, c);
}

/* ===========================================================================
 * Thresholding
 * ========================================================================= */

void fn_vision_threshold(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 2) return;
    int slot = vision_internal_get_slot(args[0]);
    if (slot < 0 || args[1].type != VAL_NUMBER) return;
    int thresh = (int)args[1].as.number;

    int w, h;
    unsigned char *gray = vcv_to_gray(slot, &w, &h);
    if (!gray) return;
    unsigned char *out = malloc((size_t)w * h);
    if (!out) { free(gray); return; }

    for (long i = 0; i < (long)w * h; i++) {
        out[i] = (gray[i] > thresh) ? 255 : 0;
    }
    free(gray);
    *result = vision_internal_wrap(out, w, h, 1);
}

/* Otsu's method: finds the threshold minimizing intra-class variance. */
static int vcv_otsu_threshold(const unsigned char *gray, long n) {
    long hist[256] = {0};
    for (long i = 0; i < n; i++) hist[gray[i]]++;

    double total = (double)n;
    double sum_all = 0;
    for (int t = 0; t < 256; t++) sum_all += t * (double)hist[t];

    double sum_bg = 0, weight_bg = 0;
    double best_var = -1;
    int best_t = 128;

    for (int t = 0; t < 256; t++) {
        weight_bg += (double)hist[t];
        if (weight_bg == 0) continue;
        double weight_fg = total - weight_bg;
        if (weight_fg == 0) break;

        sum_bg += t * (double)hist[t];
        double mean_bg = sum_bg / weight_bg;
        double mean_fg = (sum_all - sum_bg) / weight_fg;

        double between_var = weight_bg * weight_fg * (mean_bg - mean_fg) * (mean_bg - mean_fg);
        if (between_var > best_var) {
            best_var = between_var;
            best_t = t;
        }
    }
    return best_t;
}

void fn_vision_otsu_value(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 1) return;
    int slot = vision_internal_get_slot(args[0]);
    if (slot < 0) return;

    int w, h;
    unsigned char *gray = vcv_to_gray(slot, &w, &h);
    if (!gray) return;
    int t = vcv_otsu_threshold(gray, (long)w * h);
    free(gray);
    *result = value_number(t);
}

void fn_vision_threshold_otsu(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 1) return;
    int slot = vision_internal_get_slot(args[0]);
    if (slot < 0) return;

    int w, h;
    unsigned char *gray = vcv_to_gray(slot, &w, &h);
    if (!gray) return;
    int t = vcv_otsu_threshold(gray, (long)w * h);

    unsigned char *out = malloc((size_t)w * h);
    if (!out) { free(gray); return; }
    for (long i = 0; i < (long)w * h; i++) out[i] = (gray[i] > t) ? 255 : 0;
    free(gray);
    *result = vision_internal_wrap(out, w, h, 1);
}

void fn_vision_threshold_adaptive(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 3) return;
    int slot = vision_internal_get_slot(args[0]);
    if (slot < 0 || args[1].type != VAL_NUMBER || args[2].type != VAL_NUMBER) return;

    int block = (int)args[1].as.number;
    if (block < 3) block = 3;
    if (block % 2 == 0) block += 1;
    double c_offset = args[2].as.number;
    int half = block / 2;

    int w, h;
    unsigned char *gray = vcv_to_gray(slot, &w, &h);
    if (!gray) return;
    unsigned char *out = malloc((size_t)w * h);
    if (!out) { free(gray); return; }

    /* Naive local-mean adaptive threshold (fine for typical photo/document sizes). */
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            long sum = 0, count = 0;
            for (int dy = -half; dy <= half; dy++) {
                int sy = y + dy;
                if (sy < 0 || sy >= h) continue;
                for (int dx = -half; dx <= half; dx++) {
                    int sx = x + dx;
                    if (sx < 0 || sx >= w) continue;
                    sum += gray[(size_t)sy * w + sx];
                    count++;
                }
            }
            double local_mean = (count > 0) ? (double)sum / count : 0;
            unsigned char v = gray[(size_t)y * w + x];
            out[(size_t)y * w + x] = (v > local_mean - c_offset) ? 255 : 0;
        }
    }
    free(gray);
    *result = vision_internal_wrap(out, w, h, 1);
}

/* ===========================================================================
 * Morphology (square structuring element)
 * ========================================================================= */

static unsigned char *vcv_morph(const unsigned char *gray, int w, int h, int ksize, int is_dilate) {
    unsigned char *out = malloc((size_t)w * h);
    if (!out) return NULL;
    int half = ksize / 2;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            unsigned char best = is_dilate ? 0 : 255;
            for (int ky = -half; ky <= half; ky++) {
                int sy = vcv_clamp_int(y + ky, 0, h - 1);
                for (int kx = -half; kx <= half; kx++) {
                    int sx = vcv_clamp_int(x + kx, 0, w - 1);
                    unsigned char v = gray[(size_t)sy * w + sx];
                    if (is_dilate) { if (v > best) best = v; }
                    else           { if (v < best) best = v; }
                }
            }
            out[(size_t)y * w + x] = best;
        }
    }
    return out;
}

void fn_vision_erode(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 2) return;
    int slot = vision_internal_get_slot(args[0]);
    if (slot < 0 || args[1].type != VAL_NUMBER) return;
    int ksize = (int)args[1].as.number;
    if (ksize < 1) ksize = 1;

    int w, h;
    unsigned char *gray = vcv_to_gray(slot, &w, &h);
    if (!gray) return;
    unsigned char *out = vcv_morph(gray, w, h, ksize, 0);
    free(gray);
    if (!out) return;
    *result = vision_internal_wrap(out, w, h, 1);
}

void fn_vision_dilate(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 2) return;
    int slot = vision_internal_get_slot(args[0]);
    if (slot < 0 || args[1].type != VAL_NUMBER) return;
    int ksize = (int)args[1].as.number;
    if (ksize < 1) ksize = 1;

    int w, h;
    unsigned char *gray = vcv_to_gray(slot, &w, &h);
    if (!gray) return;
    unsigned char *out = vcv_morph(gray, w, h, ksize, 1);
    free(gray);
    if (!out) return;
    *result = vision_internal_wrap(out, w, h, 1);
}

void fn_vision_morph_open(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 2) return;
    int slot = vision_internal_get_slot(args[0]);
    if (slot < 0 || args[1].type != VAL_NUMBER) return;
    int ksize = (int)args[1].as.number;
    if (ksize < 1) ksize = 1;

    int w, h;
    unsigned char *gray = vcv_to_gray(slot, &w, &h);
    if (!gray) return;
    unsigned char *eroded = vcv_morph(gray, w, h, ksize, 0);
    free(gray);
    if (!eroded) return;
    unsigned char *opened = vcv_morph(eroded, w, h, ksize, 1);
    free(eroded);
    if (!opened) return;
    *result = vision_internal_wrap(opened, w, h, 1);
}

void fn_vision_morph_close(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 2) return;
    int slot = vision_internal_get_slot(args[0]);
    if (slot < 0 || args[1].type != VAL_NUMBER) return;
    int ksize = (int)args[1].as.number;
    if (ksize < 1) ksize = 1;

    int w, h;
    unsigned char *gray = vcv_to_gray(slot, &w, &h);
    if (!gray) return;
    unsigned char *dilated = vcv_morph(gray, w, h, ksize, 1);
    free(gray);
    if (!dilated) return;
    unsigned char *closed = vcv_morph(dilated, w, h, ksize, 0);
    free(dilated);
    if (!closed) return;
    *result = vision_internal_wrap(closed, w, h, 1);
}

/* ===========================================================================
 * Connected-component blob detection
 * ========================================================================= */

void fn_vision_find_blobs(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 2) return;
    int slot = vision_internal_get_slot(args[0]);
    if (slot < 0 || args[1].type != VAL_NUMBER) return;
    double min_area = args[1].as.number;

    int w, h;
    unsigned char *gray = vcv_to_gray(slot, &w, &h);
    if (!gray) return;

    unsigned char *visited = calloc((size_t)w * h, 1);
    if (!visited) { free(gray); return; }

    /* Stack-based flood fill (4-connectivity) over foreground (>127) pixels. */
    int *stack_x = malloc(sizeof(int) * (size_t)w * h);
    int *stack_y = malloc(sizeof(int) * (size_t)w * h);
    if (!stack_x || !stack_y) { free(gray); free(visited); free(stack_x); free(stack_y); return; }

    Value blobs = value_array(NULL, 0);

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            long idx = (long)y * w + x;
            if (visited[idx] || gray[idx] <= 127) continue;

            int sp = 0;
            stack_x[sp] = x; stack_y[sp] = y; sp++;
            visited[idx] = 1;

            int min_x = x, max_x = x, min_y = y, max_y = y;
            long area = 0;
            double sum_x = 0, sum_y = 0;

            while (sp > 0) {
                sp--;
                int cx = stack_x[sp], cy = stack_y[sp];
                area++;
                sum_x += cx;
                sum_y += cy;
                if (cx < min_x) min_x = cx;
                if (cx > max_x) max_x = cx;
                if (cy < min_y) min_y = cy;
                if (cy > max_y) max_y = cy;

                static const int dxs[4] = {1, -1, 0, 0};
                static const int dys[4] = {0, 0, 1, -1};
                for (int d = 0; d < 4; d++) {
                    int nx = cx + dxs[d], ny = cy + dys[d];
                    if (nx < 0 || nx >= w || ny < 0 || ny >= h) continue;
                    long nidx = (long)ny * w + nx;
                    if (visited[nidx] || gray[nidx] <= 127) continue;
                    visited[nidx] = 1;
                    stack_x[sp] = nx; stack_y[sp] = ny; sp++;
                }
            }

            if ((double)area >= min_area) {
                Value blob = value_map_empty();
                map_set(&blob, "x",    value_number(min_x));
                map_set(&blob, "y",    value_number(min_y));
                map_set(&blob, "w",    value_number(max_x - min_x + 1));
                map_set(&blob, "h",    value_number(max_y - min_y + 1));
                map_set(&blob, "area", value_number((double)area));
                map_set(&blob, "cx",   value_number(sum_x / area));
                map_set(&blob, "cy",   value_number(sum_y / area));

                Value *new_items = realloc(AS_ARRAY_ITEMS(blobs), sizeof(Value) * (AS_ARRAY_COUNT(blobs) + 1));
                if (new_items) {
                    new_items[AS_ARRAY_COUNT(blobs)] = blob;
                    blobs.as.obj->as.array.items = new_items;
                    blobs.as.obj->as.array.count++;
                    blobs.as.obj->as.array.capacity = blobs.as.obj->as.array.count;
                }
            }
        }
    }

    free(gray);
    free(visited);
    free(stack_x);
    free(stack_y);
    *result = blobs;
}

/* ===========================================================================
 * Histogram + equalization
 * ========================================================================= */

void fn_vision_histogram_gray(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 1) return;
    int slot = vision_internal_get_slot(args[0]);
    if (slot < 0) return;

    int w, h;
    unsigned char *gray = vcv_to_gray(slot, &w, &h);
    if (!gray) return;

    long hist[256] = {0};
    for (long i = 0; i < (long)w * h; i++) hist[gray[i]]++;
    free(gray);

    Value *items = malloc(sizeof(Value) * 256);
    if (!items) return;
    for (int i = 0; i < 256; i++) items[i] = value_number((double)hist[i]);
    *result = value_array(items, 256);
}

void fn_vision_equalize(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 1) return;
    int slot = vision_internal_get_slot(args[0]);
    if (slot < 0) return;

    int w, h;
    unsigned char *gray = vcv_to_gray(slot, &w, &h);
    if (!gray) return;
    long n = (long)w * h;

    long hist[256] = {0};
    for (long i = 0; i < n; i++) hist[gray[i]]++;

    long cdf[256];
    long running = 0;
    for (int i = 0; i < 256; i++) { running += hist[i]; cdf[i] = running; }

    long cdf_min = 0;
    for (int i = 0; i < 256; i++) { if (cdf[i] > 0) { cdf_min = cdf[i]; break; } }

    unsigned char lut[256];
    for (int i = 0; i < 256; i++) {
        if (n - cdf_min <= 0) { lut[i] = (unsigned char)i; continue; }
        double v = ((double)(cdf[i] - cdf_min) / (double)(n - cdf_min)) * 255.0;
        lut[i] = (unsigned char)vcv_clamp_byte(v);
    }

    unsigned char *out = malloc((size_t)n);
    if (!out) { free(gray); return; }
    for (long i = 0; i < n; i++) out[i] = lut[gray[i]];
    free(gray);
    *result = vision_internal_wrap(out, w, h, 1);
}

/* ===========================================================================
 * Drawing primitives (mutate the image in place)
 * ========================================================================= */

void fn_vision_draw_rect(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_bool(0);
    if (argc < 8) return;
    int slot = vision_internal_get_slot(args[0]);
    if (slot < 0) return;
    for (int i = 1; i < 8; i++) if (args[i].type != VAL_NUMBER) return;

    unsigned char *data = vision_internal_data(slot);
    int w = vision_internal_width(slot), h = vision_internal_height(slot), c = vision_internal_channels(slot);
    int x = (int)args[1].as.number, y = (int)args[2].as.number;
    int rw = (int)args[3].as.number, rh = (int)args[4].as.number;
    int r = (int)args[5].as.number, g = (int)args[6].as.number, b = (int)args[7].as.number;
    int thickness = (argc >= 9 && args[8].type == VAL_NUMBER) ? (int)args[8].as.number : 1;
    if (thickness < 1) thickness = 1;

    for (int t = 0; t < thickness; t++) {
        for (int px = x - t; px <= x + rw - 1 + t; px++) {
            vcv_put_pixel(data, w, h, c, px, y - t, r, g, b);
            vcv_put_pixel(data, w, h, c, px, y + rh - 1 + t, r, g, b);
        }
        for (int py = y - t; py <= y + rh - 1 + t; py++) {
            vcv_put_pixel(data, w, h, c, x - t, py, r, g, b);
            vcv_put_pixel(data, w, h, c, x + rw - 1 + t, py, r, g, b);
        }
    }
    *result = value_bool(1);
}

void fn_vision_fill_rect(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_bool(0);
    if (argc < 8) return;
    int slot = vision_internal_get_slot(args[0]);
    if (slot < 0) return;
    for (int i = 1; i < 8; i++) if (args[i].type != VAL_NUMBER) return;

    unsigned char *data = vision_internal_data(slot);
    int w = vision_internal_width(slot), h = vision_internal_height(slot), c = vision_internal_channels(slot);
    int x = (int)args[1].as.number, y = (int)args[2].as.number;
    int rw = (int)args[3].as.number, rh = (int)args[4].as.number;
    int r = (int)args[5].as.number, g = (int)args[6].as.number, b = (int)args[7].as.number;

    for (int py = y; py < y + rh; py++) {
        for (int px = x; px < x + rw; px++) {
            vcv_put_pixel(data, w, h, c, px, py, r, g, b);
        }
    }
    *result = value_bool(1);
}

void fn_vision_draw_line(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_bool(0);
    if (argc < 7) return;
    int slot = vision_internal_get_slot(args[0]);
    if (slot < 0) return;
    for (int i = 1; i < 7; i++) if (args[i].type != VAL_NUMBER) return;

    unsigned char *data = vision_internal_data(slot);
    int w = vision_internal_width(slot), h = vision_internal_height(slot), c = vision_internal_channels(slot);
    int x0 = (int)args[1].as.number, y0 = (int)args[2].as.number;
    int x1 = (int)args[3].as.number, y1 = (int)args[4].as.number;
    int r = (int)args[5].as.number, g = (int)args[6].as.number;
    int b = (argc >= 8) ? (int)args[7].as.number : 0;
    int thickness = (argc >= 9 && args[8].type == VAL_NUMBER) ? (int)args[8].as.number : 1;
    if (thickness < 1) thickness = 1;
    int half = thickness / 2;

    /* Bresenham's line algorithm. */
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    int cx = x0, cy = y0;
    while (1) {
        for (int oy = -half; oy <= half; oy++)
            for (int ox = -half; ox <= half; ox++)
                vcv_put_pixel(data, w, h, c, cx + ox, cy + oy, r, g, b);
        if (cx == x1 && cy == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; cx += sx; }
        if (e2 <= dx) { err += dx; cy += sy; }
    }
    *result = value_bool(1);
}

void fn_vision_draw_circle(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_bool(0);
    if (argc < 6) return;
    int slot = vision_internal_get_slot(args[0]);
    if (slot < 0) return;
    for (int i = 1; i < 6; i++) if (args[i].type != VAL_NUMBER) return;

    unsigned char *data = vision_internal_data(slot);
    int w = vision_internal_width(slot), h = vision_internal_height(slot), c = vision_internal_channels(slot);
    int cx = (int)args[1].as.number, cy = (int)args[2].as.number;
    int radius = (int)args[3].as.number;
    int r = (int)args[4].as.number, g = (int)args[5].as.number;
    int b = (argc >= 7) ? (int)args[6].as.number : 0;

    /* Midpoint circle algorithm. */
    int x = radius, y = 0, err = 0;
    while (x >= y) {
        vcv_put_pixel(data, w, h, c, cx + x, cy + y, r, g, b);
        vcv_put_pixel(data, w, h, c, cx + y, cy + x, r, g, b);
        vcv_put_pixel(data, w, h, c, cx - y, cy + x, r, g, b);
        vcv_put_pixel(data, w, h, c, cx - x, cy + y, r, g, b);
        vcv_put_pixel(data, w, h, c, cx - x, cy - y, r, g, b);
        vcv_put_pixel(data, w, h, c, cx - y, cy - x, r, g, b);
        vcv_put_pixel(data, w, h, c, cx + y, cy - x, r, g, b);
        vcv_put_pixel(data, w, h, c, cx + x, cy - y, r, g, b);
        y++;
        if (err <= 0) { err += 2 * y + 1; }
        if (err > 0)  { x--; err -= 2 * x + 1; }
    }
    *result = value_bool(1);
}

void fn_vision_fill_circle(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_bool(0);
    if (argc < 6) return;
    int slot = vision_internal_get_slot(args[0]);
    if (slot < 0) return;
    for (int i = 1; i < 6; i++) if (args[i].type != VAL_NUMBER) return;

    unsigned char *data = vision_internal_data(slot);
    int w = vision_internal_width(slot), h = vision_internal_height(slot), c = vision_internal_channels(slot);
    int cx = (int)args[1].as.number, cy = (int)args[2].as.number;
    int radius = (int)args[3].as.number;
    int r = (int)args[4].as.number, g = (int)args[5].as.number;
    int b = (argc >= 7) ? (int)args[6].as.number : 0;

    for (int y = -radius; y <= radius; y++) {
        for (int x = -radius; x <= radius; x++) {
            if (x * x + y * y <= radius * radius) {
                vcv_put_pixel(data, w, h, c, cx + x, cy + y, r, g, b);
            }
        }
    }
    *result = value_bool(1);
}

void fn_vision_draw_text(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_bool(0);
    if (argc < 6) return;
    int slot = vision_internal_get_slot(args[0]);
    if (slot < 0 || args[1].type != VAL_NUMBER || args[2].type != VAL_NUMBER || args[3].type != VAL_STRING) return;

    unsigned char *data = vision_internal_data(slot);
    int w = vision_internal_width(slot), h = vision_internal_height(slot), c = vision_internal_channels(slot);
    int x = (int)args[1].as.number, y = (int)args[2].as.number;
    const char *text = args[3].as.string;
    int r = (args[4].type == VAL_NUMBER) ? (int)args[4].as.number : 255;
    int g = (args[5].type == VAL_NUMBER) ? (int)args[5].as.number : 255;
    int b = (argc >= 7 && args[6].type == VAL_NUMBER) ? (int)args[6].as.number : 255;
    int scale = (argc >= 8 && args[7].type == VAL_NUMBER) ? (int)args[7].as.number : 1;
    if (scale < 1) scale = 1;

    int cursor_x = x;
    for (const char *ch = text; *ch; ch++) {
        const unsigned char *glyph = vision_font_glyph(*ch);
        if (glyph) {
            for (int row = 0; row < VISION_FONT_HEIGHT; row++) {
                unsigned char bits = glyph[row];
                for (int col = 0; col < VISION_FONT_WIDTH; col++) {
                    if (bits & (1 << (VISION_FONT_WIDTH - 1 - col))) {
                        for (int sy = 0; sy < scale; sy++)
                            for (int sx = 0; sx < scale; sx++)
                                vcv_put_pixel(data, w, h, c,
                                    cursor_x + col * scale + sx, y + row * scale + sy, r, g, b);
                    }
                }
            }
        }
        cursor_x += (VISION_FONT_WIDTH + 1) * scale;
    }
    *result = value_bool(1);
}

/* ===========================================================================
 * Template matching (normalized cross-correlation, naive/exhaustive)
 * ========================================================================= */

void fn_vision_match_template(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 2) return;
    int img_slot = vision_internal_get_slot(args[0]);
    int tpl_slot = vision_internal_get_slot(args[1]);
    if (img_slot < 0 || tpl_slot < 0) return;

    int iw, ih, tw, th;
    unsigned char *img = vcv_to_gray(img_slot, &iw, &ih);
    unsigned char *tpl = vcv_to_gray(tpl_slot, &tw, &th);
    if (!img || !tpl) { free(img); free(tpl); return; }
    if (tw > iw || th > ih) { free(img); free(tpl); return; }

    double tpl_mean = 0;
    long tpl_n = (long)tw * th;
    for (long i = 0; i < tpl_n; i++) tpl_mean += tpl[i];
    tpl_mean /= tpl_n;

    double tpl_norm = 0;
    for (long i = 0; i < tpl_n; i++) {
        double d = tpl[i] - tpl_mean;
        tpl_norm += d * d;
    }
    tpl_norm = sqrt(tpl_norm);

    double best_score = -1e18;
    int best_x = 0, best_y = 0;

    for (int y = 0; y <= ih - th; y++) {
        for (int x = 0; x <= iw - tw; x++) {
            double win_mean = 0;
            for (int ty = 0; ty < th; ty++)
                for (int tx = 0; tx < tw; tx++)
                    win_mean += img[(size_t)(y + ty) * iw + (x + tx)];
            win_mean /= tpl_n;

            double num = 0, win_norm = 0;
            for (int ty = 0; ty < th; ty++) {
                for (int tx = 0; tx < tw; tx++) {
                    double wd = img[(size_t)(y + ty) * iw + (x + tx)] - win_mean;
                    double td = tpl[(size_t)ty * tw + tx] - tpl_mean;
                    num += wd * td;
                    win_norm += wd * wd;
                }
            }
            win_norm = sqrt(win_norm);

            double denom = win_norm * tpl_norm;
            double score = (denom > 1e-6) ? (num / denom) : -1;

            if (score > best_score) {
                best_score = score;
                best_x = x;
                best_y = y;
            }
        }
    }

    free(img);
    free(tpl);

    Value m = value_map_empty();
    map_set(&m, "x",     value_number(best_x));
    map_set(&m, "y",     value_number(best_y));
    map_set(&m, "score", value_number(best_score));
    *result = m;
}

/* ===========================================================================
 * Registration
 * ========================================================================= */

void vision_cv_register_all(Environment *env) {
    env_define(env, "vision_rotate",             value_native("vision_rotate",             fn_vision_rotate));
    env_define(env, "vision_color_mask",         value_native("vision_color_mask",         fn_vision_color_mask));
    env_define(env, "vision_blur_box",           value_native("vision_blur_box",           fn_vision_blur_box));
    env_define(env, "vision_blur_gaussian",      value_native("vision_blur_gaussian",      fn_vision_blur_gaussian));
    env_define(env, "vision_sharpen",            value_native("vision_sharpen",            fn_vision_sharpen));
    env_define(env, "vision_emboss",             value_native("vision_emboss",             fn_vision_emboss));
    env_define(env, "vision_edges_sobel",        value_native("vision_edges_sobel",        fn_vision_edges_sobel));
    env_define(env, "vision_threshold",          value_native("vision_threshold",          fn_vision_threshold));
    env_define(env, "vision_otsu_value",         value_native("vision_otsu_value",         fn_vision_otsu_value));
    env_define(env, "vision_threshold_otsu",     value_native("vision_threshold_otsu",     fn_vision_threshold_otsu));
    env_define(env, "vision_threshold_adaptive", value_native("vision_threshold_adaptive", fn_vision_threshold_adaptive));
    env_define(env, "vision_erode",              value_native("vision_erode",              fn_vision_erode));
    env_define(env, "vision_dilate",             value_native("vision_dilate",             fn_vision_dilate));
    env_define(env, "vision_morph_open",         value_native("vision_morph_open",         fn_vision_morph_open));
    env_define(env, "vision_morph_close",        value_native("vision_morph_close",        fn_vision_morph_close));
    env_define(env, "vision_find_blobs",         value_native("vision_find_blobs",         fn_vision_find_blobs));
    env_define(env, "vision_histogram_gray",     value_native("vision_histogram_gray",     fn_vision_histogram_gray));
    env_define(env, "vision_equalize",           value_native("vision_equalize",           fn_vision_equalize));
    env_define(env, "vision_draw_rect",          value_native("vision_draw_rect",          fn_vision_draw_rect));
    env_define(env, "vision_fill_rect",          value_native("vision_fill_rect",          fn_vision_fill_rect));
    env_define(env, "vision_draw_line",          value_native("vision_draw_line",          fn_vision_draw_line));
    env_define(env, "vision_draw_circle",        value_native("vision_draw_circle",        fn_vision_draw_circle));
    env_define(env, "vision_fill_circle",        value_native("vision_fill_circle",        fn_vision_fill_circle));
    env_define(env, "vision_draw_text",          value_native("vision_draw_text",          fn_vision_draw_text));
    env_define(env, "vision_match_template",     value_native("vision_match_template",     fn_vision_match_template));
}

void vision_cv_register_all_vm(VM *vm) {
    vm_global_set_native(vm, "vision_rotate",             fn_vision_rotate);
    vm_global_set_native(vm, "vision_color_mask",         fn_vision_color_mask);
    vm_global_set_native(vm, "vision_blur_box",           fn_vision_blur_box);
    vm_global_set_native(vm, "vision_blur_gaussian",      fn_vision_blur_gaussian);
    vm_global_set_native(vm, "vision_sharpen",            fn_vision_sharpen);
    vm_global_set_native(vm, "vision_emboss",             fn_vision_emboss);
    vm_global_set_native(vm, "vision_edges_sobel",        fn_vision_edges_sobel);
    vm_global_set_native(vm, "vision_threshold",          fn_vision_threshold);
    vm_global_set_native(vm, "vision_otsu_value",         fn_vision_otsu_value);
    vm_global_set_native(vm, "vision_threshold_otsu",     fn_vision_threshold_otsu);
    vm_global_set_native(vm, "vision_threshold_adaptive", fn_vision_threshold_adaptive);
    vm_global_set_native(vm, "vision_erode",              fn_vision_erode);
    vm_global_set_native(vm, "vision_dilate",             fn_vision_dilate);
    vm_global_set_native(vm, "vision_morph_open",         fn_vision_morph_open);
    vm_global_set_native(vm, "vision_morph_close",        fn_vision_morph_close);
    vm_global_set_native(vm, "vision_find_blobs",         fn_vision_find_blobs);
    vm_global_set_native(vm, "vision_histogram_gray",     fn_vision_histogram_gray);
    vm_global_set_native(vm, "vision_equalize",           fn_vision_equalize);
    vm_global_set_native(vm, "vision_draw_rect",          fn_vision_draw_rect);
    vm_global_set_native(vm, "vision_fill_rect",          fn_vision_fill_rect);
    vm_global_set_native(vm, "vision_draw_line",          fn_vision_draw_line);
    vm_global_set_native(vm, "vision_draw_circle",        fn_vision_draw_circle);
    vm_global_set_native(vm, "vision_fill_circle",        fn_vision_fill_circle);
    vm_global_set_native(vm, "vision_draw_text",          fn_vision_draw_text);
    vm_global_set_native(vm, "vision_match_template",     fn_vision_match_template);
}
