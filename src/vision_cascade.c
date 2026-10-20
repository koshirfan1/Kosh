#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include "vision_lib.h"
#include "vision_cascade.h"

/* ===========================================================================
 * Cascade data structures
 * ========================================================================= */

typedef struct {
    int x, y, w, h;
    double weight;
} CascadeRect;

typedef struct {
    int rect_count;
    CascadeRect rects[3];
} CascadeFeature;

typedef struct {
    int feature_idx;
    double threshold;
    double leaf0, leaf1;
} CascadeClassifier;

typedef struct {
    double threshold;
    int classifier_start;
    int classifier_count;
} CascadeStage;

typedef struct {
    int in_use;
    int window_w, window_h;
    CascadeFeature *features;
    int feature_count;
    CascadeClassifier *classifiers;
    int classifier_count;
    CascadeStage *stages;
    int stage_count;
} HaarCascade;

#define VISION_MAX_CASCADES 16
static HaarCascade g_cascades[VISION_MAX_CASCADES];

static int vcasc_alloc_slot(void) {
    for (int i = 0; i < VISION_MAX_CASCADES; i++) if (!g_cascades[i].in_use) return i;
    return -1;
}

/* ===========================================================================
 * Tiny targeted number/tag scanner (not a general XML parser — the cascade
 * format is regular enough that scanning for specific tag markers and
 * pulling out runs of numbers between them is sufficient and far simpler
 * than a real DOM parser).
 * ========================================================================= */

static int vcasc_is_num_start(const char *p, const char *end) {
    if (p >= end) return 0;
    if (isdigit((unsigned char)*p)) return 1;
    if ((*p == '-' || *p == '+') && p + 1 < end && (isdigit((unsigned char)p[1]) || p[1] == '.')) return 1;
    if (*p == '.' && p + 1 < end && isdigit((unsigned char)p[1])) return 1;
    return 0;
}

static int vcasc_parse_doubles(const char *p, const char *end, double *arr, int max) {
    int n = 0;
    while (p < end && n < max) {
        if (!vcasc_is_num_start(p, end)) { p++; continue; }
        char *endp;
        double v = strtod(p, &endp);
        if (endp == p) { p++; continue; }
        arr[n++] = v;
        p = endp;
    }
    return n;
}

static int vcasc_count(const char *start, const char *end, const char *needle) {
    int count = 0;
    const char *p = start;
    while (p < end) {
        const char *found = strstr(p, needle);
        if (!found || found >= end) break;
        count++;
        p = found + strlen(needle);
    }
    return count;
}

/* ===========================================================================
 * Cascade XML parsing
 * ========================================================================= */

