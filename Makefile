# fgres — single-file C11 daemon. Plain make is the whole build system;
# autotools/cmake/meson are overkill for one translation unit.
# -flto here needs clang+lld (gcc's ld.bfd lacks the LTO plugin); override CC=
# only with a clang.
ifeq ($(origin CC),default)
CC := clang
endif
ANDROID_CC ?= aarch64-linux-android24-clang
CFLAGS   ?= -std=c11 -O2 -flto -Wall -Wextra -Werror
LINKER ?= lld
STRIP  ?= llvm-strip
OUT      := target

HOST  := $(OUT)/fgres
TEST  := $(OUT)/fgres_test
DROID := $(OUT)/fgres-android

.PHONY: all test android clean
all: test $(HOST) $(DROID)

android: $(DROID)

test: $(TEST)
	./$(TEST)

# clang + lld by default: -flto needs the LTO plugin (gcc's ld.bfd lacks it).
$(TEST): fgres.c | $(OUT)
	$(CC) $(CFLAGS) -fuse-ld=$(LINKER) -DFGRES_TEST $< -o $@
	$(STRIP) $@

$(HOST): fgres.c | $(OUT)
	$(CC) $(CFLAGS) -fuse-ld=$(LINKER) $< -o $@
	$(STRIP) $@

# links liblog on Android only; NDK clang is hardcoded to the API level.
$(DROID): fgres.c | $(OUT)
	$(ANDROID_CC) $(CFLAGS) $< -o $@ -llog
	$(STRIP) $@

$(OUT):
	mkdir -p $(OUT)

clean:
	rm -f $(HOST) $(TEST) $(DROID)
