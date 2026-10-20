# Kosh Math Package — v2.0.0

**Author**: Irfan Khan  
**Package**: `math`  
**Installation**: `kh install math`

## Overview

The `math` package is a comprehensive mathematics library for Kosh. Version 2.0.0 extends the original core math functions with full 3D math support including vectors (Vec2, Vec3, Vec4), 4×4 matrices, quaternions, trigonometry, interpolation functions, and geometry intersection tests. All trig functions are implemented via Taylor series with no external C math library dependency.

## Installation

```bash
kh install math
```

```kosh
import "math"
```

---

## Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `PI` | 3.141592653589793 | π |
| `TAU` | 6.283185307179586 | 2π |
| `E` | 2.718281828459045 | Euler's number |
| `DEG2RAD` | 0.017453292519943 | Degrees → radians multiplier |
| `RAD2DEG` | 57.29577951308232 | Radians → degrees multiplier |
| `EPSILON` | 0.000001 | Small epsilon for floating-point comparison |
| `INF` | 999999999999.0 | Large sentinel for "infinity" |

### Vec3 Direction Constants

| Constant | Value |
|----------|-------|
| `VEC3_ZERO` | `(0, 0, 0)` |
| `VEC3_ONE` | `(1, 1, 1)` |
| `VEC3_UP` | `(0, 1, 0)` |
| `VEC3_DOWN` | `(0, -1, 0)` |
| `VEC3_RIGHT` | `(1, 0, 0)` |
| `VEC3_LEFT` | `(-1, 0, 0)` |
| `VEC3_FORWARD` | `(0, 0, -1)` |
| `VEC3_BACK` | `(0, 0, 1)` |

### Color Constants

| Constant | (R, G, B, A) |
|----------|---------------|
| `COLOR_WHITE` | `(1, 1, 1, 1)` |
| `COLOR_BLACK` | `(0, 0, 0, 1)` |
| `COLOR_RED` | `(1, 0, 0, 1)` |
| `COLOR_GREEN` | `(0, 1, 0, 1)` |
| `COLOR_BLUE` | `(0, 0, 1, 1)` |
| `COLOR_CLEAR` | `(0, 0, 0, 0)` |

---

## Core Math Functions (v1 Compatible)

### Arithmetic

```kosh
math_sqrt(n)       # Square root (Babylonian method)
math_pow(base, exp) # Exponentiation (integer exponent)
math_abs(n)        # Absolute value
math_floor(n)      # Round down to integer
math_ceil(n)       # Round up to integer
math_round(n)      # Round to nearest integer
math_max(a, b)     # Maximum of two numbers
math_min(a, b)     # Minimum of two numbers
math_clamp(val, lo, hi)  # Clamp value to range [lo, hi]
math_sign(n)       # Sign: 1 for positive, -1 for negative, 0 for zero
```

#### Examples

```kosh
print math_sqrt(144)        # 12
print math_pow(2, 10)       # 1024
print math_abs(-5)          # 5
print math_floor(3.7)       # 3
print math_ceil(3.2)        # 4
print math_round(3.5)       # 4
print math_max(10, 20)      # 20
print math_min(10, 20)      # 10
print math_clamp(15, 0, 10) # 10
print math_sign(-42)        # -1
```

### Number Theory

```kosh
math_gcd(a, b)       # Greatest common divisor
math_lcm(a, b)       # Least common multiple
math_is_prime(n)     # Primality test (true/false)
math_factorial(n)    # Factorial (recursive)
```

#### Examples

```kosh
print math_gcd(48, 18)       # 6
print math_lcm(12, 15)       # 60
print math_is_prime(97)      # true
print math_is_prime(100)     # false
print math_factorial(6)      # 720
```

### Statistics

```kosh
math_sum(arr)       # Sum of array elements
math_mean(arr)      # Arithmetic mean (average)
math_range(start, stop, step)   # Generate range as array
```

#### Examples

```kosh
print math_sum([1, 2, 3, 4])        # 10
print math_mean([1, 2, 3, 4])       # 2.5
print math_range(0, 10, 2)          # [0, 2, 4, 6, 8]
```

---

## Trigonometry (Taylor Series)

All trig functions use Taylor series expansion — no external `math.h` dependency.

```kosh
math_rad(deg)        # Degrees → radians
math_deg(rad)        # Radians → degrees
math_sin(x)          # Sine (radians)
math_cos(x)          # Cosine (radians)
math_tan(x)          # Tangent (radians)
math_asin(x)         # Arc sine (result in radians)
math_acos(x)         # Arc cosine (result in radians)
math_atan(x)         # Arc tangent (result in radians)
math_atan2(y, x)     # Arc tangent with quadrant (result in radians)
```

