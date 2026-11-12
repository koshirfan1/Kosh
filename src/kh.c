// ===========================================================================
// kh — Kosh Package Manager
// ===========================================================================

// Windows headers FIRST to avoid TokenType conflict with our token.h
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <shlobj.h>
#  include <direct.h>
#  define MKDIR(p) _mkdir(p)
#  define RMDIR(p) _rmdir(p)
static void enable_ansi(void) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode))
        SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    SetConsoleOutputCP(CP_UTF8);
}
#else
#  define _POSIX_C_SOURCE 200809L
#  include <sys/stat.h>
#  include <unistd.h>
#  include <dirent.h>
#  define MKDIR(p) mkdir(p, 0755)
#  define RMDIR(p) rmdir(p)
static void enable_ansi(void) {}
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
#define REGISTRY_URL \
  "https://raw.githubusercontent.com/koshirfan1/Kosh/main/packages/registry.json"

#define RAW_BASE \
  "https://raw.githubusercontent.com/koshirfan1/Kosh/main/packages"

// Active registry and file base. Defaults are the constants above; the
// environment variables KH_REGISTRY_URL and KH_RAW_BASE can point kh at a
// mirror or a local test registry. Only https:// and file:// are accepted
// (never plain http://), and main() validates them before anything runs.
static const char *g_registry_url = REGISTRY_URL;
static const char *g_raw_base     = RAW_BASE;

static int url_scheme_ok(const char *u) {
    return strncmp(u, "https://", 8) == 0 || strncmp(u, "file://", 7) == 0;
}

// ---------------------------------------------------------------------------
// Get the Kosh packages directory: ~/.kosh/packages/
// Returns a heap-allocated path.  Caller must free.
// ---------------------------------------------------------------------------
static char *get_packages_dir(void) {
    char *base = NULL;

#ifdef _WIN32
    char appdata[MAX_PATH];
    char userprofile_buf[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PROFILE, NULL, 0, appdata))) {
        base = appdata;
    } else {
        /* Use GetEnvironmentVariableA instead of getenv to avoid Win32 conflicts */
        if (GetEnvironmentVariableA("USERPROFILE", userprofile_buf, MAX_PATH) > 0) {
            base = userprofile_buf;
        } else {
            base = "C:\\Users\\Default";
        }
    }
    size_t len = strlen(base) + 32;
    char *dir = malloc(len);
    snprintf(dir, len, "%s\\.kosh\\packages", base);
#else
    base = getenv("HOME");
    if (!base) base = "/tmp";
    size_t len = strlen(base) + 32;
    char *dir = malloc(len);
    snprintf(dir, len, "%s/.kosh/packages", base);
#endif
    return dir;
}

// ---------------------------------------------------------------------------
// Ensure a directory (and its parent) exists
// ---------------------------------------------------------------------------
static void mkdir_p(const char *path) {
    char tmp[1024];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char save = *p;
            *p = '\0';
            MKDIR(tmp);   // ignore errors (dir may exist)
            *p = save;
        }
    }
    MKDIR(tmp);
}

// ---------------------------------------------------------------------------
// Input validation. Package names, registry-supplied URLs and registry-
// supplied file names all end up inside a shell command line (curl via
// system()/popen()) or a filesystem path, so each one is checked against a
// strict whitelist/blacklist BEFORE it is used -- a compromised or malicious
// registry entry (or a hostile `kh install` argument) must not be able to
// inject shell syntax or escape the package directory with "../".
// ---------------------------------------------------------------------------

// Package name: 1-64 chars of [A-Za-z0-9_-], never starting with '-' or '.'.
static int valid_pkg_name(const char *name) {
    if (!name || !*name) return 0;
    size_t n = strlen(name);
    if (n > 64) return 0;
    if (name[0] == '-') return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
    }
    return 1;
}

// Relative file name inside a package dir: [A-Za-z0-9._/-], no leading '/',
// no ".." path component, no empty component.
static int valid_rel_file(const char *f) {
    if (!f || !*f || strlen(f) > 128 || f[0] == '/') return 0;
    for (const char *p = f; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == '/'))
            return 0;
    }
    if (strstr(f, "..") || strstr(f, "//")) return 0;
    return 1;
}

// True if `s` can be placed inside a double-quoted shell argument without
// being able to terminate the quote or trigger expansion.
static int shell_safe(const char *s) {
    if (!s || !*s) return 0;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7f) return 0;          // control chars / newlines
        if (c == '"' || c == '`' || c == '$' || c == '!') return 0;
#ifdef _WIN32
        if (c == '%' || c == '^') return 0;           // cmd.exe expansion
#else
        if (c == '\\') return 0;                      // escapes inside "..."
#endif
    }
    return 1;
}

// ---------------------------------------------------------------------------
// Appends a cache-busting query parameter (current unix time) to a URL.
// raw.githubusercontent.com is served through a CDN that can hold an old
// response for a while after a push — a distinct query string forces a
// fresh edge fetch regardless of whatever TTL that cache would otherwise
// honor, so `kh install`/`kh update`/`kh list` never silently serve a
// stale registry.json or package file just because a push happened
// recently. Caller must free the result.
// ---------------------------------------------------------------------------
static char *cache_busted_url(const char *url) {
    if (strncmp(url, "file://", 7) == 0) {          // local registry: no cache to bust
        char *same = malloc(strlen(url) + 1);
        strcpy(same, url);
        return same;
    }
    char *out = malloc(strlen(url) + 32);
    char sep = strchr(url, '?') ? '&' : '?';
    sprintf(out, "%s%ccb=%ld", url, sep, (long)time(NULL));
    return out;
}

// ---------------------------------------------------------------------------
// Run curl and capture output.  Returns heap string, caller must free.
// On Windows we use curl.exe (ships with Windows 10+) the same way.
// ---------------------------------------------------------------------------
static char *fetch_url(const char *url) {
    char *busted = cache_busted_url(url);

    // Build command safely (URL from our own constants, trusted). The
    // no-cache/pragma headers are a second line of defense for any
    // intermediate proxy that does honor them; the cache-busting query
    // param above is what actually guarantees a fresh fetch either way.
    char cmd[2048];
    if (!shell_safe(busted)) { free(busted); return NULL; }
    int cn = snprintf(cmd, sizeof(cmd),
             "curl -s -L --max-time 30 -H \"Cache-Control: no-cache\" -H \"Pragma: no-cache\" \"%s\"",
             busted);
    free(busted);
    if (cn < 0 || (size_t)cn >= sizeof(cmd)) return NULL;   // truncated -> refuse

    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;

    char *out = NULL;
    size_t len = 0, cap = 0;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (len + n + 1 > cap) {
            cap = cap ? (cap + n) * 2 : 8192;
            out = realloc(out, cap);
        }
        memcpy(out + len, buf, n);
        len += n;
    }
    pclose(fp);
    if (!out) return strdup("");
    out[len] = '\0';
    return out;
}

