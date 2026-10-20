#ifndef KHAN_TENSOR_LIB_H
#define KHAN_TENSOR_LIB_H

#include "interpreter.h"
#include "vm.h"

/*
 * Native tensor engine — real contiguous double storage (VAL_TENSOR,
 * see interpreter.h), not the nested-Khan-array simulation the old
 * packages/tensor/tensor.kh used (O(n^3) matmul over Value arrays, one
 * malloc per element). This is the first slice of the AI-native-core
 * direction described in docs/future plans/phase2-ai-foundation-plan.md
 * — see docs/tensor.md for the full design writeup, scope limits, and
 * why these are named with a leading underscore (internal engine
 * functions the Khan-level `Tensor` class in packages/tensor/tensor.kh
 * wraps; user code is meant to go through that class, not call these
 * directly, same convention as webi_lib.c's `_webi_*` functions).
 *
 * v1 scope: 0/1/2-D tensors for construction and conversion; elementwise
 * add/sub/mul support any matching shape plus scalar broadcasting;
 * matmul/transpose are 2-D only. No GPU, no broadcasting beyond "one
 * side is a scalar", no 3-D+ matmul (batched matmul).
 */

void fn_tensor_from_nested(Value *result, Interpreter *interp, int argc, Value *args);
void fn_tensor_to_nested(Value *result, Interpreter *interp, int argc, Value *args);
void fn_tensor_shape(Value *result, Interpreter *interp, int argc, Value *args);
void fn_tensor_ndim(Value *result, Interpreter *interp, int argc, Value *args);
void fn_tensor_numel(Value *result, Interpreter *interp, int argc, Value *args);
void fn_tensor_add(Value *result, Interpreter *interp, int argc, Value *args);
void fn_tensor_sub(Value *result, Interpreter *interp, int argc, Value *args);
void fn_tensor_mul(Value *result, Interpreter *interp, int argc, Value *args);
void fn_tensor_matmul(Value *result, Interpreter *interp, int argc, Value *args);
void fn_tensor_transpose(Value *result, Interpreter *interp, int argc, Value *args);
void fn_tensor_reshape(Value *result, Interpreter *interp, int argc, Value *args);
void fn_tensor_sum(Value *result, Interpreter *interp, int argc, Value *args);
void fn_tensor_ones_like(Value *result, Interpreter *interp, int argc, Value *args);
void fn_tensor_zeros_like(Value *result, Interpreter *interp, int argc, Value *args);
void fn_tensor_relu(Value *result, Interpreter *interp, int argc, Value *args);
void fn_tensor_relu_backward(Value *result, Interpreter *interp, int argc, Value *args);
void fn_tensor_sigmoid(Value *result, Interpreter *interp, int argc, Value *args);
void fn_tensor_tanh(Value *result, Interpreter *interp, int argc, Value *args);
void fn_tensor_sum_to_shape(Value *result, Interpreter *interp, int argc, Value *args);
void fn_tensor_div(Value *result, Interpreter *interp, int argc, Value *args);
void fn_tensor_sqrt(Value *result, Interpreter *interp, int argc, Value *args);
void fn_tensor_softmax(Value *result, Interpreter *interp, int argc, Value *args);
void fn_tensor_argmax(Value *result, Interpreter *interp, int argc, Value *args);
void fn_tensor_cross_entropy_loss(Value *result, Interpreter *interp, int argc, Value *args);
void fn_tensor_cross_entropy_grad(Value *result, Interpreter *interp, int argc, Value *args);

void tensor_register_all_vm(VM *vm);

#endif
