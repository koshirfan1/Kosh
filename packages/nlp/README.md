# Kosh NLP Package — v1.0.0

**Package**: `nlp`  
**Installation**: `kh install nlp`

## Overview

The `nlp` package provides basic Natural Language Processing utilities for Kosh, including tokenization and sentiment analysis.

## Installation

```bash
kh install nlp
```

```kosh
import "nlp"
```

---

## Functions

```kosh
sentiment(text)         # Analyze sentiment of text → score (positive = +1 per positive word, negative = -1 per negative word)
tokenize(text)          # Split text into words (removes punctuation) → array
```

#### Examples

```kosh
print sentiment("This is great and awesome")    # 2
print sentiment("This is bad and terrible")     # -2
print sentiment("This is good but sad")         # 0
print tokenize("Hello, World!")                 # ["Hello", "World"]
```

---

## Version History

| Version | Changes |
|---------|---------|
| 1.0.0 | Initial release |
