#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "tensor_lib.h"

#define TENSOR_MAX_NDIM 16

/* ── nested-Kosh-array <-> flat tensor conversion ──────────────────── */

/* Depth of a valid, perfectly rectangular nested array of numbers (a
   lone number is depth 0). -1 if `v` isn't shaped like that at all —
   ragged arrays are caught separately in nested_validate_shape, once we
   actually know what shape to check against. */
static int nested_depth(Value v) {
    if (v.type == VAL_NUMBER) return 0;
    if (v.type != VAL_ARRAY) return -1;
    int n = AS_ARRAY_COUNT(v);
    if (n == 0) return 1;
    int d = nested_depth(AS_ARRAY_ITEMS(v)[0]);
    if (d < 0) return -1;
    return d + 1;
}

static void nested_fill_shape(Value v, int *shape, int depth) {
    if (depth == 0) return;
    int n = AS_ARRAY_COUNT(v);
    shape[0] = n;
    if (n == 0) { for (int i = 1; i < depth; i++) shape[i] = 0; return; }
    nested_fill_shape(AS_ARRAY_ITEMS(v)[0], shape + 1, depth - 1);
}

static int nested_validate_shape(Value v, const int *shape, int depth) {
    if (depth == 0) return v.type == VAL_NUMBER;
    if (v.type != VAL_ARRAY || AS_ARRAY_COUNT(v) != shape[0]) return 0;
    Value *items = AS_ARRAY_ITEMS(v);
    for (int i = 0; i < shape[0]; i++)
        if (!nested_validate_shape(items[i], shape + 1, depth - 1)) return 0;
    return 1;
}

static void nested_flatten(Value v, int depth, double *out, int *idx) {
    if (depth == 0) { out[(*idx)++] = v.as.number; return; }
    int n = AS_ARRAY_COUNT(v);
    Value *items = AS_ARRAY_ITEMS(v);
    for (int i = 0; i < n; i++) nested_flatten(items[i], depth - 1, out, idx);
}

static Value nested_unflatten(const double *data, const int *shape, int depth, int *idx) {
    if (depth == 0) return value_number(data[(*idx)++]);
    int n = shape[0];
    Value *items = n > 0 ? malloc(sizeof(Value) * (size_t)n) : NULL;
    for (int i = 0; i < n; i++) items[i] = nested_unflatten(data, shape + 1, depth - 1, idx);
    return value_array(items, n);   /* value_array takes ownership of items */
}

void fn_tensor_from_nested(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 1) {
        fprintf(stderr, "Runtime error: _tensor_from_nested() expects 1 argument\n");
        return;
    }
    if (args[0].type == VAL_TENSOR) { *result = value_copy(args[0]); return; }

    int depth = nested_depth(args[0]);
    if (depth < 0 || depth > TENSOR_MAX_NDIM) {
        fprintf(stderr, "Runtime error: _tensor_from_nested() needs a number or a rectangular "
                        "nested array of numbers (max %d dimensions)\n", TENSOR_MAX_NDIM);
        return;
    }
    int shape[TENSOR_MAX_NDIM];
    nested_fill_shape(args[0], shape, depth);
    if (!nested_validate_shape(args[0], shape, depth)) {
        fprintf(stderr, "Runtime error: _tensor_from_nested() array is not rectangular "
                        "(sub-arrays have different lengths)\n");
        return;
    }
    int size = 1;
    for (int i = 0; i < depth; i++) size *= shape[i];
    double *flat = malloc(sizeof(double) * (size_t)(size > 0 ? size : 1));
    int idx = 0;
    nested_flatten(args[0], depth, flat, &idx);
    *result = value_tensor(flat, shape, depth);
    free(flat);
}

void fn_tensor_to_nested(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 1 || args[0].type != VAL_TENSOR) {
        fprintf(stderr, "Runtime error: _tensor_to_nested() expects a tensor\n");
        return;
    }
    Obj *t = args[0].as.obj;
    int idx = 0;
    *result = nested_unflatten(t->as.tensor.data, t->as.tensor.shape, t->as.tensor.ndim, &idx);
}

void fn_tensor_shape(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 1 || args[0].type != VAL_TENSOR) {
        fprintf(stderr, "Runtime error: _tensor_shape() expects a tensor\n");
        return;
    }
    int ndim = AS_TENSOR_NDIM(args[0]);
    Value *items = ndim > 0 ? malloc(sizeof(Value) * (size_t)ndim) : NULL;
    for (int i = 0; i < ndim; i++) items[i] = value_number(AS_TENSOR_SHAPE(args[0])[i]);
    *result = value_array(items, ndim);
}