static int vcasc_parse_file(const char *path, HaarCascade *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = malloc((size_t)len + 1);
    if (!text) { fclose(f); return 0; }
    size_t rd = fread(text, 1, (size_t)len, f);
    fclose(f);
    text[rd] = 0;
    const char *end = text + rd;

    memset(out, 0, sizeof(*out));

    const char *wp = strstr(text, "<width>");
    const char *hp = strstr(text, "<height>");
    if (!wp || !hp) { free(text); return 0; }
    double wv[1], hv[1];
    vcasc_parse_doubles(wp + strlen("<width>"), end, wv, 1);
    vcasc_parse_doubles(hp + strlen("<height>"), end, hv, 1);
    out->window_w = (int)wv[0];
    out->window_h = (int)hv[0];

    /* --- features --- */
    const char *feat_start = strstr(text, "<features>");
    const char *feat_end = strstr(text, "</features>");
    if (!feat_start || !feat_end) { free(text); return 0; }
    feat_start += strlen("<features>");

    int feature_count = vcasc_count(feat_start, feat_end, "<rects>");
    CascadeFeature *features = calloc((size_t)feature_count, sizeof(CascadeFeature));
    if (!features) { free(text); return 0; }

    {
        const char *p = feat_start;
        int fi = 0;
        while (fi < feature_count) {
            const char *rstart = strstr(p, "<rects>");
            if (!rstart || rstart >= feat_end) break;
            rstart += strlen("<rects>");
            const char *rend = strstr(rstart, "</rects>");
            if (!rend) break;

            double nums[15];
            int n = vcasc_parse_doubles(rstart, rend, nums, 15);
            int rc = n / 5;
            if (rc > 3) rc = 3;
            features[fi].rect_count = rc;
            for (int r = 0; r < rc; r++) {
                features[fi].rects[r].x = (int)nums[r * 5 + 0];
                features[fi].rects[r].y = (int)nums[r * 5 + 1];
                features[fi].rects[r].w = (int)nums[r * 5 + 2];
                features[fi].rects[r].h = (int)nums[r * 5 + 3];
                features[fi].rects[r].weight = nums[r * 5 + 4];
            }
            p = rend + strlen("</rects>");
            fi++;
        }
    }

    /* --- stages --- */
    const char *stages_start = strstr(text, "<stages>");
    const char *stages_end = strstr(text, "</stages>");
    if (!stages_start || !stages_end) { free(text); free(features); return 0; }

    int stage_count = vcasc_count(stages_start, stages_end, "<stageThreshold>");
    int classifier_count = vcasc_count(stages_start, stages_end, "<internalNodes>");

    CascadeStage *stages = calloc((size_t)stage_count, sizeof(CascadeStage));
    CascadeClassifier *classifiers = calloc((size_t)classifier_count, sizeof(CascadeClassifier));
    if (!stages || !classifiers) {
        free(text); free(features); free(stages); free(classifiers);
        return 0;
    }

    {
        const char *p = stages_start;
        int si = 0, ci = 0;
        while (si < stage_count) {
            const char *sth = strstr(p, "<stageThreshold>");
            if (!sth || sth >= stages_end) break;
            const char *sth_val = sth + strlen("<stageThreshold>");
            double tval[1];
            vcasc_parse_doubles(sth_val, stages_end, tval, 1);

            const char *next_sth = strstr(sth_val, "<stageThreshold>");
            const char *stage_region_end = (next_sth && next_sth < stages_end) ? next_sth : stages_end;

            stages[si].threshold = tval[0];
            stages[si].classifier_start = ci;

            const char *cp = sth_val;
            while (1) {
                const char *inode = strstr(cp, "<internalNodes>");
                if (!inode || inode >= stage_region_end) break;
                const char *inode_val = inode + strlen("<internalNodes>");
                const char *inode_end = strstr(inode_val, "</internalNodes>");
                if (!inode_end) break;
                double nodevals[4];
                vcasc_parse_doubles(inode_val, inode_end, nodevals, 4);

                const char *lval = strstr(inode_end, "<leafValues>");
                if (!lval || lval >= stage_region_end) break;
                const char *lval_val = lval + strlen("<leafValues>");
                const char *lval_end = strstr(lval_val, "</leafValues>");
                if (!lval_end) break;
                double leafvals[2];
                vcasc_parse_doubles(lval_val, lval_end, leafvals, 2);

                classifiers[ci].feature_idx = (int)nodevals[2];
                classifiers[ci].threshold   = nodevals[3];
                classifiers[ci].leaf0       = leafvals[0];
                classifiers[ci].leaf1       = leafvals[1];
                ci++;
                cp = lval_end + strlen("</leafValues>");
            }

            stages[si].classifier_count = ci - stages[si].classifier_start;
            p = stage_region_end;
            si++;
        }
    }

    free(text);
    out->features = features;
    out->feature_count = feature_count;
    out->stages = stages;
    out->stage_count = stage_count;
    out->classifiers = classifiers;
    out->classifier_count = classifier_count;
    return (out->feature_count > 0 && out->stage_count > 0);
}

static void vcasc_free_cascade(HaarCascade *c) {
    free(c->features);
    free(c->classifiers);
    free(c->stages);
    memset(c, 0, sizeof(*c));
}

/* ===========================================================================
 * Integral image
 * ========================================================================= */

typedef struct {
    double *sum;
    double *sqsum;
    int stride; /* w + 1 */
} IntegralImage;

