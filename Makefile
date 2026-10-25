# Quartix - cross-build for Windows Vista/7/8.1 x64 (NT 6.0 floor) with mingw-w64 (msvcrt runtime).
ifeq ($(origin CC),default)
CC      = x86_64-w64-mingw32-gcc
endif
ifeq ($(origin AR),default)
AR      = x86_64-w64-mingw32-ar
endif
WINDRES ?= x86_64-w64-mingw32-windres
WINE    ?= wine
PYTHON  ?= python3

BEARSSL = third_party/bearssl
CFLAGS  += -std=c99 -O2 -Wall -Wextra -Werror -municode -I$(BEARSSL)/inc \
           -D_WIN32_WINNT=0x0600 -DWINVER=0x0600 -D__USE_MINGW_ANSI_STDIO=1
# Mark the image for NT 6.0 (Vista): it loads on Vista, 7 and 8.1, and XP refuses it.
LDFLAGS += -municode -static-libgcc \
           -Wl,--major-os-version,6,--minor-os-version,0 \
           -Wl,--major-subsystem-version,6,--minor-subsystem-version,0
LDLIBS  += -lshell32 -lws2_32 -liphlpapi -ladvapi32 -lcomctl32 -lcomdlg32 -lgdi32 -luser32

# Ed25519 public key (64 hex chars) that update signatures must verify against.
# Without one the build refuses all updates. See tools/sign_update.py.
UPDATE_PUBKEY ?= $(shell cat keys/update_pubkey.txt 2>/dev/null)
# Where `Quartix.exe update` looks for Quartix.exe and Quartix.exe.sig.
UPDATE_BASE ?= https://github.com/joshuapaul992-blip/Quartix/releases/latest/download/
# TESTING=1 lets the binary trust an extra CA from QUARTIX_TEST_CA; tests only.
TESTING ?= 0

SRC = src/main.c src/gui.c src/paths.c src/apk.c src/elf.c src/loader.c src/shim.c src/thread.c src/format.c src/stdio.c src/scan.c src/os.c src/cstd.c src/android.c src/socket.c src/gl.c src/runtime.c src/sha256.c src/update.c src/net.c src/log.c \
      src/gen/trust_anchors.c
MONO = third_party/monocypher/monocypher.c third_party/monocypher/monocypher-ed25519.c
# Only the inflate half of miniz (tinfl) is used, for deflated entries in APKs and bundles.
MINIZ = third_party/miniz/miniz.c
MINIZ_DEFS = -DMINIZ_NO_STDIO -DMINIZ_NO_ARCHIVE_APIS -DMINIZ_NO_TIME -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES
OUT ?= build/Quartix.exe

BEARSSL_SRC = $(shell find $(BEARSSL)/src -name '*.c')
BEARSSL_OBJ = $(patsubst $(BEARSSL)/src/%.c,build/bearssl/%.o,$(BEARSSL_SRC))
BEARSSL_LIB = build/libbearssl.a

DEFS = -DQX_UPDATE_PUBKEY='"$(UPDATE_PUBKEY)"' -DQX_UPDATE_BASE='"$(UPDATE_BASE)"'
ifeq ($(TESTING),1)
DEFS += -DQX_TESTING
endif

all: $(OUT)

# Rebuilt every time: the key, update URL and test flag can change between calls.
$(OUT): $(SRC) $(MONO) $(MINIZ) src/quartix.h src/version.h res/quartix.rc res/quartix.manifest $(BEARSSL_LIB) FORCE
	@mkdir -p $(dir $@)
	$(CC) -std=c99 -O2 -c third_party/monocypher/monocypher.c -o $(@:.exe=)-mono.o
	$(CC) -std=c99 -O2 -c third_party/monocypher/monocypher-ed25519.c -o $(@:.exe=)-mono-ed.o
	$(CC) -std=c99 -O2 $(MINIZ_DEFS) -c $(MINIZ) -o $(@:.exe=)-miniz.o
	$(WINDRES) -I res res/quartix.rc -O coff -o $(@:.exe=)-res.o
	$(CC) $(CFLAGS) $(DEFS) $(SRC) $(@:.exe=)-mono.o $(@:.exe=)-mono-ed.o $(@:.exe=)-miniz.o $(@:.exe=)-res.o $(BEARSSL_LIB) \
	      -o $@ $(LDFLAGS) $(LDLIBS)

build/bearssl/%.o: $(BEARSSL)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) -O2 -D_WIN32_WINNT=0x0600 -I$(BEARSSL)/inc -I$(BEARSSL)/src -c $< -o $@

$(BEARSSL_LIB): $(BEARSSL_OBJ)
	rm -f $@
	$(AR) rcs $@ $^

check: $(OUT)
	$(PYTHON) tools/check_win_pe.py $(OUT)

test: check
	WINE="$(WINE)" MAKE="$(MAKE)" $(PYTHON) tests/run_tests.py $(OUT)

clean:
	rm -rf build

FORCE:

.PHONY: all check test clean FORCE
