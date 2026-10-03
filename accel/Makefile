# Builds NVMetalAccel.kext (the IOAccelerator that makes Metal list the GPU), Command Line Tools only.
#   make           the kext
#   make decl      regenerates src/IOAccelFamily2_decl.h from this Mac's kernel collections
#   make verify    checks every vtable slot of our classes against the kernel's
#   make check     resolves the kext's symbols against the running kernel (no load)

NAME     := NVMetalAccel
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

OBJS := $(BUILD)/NVMetalAccel.o $(BUILD)/kmod_info.o

.PHONY: all decl verify check clean
all: $(KEXT)

$(BUILD)/%.o: src/%.cpp src/IOAccelFamily2_decl.h | $(BUILD)
	clang++ $(CXXFLAGS) -c $< -o $@

$(BUILD)/%.o: src/%.c | $(BUILD)
	clang $(CFLAGS) -c $< -o $@

$(KEXT): $(OBJS) Info.plist
	mkdir -p $(KEXT)/Contents/MacOS
	clang++ $(LDFLAGS) $(OBJS) -o $(KEXT)/Contents/MacOS/$(NAME)
	cp Info.plist $(KEXT)/Contents/Info.plist
	find $(KEXT) -name '._*' -delete; xattr -cr $(KEXT)
	codesign --force --sign - $(KEXT)

decl:
	python3 tools/gen_decl.py > src/IOAccelFamily2_decl.h

verify: | $(BUILD)
	clang++ $(CXXFLAGS) -c src/NVMetalAccel.cpp -o /dev/null -Xclang -fdump-vtable-layouts > $(BUILD)/vtables.txt
	python3 tools/verify_vtables.py $(BUILD)/vtables.txt

check: $(KEXT)
	kmutil libraries -p $(KEXT) --undef-symbols

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)