#### Examples

```kosh
print math_sin(PI / 2)          # 1.0
print math_cos(0)               # 1.0
print math_rad(180)             # 3.14159...
print math_deg(PI)              # 180
print math_atan2(1, 0)          # 1.5708 (PI/2)
```

---

## 2D Vectors (Vec2)

Represented as maps: `{"x": number, "y": number}`.

### Construction & Properties

```kosh
vec2(x, y)              # Create a Vec2
vec2_add(a, b)          # Component-wise addition
vec2_sub(a, b)          # Component-wise subtraction
vec2_scale(v, s)        # Scalar multiplication
vec2_dot(a, b)          # Dot product
vec2_len(v)             # Magnitude (length)
vec2_norm(v)            # Normalize to unit length
vec2_dist(a, b)         # Distance between two vectors
vec2_lerp(a, b, t)      # Linear interpolation
vec2_angle(v)           # Angle from x-axis (radians)
vec2_perp(v)            # Perpendicular vector (rotate 90°)
vec2_eq(a, b)           # Equality with EPSILON tolerance
vec2_str(v)             # Human-readable string: "vec2(x, y)"
```

#### Example

```kosh
let v1 = vec2(3, 4)
let v2 = vec2(1, 2)

print vec2_len(v1)              # 5
print vec2_dot(v1, v2)          # 11
print vec2_add(v1, v2)          # {"x":4, "y":6}
print vec2_norm(v1)             # {"x":0.6, "y":0.8}
print vec2_dist(v1, v2)         # 2.828...
print vec2_str(v1)              # "vec2(3, 4)"
```

---

## 3D Vectors (Vec3)

Represented as maps: `{"x": number, "y": number, "z": number}`.

### Construction & Properties

```kosh
vec3(x, y, z)           # Create a Vec3
vec3_add(a, b)          # Component-wise addition
vec3_sub(a, b)          # Component-wise subtraction
vec3_scale(v, s)        # Scalar multiplication
vec3_neg(v)             # Negation
vec3_dot(a, b)          # Dot product
vec3_cross(a, b)        # Cross product
vec3_len(v)             # Magnitude
vec3_len_sq(v)          # Squared magnitude (avoids sqrt)
vec3_norm(v)            # Normalize to unit length
vec3_dist(a, b)         # Distance between two vectors
vec3_lerp(a, b, t)      # Linear interpolation
vec3_reflect(v, normal) # Reflection vector
vec3_angle(a, b)        # Angle between vectors (radians)
vec3_project(v, onto)   # Project v onto onto
vec3_eq(a, b)           # Equality with tolerance
vec3_str(v)             # Human-readable string
```

#### Example

```kosh
let v1 = vec3(1, 0, 0)
let v2 = vec3(0, 1, 0)

print vec3_cross(v1, v2)        # {"x":0, "y":0, "z":1}
print vec3_dot(v1, v2)          # 0
print vec3_len(vec3(1, 2, 3))   # 3.741...
print vec3_str(v1)              # "vec3(1, 0, 0)"
```

---

## 4D Vectors (Vec4) / Colors

Represented as maps: `{"x": number, "y": number, "z": number, "w": number}`. The `color()` function creates a Vec4 with semantic RGBA naming.

```kosh
vec4(x, y, z, w)        # Create a Vec4
vec4_add(a, b)          # Component-wise addition
vec4_scale(v, s)        # Scalar multiplication
vec4_dot(a, b)          # Dot product
vec4_len(v)             # Magnitude
vec4_norm(v)            # Normalize

color(r, g, b, a)       # Create an RGBA color (returns Vec4)
```

#### Example

```kosh
let c = color(1, 0, 0, 1)       # Red, fully opaque
print c["x"]                     # 1.0 (R)
print c["y"]                     # 0.0 (G)
print c["z"]                     # 0.0 (B)
print c["w"]                     # 1.0 (A)
```

---

## Quaternions

Represented as maps: `{"w": number, "x": number, "y": number, "z": number}`.