static int vcasc_build_integral(const unsigned char *gray, int w, int h, IntegralImage *ii) {
    int stride = w + 1;
    ii->stride = stride;
    ii->sum = calloc((size_t)stride * (h + 1), sizeof(double));
    ii->sqsum = calloc((size_t)stride * (h + 1), sizeof(double));
    if (!ii->sum || !ii->sqsum) { free(ii->sum); free(ii->sqsum); return 0; }

    for (int y = 1; y <= h; y++) {
        double row_sum = 0, row_sqsum = 0;
        for (int x = 1; x <= w; x++) {
            unsigned char v = gray[(size_t)(y - 1) * w + (x - 1)];
            row_sum += v;
            row_sqsum += (double)v * v;
            ii->sum[y * stride + x]   = ii->sum[(y - 1) * stride + x] + row_sum;
            ii->sqsum[y * stride + x] = ii->sqsum[(y - 1) * stride + x] + row_sqsum;
        }
    }
    return 1;
}

static inline double vcasc_rect_sum(const double *ii, int stride, int x, int y, int w, int h) {
    return ii[(size_t)(y + h) * stride + (x + w)] - ii[(size_t)y * stride + (x + w)]
         - ii[(size_t)(y + h) * stride + x] + ii[(size_t)y * stride + x];
}

/* ===========================================================================
 * Detection
 * ========================================================================= */

typedef struct { int x, y, w, h; } DetRect;

static int vcasc_rects_similar(DetRect *a, DetRect *b) {
    int min_w = a->w < b->w ? a->w : b->w;
    double eps = 0.3;
    int dist_x = abs(a->x - b->x);
    int dist_y = abs(a->y - b->y);
    int dist_w = abs(a->w - b->w);
    return dist_x <= min_w * eps && dist_y <= min_w * eps && dist_w <= min_w * eps * 2;
}

static int vcasc_uf_find(int *parent, int i) {
    while (parent[i] != i) { parent[i] = parent[parent[i]]; i = parent[i]; }
    return i;
}

static void vcasc_uf_union(int *parent, int a, int b) {
    int ra = vcasc_uf_find(parent, a), rb = vcasc_uf_find(parent, b);
    if (ra != rb) parent[ra] = rb;
}

/* Evaluate the full cascade at one window. Returns 1 if it passed every stage. */
static int vcasc_eval_window(HaarCascade *cascade, const IntegralImage *ii,
                              int x, int y, int window_size, double scale) {
    double area = (double)window_size * window_size;
    double total = vcasc_rect_sum(ii->sum, ii->stride, x, y, window_size, window_size);
    double sqtotal = vcasc_rect_sum(ii->sqsum, ii->stride, x, y, window_size, window_size);
    double mean = total / area;
    double variance = sqtotal / area - mean * mean;
    if (variance <= 0) return 0;
    double std = sqrt(variance);

    for (int s = 0; s < cascade->stage_count; s++) {
        CascadeStage *stage = &cascade->stages[s];
        double stage_sum = 0;

        for (int ci = 0; ci < stage->classifier_count; ci++) {
            CascadeClassifier *clf = &cascade->classifiers[stage->classifier_start + ci];
            CascadeFeature *feat = &cascade->features[clf->feature_idx];
            double feat_val = 0;

            for (int r = 0; r < feat->rect_count; r++) {
                CascadeRect *rc = &feat->rects[r];
                int rx = (int)(rc->x * scale + 0.5);
                int ry = (int)(rc->y * scale + 0.5);
                int rw = (int)(rc->w * scale + 0.5);
                int rh = (int)(rc->h * scale + 0.5);
                if (rw < 1) rw = 1;
                if (rh < 1) rh = 1;
                if (rx + rw > window_size) rw = window_size - rx;
                if (ry + rh > window_size) rh = window_size - ry;
                if (rw <= 0 || rh <= 0) continue;

                double rsum = vcasc_rect_sum(ii->sum, ii->stride, x + rx, y + ry, rw, rh);
                feat_val += rc->weight * rsum;
            }
            feat_val /= area;

            stage_sum += (feat_val < clf->threshold * std) ? clf->leaf0 : clf->leaf1;
        }

        if (stage_sum < stage->threshold) return 0;
    }
    return 1;
}

