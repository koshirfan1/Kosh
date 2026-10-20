# Kosh Tensor Package — v1.0.0

**Package**: `tensor`  
**Installation**: `kh install tensor`

## Overview

The `tensor` package provides multi-dimensional array math for AI applications. It supports matrix multiplication, vector dot products, element-wise operations, and scalar multiplication.

## Installation

```bash
kh install tensor
```

```kosh
import "tensor"
```

---

## Functions

```kosh
matmul(A, B)            # Matrix multiplication (A × B) → matrix or nil if dimension mismatch
dot(a, b)               # Vector dot product → number or nil
add(a, b)               # Element-wise vector addition → array
scale(A, s)             # Scalar multiplication of matrix → matrix
```

#### Examples

```kosh
# Matrix multiplication
let A = [[1, 2], [3, 4]]
let B = [[5, 6], [7, 8]]
let C = matmul(A, B)
# C = [[19, 22], [43, 50]]

# Dot product
let v1 = [1, 2, 3]
let v2 = [4, 5, 6]
print dot(v1, v2)       # 32

# Element-wise addition
print add([1, 2, 3], [4, 5, 6])  # [5, 7, 9]

# Scalar multiplication
let M = [[1, 2], [3, 4]]
let scaled = scale(M, 2)
# scaled = [[2, 4], [6, 8]]
```

---

## Version History

| Version | Changes |
|---------|---------|
| 1.0.0 | Initial release |
