# Kosh FS Package — v1.0.0

**Author**: Irfan Khan  
**Package**: `fs`  
**Installation**: `kh install fs`

## Overview

The `fs` package provides file system utilities for Kosh, including reading, writing, appending, copying files, path manipulation, and JSON config file management.

## Installation

```bash
kh install fs
```

```kosh
import "fs"
```

---

## Reading Files

```kosh
fs_read(path)           # Read entire file as string
fs_read_lines(path)     # Read file and split into lines → array
fs_read_json(path)      # Read and decode JSON file → map/array/nil
```

#### Examples

```kosh
let data = fs_read("config.txt")
print data

let lines = fs_read_lines("data.csv")
for line in lines:
    print line

let config = fs_read_json("settings.json")
print config["theme"]
```

---

## Writing Files

```kosh
fs_write(path, content)         # Write string to file (overwrites)
fs_write_json(path, data)       # Encode data as JSON and write
fs_append(path, content)        # Append string to file
fs_append_line(path, line)      # Append line (with newline) to file
```

#### Examples

```kosh
fs_write("out.txt", "Hello, Kosh!")
fs_write_json("data.json", {"name": "Irfan", "score": 100})
fs_append("log.txt", "new entry\n")
fs_append_line("log.txt", "another line")
```

---

## File Checks

```kosh
fs_exists(path)         # Check if file exists → bool
fs_is_empty(path)       # Check if file is empty → bool
fs_size(path)           # Get file size in bytes → number
```

#### Examples

```kosh
if fs_exists("config.json"):
    print "Config exists"
    if fs_is_empty("config.json"):
        print "But it's empty"
    else:
        print "Size: " + str(fs_size("config.json")) + " bytes"
```

---

## Path Helpers

```kosh
fs_basename(path)       # Get filename from path
fs_dirname(path)        # Get directory from path
fs_extname(path)        # Get file extension (e.g. ".txt")
fs_stem(path)           # Get filename without extension
fs_join(a, b)           # Join two path segments
```

#### Examples

```kosh
print fs_basename("/home/user/file.txt")    # "file.txt"
print fs_dirname("/home/user/file.txt")     # "/home/user"
print fs_extname("image.png")               # ".png"
print fs_stem("archive.tar.gz")             # "archive.tar"
print fs_join("docs", "packages")           # "docs/packages"
```

---

## Copy / Move

```kosh
fs_copy(src, dst)       # Copy file from src to dst → bool
```

#### Example

```kosh
if fs_copy("backup.txt", "restore.txt"):
    print "File copied successfully"
else:
    print "Copy failed"
```

---

## JSON Config Helpers

```kosh
fs_config_read(path)            # Read JSON config file (or empty map)
fs_config_get(path, key, default)  # Get config value with default
fs_config_set(path, key, value)    # Set config value and save
```

#### Examples

```kosh
# Read config
let theme = fs_config_get("settings.json", "theme", "light")
print "Current theme: " + theme

# Update config
fs_config_set("settings.json", "theme", "dark")
fs_config_set("settings.json", "font_size", 14)
```

---

## Complete Example

```kosh
import "fs"

# Write initial config
let config = {
    "app_name": "MyApp",
    "version": "1.0",
    "debug": true
}
fs_write_json("app_config.json", config)

# Read and modify
let loaded = fs_read_json("app_config.json")
loaded["version"] = "1.1"
loaded["debug"] = false
fs_write_json("app_config.json", loaded)

# Log the operation
fs_append_line("app.log", "Config updated to v1.1")

# Verify
print "Config exists: " + str(fs_exists("app_config.json"))
print "Log size: " + str(fs_size("app.log")) + " bytes"
```

---

## Version History

| Version | Changes |
|---------|---------|
| 1.0.0 | Initial release |