void fn_tensor_ndim(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 1 || args[0].type != VAL_TENSOR) {
        fprintf(stderr, "Runtime error: _tensor_ndim() expects a tensor\n");
        return;
    }
    *result = value_number(AS_TENSOR_NDIM(args[0]));
}

void fn_tensor_numel(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 1 || args[0].type != VAL_TENSOR) {
        fprintf(stderr, "Runtime error: _tensor_numel() expects a tensor\n");
        return;
    }
    *result = value_number(AS_TENSOR_SIZE(args[0]));
}

/* ── elementwise add/sub/mul, with scalar broadcasting ──────────────── */

typedef enum { TOP_ADD, TOP_SUB, TOP_MUL, TOP_DIV } TensorOp;

static double tensor_apply_op(TensorOp op, double x, double y) {
    switch (op) {
        case TOP_ADD: return x + y;
        case TOP_SUB: return x - y;
        case TOP_MUL: return x * y;
        case TOP_DIV: return x / y;
    }
    return 0.0;
}

/* IMPORTANT: a plain NUMBER never carries shape, so whenever at least one
   side is an actual VAL_TENSOR, the result must keep THAT tensor's own
   shape — even when that tensor happens to have exactly one element
   (shape (1,1), say). Collapsing a real size-1 tensor's shape down to a
   bare 0-D result the moment it's combined with a number or with another
   size-1 tensor is NOT a cosmetic quirk: a Dense(1,1) layer's weight is
   exactly a size-1 tensor, and an earlier version of this function did
   exactly that collapse — silently corrupting the parameter's shape the
   first time an optimizer step did `w.buf = _tensor_sub(w.buf, ...)`,
   breaking the single simplest model you could possibly train. Found via
   that exact scenario; see docs/tensor.md. */
/* 2-D only: per-axis broadcast where a dimension is compatible if equal
   or exactly 1 -- e.g. a (1,n) bias row against an (m,n) batch, or the
   (m,1) column analogue. This is what a Dense layer's bias actually
   needs for batch training (out_features > 1, batch_size > 1
   simultaneously); found missing by training exactly that. It also
   subsumes the plain "one side is a (1,1) tensor" case. NOT general
   numpy broadcasting: anything beyond 2-D, or a dimension that's
   neither equal nor 1, still falls through to the narrower rules below
   (or the final shape-mismatch error). Returns 1 if it handled the op
   (result written), 0 if this rule doesn't apply. */
static int tensor_try_2d_broadcast(Value *result, Value a, Value b, TensorOp op) {
    if (AS_TENSOR_NDIM(a) != 2 || AS_TENSOR_NDIM(b) != 2) return 0;
    int am = AS_TENSOR_SHAPE(a)[0], an = AS_TENSOR_SHAPE(a)[1];
    int bm = AS_TENSOR_SHAPE(b)[0], bn = AS_TENSOR_SHAPE(b)[1];
    if (am == bm && an == bn) return 0;   /* exact match handled elsewhere */
    int m_ok = (am == bm) || am == 1 || bm == 1;
    int n_ok = (an == bn) || an == 1 || bn == 1;
    if (!m_ok || !n_ok) return 0;
    int m = am > bm ? am : bm;
    int n = an > bn ? an : bn;
    double *out = malloc(sizeof(double) * (size_t)m * (size_t)n);
    double *ad = AS_TENSOR_DATA(a), *bd = AS_TENSOR_DATA(b);
    for (int i = 0; i < m; i++) {
        for (int j = 0; j < n; j++) {
            int ai = (am == 1) ? 0 : i, aj = (an == 1) ? 0 : j;
            int bi = (bm == 1) ? 0 : i, bj = (bn == 1) ? 0 : j;
            out[i * n + j] = tensor_apply_op(op, ad[ai * an + aj], bd[bi * bn + bj]);
        }
    }
    int shape[2] = { m, n };
    *result = value_tensor(out, shape, 2);
    free(out);
    return 1;
}

