#ifndef KHAN_INT8ARRAY_LIB_H
#define KHAN_INT8ARRAY_LIB_H

#include "interpreter.h"
#include "vm.h"

/*
 * int8array_lib — real packed int8 storage, a genuine Value type
 * (VAL_INT8ARRAY), not a map-with-doubles simulation like quant.kh's
 * quantize_per_channel() and friends. This is the piece that was missing
 * for quant.kh's validated findings (per-channel scale, outlier
 * decomposition) to turn into actual memory savings instead of just
 * accuracy behavior - see the chat for the discussion of that gap.
 *
 * Deliberately narrow scope for this first slice: one flat packed array
 * of signed bytes plus a single scale (symmetric quantization, matching
 * quant.kh's own convention). No shape/strides, no dtype beyond int8, no
 * arithmetic kernels (add/matmul) on it yet, and no `[]` indexing syntax -
 * read it back via int8_get()/int8_unpack(). This is real storage, not a
 * new Tensor type; it proves the specific missing piece (can Khan store
 * something smaller than a double) rather than the whole Tensor Engine.
 *
 * Registers:
 *   int8_pack(array, scale)   -> int8array value
 *       Quantizes a Khan array of numbers into packed signed bytes using
 *       the given scale (round-to-nearest, clipped to [-127, 127]). Pass
 *       whatever scale your own calibration strategy computed - this
 *       function only does the storage/packing, not the calibration
 *       (quant.kh already covers per-channel/outlier-aware scale choice).
 *   int8_get(qarray, index)   -> number (dequantized: data[index] * scale)
 *   int8_unpack(qarray)       -> array (all elements dequantized)
 *   int8_len(qarray)          -> number (element count)
 *   int8_byte_size(qarray)    -> number (actual bytes used - count * 1,
 *       here for the real memory-savings comparison against a regular
 *       Khan array's count * 8 bytes; not a new fact int8_len() couldn't
 *       already tell you, just named for that specific comparison)
 *   int8_dot(qarray, array)   -> number (dot product against a regular
 *       Khan array of numbers, computed directly on the packed bytes -
 *       does NOT call int8_unpack() internally. Mathematically identical
 *       to dot(int8_unpack(qarray), array) - confirmed to match exactly
 *       in testing (same distributed-multiply math, reordered).
 *
 *       Measured 200x on 50k elements: int8_dot() took 0.010s total vs
 *       2.94s for unpack-then-dot(), a ~280x difference - but be honest
 *       about WHY: int8_unpack() alone only accounts for ~4% of that
 *       2.94s (0.12s measured separately). The other ~96% is tensor.kh's
 *       dot() being pure interpreted Khan, not native C. So this number
 *       is real, but it's mostly evidence for "Khan needs native tensor
 *       kernels, not pure-Khan loops" (already flagged in the roadmap
 *       docs) rather than proof that avoiding the unpack step specifically
 *       is what's fast. int8_dot()'s honest, isolated advantage is being
 *       one native loop instead of an allocation + a native loop + an
 *       interpreted loop - real, but smaller than the headline number.
 */
void int8array_register_all(Environment *env);
void int8array_register_all_vm(VM *vm);

#endif