// ---------------------------------------------------------------------------
// Download URL and save to a file path.
// Returns 1 on success, 0 on failure.
// ---------------------------------------------------------------------------
static int download_to_file(const char *url, const char *filepath) {
    char *busted = cache_busted_url(url);

    char cmd[2048];
    if (!shell_safe(busted) || !shell_safe(filepath)) { free(busted); return 0; }
    int cn = snprintf(cmd, sizeof(cmd),
             "curl -s -L --max-time 30 -H \"Cache-Control: no-cache\" -H \"Pragma: no-cache\" -o \"%s\" \"%s\"",
             filepath, busted);
    free(busted);
    if (cn < 0 || (size_t)cn >= sizeof(cmd)) return 0;      // truncated -> refuse

    int rc = system(cmd);
    return rc == 0;
}

// ---------------------------------------------------------------------------
// Minimal JSON field extractor.
// Finds  "key": "value"  in a JSON string.
// Returns heap string or NULL.  Caller must free.
// ---------------------------------------------------------------------------
static char *json_get_string(const char *json, const char *key) {
    // Build search pattern: "key":
    char pat[256];
    snprintf(pat, sizeof(pat), "\"%s\"", key);

    const char *pos = strstr(json, pat);
    if (!pos) return NULL;
    pos += strlen(pat);

    // Skip whitespace and colon
    while (*pos && (isspace((unsigned char)*pos) || *pos == ':')) pos++;

    if (*pos != '"') return NULL;
    pos++; // skip opening quote

    // Find closing quote (not escaped)
    const char *start = pos;
    while (*pos && *pos != '"') {
        if (*pos == '\\') pos++; // skip escape
        if (*pos) pos++;
    }
    size_t len = (size_t)(pos - start);
    char *result = malloc(len + 1);
    memcpy(result, start, len);
    result[len] = '\0';
    return result;
}

// ---------------------------------------------------------------------------
// Minimal JSON string-array extractor.
// Finds  "key": ["a", "b", "c"]  in a JSON string.
// Returns a heap-allocated array of heap-allocated strings (caller frees
// each string, then the array), and sets *out_count. Returns NULL and sets
// *out_count = 0 if the key isn't present or isn't an array.
// ---------------------------------------------------------------------------
static char **json_get_string_array(const char *json, const char *key, int *out_count) {
    *out_count = 0;
    char pat[256];
    snprintf(pat, sizeof(pat), "\"%s\"", key);

    const char *pos = strstr(json, pat);
    if (!pos) return NULL;
    pos += strlen(pat);

    while (*pos && (isspace((unsigned char)*pos) || *pos == ':')) pos++;
    if (*pos != '[') return NULL;
    pos++; // skip '['

    char **items = NULL;
    int count = 0;
    int capacity = 0;

    while (*pos && *pos != ']') {
        while (*pos && isspace((unsigned char)*pos)) pos++;
        if (*pos == ',') { pos++; continue; }
        if (*pos != '"') break;
        pos++; // skip opening quote
        const char *start = pos;
        while (*pos && *pos != '"') {
            if (*pos == '\\') pos++;
            if (*pos) pos++;
        }
        size_t len = (size_t)(pos - start);
        char *item = malloc(len + 1);
        memcpy(item, start, len);
        item[len] = '\0';
        if (*pos == '"') pos++;

        if (count == capacity) {
            capacity = capacity == 0 ? 4 : capacity * 2;
            items = realloc(items, sizeof(char *) * capacity);
        }
        items[count++] = item;
    }

    *out_count = count;
    return items;
}