static void tensor_binop(Value *result, const char *name, Value a, Value b, TensorOp op) {
    *result = value_nil();
    int a_is_t = a.type == VAL_TENSOR, b_is_t = b.type == VAL_TENSOR;
    int a_is_n = a.type == VAL_NUMBER, b_is_n = b.type == VAL_NUMBER;
    if (!(a_is_t || a_is_n) || !(b_is_t || b_is_n)) {
        fprintf(stderr, "Runtime error: %s() arguments must be tensors or numbers\n", name);
        return;
    }

    /* both tensors, exact same shape -> ordinary elementwise */
    if (a_is_t && b_is_t && AS_TENSOR_NDIM(a) == AS_TENSOR_NDIM(b) &&
        AS_TENSOR_SIZE(a) == AS_TENSOR_SIZE(b) &&
        (AS_TENSOR_NDIM(a) == 0 ||
         memcmp(AS_TENSOR_SHAPE(a), AS_TENSOR_SHAPE(b), sizeof(int) * (size_t)AS_TENSOR_NDIM(a)) == 0)) {
        int n = AS_TENSOR_SIZE(a);
        double *out = malloc(sizeof(double) * (size_t)(n > 0 ? n : 1));
        for (int i = 0; i < n; i++) out[i] = tensor_apply_op(op, AS_TENSOR_DATA(a)[i], AS_TENSOR_DATA(b)[i]);
        *result = value_tensor(out, AS_TENSOR_SHAPE(a), AS_TENSOR_NDIM(a));
        free(out);
        return;
    }

    /* both tensors, 2-D, per-axis-compatible but not identical shapes
       (row/column broadcast — see tensor_try_2d_broadcast above) */
    if (a_is_t && b_is_t && tensor_try_2d_broadcast(result, a, b, op)) {
        return;
    }

    /* exactly one side is a tensor, the other a plain number: broadcast
       the number, always keeping the tensor side's own shape. */
    if (a_is_t && b_is_n) {
        int n = AS_TENSOR_SIZE(a);
        double *out = malloc(sizeof(double) * (size_t)(n > 0 ? n : 1));
        for (int i = 0; i < n; i++) out[i] = tensor_apply_op(op, AS_TENSOR_DATA(a)[i], b.as.number);
        *result = value_tensor(out, AS_TENSOR_SHAPE(a), AS_TENSOR_NDIM(a));
        free(out);
        return;
    }
    if (b_is_t && a_is_n) {
        int n = AS_TENSOR_SIZE(b);
        double *out = malloc(sizeof(double) * (size_t)(n > 0 ? n : 1));
        for (int i = 0; i < n; i++) out[i] = tensor_apply_op(op, a.as.number, AS_TENSOR_DATA(b)[i]);
        *result = value_tensor(out, AS_TENSOR_SHAPE(b), AS_TENSOR_NDIM(b));
        free(out);
        return;
    }

    /* both plain numbers -> a 0-D tensor (there's no shape to preserve) */
    if (a_is_n && b_is_n) {
        double out = tensor_apply_op(op, a.as.number, b.as.number);
        *result = value_tensor(&out, NULL, 0);
        return;
    }

    /* both tensors, different shapes: only a size-1-vs-bigger broadcast
       is supported. The bigger side's shape always wins; if both sides
       have exactly one element (e.g. a bare 0-D scalar vs. a shape-(1,1)
       tensor), keep whichever has the larger ndim, so a genuinely-shaped
       size-1 tensor's shape is never discarded in favor of a bare 0-D
       one purely because of evaluation order. */
    if (a_is_t && b_is_t) {
        int asz = AS_TENSOR_SIZE(a), bsz = AS_TENSOR_SIZE(b);
        if (bsz != 1 && asz == 1) {
            double scal = AS_TENSOR_DATA(a)[0];
            int n = bsz;
            double *out = malloc(sizeof(double) * (size_t)(n > 0 ? n : 1));
            for (int i = 0; i < n; i++) out[i] = tensor_apply_op(op, scal, AS_TENSOR_DATA(b)[i]);
            *result = value_tensor(out, AS_TENSOR_SHAPE(b), AS_TENSOR_NDIM(b));
            free(out);
            return;
        }
        if (asz != 1 && bsz == 1) {
            double scal = AS_TENSOR_DATA(b)[0];
            int n = asz;
            double *out = malloc(sizeof(double) * (size_t)(n > 0 ? n : 1));
            for (int i = 0; i < n; i++) out[i] = tensor_apply_op(op, AS_TENSOR_DATA(a)[i], scal);
            *result = value_tensor(out, AS_TENSOR_SHAPE(a), AS_TENSOR_NDIM(a));
            free(out);
            return;
        }
        if (asz == 1 && bsz == 1) {
            const Value *shaped = AS_TENSOR_NDIM(a) >= AS_TENSOR_NDIM(b) ? &a : &b;
            double out = tensor_apply_op(op, AS_TENSOR_DATA(a)[0], AS_TENSOR_DATA(b)[0]);
            *result = value_tensor(&out, AS_TENSOR_SHAPE(*shaped), AS_TENSOR_NDIM(*shaped));
            return;
        }
    }

    fprintf(stderr, "Runtime error: %s() shape mismatch (%s vs %s)\n", name,
            a_is_t ? "tensor" : "number", b_is_t ? "tensor" : "number");
}

