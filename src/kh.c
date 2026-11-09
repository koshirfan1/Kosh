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

    // Write a local package.json
    char meta_path[2048];
    snprintf(meta_path, sizeof(meta_path), "%s/package.json", pkg_dir);
    FILE *mf = fopen(meta_path, "w");
    if (mf) {
        fprintf(mf, "{\n  \"name\": \"%s\",\n  \"version\": \"%s\",\n"
                    "  \"description\": \"%s\"\n}\n",
                name, version ? version : "?", desc ? desc : "");
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
// CMD: kh remove <name>
// ---------------------------------------------------------------------------
static int cmd_remove(const char *name) {
    char *pkgs_dir = get_packages_dir();
    char pkg_file[1024], meta_file[1024], pkg_dir[1024];
    snprintf(pkg_file,  sizeof(pkg_file),  "%s/%s/%s.kh",      pkgs_dir, name, name);
    snprintf(meta_file, sizeof(meta_file), "%s/%s/package.json", pkgs_dir, name);
    snprintf(pkg_dir,   sizeof(pkg_dir),   "%s/%s",             pkgs_dir, name);
    free(pkgs_dir);

    int removed = 0;
    if (remove(pkg_file)  == 0) removed = 1;
    if (remove(meta_file) == 0) removed = 1;
    if (removed) {
#ifdef _WIN32
        _rmdir(pkg_dir);
#else
        rmdir(pkg_dir);
#endif
        char msg[256];
        snprintf(msg, sizeof(msg), "Removed package \"%s\".", name);
        print_ok(msg);
        return 0;
    } else {
        char msg[256];
        snprintf(msg, sizeof(msg), "Package \"%s\" is not installed.", name);
        print_err(msg);
        return 1;
    }
}

// ---------------------------------------------------------------------------
// CMD: kh list
// ---------------------------------------------------------------------------
static int cmd_list(void) {
    print_info_line("Fetching registry...");

    char *registry = fetch_url(g_registry_url);
    if (!registry || strlen(registry) < 10) {
        print_err("Could not reach registry.");
        free(registry);
        return 1;
    }

    print_header("\nKosh Package Registry");
    printf("%-16s %-8s  %s\n", "NAME", "VERSION", "DESCRIPTION");
    printf("%-16s %-8s  %s\n", "────────────────", "───────", "───────────────────────────────────");

    const char *p = registry;
    int count = 0;
    while ((p = strstr(p, "\"name\"")) != NULL) {
        char *name    = json_get_string(p, "name");
        char *version = json_get_string(p, "version");
        char *desc    = json_get_string(p, "description");

        if (name && version && desc) {
            // Truncate description to 52 chars
            if (strlen(desc) > 52) desc[52] = '\0';
            printf("\033[1;32m%-16s\033[0m %-8s  %s\n", name, version, desc);
            count++;
        }

        free(name); free(version); free(desc);
        p++;
    }
    free(registry);

    printf("\n%d package(s) available.\n", count);
    printf("Run \033[1mkh install <name>\033[0m to install.\n");
    return 0;
}

// ---------------------------------------------------------------------------
// CMD: kh installed
// ---------------------------------------------------------------------------
static int cmd_installed(void) {
    char *pkgs_dir = get_packages_dir();

    print_header("\nInstalled Kosh Packages");
    printf("Location: %s\n\n", pkgs_dir);

#ifdef _WIN32
    // Use FindFirstFile / FindNextFile on Windows
    char search[1024];
    snprintf(search, sizeof(search), "%s/*", pkgs_dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(search, &fd);
    int count = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (strcmp(fd.cFileName, ".") == 0 ||
                    strcmp(fd.cFileName, "..") == 0) continue;

                // Read version from package.json
                char meta[1024];
                snprintf(meta, sizeof(meta), "%s\\%s\\package.json",
                         pkgs_dir, fd.cFileName);
                char *version = NULL;
                FILE *mf = fopen(meta, "r");
                if (mf) {
                    char buf[512]; size_t n = fread(buf, 1, 511, mf);
                    buf[n] = '\0'; fclose(mf);
                    version = json_get_string(buf, "version");
                }
                printf("\033[1;32m%-16s\033[0m %s\n",
                       fd.cFileName, version ? version : "?");
                free(version);
                count++;
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    // Use popen("ls") to list directories — avoids needing dirent portability issues
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "ls \"%s\" 2>/dev/null", pkgs_dir);
    FILE *fp = popen(cmd, "r");
    int count = 0;
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            // Strip newline
            line[strcspn(line, "\n")] = '\0';
            if (strlen(line) == 0) continue;

            // Read version
            char meta[1024];
            snprintf(meta, sizeof(meta), "%s/%s/package.json", pkgs_dir, line);
            char *version = NULL;
            FILE *mf = fopen(meta, "r");
            if (mf) {
                char buf[512]; size_t n = fread(buf, 1, 511, mf);
                buf[n] = '\0'; fclose(mf);
                version = json_get_string(buf, "version");
            }
            printf("\033[1;32m%-16s\033[0m %s\n", line, version ? version : "?");
            free(version);
            count++;
        }
        pclose(fp);
    }
#endif

    free(pkgs_dir);

    if (count == 0) {
        printf("No packages installed yet.\n");
        printf("Run \033[1mkh install <name>\033[0m to install one.\n");
    } else {
        printf("\n%d package(s) installed.\n", count);
    }
    printf("\nIn your .kh files, use: import \"<name>\"\n");
    return 0;
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
        printf("  kh remove  <name>    Remove a package\n");
        printf("  kh list              List all available packages\n");
        printf("  kh installed         Show installed packages\n");
        printf("  kh info <name>       Show package details\n");
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
    if (strcmp(cmd, "remove") == 0) {
        if (argc < 3) { print_err("Usage: kh remove <name>"); return 1; }
        if (!valid_pkg_name(argv[2])) { print_err("Invalid package name (allowed: letters, digits, _ and -)."); return 1; }
        return cmd_remove(argv[2]);
    }
    if (strcmp(cmd, "list") == 0) {
        return cmd_list();
    }
    if (strcmp(cmd, "installed") == 0) {
        return cmd_installed();
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