// ---------------------------------------------------------------------------
// Parse the registry JSON and find a package entry by name.
// Returns heap-allocated JSON fragment for the package object, or NULL.
// ---------------------------------------------------------------------------
static char *find_package_in_registry(const char *registry, const char *name) {
    const char *p = registry;
    while ((p = strstr(p, "\"name\"")) != NULL) {
        // Check if the name value matches
        char *found = json_get_string(p, "name");
        if (!found) { p++; continue; }

        int match = (strcmp(found, name) == 0);
        free(found);

        if (match) {
            // Find the enclosing { for this object
            const char *obj_start = p;
            while (obj_start > registry && *obj_start != '{') obj_start--;
            // Find closing }
            int depth = 0;
            const char *obj_end = obj_start;
            while (*obj_end) {
                if (*obj_end == '{') depth++;
                else if (*obj_end == '}') { depth--; if (depth == 0) { obj_end++; break; } }
                obj_end++;
            }
            size_t len = (size_t)(obj_end - obj_start);
            char *obj = malloc(len + 1);
            memcpy(obj, obj_start, len);
            obj[len] = '\0';
            return obj;
        }
        p++;
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// Print a colored header line (ANSI, degrades gracefully)
// ---------------------------------------------------------------------------
static void print_header(const char *text) {
    printf("\033[1;36m%s\033[0m\n", text);
}
static void print_ok(const char *text) {
    printf("\033[1;32m✓\033[0m %s\n", text);
}
static void print_err(const char *text) {
    printf("\033[1;31m✗\033[0m %s\n", text);
}
static void print_info_line(const char *text) {
    printf("\033[33m→\033[0m %s\n", text);
}

// ---------------------------------------------------------------------------
// SHA-256 (FIPS 180-4). Used to verify every downloaded file against the
// checksum the registry publishes for it. kh is a standalone program, so this
// is a small self-contained implementation (checked against hashlib and the
// NIST vectors in tests/kh/test_kh_install.py).
// ---------------------------------------------------------------------------
typedef struct { uint32_t h[8]; uint64_t bits; unsigned char buf[64]; size_t n; } sha256_ctx;

static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(sha256_ctx *c, const unsigned char *p) {
    uint32_t w[64], a, b, cc, d, e, f, g, h, t1, t2;
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4*i] << 24) | ((uint32_t)p[4*i+1] << 16) | ((uint32_t)p[4*i+2] << 8) | (uint32_t)p[4*i+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR32(w[i-15], 7) ^ ROR32(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = ROR32(w[i-2], 17) ^ ROR32(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (int i = 0; i < 64; i++) {
        t1 = h + (ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        t2 = (ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}
static void sha256_init(sha256_ctx *c) {
    static const uint32_t iv[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    memcpy(c->h, iv, sizeof(iv)); c->bits = 0; c->n = 0;
}
static void sha256_update(sha256_ctx *c, const unsigned char *data, size_t len) {
    c->bits += (uint64_t)len * 8;
    while (len > 0) {
        size_t take = 64 - c->n; if (take > len) take = len;
        memcpy(c->buf + c->n, data, take);
        c->n += take; data += take; len -= take;
        if (c->n == 64) { sha256_block(c, c->buf); c->n = 0; }
    }
}
static void sha256_final(sha256_ctx *c, unsigned char out[32]) {
    uint64_t bits = c->bits;
    unsigned char pad = 0x80;
    sha256_update(c, &pad, 1);
    unsigned char zero = 0;
    while (c->n != 56) sha256_update(c, &zero, 1);
    unsigned char lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (unsigned char)(bits >> (56 - 8 * i));
    sha256_update(c, lenb, 8);
    for (int i = 0; i < 8; i++) {
        out[4*i] = (unsigned char)(c->h[i] >> 24); out[4*i+1] = (unsigned char)(c->h[i] >> 16);
        out[4*i+2] = (unsigned char)(c->h[i] >> 8); out[4*i+3] = (unsigned char)c->h[i];
    }
}

// Hex SHA-256 of a file's bytes into out[65]. Returns 1 on success.
static int hash_file_hex(const char *path, char out[65]) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    sha256_ctx c; sha256_init(&c);
    unsigned char buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) sha256_update(&c, buf, n);
    int bad = ferror(f);
    fclose(f);
    if (bad) return 0;
    unsigned char d[32]; sha256_final(&c, d);
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[2*i] = hx[d[i] >> 4]; out[2*i+1] = hx[d[i] & 15]; }
    out[64] = '\0';
    return 1;
}

// Looks up the checksum for `file` in the entry's  "sha256": { "file": "<64 hex>", ... }
// object. Returns 1 and fills out[65] (lower-case) only for a well-formed
// 64-hex-digit value; anything else (missing key, wrong length, non-hex) is 0.
static int json_get_hash(const char *pkg, const char *file, char out[65]) {
    const char *h = strstr(pkg, "\"sha256\"");
    if (!h) return 0;
    h += 8;
    while (*h && (isspace((unsigned char)*h) || *h == ':')) h++;
    if (*h != '{') return 0;
    const char *end = h; int depth = 0;
    while (*end) {
        if (*end == '{') depth++;
        else if (*end == '}') { depth--; if (depth == 0) break; }
        end++;
    }
    size_t rlen = (size_t)(end - h);
    char *region = malloc(rlen + 1);
    if (!region) return 0;
    memcpy(region, h, rlen); region[rlen] = '\0';

    char pat[512];
    snprintf(pat, sizeof(pat), "\"%s\"", file);
    int ok = 0;
    const char *k = strstr(region, pat);
    if (k) {
        k += strlen(pat);
        while (*k && (isspace((unsigned char)*k) || *k == ':')) k++;
        if (*k == '"') {
            k++;
            int i = 0;
            while (i < 64 && isxdigit((unsigned char)k[i])) { out[i] = (char)tolower((unsigned char)k[i]); i++; }
            if (i == 64 && k[64] == '"') { out[64] = '\0'; ok = 1; }
        }
    }
    free(region);
    return ok;
}

static int move_file(const char *src, const char *dst) {
#ifdef _WIN32
    return MoveFileExA(src, dst, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
#else
    return rename(src, dst);
#endif
}

// Make the directory part of "<base>/<rel>" exist (rel may contain '/').
static void mkdir_parent(const char *base, const char *rel) {
    char path[2048];
    snprintf(path, sizeof(path), "%s/%s", base, rel);
    char *slash = strrchr(path, '/');
    if (slash) { *slash = '\0'; mkdir_p(path); }
}

// Delete the staged files and every directory kh created for them.
static void staging_cleanup(const char *stage, char **files, int count) {
    for (int i = 0; i < count; i++) {
        char p[2048];
        snprintf(p, sizeof(p), "%s/%s", stage, files[i]);
        remove(p);
        char *slash;
        while ((slash = strrchr(p, '/')) != NULL && strlen(p) > strlen(stage)) {
            *slash = '\0';
            if (strcmp(p, stage) == 0) break;
            RMDIR(p);
        }
    }
    RMDIR(stage);
}

// Dependency recursion guard: the packages currently being installed (a cycle
// stops here instead of recursing forever) and a hard depth limit.
#define KH_MAX_DEPTH 8
static char g_stack[KH_MAX_DEPTH + 1][80];
static int  g_depth = 0;

static int is_installed(const char *name) {
    char *dir = get_packages_dir();
    char path[2048];
    snprintf(path, sizeof(path), "%s/%s/%s.kh", dir, name, name);
    free(dir);
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

// ---------------------------------------------------------------------------
// CMD: kh install <n> [--no-verify]
//
// Every file is downloaded into a staging directory first, checked against the
// SHA-256 the registry publishes for it, and only then moved into place. If
// anything fails (download, missing/malformed checksum, mismatch) nothing is
// installed and an existing install of the package is left untouched. Packages
// listed under "dependencies" are installed first, the same way.
//
// What this does and does not protect against: it catches corrupted or
// truncated downloads, and files that changed without the registry being
// updated (including a CDN serving mixed versions). It does NOT protect against
// someone who can change both a package file and the registry that lists its
// checksum - that needs a signed registry, which does not exist yet.
// ---------------------------------------------------------------------------
static int cmd_install(const char *name, int verify) {
    if (g_depth >= KH_MAX_DEPTH) {
        print_err("Dependency chain is too deep — refusing.");
        return 1;
    }
    snprintf(g_stack[g_depth], sizeof(g_stack[g_depth]), "%s", name);
    g_depth++;
    print_info_line("Fetching registry...");

    char *registry = fetch_url(g_registry_url);
    if (!registry || strlen(registry) < 10) {
        print_err("Could not reach registry. Check your internet connection.");
        free(registry);
        g_depth--;
        return 1;
    }

    char *pkg = find_package_in_registry(registry, name);
    free(registry);

    if (!pkg) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Package \"%s\" not found in registry.", name);
        print_err(msg);
        printf("  Run \033[1mkh list\033[0m to see available packages.\n");
        g_depth--;
        return 1;
    }

    char *version = json_get_string(pkg, "version");
    char *url     = json_get_string(pkg, "url");
    char *desc    = json_get_string(pkg, "description");
    int extra_count = 0;
    char **extra_files = json_get_string_array(pkg, "files", &extra_count);
    int dep_count = 0;
    char **deps = json_get_string_array(pkg, "dependencies", &dep_count);

    int rc = 1;
    char **files = NULL;      // destination names, files[0] is the main file
    char **urls  = NULL;
    char (*hashes)[65] = NULL;
    int nfiles = 0;
    char stage[1536] = "";
    int staged = 0;           // number of files that exist in the staging dir

    if (!url) {
        print_err("Registry entry is missing download URL.");
        goto done;
    }
    // The main-file URL comes from the registry: https only, or file:// when the
    // user themselves pointed KH_REGISTRY_URL at a local registry.
    if (!(strncmp(url, "https://", 8) == 0 ||
          (strncmp(g_registry_url, "file://", 7) == 0 && strncmp(url, "file://", 7) == 0))) {
        print_err("Registry entry has a download URL that is not https:// — refusing.");
        goto done;
    }

    nfiles = 1 + extra_count;
    files  = calloc((size_t)nfiles, sizeof(char *));
    urls   = calloc((size_t)nfiles, sizeof(char *));
    hashes = calloc((size_t)nfiles, sizeof(*hashes));
    if (!files || !urls || !hashes) { print_err("Out of memory."); goto done; }

    char mainname[160];
    snprintf(mainname, sizeof(mainname), "%s.kh", name);
    files[0] = malloc(strlen(mainname) + 1); strcpy(files[0], mainname);
    urls[0]  = malloc(strlen(url) + 1);      strcpy(urls[0], url);
    for (int i = 0; i < extra_count; i++) {
        if (!valid_rel_file(extra_files[i])) {
            print_err("Registry lists an unsafe file name — refusing to install.");
            goto done;
        }
        files[i + 1] = extra_files[i];       // ownership moves to files[]
        extra_files[i] = NULL;
        char u[1536];
        snprintf(u, sizeof(u), "%s/%s/%s", g_raw_base, name, files[i + 1]);
        urls[i + 1] = malloc(strlen(u) + 1); strcpy(urls[i + 1], u);
    }

    // Every file needs a published checksum, or the install is refused.
    if (verify) {
        for (int i = 0; i < nfiles; i++) {
            if (!json_get_hash(pkg, files[i], hashes[i])) {
                char m[400];
                snprintf(m, sizeof(m), "Registry has no valid SHA-256 for '%s' — refusing to install unverified code.", files[i]);
                print_err(m);
                printf("  (Pass \033[1m--no-verify\033[0m to install without checking. Only do that for a registry you trust.)\n");
                goto done;
            }
        }
    } else {
        printf("\033[1;33m!\033[0m WARNING: installing WITHOUT checksum verification (--no-verify).\n");
    }

    // Dependencies first (each one is installed and verified exactly like this
    // package). Anything already installed is left alone.
    for (int i = 0; i < dep_count; i++) {
        if (!valid_pkg_name(deps[i])) {
            print_err("Registry lists an invalid dependency name — refusing to install.");
            goto done;
        }
        int on_stack = 0;
        for (int k = 0; k < g_depth; k++) if (strcmp(g_stack[k], deps[i]) == 0) on_stack = 1;
        if (on_stack || is_installed(deps[i])) continue;
        char dm[300];
        snprintf(dm, sizeof(dm), "%s needs %s — installing it first...", name, deps[i]);
        print_info_line(dm);
        if (cmd_install(deps[i], verify) != 0) {
            snprintf(dm, sizeof(dm), "Could not install dependency '%s' — %s was not installed.", deps[i], name);
            print_err(dm);
            goto done;
        }
    }

    char *pkgs_dir = get_packages_dir();
    char pkg_dir[1024];
    snprintf(pkg_dir, sizeof(pkg_dir), "%s/%s", pkgs_dir, name);
    snprintf(stage, sizeof(stage), "%s/.kh-staging-%s", pkgs_dir, name);
    free(pkgs_dir);

    char msg[512];
    snprintf(msg, sizeof(msg), "Installing %s@%s (%d file%s)...", name, version ? version : "?", nfiles, nfiles == 1 ? "" : "s");
    print_info_line(msg);

    mkdir_p(stage);
    for (int i = 0; i < nfiles; i++) {
        mkdir_parent(stage, files[i]);
        char dest[2048];
        snprintf(dest, sizeof(dest), "%s/%s", stage, files[i]);
        staged = i + 1;                       // count it now so a partial file is cleaned up too
        if (!download_to_file(urls[i], dest)) {
            char m[400];
            snprintf(m, sizeof(m), "Download failed for '%s' — nothing was installed.", files[i]);
            print_err(m);
            goto done;
        }
        if (verify) {
            char got[65];
            if (!hash_file_hex(dest, got)) {
                print_err("Could not read the downloaded file to verify it — nothing was installed.");
                goto done;
            }
            if (strcmp(got, hashes[i]) != 0) {
                char m[400];
                snprintf(m, sizeof(m), "Checksum mismatch for '%s' — nothing was installed.", files[i]);
                print_err(m);
                printf("  expected %s\n  got      %s\n", hashes[i], got);
                printf("  The file differs from what the registry lists (corrupted download, a stale cache,\n"
                       "  or a tampered file). Try again in a few minutes; if it persists, report it.\n");
                goto done;
            }
        }
    }

    // Everything verified: move into place.
    mkdir_p(pkg_dir);
    for (int i = 0; i < nfiles; i++) {
        mkdir_parent(pkg_dir, files[i]);
        char from[2048], to[2048];
        snprintf(from, sizeof(from), "%s/%s", stage, files[i]);
        snprintf(to,   sizeof(to),   "%s/%s", pkg_dir, files[i]);
        if (move_file(from, to) != 0) {
            char m[400];
            snprintf(m, sizeof(m), "Could not write '%s' into the package directory.", files[i]);
            print_err(m);
            goto done;
        }
    }
    staged = nfiles;

    // Write a local package.json. Besides name/version it records the file list,
    // dependencies and (when verified) each file's SHA-256, so `kh show`, `kh check`
    // and `kh list` work offline. "description" goes last on purpose: the small
    // JSON reader finds keys by searching, and free text must not shadow a key.
    char meta_path[2048];
    snprintf(meta_path, sizeof(meta_path), "%s/package.json", pkg_dir);
    FILE *mf = fopen(meta_path, "w");
    if (mf) {
        fprintf(mf, "{\n  \"name\": \"%s\",\n  \"version\": \"%s\",\n  \"files\": [", name, version ? version : "?");
        for (int i = 0; i < nfiles; i++) fprintf(mf, "%s\"%s\"", i ? ", " : "", files[i]);
        fprintf(mf, "],\n  \"dependencies\": [");
        for (int i = 0; i < dep_count; i++) fprintf(mf, "%s\"%s\"", i ? ", " : "", deps[i]);
        fprintf(mf, "]");
        if (verify) {
            fprintf(mf, ",\n  \"sha256\": {");
            for (int i = 0; i < nfiles; i++) fprintf(mf, "%s\n    \"%s\": \"%s\"", i ? "," : "", files[i], hashes[i]);
            fprintf(mf, "\n  }");
        }
        fprintf(mf, ",\n  \"description\": \"%s\"\n}\n", desc ? desc : "");
        fclose(mf);
    }

    if (verify) {
        snprintf(msg, sizeof(msg), "Verified %d file%s (SHA-256).", nfiles, nfiles == 1 ? "" : "s");
        print_info_line(msg);
    }
    snprintf(msg, sizeof(msg), "Installed %s@%s  →  use: import \"%s\"", name, version ? version : "?", name);
    print_ok(msg);
    rc = 0;

done:
    if (stage[0]) staging_cleanup(stage, files, staged);
    for (int i = 0; i < nfiles; i++) { if (files) free(files[i]); if (urls) free(urls[i]); }
    for (int i = 0; i < extra_count; i++) free(extra_files ? extra_files[i] : NULL);
    for (int i = 0; i < dep_count; i++) free(deps ? deps[i] : NULL);
    free(files); free(urls); free(hashes); free(extra_files); free(deps);
    free(pkg); free(version); free(url); free(desc);
    g_depth--;
    return rc;
}

// ---------------------------------------------------------------------------
// Installed-package helpers (used by list / show / outdated / update / check /
// freeze / remove). Everything here reads only ~/.kosh/packages.
// ---------------------------------------------------------------------------
static void free_strs(char **a, int n) {
    if (!a) return;
    for (int i = 0; i < n; i++) free(a[i]);
    free(a);
}

static char *read_text_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) { fclose(f); return NULL; }
    size_t n;
    while ((n = fread(buf + len, 1, cap - len - 1, f)) > 0) {
        len += n;
        if (len + 1 >= cap) {
            cap *= 2;
            char *t = realloc(buf, cap);
            if (!t) { free(buf); fclose(f); return NULL; }
            buf = t;
        }
    }
    fclose(f);
    buf[len] = '\0';
    return buf;
}

static int file_readable(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

static int path_exists(const char *path) {
#ifdef _WIN32
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(path, &st) == 0;
#endif
}

static int cmp_strp(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

// Names of everything inside `dir` (no "." / ".."). Heap array of heap strings.
static char **list_dir_names(const char *dir, int *count) {
    *count = 0;
    char **out = NULL; int n = 0, cap = 0;
#ifdef _WIN32
    char search[1100];
    snprintf(search, sizeof(search), "%s/*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(search, &fd);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    do {
        const char *nm = fd.cFileName;
#else
    DIR *d = opendir(dir);
    if (!d) return NULL;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *nm = de->d_name;
#endif
        if (strcmp(nm, ".") != 0 && strcmp(nm, "..") != 0) {
            if (n == cap) {
                cap = cap ? cap * 2 : 16;
                char **t = realloc(out, sizeof(char *) * (size_t)cap);
                if (!t) break;
                out = t;
            }
            out[n] = malloc(strlen(nm) + 1);
            if (!out[n]) break;
            strcpy(out[n], nm);
            n++;
        }
#ifdef _WIN32
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    }
    closedir(d);
#endif
    *count = n;
    return out;
}

// Sorted names of every valid install (a directory holding <name>/<name>.kh).
static char **installed_names(int *count) {
    char *pd = get_packages_dir();
    int n = 0;
    char **all = list_dir_names(pd, &n);
    free(pd);
    char **out = calloc((size_t)n + 1, sizeof(char *));
    int c = 0;
    for (int i = 0; i < n; i++) {
        if (out && valid_pkg_name(all[i]) && is_installed(all[i])) out[c++] = all[i];
        else free(all[i]);
    }
    free(all);
    if (out) qsort(out, (size_t)c, sizeof(char *), cmp_strp);
    *count = out ? c : 0;
    return out;
}

static char *load_meta(const char *name) {
    char *pd = get_packages_dir();
    char path[2048];
    snprintf(path, sizeof(path), "%s/%s/package.json", pd, name);
    free(pd);
    return read_text_file(path);
}

// Installed version, or a copy of "?" when it cannot be read. Caller frees.
static char *installed_version(const char *name) {
    char *meta = load_meta(name);
    char *v = meta ? json_get_string(meta, "version") : NULL;
    free(meta);
    if (!v) { v = malloc(2); if (v) strcpy(v, "?"); }
    return v;
}

static int meta_requires(const char *meta, const char *dep) {
    int n = 0;
    char **d = json_get_string_array(meta, "dependencies", &n);
    int hit = 0;
    for (int i = 0; i < n; i++) if (strcmp(d[i], dep) == 0) hit = 1;
    free_strs(d, n);
    return hit;
}

// Plain dotted numbers (1.2.3) compare numerically; anything else just has to differ.
static int ver_numeric(const char *v) {
    if (!v || !*v) return 0;
    for (; *v; v++) if (!(isdigit((unsigned char)*v) || *v == '.')) return 0;
    return 1;
}
static int ver_cmp(const char *a, const char *b) {
    while (*a || *b) {
        long x = 0, y = 0;
        while (isdigit((unsigned char)*a)) { if (x < 1000000000L) x = x * 10 + (*a - '0'); a++; }
        while (isdigit((unsigned char)*b)) { if (y < 1000000000L) y = y * 10 + (*b - '0'); b++; }
        if (x != y) return x < y ? -1 : 1;
        if (*a == '.') a++;
        if (*b == '.') b++;
    }
    return 0;
}
static int is_newer(const char *latest, const char *installed) {
    if (ver_numeric(latest) && ver_numeric(installed)) return ver_cmp(latest, installed) > 0;
    return strcmp(latest, installed) != 0;
}

// Delete a file or a whole directory tree. Symlinks are removed, never followed.
static int remove_tree(const char *path) {
#ifdef _WIN32
    DWORD at = GetFileAttributesA(path);
    if (at == INVALID_FILE_ATTRIBUTES) return -1;
    if (at & FILE_ATTRIBUTE_DIRECTORY) {
        if (!(at & FILE_ATTRIBUTE_REPARSE_POINT)) {
            int n = 0;
            char **names = list_dir_names(path, &n);
            for (int i = 0; i < n; i++) {
                char child[2048];
                snprintf(child, sizeof(child), "%s/%s", path, names[i]);
                remove_tree(child);
            }
            free_strs(names, n);
        }
        return RemoveDirectoryA(path) ? 0 : -1;
    }
    SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
    return DeleteFileA(path) ? 0 : -1;
#else
    struct stat st;
    if (lstat(path, &st) != 0) return -1;
    if (S_ISDIR(st.st_mode)) {
        int n = 0;
        char **names = list_dir_names(path, &n);
        for (int i = 0; i < n; i++) {
            char child[2048];
            snprintf(child, sizeof(child), "%s/%s", path, names[i]);
            remove_tree(child);
        }
        free_strs(names, n);
        return rmdir(path);
    }
    return unlink(path);
#endif
}

static int ci_contains(const char *hay, const char *needle) {
    if (!needle || !*needle) return 1;
    size_t nl = strlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < nl && hay[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nl) return 1;
    }
    return 0;
}

static char *fetch_registry_or_err(void) {
    char *registry = fetch_url(g_registry_url);
    if (!registry || strlen(registry) < 10) {
        print_err("Could not reach registry.");
        free(registry);
        return NULL;
    }
    return registry;
}

// ---------------------------------------------------------------------------
// CMD: kh remove <name>   (alias: uninstall)
// Deletes the whole package folder, including sibling files (older versions
// of kh left those behind and could not remove the folder).
// ---------------------------------------------------------------------------
static int cmd_remove(const char *name) {
    char *pkgs_dir = get_packages_dir();
    char pkg_dir[1024];
    snprintf(pkg_dir, sizeof(pkg_dir), "%s/%s", pkgs_dir, name);
    free(pkgs_dir);

    if (!path_exists(pkg_dir)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Package \"%s\" is not installed.", name);
        print_err(msg);
        return 1;
    }

    // Warn (pip-style) if something installed still needs it.
    int n = 0;
    char **names = installed_names(&n);
    for (int i = 0; i < n; i++) {
        if (strcmp(names[i], name) == 0) continue;
        char *meta = load_meta(names[i]);
        if (meta && meta_requires(meta, name)) {
            printf("\033[1;33m!\033[0m %s depends on %s and will not import until you reinstall it.\n", names[i], name);
        }
        free(meta);
    }
    free_strs(names, n);

    if (remove_tree(pkg_dir) != 0 && path_exists(pkg_dir)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Could not fully remove \"%s\" (a file may be in use).", name);
        print_err(msg);
        return 1;
    }
    char msg[256];
    snprintf(msg, sizeof(msg), "Removed package \"%s\".", name);
    print_ok(msg);
    return 0;
}

// ---------------------------------------------------------------------------
// CMD: kh list [--outdated]      (also: kh installed, kh outdated)
// Like `pip list`: what is installed on this computer. Works offline.
// With --outdated it asks the registry and shows only packages with a newer version.
// ---------------------------------------------------------------------------
static int cmd_list(int outdated_only) {
    int n = 0;
    char **names = installed_names(&n);
    char *registry = NULL;

    if (outdated_only) {
        print_info_line("Checking the registry for newer versions...");
        registry = fetch_registry_or_err();
        if (!registry) { free_strs(names, n); return 1; }
    }

    if (n == 0) {
        printf("No packages installed yet.\n");
        printf("Run \033[1mkh install <name>\033[0m to install one, or \033[1mkh search\033[0m to browse.\n");
        free(registry); free_strs(names, n);
        return 0;
    }

    if (outdated_only) {
        print_header("\nOutdated Kosh packages");
        printf("%-18s %-10s %s\n", "NAME", "INSTALLED", "LATEST");
        printf("%-18s %-10s %s\n", "──────────────────", "─────────", "──────");
    } else {
        print_header("\nInstalled Kosh packages");
        printf("%-18s %s\n", "NAME", "VERSION");
        printf("%-18s %s\n", "──────────────────", "───────");
    }

    int shown = 0;
    for (int i = 0; i < n; i++) {
        char *ver = installed_version(names[i]);
        if (outdated_only) {
            char *entry = find_package_in_registry(registry, names[i]);
            char *latest = entry ? json_get_string(entry, "version") : NULL;
            if (latest && ver && is_newer(latest, ver)) {
                printf("\033[1;32m%-18s\033[0m %-10s \033[1;33m%s\033[0m\n", names[i], ver, latest);
                shown++;
            }
            free(entry); free(latest);
        } else {
            printf("\033[1;32m%-18s\033[0m %s\n", names[i], ver ? ver : "?");
            shown++;
        }
        free(ver);
    }

    if (outdated_only) {
        if (shown == 0) printf("Everything is up to date (%d package%s checked).\n", n, n == 1 ? "" : "s");
        else printf("\n%d package(s) can be updated. Run \033[1mkh update --all\033[0m.\n", shown);
    } else {
        printf("\n%d package(s) installed.  In your .kh files: import \"<name>\"\n", shown);
    }
    free(registry);
    free_strs(names, n);
    return 0;
}

// ---------------------------------------------------------------------------
// CMD: kh freeze      name==version for every installed package (scriptable)
// ---------------------------------------------------------------------------
static int cmd_freeze(void) {
    int n = 0;
    char **names = installed_names(&n);
    for (int i = 0; i < n; i++) {
        char *ver = installed_version(names[i]);
        printf("%s==%s\n", names[i], ver ? ver : "?");
        free(ver);
    }
    free_strs(names, n);
    return 0;
}

// ---------------------------------------------------------------------------
// CMD: kh search [term]     browse the registry (what `kh list` used to do)
// ---------------------------------------------------------------------------
static int cmd_search(const char *term) {
    print_info_line("Fetching registry...");
    char *registry = fetch_registry_or_err();
    if (!registry) return 1;

    print_header(term ? "\nMatching packages in the Kosh registry" : "\nKosh Package Registry");
    printf("%-16s %-8s  %s\n", "NAME", "VERSION", "DESCRIPTION");
    printf("%-16s %-8s  %s\n", "────────────────", "───────", "───────────────────────────────────");

    const char *p = registry;
    int count = 0;
    while ((p = strstr(p, "\"name\"")) != NULL) {
        char *name    = json_get_string(p, "name");
        char *version = json_get_string(p, "version");
        char *desc    = json_get_string(p, "description");
        if (name && version && desc && (ci_contains(name, term) || ci_contains(desc, term))) {
            size_t cut = strlen(desc);
            if (cut > 52) {
                cut = 52;
                while (cut > 0 && (((unsigned char)desc[cut]) & 0xC0) == 0x80) cut--;   // don't split UTF-8
                desc[cut] = '\0';
            }
            int have = valid_pkg_name(name) && is_installed(name);
            printf("\033[1;32m%-16s\033[0m %-8s  %s%s\n", name, version, desc, have ? "  \033[36m[installed]\033[0m" : "");
            count++;
        }
        free(name); free(version); free(desc);
        p++;
    }
    free(registry);

    if (count == 0) printf("No packages match.\n");
    else printf("\n%d package(s) %s.  Run \033[1mkh install <name>\033[0m to install.\n", count, term ? "found" : "available");
    return 0;
}

// ---------------------------------------------------------------------------
// CMD: kh show <name>     details of an INSTALLED package (like `pip show`)
// ---------------------------------------------------------------------------
static int cmd_show(const char *name) {
    if (!is_installed(name)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Package \"%s\" is not installed. (kh info %s shows the registry entry.)", name, name);
        print_err(msg);
        return 1;
    }
    char *pd = get_packages_dir();
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/%s", pd, name);
    free(pd);

    char *meta = load_meta(name);
    char *ver  = meta ? json_get_string(meta, "version") : NULL;
    char *desc = meta ? json_get_string(meta, "description") : NULL;
    int nfiles = 0, ndeps = 0;
    char **files = meta ? json_get_string_array(meta, "files", &nfiles) : NULL;
    char **deps  = meta ? json_get_string_array(meta, "dependencies", &ndeps) : NULL;
    char probe[65];
    int has_sums = meta && json_get_hash(meta, nfiles ? files[0] : name, probe);

    print_header("");
    printf("  \033[1mName:\033[0m        %s\n", name);
    printf("  \033[1mVersion:\033[0m     %s\n", ver ? ver : "?");
    printf("  \033[1mSummary:\033[0m     %s\n", desc && *desc ? desc : "?");
    printf("  \033[1mLocation:\033[0m    %s\n", dir);
    printf("  \033[1mChecksums:\033[0m   %s\n", has_sums ? "recorded at install (kh check re-verifies them)" : "not recorded (installed by an older kh, or with --no-verify)");

    printf("  \033[1mRequires:\033[0m    ");
    if (ndeps == 0) printf("(none)");
    for (int i = 0; i < ndeps; i++) printf("%s%s", i ? ", " : "", deps[i]);
    printf("\n");

    printf("  \033[1mRequired-by:\033[0m ");
    int n = 0, by = 0;
    char **names = installed_names(&n);
    for (int i = 0; i < n; i++) {
        if (strcmp(names[i], name) == 0) continue;
        char *m = load_meta(names[i]);
        if (m && meta_requires(m, name)) { printf("%s%s", by ? ", " : "", names[i]); by++; }
        free(m);
    }
    if (!by) printf("(none)");
    printf("\n");
    free_strs(names, n);

    if (nfiles > 0) {
        printf("  \033[1mFiles (%d):\033[0m\n", nfiles);
        for (int i = 0; i < nfiles; i++) printf("    %s\n", files[i]);
    } else {
        printf("  \033[1mFiles:\033[0m       %s.kh\n", name);
    }
    printf("\n  Use it with: \033[1mimport \"%s\"\033[0m\n", name);

    free(meta); free(ver); free(desc);
    free_strs(files, nfiles); free_strs(deps, ndeps);
    return 0;
}

