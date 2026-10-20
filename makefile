# ─────────────────────────────────────────────────────────────
#  Khan Language — Makefile
#  Builds: khan.exe (The High-Performance Bytecode VM)
# ─────────────────────────────────────────────────────────────

CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -O2 -Isrc \
          -D_POSIX_C_SOURCE=200809L \
          -Wno-implicit-function-declaration \
          -Wno-builtin-declaration-mismatch \
          -Wno-cast-function-type

ifeq ($(OS),Windows_NT)
    # -static (+ the two -static-lib* flags for good measure) links
    # everything statically into khan.exe/kh.exe, including sqlite3 —
    # WITHOUT this, MinGW dynamically links against libsqlite3-0.dll
    # (and libgcc/libwinpthread's own DLLs) from wherever the build
    # machine's MSYS2 install happens to keep them, which is NOT a
    # normal Windows machine's PATH. The result: khan.exe builds and
    # runs fine on the machine that built it, then fails on every
    # OTHER machine with "libsqlite3-0.dll was not found" the instant
    # someone actually installs it — found via a real install (a
    # screenshot of exactly that dialog), not by inspection.
    #
    # VERIFIED (not just asserted) via an actual x86_64-w64-mingw32-gcc
    # cross-compile of this whole codebase + a stub sqlite3, followed by
    # objdump -p on the result: with -static, the ONLY DLLs the binary
    # imports from are ADVAPI32.dll, KERNEL32.dll, msvcrt.dll,
    # WINHTTP.dll, WS2_32.dll — every one of those ships with Windows
    # itself on every machine, nothing MinGW-specific (no
    # libgcc_s_seh-1.dll, no libwinpthread-1.dll) survived in the import
    # table at all. That cross-compile used a stub in place of the real
    # sqlite3 (no MinGW-targeted sqlite3 package was available to test
    # against directly) — if MSYS2's real `mingw-w64-x86_64-sqlite3`
    # package turns out to only ship a dynamic import library and no
    # true static libsqlite3.a in some future version, `-static` here
    # will fail LOUDLY at link time ("cannot find -lsqlite3" or similar),
    # not silently produce another DLL-dependent binary — if that
    # happens, the fallback is bundling libsqlite3-0.dll (found under
    # MSYS2's mingw64\bin) alongside khan.exe/kh.exe in
    # khan-installer.iss's [Files] section instead of relying on static
    # linking for that one library specifically.
    LDFLAGS  = -static -static-libgcc -static-libstdc++ -lm -lwinhttp -lshell32 -lws2_32 -ladvapi32 -lsqlite3
    EXT      = .exe
else
    LDFLAGS  = -lm -lsqlite3
    EXT      =
endif

# All Source Files for the unified High-Performance Khan
SRCS = \
    src/lexer.c         \
    src/parser.c        \
    src/ast.c           \
    src/chunk.c         \
    src/value.c         \
    src/compiler.c      \
    src/vm.c            \
    src/vm_libs.c       \
    src/interpreter.c   \
    src/khan_stdlib.c   \
    src/json_lib.c      \
    src/datetime_lib.c  \
    src/requests_lib.c  \
    src/webi_lib.c      \
    src/sqlite_lib.c    \
    src/vision_lib.c    \
    src/vision_cv.c     \
    src/vision_cascade.c \
    src/tensor_lib.c    \
    src/main.c

KH_SRCS = src/kh.c

# Optional LLM (llama.cpp/GGUF) bridge - opt-in only, like ocr's tesseract
# dependency, so the default build never requires it. Point LLAMA_CPP_DIR at
# a checkout with its static libs already built (cmake -B build && cmake
# --build build --target llama), then: make LLM=1
LLAMA_CPP_DIR ?= third_party/llama.cpp
ifdef LLM
    CFLAGS  += -DLLM_SUPPORT -I$(LLAMA_CPP_DIR)/include -I$(LLAMA_CPP_DIR)/ggml/include
    LDFLAGS += -L$(LLAMA_CPP_DIR)/build/src -L$(LLAMA_CPP_DIR)/build/ggml/src \
               -lllama -lggml -lggml-base -lggml-cpu -lstdc++ -fopenmp
    SRCS    += src/llm_lib.c
endif

.PHONY: all khan kh clean install uninstall

all: khan$(EXT) kh$(EXT)

khan$(EXT): $(SRCS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
	@echo "  Built khan$(EXT) (High-Performance VM Edition)"

kh$(EXT): $(KH_SRCS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
	@echo "  Built kh$(EXT) (Package Manager)"

clean:
ifeq ($(OS),Windows_NT)
	-del /Q /F src\*.o khan.exe kh.exe 2>nul
else
	rm -f src/*.o khan kh
endif

# Installs the two built binaries somewhere on PATH. Unix-only (Windows
# users: copy khan.exe/kh.exe wherever you like and add that folder to
# PATH yourself — no installer for that platform yet). Defaults to
# ~/.khan/bin specifically so this never needs sudo/root: a curl-piped
# install script (see install.sh) running as a normal user should
# never need to ask for elevated permissions to put a couple of
# binaries somewhere. Override with `make install PREFIX=/usr/local`
# if you'd rather have it on the system-wide PATH already.
PREFIX ?= $(HOME)/.khan
BINDIR  = $(PREFIX)/bin

install: khan kh
ifeq ($(OS),Windows_NT)
	@echo "make install isn't wired up for Windows via this makefile directly --"
	@echo "use install.ps1 instead, which handles copying khan.exe/kh.exe AND"
	@echo "persisting PATH the way Python/Node's Windows installers do:"
	@echo ""
	@echo "    powershell -ExecutionPolicy Bypass -File install.ps1"
	@echo ""
	@echo "(or: irm https://raw.githubusercontent.com/khandev1211-cpu/Khan/main/install.ps1 | iex)"
	@exit 1
else
	mkdir -p "$(BINDIR)"
	cp khan "$(BINDIR)/khan"
	cp kh "$(BINDIR)/kh"
	@echo ""
	@echo "  Installed khan and kh to $(BINDIR)"
	@echo ""
	@if echo ":$$PATH:" | grep -q ":$(BINDIR):"; then \
		echo "  $(BINDIR) is already on your PATH — you're done. Try: khan --version"; \
	else \
		echo "  $(BINDIR) is NOT on your PATH yet. Add this to your shell's"; \
		echo "  rc file (~/.bashrc, ~/.zshrc, etc.) and restart your shell:"; \
		echo ""; \
		echo "      export PATH=\"$(BINDIR):\$$PATH\""; \
		echo ""; \
		echo "  (install.sh does this step for you automatically — this raw"; \
		echo "  \`make install\` doesn't touch your shell config on its own.)"; \
	fi
endif

uninstall:
ifeq ($(OS),Windows_NT)
	@echo "No makefile-driven uninstall on Windows -- delete the folder"
	@echo "install.ps1 installed to (default: %USERPROFILE%\.khan) and remove"
	@echo "it from your User PATH via System Properties, or:"
	@echo "    [Environment]::SetEnvironmentVariable('Path', (([Environment]::GetEnvironmentVariable('Path','User') -split ';') -notmatch '\.khan\\bin' -join ';'), 'User')"
else
	rm -f "$(BINDIR)/khan" "$(BINDIR)/kh"
	@echo "Removed khan and kh from $(BINDIR)."
	@echo "(Any PATH entry added to your shell rc file is left in place —"
	@echo " remove the \"export PATH=...$(PREFIX)...\" line yourself if you"
	@echo " added it and don't want it anymore.)"
endif