void fn_tensor_add(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    if (argc < 2) { *result = value_nil(); fprintf(stderr, "Runtime error: _tensor_add() expects 2 arguments\n"); return; }
    tensor_binop(result, "_tensor_add", args[0], args[1], TOP_ADD);
}
void fn_tensor_sub(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    if (argc < 2) { *result = value_nil(); fprintf(stderr, "Runtime error: _tensor_sub() expects 2 arguments\n"); return; }
    tensor_binop(result, "_tensor_sub", args[0], args[1], TOP_SUB);
}
void fn_tensor_mul(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    if (argc < 2) { *result = value_nil(); fprintf(stderr, "Runtime error: _tensor_mul() expects 2 arguments\n"); return; }
    tensor_binop(result, "_tensor_mul", args[0], args[1], TOP_MUL);
}
void fn_tensor_div(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    if (argc < 2) { *result = value_nil(); fprintf(stderr, "Runtime error: _tensor_div() expects 2 arguments\n"); return; }
    tensor_binop(result, "_tensor_div", args[0], args[1], TOP_DIV);
}

/* ── matmul / transpose (2-D only in this version) ──────────────────── */

void fn_tensor_matmul(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 2 || args[0].type != VAL_TENSOR || args[1].type != VAL_TENSOR) {
        fprintf(stderr, "Runtime error: _tensor_matmul() expects two tensors\n");
        return;
    }
    if (AS_TENSOR_NDIM(args[0]) != 2 || AS_TENSOR_NDIM(args[1]) != 2) {
        fprintf(stderr, "Runtime error: _tensor_matmul() only supports 2-D tensors in this version\n");
        return;
    }
    int m = AS_TENSOR_SHAPE(args[0])[0], k = AS_TENSOR_SHAPE(args[0])[1];
    int k2 = AS_TENSOR_SHAPE(args[1])[0], n = AS_TENSOR_SHAPE(args[1])[1];
    if (k != k2) {
        fprintf(stderr, "Runtime error: _tensor_matmul() shape mismatch: (%d,%d) x (%d,%d)\n", m, k, k2, n);
        return;
    }
    double *out = malloc(sizeof(double) * (size_t)m * (size_t)n);
    double *ad = AS_TENSOR_DATA(args[0]), *bd = AS_TENSOR_DATA(args[1]);
    for (int i = 0; i < m; i++) {
        for (int j = 0; j < n; j++) {
            double sum = 0.0;
            for (int t = 0; t < k; t++) sum += ad[i * k + t] * bd[t * n + j];
            out[i * n + j] = sum;
        }
    }
    int shape[2] = { m, n };
    *result = value_tensor(out, shape, 2);
    free(out);
}

void fn_tensor_transpose(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 1 || args[0].type != VAL_TENSOR || AS_TENSOR_NDIM(args[0]) != 2) {
        fprintf(stderr, "Runtime error: _tensor_transpose() only supports 2-D tensors in this version\n");
        return;
    }
    int m = AS_TENSOR_SHAPE(args[0])[0], n = AS_TENSOR_SHAPE(args[0])[1];
    double *src = AS_TENSOR_DATA(args[0]);
    double *out = malloc(sizeof(double) * (size_t)m * (size_t)n);
    for (int i = 0; i < m; i++)
        for (int j = 0; j < n; j++)
            out[j * m + i] = src[i * n + j];
    int shape[2] = { n, m };
    *result = value_tensor(out, shape, 2);
    free(out);
}

void fn_tensor_reshape(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 2 || args[0].type != VAL_TENSOR || args[1].type != VAL_ARRAY) {
        fprintf(stderr, "Runtime error: _tensor_reshape() expects (tensor, shape_array)\n");
        return;
    }
    int ndim = AS_ARRAY_COUNT(args[1]);
    if (ndim > TENSOR_MAX_NDIM) {
        fprintf(stderr, "Runtime error: _tensor_reshape() too many dimensions (max %d)\n", TENSOR_MAX_NDIM);
        return;
    }
    int shape[TENSOR_MAX_NDIM];
    int size = 1;
    Value *items = AS_ARRAY_ITEMS(args[1]);
    for (int i = 0; i < ndim; i++) {
        if (items[i].type != VAL_NUMBER) {
            fprintf(stderr, "Runtime error: _tensor_reshape() shape must be an array of numbers\n");
            return;
        }
        shape[i] = (int)items[i].as.number;
        size *= shape[i];
    }
    if (size != AS_TENSOR_SIZE(args[0])) {
        fprintf(stderr, "Runtime error: _tensor_reshape() total size mismatch (%d vs %d)\n",
                size, AS_TENSOR_SIZE(args[0]));
        return;
    }
    *result = value_tensor(AS_TENSOR_DATA(args[0]), shape, ndim);
}