// ---------------------------------------------------------------------------
// CMD: kh check [name]    like `pip check`, offline. Reports, for every
// installed package: missing files, files whose SHA-256 no longer matches what
// was verified at install time, and dependencies that are not installed.
// Exit code 1 when anything is wrong.
// ---------------------------------------------------------------------------
static int check_one(const char *name) {
    int problems = 0;
    char *pd = get_packages_dir();
    char *meta = load_meta(name);
    if (!meta) {
        printf("\033[1;31m✗\033[0m %s: package.json is missing or unreadable (reinstall: kh install %s)\n", name, name);
        free(pd);
        return 1;
    }
    int nfiles = 0, ndeps = 0;
    char **files = json_get_string_array(meta, "files", &nfiles);
    char **deps  = json_get_string_array(meta, "dependencies", &ndeps);

    char mainname[160];
    snprintf(mainname, sizeof(mainname), "%s.kh", name);
    int total = nfiles > 0 ? nfiles : 1;
    for (int i = 0; i < total; i++) {
        const char *rel = nfiles > 0 ? files[i] : mainname;
        if (!valid_rel_file(rel)) {
            printf("\033[1;31m✗\033[0m %s: package.json lists an unsafe file name\n", name);
            problems++;
            continue;
        }
        char path[2048];
        snprintf(path, sizeof(path), "%s/%s/%s", pd, name, rel);
        if (!file_readable(path)) {
            printf("\033[1;31m✗\033[0m %s: missing file %s\n", name, rel);
            problems++;
            continue;
        }
        char want[65], got[65];
        if (json_get_hash(meta, rel, want)) {
            if (!hash_file_hex(path, got)) {
                printf("\033[1;31m✗\033[0m %s: cannot read %s to verify it\n", name, rel);
                problems++;
            } else if (strcmp(want, got) != 0) {
                printf("\033[1;31m✗\033[0m %s: %s was modified after install (SHA-256 differs)\n", name, rel);
                problems++;
            }
        }
    }
    for (int i = 0; i < ndeps; i++) {
        if (!valid_pkg_name(deps[i]) || !is_installed(deps[i])) {
            printf("\033[1;31m✗\033[0m %s requires %s, which is not installed\n", name, deps[i]);
            problems++;
        }
    }
    free(pd); free(meta);
    free_strs(files, nfiles); free_strs(deps, ndeps);
    return problems;
}

