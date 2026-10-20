// ===========================================================================
// kh — Khan Package Manager
// ===========================================================================

// Windows headers FIRST to avoid TokenType conflict with our token.h
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <shlobj.h>
#  include <direct.h>
#  define MKDIR(p) _mkdir(p)
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
static void enable_ansi(void) {}
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
#define REGISTRY_URL \
  "https://raw.githubusercontent.com/khandev1211-cpu/Khan/main/packages/registry.json"

#define RAW_BASE \
  "https://raw.githubusercontent.com/khandev1211-cpu/Khan/main/packages"

// ---------------------------------------------------------------------------
// Get the Khan packages directory: ~/.khan/packages/
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
    snprintf(dir, len, "%s\\.khan\\packages", base);
#else
    base = getenv("HOME");
    if (!base) base = "/tmp";
    size_t len = strlen(base) + 32;
    char *dir = malloc(len);
    snprintf(dir, len, "%s/.khan/packages", base);
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
// Appends a cache-busting query parameter (current unix time) to a URL.
// raw.githubusercontent.com is served through a CDN that can hold an old
// response for a while after a push — a distinct query string forces a
// fresh edge fetch regardless of whatever TTL that cache would otherwise
// honor, so `kh install`/`kh update`/`kh list` never silently serve a
// stale registry.json or package file just because a push happened
// recently. Caller must free the result.
// ---------------------------------------------------------------------------
static char *cache_busted_url(const char *url) {
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
    snprintf(cmd, sizeof(cmd),
             "curl -s -L --max-time 30 -H \"Cache-Control: no-cache\" -H \"Pragma: no-cache\" \"%s\"",
             busted);
    free(busted);

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
    snprintf(cmd, sizeof(cmd),
             "curl -s -L --max-time 30 -H \"Cache-Control: no-cache\" -H \"Pragma: no-cache\" -o \"%s\" \"%s\"",
             filepath, busted);
    free(busted);

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
// CMD: kh install <name>
// ---------------------------------------------------------------------------
static int cmd_install(const char *name) {
    print_info_line("Fetching registry...");

    char *registry = fetch_url(REGISTRY_URL);
    if (!registry || strlen(registry) < 10) {
        print_err("Could not reach registry. Check your internet connection.");
        free(registry);
        return 1;
    }

    char *pkg = find_package_in_registry(registry, name);
    free(registry);

    if (!pkg) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Package \"%s\" not found in registry.", name);
        print_err(msg);
        printf("  Run \033[1mkh list\033[0m to see available packages.\n");
        return 1;
    }

    char *version = json_get_string(pkg, "version");
    char *url     = json_get_string(pkg, "url");
    char *desc    = json_get_string(pkg, "description");
    int extra_count = 0;
    char **extra_files = json_get_string_array(pkg, "files", &extra_count);
    free(pkg);

    if (!url) {
        print_err("Registry entry is missing download URL.");
        free(version); free(desc);
        return 1;
    }

    // Build install path: ~/.khan/packages/<name>/<name>.kh
    char *pkgs_dir = get_packages_dir();
    char pkg_dir[1024], pkg_file[1024];
    snprintf(pkg_dir,  sizeof(pkg_dir),  "%s/%s", pkgs_dir, name);
    snprintf(pkg_file, sizeof(pkg_file), "%s/%s/%s.kh", pkgs_dir, name, name);
    free(pkgs_dir);

    mkdir_p(pkg_dir);

    char msg[512];
    snprintf(msg, sizeof(msg), "Installing %s@%s...",
             name, version ? version : "?");
    print_info_line(msg);

    if (!download_to_file(url, pkg_file)) {
        print_err("Download failed.");
        free(version); free(url); free(desc);
        return 1;
    }

    // Multi-file packages (e.g. webi, split across app.kh/routing.kh/
    // request.kh/...) list their extra files in a "files" array in the
    // registry. Fetch each one from the same package directory on the
    // registry host — without this, a package that imports sibling files
    // internally would install "successfully" but be broken the moment any
    // of its code ran.
    if (extra_count > 0) {
        char progress[128];
        snprintf(progress, sizeof(progress), "Fetching %d additional file(s)...", extra_count);
        print_info_line(progress);
        for (int i = 0; i < extra_count; i++) {
            char file_url[1536], file_path[1536];
            snprintf(file_url, sizeof(file_url), "%s/%s/%s", RAW_BASE, name, extra_files[i]);
            snprintf(file_path, sizeof(file_path), "%s/%s", pkg_dir, extra_files[i]);
            if (!download_to_file(file_url, file_path)) {
                char errmsg[256];
                snprintf(errmsg, sizeof(errmsg), "Failed to download '%s' — package may be incomplete.", extra_files[i]);
                print_err(errmsg);
            }
            free(extra_files[i]);
        }
        free(extra_files);
    }

    // Write a local package.json
    char meta_path[2048];
    snprintf(meta_path, sizeof(meta_path), "%s/package.json", pkg_dir);
    FILE *mf = fopen(meta_path, "w");
    if (mf) {
        fprintf(mf, "{\n  \"name\": \"%s\",\n  \"version\": \"%s\",\n"
                    "  \"description\": \"%s\"\n}\n",
                name,
                version ? version : "?",
                desc    ? desc    : "");
        fclose(mf);
    }

    snprintf(msg, sizeof(msg), "Installed %s@%s  →  use: import \"%s\"",
             name, version ? version : "?", name);
    print_ok(msg);

    free(version); free(url); free(desc);
    return 0;
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

    char *registry = fetch_url(REGISTRY_URL);
    if (!registry || strlen(registry) < 10) {
        print_err("Could not reach registry.");
        free(registry);
        return 1;
    }

    print_header("\nKhan Package Registry");
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

    print_header("\nInstalled Khan Packages");
    printf("Location: %s\n\n", pkgs_dir);

#ifdef _WIN32
    // Use FindFirstFile / FindNextFile on Windows
    char search[1024];
    snprintf(search, sizeof(search), "%s\\*", pkgs_dir);
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
    char *registry = fetch_url(REGISTRY_URL);
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
        print_header("kh — Khan Package Manager");
        printf("\nUsage:\n");
        printf("  kh install <name>    Install a package\n");
        printf("  kh remove  <name>    Remove a package\n");
        printf("  kh list              List all available packages\n");
        printf("  kh installed         Show installed packages\n");
        printf("  kh info <name>       Show package details\n");
        printf("\nExample:\n");
        printf("  kh install math\n");
        printf("  kh install strings\n");
        printf("  kh install colors\n");
        return 0;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "install") == 0) {
        if (argc < 3) { print_err("Usage: kh install <name>"); return 1; }
        return cmd_install(argv[2]);
    }
    if (strcmp(cmd, "remove") == 0) {
        if (argc < 3) { print_err("Usage: kh remove <name>"); return 1; }
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
        return cmd_info(argv[2]);
    }

    char msg[256];
    snprintf(msg, sizeof(msg), "Unknown command: \"%s\"", cmd);
    print_err(msg);
    printf("Run \033[1mkh\033[0m with no arguments to see usage.\n");
    return 1;
}