void fn_tensor_sum(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 1 || args[0].type != VAL_TENSOR) {
        fprintf(stderr, "Runtime error: _tensor_sum() expects a tensor\n");
        return;
    }
    double total = 0.0;
    int n = AS_TENSOR_SIZE(args[0]);
    double *d = AS_TENSOR_DATA(args[0]);
    for (int i = 0; i < n; i++) total += d[i];
    *result = value_tensor(&total, NULL, 0);
}

/* Reduces `g` down to `target_shape` by summing over whichever axes were
   broadcast to produce `g` in the first place — the backward-pass
   counterpart of tensor_try_2d_broadcast's forward broadcasting (and of
   the plain scalar-number broadcast in tensor_binop, which this also
   covers via the 0-D/size-1 path below). Without this, a broadcast
   operand's OWN gradient would inherit the bigger, broadcast-output
   shape, and assigning that back onto the operand (e.g. an optimizer
   step) would silently grow the operand past its real shape — found via
   training a Dense layer's bias across a batch. */
void fn_tensor_sum_to_shape(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 2 || args[0].type != VAL_TENSOR || args[1].type != VAL_ARRAY) {
        fprintf(stderr, "Runtime error: _tensor_sum_to_shape() expects (tensor, shape_array)\n");
        return;
    }
    Value g = args[0];
    int target_ndim = AS_ARRAY_COUNT(args[1]);
    if (target_ndim > TENSOR_MAX_NDIM) {
        fprintf(stderr, "Runtime error: _tensor_sum_to_shape() too many dimensions (max %d)\n", TENSOR_MAX_NDIM);
        return;
    }
    int target_shape[TENSOR_MAX_NDIM];
    Value *items = AS_ARRAY_ITEMS(args[1]);
    int target_size = 1;
    for (int i = 0; i < target_ndim; i++) {
        if (items[i].type != VAL_NUMBER) {
            fprintf(stderr, "Runtime error: _tensor_sum_to_shape() shape must be an array of numbers\n");
            return;
        }
        target_shape[i] = (int)items[i].as.number;
        target_size *= target_shape[i];
    }

    if (target_size == AS_TENSOR_SIZE(g)) {
        /* nothing was actually broadcast -- just reshape */
        *result = value_tensor(AS_TENSOR_DATA(g), target_shape, target_ndim);
        return;
    }

    /* fully collapsed to a scalar (0-D, or any all-1s shape) */
    if (target_size == 1) {
        double total = 0.0;
        int n = AS_TENSOR_SIZE(g);
        double *gd = AS_TENSOR_DATA(g);
        for (int i = 0; i < n; i++) total += gd[i];
        *result = value_tensor(&total, target_shape, target_ndim);
        return;
    }

    /* row and/or column broadcast: target and g are both 2-D, and each
       of target's dims is either g's own or 1 */
    if (target_ndim == 2 && AS_TENSOR_NDIM(g) == 2) {
        int gm = AS_TENSOR_SHAPE(g)[0], gn = AS_TENSOR_SHAPE(g)[1];
        int tm = target_shape[0], tn = target_shape[1];
        if ((tm == gm || tm == 1) && (tn == gn || tn == 1)) {
            double *out = calloc((size_t)tm * (size_t)tn, sizeof(double));
            double *gd = AS_TENSOR_DATA(g);
            for (int i = 0; i < gm; i++) {
                for (int j = 0; j < gn; j++) {
                    int ti = (tm == 1) ? 0 : i;
                    int tj = (tn == 1) ? 0 : j;
                    out[ti * tn + tj] += gd[i * gn + j];
                }
            }
            *result = value_tensor(out, target_shape, 2);
            free(out);
            return;
        }
    }

    fprintf(stderr, "Runtime error: _tensor_sum_to_shape() unsupported reduction (%d elements to shape of %d)\n",
            AS_TENSOR_SIZE(g), target_size);
}