static int cmd_check(const char *only) {
    int problems = 0, checked = 0;
    if (only) {
        if (!is_installed(only)) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Package \"%s\" is not installed.", only);
            print_err(msg);
            return 1;
        }
        problems = check_one(only);
        checked = 1;
    } else {
        int n = 0;
        char **names = installed_names(&n);
        for (int i = 0; i < n; i++) { problems += check_one(names[i]); checked++; }
        free_strs(names, n);
    }
    if (problems == 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "No broken packages found (%d checked).", checked);
        print_ok(msg);
        return 0;
    }
    printf("\n%d problem(s) found. Reinstall with \033[1mkh install <name>\033[0m.\n", problems);
    return 1;
}

// ---------------------------------------------------------------------------
// CMD: kh update <name> | kh update --all [--no-verify]
// Reinstalls (verified, atomic) every named package whose registry version is newer.
// ---------------------------------------------------------------------------
static int cmd_update(const char *only, int verify) {
    print_info_line("Checking the registry for newer versions...");
    char *registry = fetch_registry_or_err();
    if (!registry) return 1;

    int n = 0;
    char **names = NULL;
    if (only) {
        if (!is_installed(only)) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Package \"%s\" is not installed. Use kh install %s.", only, only);
            print_err(msg);
            free(registry);
            return 1;
        }
        names = calloc(1, sizeof(char *));
        if (names) { names[0] = malloc(strlen(only) + 1); if (names[0]) { strcpy(names[0], only); n = 1; } }
    } else {
        names = installed_names(&n);
    }

    int updated = 0, failed = 0, current = 0;
    for (int i = 0; i < n; i++) {
        char *ver = installed_version(names[i]);
        char *entry = find_package_in_registry(registry, names[i]);
        char *latest = entry ? json_get_string(entry, "version") : NULL;
        if (!latest) {
            printf("\033[1;33m!\033[0m %s is not in the registry; skipped.\n", names[i]);
        } else if (ver && is_newer(latest, ver)) {
            char m[300];
            snprintf(m, sizeof(m), "Updating %s %s -> %s", names[i], ver, latest);
            print_info_line(m);
            if (cmd_install(names[i], verify) == 0) updated++; else failed++;
        } else {
            current++;
        }
        free(ver); free(entry); free(latest);
    }
    free(registry);
    free_strs(names, n);

    printf("\n%d updated, %d already up to date", updated, current);
    if (failed) printf(", \033[1;31m%d failed\033[0m (nothing was changed for those)", failed);
    printf(".\n");
    return failed ? 1 : 0;
}

