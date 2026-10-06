# Builds NVFramebuffer.kext (the IOFramebuffer for the display NVBringup lights), Command Line Tools only.
#   make           the kext
#   make verify    checks every vtable slot of NVFramebuffer against the kernel's IOFramebuffer
#   make vdisplay  build/nvvdisplay, the virtual display that feeds the HDMI monitor (vdisplay/)
#   make check     resolves the kext's symbols against the running kernel (no load)

NAME     := NVFramebuffer
BUILD    := build
KEXT     := $(BUILD)/$(NAME).kext
SDK      := $(shell xcrun --sdk macosx --show-sdk-path)
KHEADERS := $(SDK)/System/Library/Frameworks/Kernel.framework/Headers

ARCHFLAGS := -arch x86_64 -mmacosx-version-min=14.0 -isysroot $(SDK)
KFLAGS    := $(ARCHFLAGS) -mkernel -nostdinc -I$(KHEADERS) \
             -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
             -fno-builtin -fno-common -O2 -g -Wall -Wextra -Wno-unused-parameter \
             -Wno-deprecated-declarations -Wno-inconsistent-missing-override
CXXFLAGS  := $(KFLAGS) -std=gnu++17 -fapple-kext -fno-exceptions -fno-rtti
CFLAGS    := $(KFLAGS) -std=gnu11
LDFLAGS   := $(ARCHFLAGS) -nostdlib -Xlinker -kext -Xlinker -export_dynamic \
             -lkmodc++ -lkmod -lcc_kext

OBJS := $(BUILD)/NVFramebuffer.o $(BUILD)/kmod_info.o

.PHONY: all verify check vdisplay clean
VDISPLAY := $(BUILD)/nvvdisplay
all: $(KEXT) $(VDISPLAY)

vdisplay: $(VDISPLAY)

$(VDISPLAY): vdisplay/nvvdisplay.m ../tools/libnvmac.c ../tools/libnvmac.h ../src/nv_uapi.h | $(BUILD)
	clang -fobjc-arc -O2 -Wall -Wextra -Wno-unused-parameter -I../tools -I../src vdisplay/nvvdisplay.m ../tools/libnvmac.c \
		-framework Foundation -framework CoreGraphics -framework IOSurface -framework IOKit -o $@
	codesign --force --sign - --identifier io.github.kvarun-p.nvvdisplay $@

$(BUILD)/%.o: src/%.cpp | $(BUILD)
	clang++ $(CXXFLAGS) -c $< -o $@

$(BUILD)/%.o: src/%.c | $(BUILD)
	clang $(CFLAGS) -c $< -o $@

$(KEXT): $(OBJS) Info.plist
	mkdir -p $(KEXT)/Contents/MacOS
	clang++ $(LDFLAGS) $(OBJS) -o $(KEXT)/Contents/MacOS/$(NAME)
	cp Info.plist $(KEXT)/Contents/Info.plist
	find $(KEXT) -name '._*' -delete; xattr -cr $(KEXT)
	codesign --force --sign - $(KEXT)

verify: | $(BUILD)
	clang++ $(CXXFLAGS) -c src/NVFramebuffer.cpp -o /dev/null -Xclang -fdump-vtable-layouts > $(BUILD)/vtables.txt
	python3 tools/verify_vtables.py $(BUILD)/vtables.txt

check: $(KEXT)
	kmutil libraries -p $(KEXT) --undef-symbols

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)