static DetRect *vcasc_detect(const unsigned char *gray, int img_w, int img_h, HaarCascade *cascade,
                              double scale_factor, int min_neighbors, int min_size, int max_size,
                              int *out_count) {
    *out_count = 0;
    IntegralImage ii;
    if (!vcasc_build_integral(gray, img_w, img_h, &ii)) return NULL;

    int base = cascade->window_w;
    if (base < 1) base = 20;

    DetRect *candidates = NULL;
    int cand_count = 0, cand_cap = 0;

    double scale = 1.0;
    int window_size = base;
    int smaller_dim = img_w < img_h ? img_w : img_h;

    while (window_size <= smaller_dim && (max_size <= 0 || window_size <= max_size)) {
        if (window_size >= min_size) {
            int step = (int)(window_size * 0.1);
            if (step < 1) step = 1;

            for (int y = 0; y + window_size <= img_h; y += step) {
                for (int x = 0; x + window_size <= img_w; x += step) {
                    if (vcasc_eval_window(cascade, &ii, x, y, window_size, scale)) {
                        if (cand_count >= cand_cap) {
                            cand_cap = cand_cap ? cand_cap * 2 : 64;
                            candidates = realloc(candidates, sizeof(DetRect) * (size_t)cand_cap);
                        }
                        candidates[cand_count].x = x;
                        candidates[cand_count].y = y;
                        candidates[cand_count].w = window_size;
                        candidates[cand_count].h = window_size;
                        cand_count++;
                    }
                }
            }
        }

        double next_scale = scale * scale_factor;
        int next_window = (int)(base * next_scale + 0.5);
        if (next_window <= window_size) next_window = window_size + 1;
        window_size = next_window;
        scale = next_scale;
    }

    free(ii.sum);
    free(ii.sqsum);

    if (cand_count == 0) { *out_count = 0; return NULL; }

    /* group overlapping candidates via union-find, keep groups with enough support */
    int *parent = malloc(sizeof(int) * (size_t)cand_count);
    for (int i = 0; i < cand_count; i++) parent[i] = i;
    for (int i = 0; i < cand_count; i++) {
        for (int j = i + 1; j < cand_count; j++) {
            if (vcasc_rects_similar(&candidates[i], &candidates[j])) {
                vcasc_uf_union(parent, i, j);
            }
        }
    }

    double *acc_x = calloc((size_t)cand_count, sizeof(double));
    double *acc_y = calloc((size_t)cand_count, sizeof(double));
    double *acc_w = calloc((size_t)cand_count, sizeof(double));
    double *acc_h = calloc((size_t)cand_count, sizeof(double));
    int *acc_n = calloc((size_t)cand_count, sizeof(int));

    for (int i = 0; i < cand_count; i++) {
        int r = vcasc_uf_find(parent, i);
        acc_x[r] += candidates[i].x;
        acc_y[r] += candidates[i].y;
        acc_w[r] += candidates[i].w;
        acc_h[r] += candidates[i].h;
        acc_n[r]++;
    }

    DetRect *results = malloc(sizeof(DetRect) * (size_t)cand_count);
    int result_count = 0;
    for (int i = 0; i < cand_count; i++) {
        if (vcasc_uf_find(parent, i) != i) continue; /* only roots */
        if (acc_n[i] >= min_neighbors) {
            results[result_count].x = (int)(acc_x[i] / acc_n[i]);
            results[result_count].y = (int)(acc_y[i] / acc_n[i]);
            results[result_count].w = (int)(acc_w[i] / acc_n[i]);
            results[result_count].h = (int)(acc_h[i] / acc_n[i]);
            result_count++;
        }
    }

    free(parent);
    free(acc_x); free(acc_y); free(acc_w); free(acc_h); free(acc_n);
    free(candidates);

    *out_count = result_count;
    return results;
}

/* ===========================================================================
 * Native functions
 * ========================================================================= */

