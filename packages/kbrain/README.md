# Kosh KBrain Package — v1.0.0

**Package**: `kbrain`  
**Installation**: `kh install kbrain`

## Overview

The `kbrain` package provides simple machine learning and AI utilities for Kosh. It includes a linear regression model with gradient descent training.

## Installation

```bash
kh install kbrain
```

```kosh
import "kbrain"
```

---

## Functions

```kosh
linear_regression()                 # Create a new linear regression model
train(model, x, y)                  # Train model using gradient descent → updated model
predict(model, x)                   # Make a prediction for input x
set_params(model, lr, epochs)       # Set learning rate and training epochs
```

#### Example

```kosh
# Create model
let model = linear_regression()
model = set_params(model, 0.01, 1000)

# Training data: y = 2x + 1
let x = [[1], [2], [3], [4], [5]]
let y = [3, 5, 7, 9, 11]

# Train
model = train(model, x, y)

# Predict
print predict(model, [6])   # ~13
print predict(model, [7])   # ~15
```

---

## Version History

| Version | Changes |
|---------|---------|
| 1.0.0 | Initial release |
