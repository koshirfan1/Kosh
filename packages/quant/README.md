# quant — Accuracy-Preserving Quantization for Kosh

**Version:** 1.0.0  
**Package:** `quant`  
**Author:** khandev1211-cpu  
**Developer:** Irfan Khan

## Overview

`quant` is a pure-Kosh quantization package that simulates integer quantization behavior **without** requiring native dtypes or a Tensor type. It provides a full toolbox for measuring and improving quantization accuracy: per-channel calibration, bias correction, outlier-aware sparse decomposition, and mixed-precision strategies — all implemented in Kosh's own syntax using only `round()`, `abs()`, `max()`, `pow()`, and `sqrt()` (all available in the standard library).

## Important Honesty Note

This package simulates quantization's **accuracy** behavior — it does **not** give you real int8 storage or faster int8 arithmetic, because Kosh has no dtype system yet (every number is still a native `double` under the hood). Use this to:

- ✅ Validate a calibration/granularity strategy against your own data
- ✅ Understand how per-channel vs per-tensor quantization affects error
- ✅ Prototype and measure trade-offs before investing in native dtype + Tensor work

## API Reference

A quantized matrix is represented as:
```json
{"data": [[...]...], "scales": [s0, s1, ...], "bits": bits}
```

`"data"` holds dequantized (round-tripped) values — one scale per row (per output channel).

### `quantize_per_channel(W, bits)`

Per-channel symmetric quantization. Each row of matrix `W` gets its own scale based on the row's max absolute value.

| Parameter | Type | Description |
|-----------|------|-------------|
| `W` | Array of Arrays | Weight matrix (rows = output channels) |
| `bits` | Number | Target bit-width (e.g., 8 for int8) |

**Returns:** `{"data": matrix, "scales": [s0, s1, ...], "bits": bits}`

### `quantize_per_channel_calibrated(W, keep_fraction)`

Like `quantize_per_channel`, but clips the top `(1 - keep_fraction)` of extreme absolute values before setting the scale — useful for **activation** ranges where outliers are often noisy.

| Parameter | Type | Description |
|-----------|------|-------------|
| `W` | Array of Arrays | Activation matrix |
| `keep_fraction` | Number | Fraction of values to keep (e.g., 0.95) |

**Important:** This is designed for activation calibration. Do **not** use it for weight outliers — use `quantize_outlier_aware()` instead.

### `quant_error(W_true, W_quant)`

Relative L2 error (Frobenius norm) between the original and quantized matrices.

| Parameter | Type | Description |
|-----------|------|-------------|
| `W_true` | Array of Arrays | Original matrix |
| `W_quant` | Array of Arrays | Quantized matrix |

**Returns:** Number (relative error, 0 = perfect)

### `quantize_per_channel_bias_corrected(W, bits)`

Like `quantize_per_channel`, but measures each channel's mean signed error after quantization and adds it back as a bias correction. Most useful when a channel's values are not symmetric around zero.

### `quantize_outlier_aware(W, bits, outlier_fraction)`

The LLM.int8() idea: pulls extreme values out per row, keeps them at full precision, and quantizes only what's left with a scale no longer distorted by outliers.

| Parameter | Type | Description |
|-----------|------|-------------|
| `W` | Array of Arrays | Weight matrix |
| `bits` | Number | Target bit-width |
| `outlier_fraction` | Number | Fraction of values to treat as outliers (e.g., 0.01) |

**Returns:** `{"data": matrix, "scales": [...], "bits": bits, "outliers": [...]}`

### `quantize_mixed_precision(W, bits, error_threshold)`

Quantizes everything, measures each row's relative error, and keeps any row above `error_threshold` at full precision. Reports how many rows were kept full-precision.

**Returns:** `{"data": matrix, "scales": [...], "bits": bits, "full_precision_rows": count}`

## Usage

```kosh
import "quant"

# A 3×4 weight matrix (3 output channels, 4 inputs)
let W = [
    [0.5, -0.3,  0.8, -0.1],
    [0.2,  0.9, -0.4,  0.6],
    [0.7, -0.8,  0.1,  1.2]
]

# Quantize to int4
let q = quantize_per_channel(W, 4)
print quant_error(W, q["data"])  # ~3-6% for int4

# Try outlier-aware for aggressive bit-widths
let q2 = quantize_outlier_aware(W, 4, 0.05)
print quant_error(W, q2["data"])

# Or mixed-precision to protect sensitive rows
let q3 = quantize_mixed_precision(W, 4, 0.05)
print "Rows kept full precision:", q3["full_precision_rows"]
```

## Functions Summary

| Function | Purpose |
|----------|---------|
| `quantize_per_channel(W, bits)` | Baseline per-channel symmetric quantization |
| `quantize_per_channel_calibrated(W, keep_fraction)` | Activation-range calibrated quantization |
| `quant_error(W_true, W_quant)` | Relative L2 error measurement |
| `quantize_per_channel_bias_corrected(W, bits)` | Per-channel bias-corrected quantization |
| `quantize_outlier_aware(W, bits, outlier_fraction)` | Sparse-decomposition outlier handling |
| `quantize_mixed_precision(W, bits, error_threshold)` | Row-granularity mixed-precision fallback |

## Technical Notes

- All quantization functions operate on matrices as arrays of arrays (rows = output channels).
- The package simulates **symmetric** quantization (range `[-qmax, qmax]` where `qmax = 2^(bits-1) - 1`).
- Per-channel scaling (one scale per row) is used throughout — this is standard practice for weight quantization and significantly outperforms per-tensor scaling.
- For a deeper discussion of quantization theory and the empirical results that informed this implementation, see `docs/llm.md` in the Kosh root.

## License

MIT — part of the Kosh Programming Language project.