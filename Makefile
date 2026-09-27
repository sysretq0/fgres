# fgres — single-file C11 daemon. Plain make is the whole build system;
# autotools/cmake/meson are overkill for one translation unit.
# -flto here needs clang+lld (gcc's ld.bfd lacks the LTO plugin); override CC=
# only with a clang.
ifeq ($(origin CC),default)
CC := clang
endif
ANDROID_API ?= 24
TRIPLE_arm64-v8a := aarch64-linux-android
TRIPLE_armeabi-v7a := armv7a-linux-androideabi
TRIPLE_x86_64 := x86_64-linux-android
TRIPLE_x86 := i686-linux-android
CFLAGS   ?= -std=c11 -O2 -flto -Wall -Wextra -Werror
LINKER ?= lld
STRIP  ?= llvm-strip
OUT      := target

HOST  := $(OUT)/fgres
TEST  := $(OUT)/fgres_test
DROIDS := $(addprefix $(OUT)/fgres-android-,arm64-v8a armeabi-v7a x86_64 x86)

.PHONY: all test android clean
all: test $(HOST) $(DROIDS)

android: $(DROIDS)

test: $(TEST)
	./$(TEST)

# clang + lld by default: -flto needs the LTO plugin (gcc's ld.bfd lacks it).
$(TEST): fgres.c | $(OUT)
	$(CC) $(CFLAGS) -fuse-ld=$(LINKER) -DFGRES_TEST $< -o $@
	$(STRIP) $@

$(HOST): fgres.c | $(OUT)
	$(CC) $(CFLAGS) -fuse-ld=$(LINKER) $< -o $@
	$(STRIP) $@

# one rule per ABI: $(TRIPLE_<abi>) + API + -clang (links liblog on Android).
$(OUT)/fgres-android-%: fgres.c | $(OUT)
	$(TRIPLE_$*)$(ANDROID_API)-clang $(CFLAGS) $< -o $@ -llog
	$(STRIP) $@

$(OUT):
	mkdir -p $(OUT)

clean:
	rm -f $(HOST) $(TEST) $(DROIDS)