void fn_tensor_ones_like(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 1 || args[0].type != VAL_TENSOR) {
        fprintf(stderr, "Runtime error: _tensor_ones_like() expects a tensor\n");
        return;
    }
    int n = AS_TENSOR_SIZE(args[0]);
    double *ones = malloc(sizeof(double) * (size_t)(n > 0 ? n : 1));
    for (int i = 0; i < n; i++) ones[i] = 1.0;
    *result = value_tensor(ones, AS_TENSOR_SHAPE(args[0]), AS_TENSOR_NDIM(args[0]));
    free(ones);
}

void fn_tensor_zeros_like(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 1 || args[0].type != VAL_TENSOR) {
        fprintf(stderr, "Runtime error: _tensor_zeros_like() expects a tensor\n");
        return;
    }
    /* value_tensor() zero-fills when data == NULL */
    *result = value_tensor(NULL, AS_TENSOR_SHAPE(args[0]), AS_TENSOR_NDIM(args[0]));
}

/* ── activations (for the nn package) ───────────────────────────────── */
/* Forward only, plus one backward helper for ReLU (its gradient needs a
   0/1 mask that isn't expressible by composing the existing elementwise
   ops). Sigmoid/tanh's backward is instead composed in Kosh from their
   own *output* plus existing mul/sub (g * out * (1 - out), and
   g * (1 - out*out) respectively) -- see packages/nn/nn.kh -- so they
   don't need a dedicated native backward function the way ReLU does. */

void fn_tensor_relu(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 1 || args[0].type != VAL_TENSOR) {
        fprintf(stderr, "Runtime error: _tensor_relu() expects a tensor\n");
        return;
    }
    int n = AS_TENSOR_SIZE(args[0]);
    double *out = malloc(sizeof(double) * (size_t)(n > 0 ? n : 1));
    double *d = AS_TENSOR_DATA(args[0]);
    for (int i = 0; i < n; i++) out[i] = d[i] > 0.0 ? d[i] : 0.0;
    *result = value_tensor(out, AS_TENSOR_SHAPE(args[0]), AS_TENSOR_NDIM(args[0]));
    free(out);
}

void fn_tensor_relu_backward(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 2 || args[0].type != VAL_TENSOR || args[1].type != VAL_TENSOR) {
        fprintf(stderr, "Runtime error: _tensor_relu_backward() expects (grad, x)\n");
        return;
    }
    if (AS_TENSOR_SIZE(args[0]) != AS_TENSOR_SIZE(args[1])) {
        fprintf(stderr, "Runtime error: _tensor_relu_backward() shape mismatch\n");
        return;
    }
    int n = AS_TENSOR_SIZE(args[0]);
    double *out = malloc(sizeof(double) * (size_t)(n > 0 ? n : 1));
    double *g = AS_TENSOR_DATA(args[0]), *x = AS_TENSOR_DATA(args[1]);
    for (int i = 0; i < n; i++) out[i] = x[i] > 0.0 ? g[i] : 0.0;
    *result = value_tensor(out, AS_TENSOR_SHAPE(args[0]), AS_TENSOR_NDIM(args[0]));
    free(out);
}

void fn_tensor_sigmoid(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 1 || args[0].type != VAL_TENSOR) {
        fprintf(stderr, "Runtime error: _tensor_sigmoid() expects a tensor\n");
        return;
    }
    int n = AS_TENSOR_SIZE(args[0]);
    double *out = malloc(sizeof(double) * (size_t)(n > 0 ? n : 1));
    double *d = AS_TENSOR_DATA(args[0]);
    for (int i = 0; i < n; i++) out[i] = 1.0 / (1.0 + exp(-d[i]));
    *result = value_tensor(out, AS_TENSOR_SHAPE(args[0]), AS_TENSOR_NDIM(args[0]));
    free(out);
}

void fn_tensor_tanh(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 1 || args[0].type != VAL_TENSOR) {
        fprintf(stderr, "Runtime error: _tensor_tanh() expects a tensor\n");
        return;
    }
    int n = AS_TENSOR_SIZE(args[0]);
    double *out = malloc(sizeof(double) * (size_t)(n > 0 ? n : 1));
    double *d = AS_TENSOR_DATA(args[0]);
    for (int i = 0; i < n; i++) out[i] = tanh(d[i]);
    *result = value_tensor(out, AS_TENSOR_SHAPE(args[0]), AS_TENSOR_NDIM(args[0]));
    free(out);
}