```kosh
quat(w, x, y, z)                # Create a quaternion
quat_identity()                 # Identity quaternion (no rotation)
quat_mul(a, b)                  # Quaternion multiplication (compose rotations)
quat_conj(q)                    # Conjugate
quat_len(q)                     # Magnitude
quat_norm(q)                    # Normalize to unit quaternion
quat_from_axis_angle(axis, angle)  # Create from axis + angle (radians)
quat_from_euler(pitch, yaw, roll)  # Create from Euler angles (radians)
quat_rotate_vec3(q, v)          # Rotate a Vec3 by quaternion
quat_slerp(a, b, t)             # Spherical linear interpolation
quat_to_euler(q)                # Decompose to Euler angles → Vec3(pitch, yaw, roll)
```

#### Example

```kosh
let q = quat_from_axis_angle(VEC3_UP, PI / 2)  # 90° around Y
let v = vec3(1, 0, 0)
let rotated = quat_rotate_vec3(q, v)            # (0, 0, -1) — rotated 90°
```

---

## 4×4 Matrices (Mat4)

Represented as flat 16-element arrays in row-major order: `[m00, m01, m02, m03, m10, m11, m12, m13, ...]`.

```kosh
mat4(m)                     # Wrap / cast an array as matrix
mat4_identity()             # Identity matrix
mat4_get(m, row, col)       # Get element at (row, col)
mat4_mul(a, b)              # Matrix multiplication
mat4_mul_vec4(m, v)         # Transform a Vec4
mat4_translate(tx, ty, tz)  # Translation matrix
mat4_scale(sx, sy, sz)      # Scale matrix
mat4_rotate_x(angle)        # Rotation around X axis (radians)
mat4_rotate_y(angle)        # Rotation around Y axis (radians)
mat4_rotate_z(angle)        # Rotation around Z axis (radians)
mat4_from_quat(q)           # Convert quaternion to rotation matrix
mat4_perspective(fov, aspect, near, far)  # Perspective projection matrix
mat4_look_at(eye, target, up)             # View matrix (camera look-at)
mat4_transpose(m)           # Transpose matrix
mat4_trs(pos, rot, scl)     # TRS (Translate-Rotate-Scale) composition
```

#### Example

```kosh
let m = mat4_translate(1, 2, 3)
let v = vec4(0, 0, 0, 1)
let result = mat4_mul_vec4(m, v)  # {"x":1, "y":2, "z":3, "w":1}
```

---

## Interpolation Functions

```kosh
lerp(a, b, t)                   # Linear interpolation
lerp_clamp(a, b, t)             # Linear interpolation with t clamped to [0, 1]
smoothstep(edge0, edge1, x)     # Smooth Hermite interpolation
smootherstep(edge0, edge1, x)   # Even smoother (5th-order) interpolation
ease_in(t)                      # Quadratic ease-in (t²)
ease_out(t)                     # Quadratic ease-out (t(2-t))
ease_in_out(t)                  # Quadratic ease-in-out
bezier(p0, p1, p2, p3, t)      # Cubic Bézier curve evaluation
remap(val, in_lo, in_hi, out_lo, out_hi)  # Remap from one range to another
```

#### Examples

```kosh
print lerp(0, 10, 0.5)            # 5
print smoothstep(0, 1, 0.5)       # 0.5
print remap(50, 0, 100, 0, 1)     # 0.5
```

---

## Geometry Helpers

### Ray

```kosh
ray(origin, direction)      # Create a ray
ray_at(r, t)                # Get point at parameter t along ray
```

### AABB (Axis-Aligned Bounding Box)

```kosh
aabb(min_pt, max_pt)        # Create AABB
aabb_center(box)            # Center point
aabb_size(box)              # Size (extents)
aabb_contains(box, point)   # Check if point is inside
aabb_intersects(a, b)       # Check if two AABBs overlap
```

### Sphere

```kosh
sphere(center, radius)      # Create a sphere
sphere_contains(s, point)   # Check if point is inside sphere
sphere_intersects(a, b)     # Check if two spheres overlap
```

### Intersection Tests

```kosh
ray_sphere(r, s)            # Ray-sphere intersection → t parameter or nil
ray_aabb(r, box)            # Ray-AABB intersection → t parameter or nil
```

#### Example

```kosh
let r = ray(vec3(0, 0, 0), vec3(1, 0, 0))
let s = sphere(vec3(10, 0, 0), 2)
let t = ray_sphere(r, s)
if t != nil:
    print "Hit at distance " + str(t)  # 8
```

---

## Version History

| Version | Changes |
|---------|---------|
| 2.0.0 | Added Vec2, Vec3, Vec4, Mat4, Quaternion, Trigonometry, Interpolation, Geometry helpers |
| 1.0.0 | Initial release: sqrt, pow, gcd, lcm, factorial, mean, is_prime, etc. |
