# Quartix - cross-build for Windows XP x64 (NT 5.2) with mingw-w64 (msvcrt runtime).
ifeq ($(origin CC),default)
CC      = x86_64-w64-mingw32-gcc
endif
WINE    ?= wine
PYTHON  ?= python3

CFLAGS  += -std=c99 -O2 -Wall -Wextra -Werror -municode \
           -D_WIN32_WINNT=0x0502 -DWINVER=0x0502 -D__USE_MINGW_ANSI_STDIO=1
# Mark the image as runnable on NT 5.2; newer linkers default to 6.0, which XP refuses.
LDFLAGS += -municode -static-libgcc \
           -Wl,--major-os-version,5,--minor-os-version,2 \
           -Wl,--major-subsystem-version,5,--minor-subsystem-version,2
LDLIBS  += -lshell32

# Ed25519 public key (64 hex chars) that update signatures must verify against.
# Without one the build refuses all updates. See tools/sign_update.py.
UPDATE_PUBKEY ?= $(shell cat keys/update_pubkey.txt 2>/dev/null)

SRC = src/main.c src/paths.c src/apk.c src/sha256.c src/update.c src/log.c
MONO = third_party/monocypher/monocypher.c third_party/monocypher/monocypher-ed25519.c
OUT ?= build/Quartix.exe

all: $(OUT)

$(OUT): $(SRC) $(MONO) src/quartix.h FORCE
	@mkdir -p $(dir $@)
	$(CC) -std=c99 -O2 -c third_party/monocypher/monocypher.c -o $(@:.exe=)-mono.o
	$(CC) -std=c99 -O2 -c third_party/monocypher/monocypher-ed25519.c -o $(@:.exe=)-mono-ed.o
	$(CC) $(CFLAGS) -DQX_UPDATE_PUBKEY='"$(UPDATE_PUBKEY)"' $(SRC) $(@:.exe=)-mono.o $(@:.exe=)-mono-ed.o -o $@ $(LDFLAGS) $(LDLIBS)

check: $(OUT)
	$(PYTHON) tools/check_xp_pe.py $(OUT)

test: check
	WINE="$(WINE)" MAKE="$(MAKE)" $(PYTHON) tests/run_tests.py $(OUT)

clean:
	rm -rf build

FORCE:

.PHONY: all check test clean FORCE