void fn_vision_cascade_load(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 1 || args[0].type != VAL_STRING) return;

    int slot = vcasc_alloc_slot();
    if (slot < 0) return;

    if (!vcasc_parse_file(args[0].as.string, &g_cascades[slot])) {
        vcasc_free_cascade(&g_cascades[slot]);
        return;
    }
    g_cascades[slot].in_use = 1;

    Value m = value_map_empty();
    map_set(&m, "__cascade_id", value_number(slot));
    map_set(&m, "window_w", value_number(g_cascades[slot].window_w));
    map_set(&m, "window_h", value_number(g_cascades[slot].window_h));
    map_set(&m, "stages", value_number(g_cascades[slot].stage_count));
    *result = m;
}

void fn_vision_cascade_free(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 1 || args[0].type != VAL_MAP) return;
    Value *idv = map_get(&args[0], "__cascade_id");
    if (!idv || idv->type != VAL_NUMBER) return;
    int slot = (int)idv->as.number;
    if (slot < 0 || slot >= VISION_MAX_CASCADES || !g_cascades[slot].in_use) return;
    vcasc_free_cascade(&g_cascades[slot]);
}

void fn_vision_detect_objects(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_array(NULL, 0);
    if (argc < 5) return;

    int img_slot = vision_internal_get_slot(args[0]);
    if (img_slot < 0) return;
    if (args[1].type != VAL_MAP) return;
    Value *idv = map_get(&args[1], "__cascade_id");
    if (!idv || idv->type != VAL_NUMBER) return;
    int cslot = (int)idv->as.number;
    if (cslot < 0 || cslot >= VISION_MAX_CASCADES || !g_cascades[cslot].in_use) return;

    if (args[2].type != VAL_NUMBER || args[3].type != VAL_NUMBER || args[4].type != VAL_NUMBER) return;
    double scale_factor = args[2].as.number;
    int min_neighbors = (int)args[3].as.number;
    int min_size = (int)args[4].as.number;
    if (scale_factor <= 1.0) scale_factor = 1.1;
    if (min_neighbors < 1) min_neighbors = 1;
    if (min_size < g_cascades[cslot].window_w) min_size = g_cascades[cslot].window_w;

    unsigned char *data = vision_internal_data(img_slot);
    int w = vision_internal_width(img_slot);
    int h = vision_internal_height(img_slot);
    int c = vision_internal_channels(img_slot);
    if (!data) return;

    unsigned char *gray = malloc((size_t)w * h);
    if (!gray) return;
    for (long i = 0; i < (long)w * h; i++) {
        unsigned char *p = data + i * c;
        double lum = (c >= 3) ? (0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]) : p[0];
        if (lum < 0) lum = 0;
        if (lum > 255) lum = 255;
        gray[i] = (unsigned char)(lum + 0.5);
    }

    int count = 0;
    DetRect *dets = vcasc_detect(gray, w, h, &g_cascades[cslot], scale_factor, min_neighbors, min_size, 0, &count);
    free(gray);

    Value arr = value_array(NULL, 0);
    if (count > 0 && dets) {
        Value *items = malloc(sizeof(Value) * (size_t)count);
        for (int i = 0; i < count; i++) {
            Value m = value_map_empty();
            map_set(&m, "x", value_number(dets[i].x));
            map_set(&m, "y", value_number(dets[i].y));
            map_set(&m, "w", value_number(dets[i].w));
            map_set(&m, "h", value_number(dets[i].h));
            items[i] = m;
        }
        arr.as.obj->as.array.items = items;
        arr.as.obj->as.array.count = count;
        arr.as.obj->as.array.capacity = count;
    }
    free(dets);
    *result = arr;
}

/* ===========================================================================
 * Registration
 * ========================================================================= */

void vision_cascade_register_all(Environment *env) {
    env_define(env, "vision_cascade_load",   value_native("vision_cascade_load",   fn_vision_cascade_load));
    env_define(env, "vision_cascade_free",   value_native("vision_cascade_free",   fn_vision_cascade_free));
    env_define(env, "vision_detect_objects", value_native("vision_detect_objects", fn_vision_detect_objects));
}

void vision_cascade_register_all_vm(VM *vm) {
    vm_global_set_native(vm, "vision_cascade_load",   fn_vision_cascade_load);
    vm_global_set_native(vm, "vision_cascade_free",   fn_vision_cascade_free);
    vm_global_set_native(vm, "vision_detect_objects", fn_vision_detect_objects);
}