// ---------------------------------------------------------------------------
// CMD: kh info <name>
// ---------------------------------------------------------------------------
static int cmd_info(const char *name) {
    print_info_line("Fetching registry...");
    char *registry = fetch_url(g_registry_url);
    if (!registry || strlen(registry) < 10) {
        print_err("Could not reach registry.");
        free(registry);
        return 1;
    }

    char *pkg = find_package_in_registry(registry, name);
    free(registry);

    if (!pkg) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Package \"%s\" not found.", name);
        print_err(msg);
        return 1;
    }

    char *version   = json_get_string(pkg, "version");
    char *desc      = json_get_string(pkg, "description");
    char *author    = json_get_string(pkg, "author");
    char *developer = json_get_string(pkg, "developer");
    char *url       = json_get_string(pkg, "url");
    free(pkg);

    print_header("\nPackage Info");
    printf("  \033[1mName:\033[0m        %s\n", name);
    printf("  \033[1mVersion:\033[0m     %s\n", version   ? version   : "?");
    printf("  \033[1mDeveloper:\033[0m   %s\n", developer ? developer : "?");
    printf("  \033[1mAuthor:\033[0m      %s\n", author    ? author    : "?");
    printf("  \033[1mDescription:\033[0m %s\n", desc      ? desc      : "?");
    printf("  \033[1mURL:\033[0m         %s\n", url       ? url       : "?");
    if (is_installed(name)) {
        char *iv = installed_version(name);
        printf("  \033[1mInstalled:\033[0m   %s%s\n", iv ? iv : "?",
               (iv && version && is_newer(version, iv)) ? "  (update available: kh update " : "");
        if (iv && version && is_newer(version, iv)) printf("%s)\n", name);
        free(iv);
    } else {
        printf("  \033[1mInstalled:\033[0m   no\n");
    }
    printf("\n  Install with: \033[1mkh install %s\033[0m\n", name);

    free(version); free(desc); free(author); free(developer); free(url);
    return 0;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char *argv[]) {
    enable_ansi();
    if (argc < 2) {
        print_header("kh — Kosh Package Manager");
        printf("\nUsage:\n");
        printf("  kh install <name>    Install a package and its dependencies (every file is checked\n");
        printf("                       against the registry's SHA-256; --no-verify skips the check)\n");
        printf("  kh remove  <name>    Remove a package (alias: uninstall)\n");
        printf("  kh list              List installed packages and versions (offline)\n");
        printf("  kh list --outdated   Only packages with a newer version in the registry (alias: kh outdated)\n");
        printf("  kh show <name>       Details of an installed package: files, requires, required-by\n");
        printf("  kh update <name>     Update one installed package   (kh update --all: update everything)\n");
        printf("  kh check [name]      Find missing/modified files and missing dependencies (offline)\n");
        printf("  kh freeze            Print installed packages as name==version\n");
        printf("  kh search [term]     Browse or search the registry\n");
        printf("  kh info <name>       Show a package's registry entry\n");
        printf("  kh sha256 <file>     Print a file's SHA-256\n");
        printf("\nExample:\n");
        printf("  kh install math\n");
        printf("  kh install strings\n");
        printf("  kh install colors\n");
        return 0;
    }

    const char *cmd = argv[1];

    // Optional mirror / test registry (https:// or file:// only).
    const char *env_reg = getenv("KH_REGISTRY_URL");
    const char *env_raw = getenv("KH_RAW_BASE");
    if (env_reg && *env_reg) {
        if (!url_scheme_ok(env_reg)) { print_err("KH_REGISTRY_URL must start with https:// or file://"); return 1; }
        g_registry_url = env_reg;
    }
    if (env_raw && *env_raw) {
        if (!url_scheme_ok(env_raw)) { print_err("KH_RAW_BASE must start with https:// or file://"); return 1; }
        g_raw_base = env_raw;
    }

    if (strcmp(cmd, "sha256") == 0) {
        if (argc < 3) { print_err("Usage: kh sha256 <file>"); return 1; }
        char hex[65];
        if (!hash_file_hex(argv[2], hex)) { print_err("Cannot read that file."); return 1; }
        printf("%s  %s\n", hex, argv[2]);
        return 0;
    }
    if (strcmp(cmd, "install") == 0) {
        if (argc < 3) { print_err("Usage: kh install <name> [--no-verify]"); return 1; }
        if (!valid_pkg_name(argv[2])) { print_err("Invalid package name (allowed: letters, digits, _ and -)."); return 1; }
        int verify = 1;
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--no-verify") == 0) verify = 0;
            else { char m[300]; snprintf(m, sizeof(m), "Unknown option \"%s\"", argv[i]); print_err(m); return 1; }
        }
        return cmd_install(argv[2], verify);
    }
    if (strcmp(cmd, "remove") == 0 || strcmp(cmd, "uninstall") == 0) {
        if (argc < 3) { print_err("Usage: kh remove <name>"); return 1; }
        if (!valid_pkg_name(argv[2])) { print_err("Invalid package name (allowed: letters, digits, _ and -)."); return 1; }
        return cmd_remove(argv[2]);
    }
    if (strcmp(cmd, "list") == 0 || strcmp(cmd, "installed") == 0 || strcmp(cmd, "outdated") == 0) {
        int outdated = strcmp(cmd, "outdated") == 0;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--outdated") == 0 || strcmp(argv[i], "-o") == 0) outdated = 1;
            else { char m[300]; snprintf(m, sizeof(m), "Unknown option \"%s\"", argv[i]); print_err(m); return 1; }
        }
        return cmd_list(outdated);
    }
    if (strcmp(cmd, "freeze") == 0) {
        return cmd_freeze();
    }
    if (strcmp(cmd, "search") == 0) {
        if (argc > 3) { print_err("Usage: kh search [term]"); return 1; }
        return cmd_search(argc == 3 ? argv[2] : NULL);
    }
    if (strcmp(cmd, "show") == 0) {
        if (argc < 3) { print_err("Usage: kh show <name>"); return 1; }
        if (!valid_pkg_name(argv[2])) { print_err("Invalid package name (allowed: letters, digits, _ and -)."); return 1; }
        return cmd_show(argv[2]);
    }
    if (strcmp(cmd, "check") == 0) {
        if (argc > 3) { print_err("Usage: kh check [name]"); return 1; }
        if (argc == 3 && !valid_pkg_name(argv[2])) { print_err("Invalid package name (allowed: letters, digits, _ and -)."); return 1; }
        return cmd_check(argc == 3 ? argv[2] : NULL);
    }
    if (strcmp(cmd, "update") == 0 || strcmp(cmd, "upgrade") == 0) {
        const char *target = NULL; int all = 0, verify = 1;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--all") == 0) all = 1;
            else if (strcmp(argv[i], "--no-verify") == 0) verify = 0;
            else if (argv[i][0] != '-' && !target) target = argv[i];
            else { char m[300]; snprintf(m, sizeof(m), "Unknown option \"%s\"", argv[i]); print_err(m); return 1; }
        }
        if (all == (target != NULL)) { print_err("Usage: kh update <name>   or   kh update --all"); return 1; }
        if (target && !valid_pkg_name(target)) { print_err("Invalid package name (allowed: letters, digits, _ and -)."); return 1; }
        return cmd_update(target, verify);
    }
    if (strcmp(cmd, "info") == 0) {
        if (argc < 3) { print_err("Usage: kh info <name>"); return 1; }
        if (!valid_pkg_name(argv[2])) { print_err("Invalid package name (allowed: letters, digits, _ and -)."); return 1; }
        return cmd_info(argv[2]);
    }

    char msg[256];
    snprintf(msg, sizeof(msg), "Unknown command: \"%s\"", cmd);
    print_err(msg);
    printf("Run \033[1mkh\033[0m with no arguments to see usage.\n");
    return 1;
}