void fn_tensor_sqrt(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 1 || args[0].type != VAL_TENSOR) {
        fprintf(stderr, "Runtime error: _tensor_sqrt() expects a tensor\n");
        return;
    }
    int n = AS_TENSOR_SIZE(args[0]);
    double *out = malloc(sizeof(double) * (size_t)(n > 0 ? n : 1));
    double *d = AS_TENSOR_DATA(args[0]);
    for (int i = 0; i < n; i++) out[i] = sqrt(d[i]);
    *result = value_tensor(out, AS_TENSOR_SHAPE(args[0]), AS_TENSOR_NDIM(args[0]));
    free(out);
}

/* ── classification: softmax, argmax, cross-entropy (for the nn
   package's CrossEntropy loss) ─────────────────────────────────────── */

void fn_tensor_softmax(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 1 || args[0].type != VAL_TENSOR || AS_TENSOR_NDIM(args[0]) != 2) {
        fprintf(stderr, "Runtime error: _tensor_softmax() expects a 2-D tensor (rows = batch, cols = classes)\n");
        return;
    }
    int m = AS_TENSOR_SHAPE(args[0])[0], n = AS_TENSOR_SHAPE(args[0])[1];
    double *d = AS_TENSOR_DATA(args[0]);
    double *out = malloc(sizeof(double) * (size_t)m * (size_t)n);
    for (int i = 0; i < m; i++) {
        double maxv = d[i * n];
        for (int j = 1; j < n; j++) if (d[i * n + j] > maxv) maxv = d[i * n + j];
        double sum = 0.0;
        for (int j = 0; j < n; j++) {
            double e = exp(d[i * n + j] - maxv);   /* subtract row max: numerically stable */
            out[i * n + j] = e;
            sum += e;
        }
        for (int j = 0; j < n; j++) out[i * n + j] /= sum;
    }
    int shape[2] = { m, n };
    *result = value_tensor(out, shape, 2);
    free(out);
}

void fn_tensor_argmax(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 1 || args[0].type != VAL_TENSOR || AS_TENSOR_NDIM(args[0]) != 2) {
        fprintf(stderr, "Runtime error: _tensor_argmax() expects a 2-D tensor\n");
        return;
    }
    int m = AS_TENSOR_SHAPE(args[0])[0], n = AS_TENSOR_SHAPE(args[0])[1];
    double *d = AS_TENSOR_DATA(args[0]);
    Value *items = malloc(sizeof(Value) * (size_t)(m > 0 ? m : 1));
    for (int i = 0; i < m; i++) {
        int best = 0;
        double bestv = d[i * n];
        for (int j = 1; j < n; j++) {
            if (d[i * n + j] > bestv) { bestv = d[i * n + j]; best = j; }
        }
        items[i] = value_number((double)best);
    }
    *result = value_array(items, m);
}

/* probs: (N,C) tensor from _tensor_softmax. target: a plain Kosh array
   of N class indices (0..C-1), NOT a one-hot tensor — ergonomics over a
   separate one-hot-encoding step, matching how most frameworks' own
   CrossEntropyLoss accepts class indices directly. */
void fn_tensor_cross_entropy_loss(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 2 || args[0].type != VAL_TENSOR || args[1].type != VAL_ARRAY || AS_TENSOR_NDIM(args[0]) != 2) {
        fprintf(stderr, "Runtime error: _tensor_cross_entropy_loss() expects (probs: 2-D tensor, target: class-index array)\n");
        return;
    }
    int m = AS_TENSOR_SHAPE(args[0])[0], n = AS_TENSOR_SHAPE(args[0])[1];
    if (AS_ARRAY_COUNT(args[1]) != m) {
        fprintf(stderr, "Runtime error: _tensor_cross_entropy_loss() target length (%d) must match batch size (%d)\n",
                AS_ARRAY_COUNT(args[1]), m);
        return;
    }
    double *p = AS_TENSOR_DATA(args[0]);
    Value *targets = AS_ARRAY_ITEMS(args[1]);
    double total = 0.0;
    for (int i = 0; i < m; i++) {
        if (targets[i].type != VAL_NUMBER) {
            fprintf(stderr, "Runtime error: _tensor_cross_entropy_loss() target must be an array of numbers\n");
            return;
        }
        int cls = (int)targets[i].as.number;
        if (cls < 0 || cls >= n) {
            fprintf(stderr, "Runtime error: _tensor_cross_entropy_loss() target class index %d out of range [0,%d)\n", cls, n);
            return;
        }
        double pi = p[i * n + cls];
        if (pi < 1e-12) pi = 1e-12;   /* avoid log(0) */
        total += log(pi);
    }
    double loss = -total / (double)m;
    *result = value_tensor(&loss, NULL, 0);
}

