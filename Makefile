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

SRC = src/main.c src/paths.c src/apk.c src/sha256.c src/update.c src/log.c
OUT = build/Quartix.exe

all: $(OUT)

$(OUT): $(SRC) src/quartix.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRC) -o $@ $(LDFLAGS) $(LDLIBS)

check: $(OUT)
	$(PYTHON) tools/check_xp_pe.py $(OUT)

test: check
	WINE="$(WINE)" $(PYTHON) tests/run_tests.py $(OUT)

clean:
	rm -rf build

.PHONY: all check test clean
