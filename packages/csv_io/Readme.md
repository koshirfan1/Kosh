# Kosh CSV I/O Package — v1.0.0

**Package**: `csv_io`  
**Installation**: `kh install csv_io`

## Overview

The `csv_io` package provides CSV file reading and writing for Kosh. It reads CSV files into arrays of maps (using the first row as headers) and writes arrays of maps back to CSV format.

## Installation

```bash
kh install csv_io
```

```kosh
import "csv_io"
```

---

## Functions

```kosh
csv_read(path)          # Read CSV file → array of maps (first row = headers)
csv_write(path, rows)   # Write array of maps to CSV file → bool
```

#### Example

Given `data.csv`:
```
name,age,city
Alice,30,New York
Bob,25,London
Charlie,35,Tokyo
```

```kosh
let data = csv_read("data.csv")
for row in data:
    print row["name"] + " is " + row["age"] + " from " + row["city"]
```

Output:
```
Alice is 30 from New York
Bob is 25 from London
Charlie is 35 from Tokyo
```

---

## Writing CSV

```kosh
let rows = [
    {"product": "Widget", "price": "9.99", "stock": "100"},
    {"product": "Gadget", "price": "24.99", "stock": "50"},
    {"product": "Doohickey", "price": "4.99", "stock": "200"}
]

csv_write("products.csv", rows)
```

Produces `products.csv`:
```
product,price,stock
Widget,9.99,100
Gadget,24.99,50
Doohickey,4.99,200
```

---

## Complete Example

```kosh
import "csv_io"

# Read existing data
let people = csv_read("people.csv")
print "Loaded " + str(len(people)) + " records"

# Add a new record
people = push(people, {"name": "Diana", "age": "28", "city": "Paris"})

# Write back
csv_write("people.csv", people)
print "Saved " + str(len(people)) + " records"
```

---

## Version History

| Version | Changes |
|---------|---------|
| 1.0.0 | Initial release |