/* The closed-form gradient of softmax+cross-entropy combined, fused for
   the same numerical-stability and simplicity reasons real frameworks
   fuse these two: d(loss)/d(logits) = (probs - one_hot(target)) / N.
   Builds the one-hot structure implicitly rather than materializing it
   as a separate tensor. */
void fn_tensor_cross_entropy_grad(Value *result, Interpreter *interp, int argc, Value *args) {
    (void)interp;
    *result = value_nil();
    if (argc < 2 || args[0].type != VAL_TENSOR || args[1].type != VAL_ARRAY || AS_TENSOR_NDIM(args[0]) != 2) {
        fprintf(stderr, "Runtime error: _tensor_cross_entropy_grad() expects (probs: 2-D tensor, target: class-index array)\n");
        return;
    }
    int m = AS_TENSOR_SHAPE(args[0])[0], n = AS_TENSOR_SHAPE(args[0])[1];
    if (AS_ARRAY_COUNT(args[1]) != m) {
        fprintf(stderr, "Runtime error: _tensor_cross_entropy_grad() target length (%d) must match batch size (%d)\n",
                AS_ARRAY_COUNT(args[1]), m);
        return;
    }
    double *p = AS_TENSOR_DATA(args[0]);
    Value *targets = AS_ARRAY_ITEMS(args[1]);
    double *out = malloc(sizeof(double) * (size_t)m * (size_t)n);
    memcpy(out, p, sizeof(double) * (size_t)m * (size_t)n);
    for (int i = 0; i < m; i++) {
        if (targets[i].type != VAL_NUMBER) {
            free(out);
            fprintf(stderr, "Runtime error: _tensor_cross_entropy_grad() target must be an array of numbers\n");
            return;
        }
        int cls = (int)targets[i].as.number;
        if (cls < 0 || cls >= n) {
            free(out);
            fprintf(stderr, "Runtime error: _tensor_cross_entropy_grad() target class index %d out of range [0,%d)\n", cls, n);
            return;
        }
        out[i * n + cls] -= 1.0;
    }
    int total = m * n;
    for (int i = 0; i < total; i++) out[i] /= (double)m;
    int shape[2] = { m, n };
    *result = value_tensor(out, shape, 2);
    free(out);
}

void tensor_register_all_vm(VM *vm) {
    vm_global_set_native(vm, "_tensor_from_nested", fn_tensor_from_nested);
    vm_global_set_native(vm, "_tensor_to_nested",   fn_tensor_to_nested);
    vm_global_set_native(vm, "_tensor_shape",       fn_tensor_shape);
    vm_global_set_native(vm, "_tensor_ndim",        fn_tensor_ndim);
    vm_global_set_native(vm, "_tensor_numel",       fn_tensor_numel);
    vm_global_set_native(vm, "_tensor_add",         fn_tensor_add);
    vm_global_set_native(vm, "_tensor_sub",         fn_tensor_sub);
    vm_global_set_native(vm, "_tensor_mul",         fn_tensor_mul);
    vm_global_set_native(vm, "_tensor_matmul",      fn_tensor_matmul);
    vm_global_set_native(vm, "_tensor_transpose",   fn_tensor_transpose);
    vm_global_set_native(vm, "_tensor_reshape",     fn_tensor_reshape);
    vm_global_set_native(vm, "_tensor_sum",         fn_tensor_sum);
    vm_global_set_native(vm, "_tensor_ones_like",   fn_tensor_ones_like);
    vm_global_set_native(vm, "_tensor_zeros_like",  fn_tensor_zeros_like);
    vm_global_set_native(vm, "_tensor_relu",          fn_tensor_relu);
    vm_global_set_native(vm, "_tensor_relu_backward", fn_tensor_relu_backward);
    vm_global_set_native(vm, "_tensor_sigmoid",       fn_tensor_sigmoid);
    vm_global_set_native(vm, "_tensor_tanh",          fn_tensor_tanh);
    vm_global_set_native(vm, "_tensor_sum_to_shape",  fn_tensor_sum_to_shape);
    vm_global_set_native(vm, "_tensor_div",               fn_tensor_div);
    vm_global_set_native(vm, "_tensor_sqrt",              fn_tensor_sqrt);
    vm_global_set_native(vm, "_tensor_softmax",           fn_tensor_softmax);
    vm_global_set_native(vm, "_tensor_argmax",            fn_tensor_argmax);
    vm_global_set_native(vm, "_tensor_cross_entropy_loss", fn_tensor_cross_entropy_loss);
    vm_global_set_native(vm, "_tensor_cross_entropy_grad", fn_tensor_cross_entropy_grad);
}
