# Builds NVHDAPatch.kext, a Lilu plugin that lets AppleHDAController drive the GPU's HDMI audio function (see
# src/NVHDAPatch.cpp). Command Line Tools only. Lilu's headers and plugin glue come from a Lilu source tree (BSD-3), and
# Lilu builds only against acidanthera's MacKernelSDK (kernel headers + libkmod), not the Xcode SDK's Kernel.framework:
#   make LILU=<Lilu checkout>/Lilu KSDK=<MacKernelSDK checkout>   (defaults: the checkouts in the scratchpad)
#   make check                          resolves the kext's symbols against the running kernel and Lilu (no load)

NAME     := NVHDAPatch
VERSION  := 0.1.0
BUILD    := build
KEXT     := $(BUILD)/$(NAME).kext
LILU     ?= $(HOME)/Projects/GPU/scratchpad/hda-patch/lilu-1.7.2/Lilu
SDK      := $(shell xcrun --sdk macosx --show-sdk-path)
KSDK     ?= $(HOME)/Projects/GPU/scratchpad/hda-patch/MacKernelSDK
KHEADERS := $(KSDK)/Headers

ARCHFLAGS := -arch x86_64 -mmacosx-version-min=14.0 -isysroot $(SDK)
KFLAGS    := $(ARCHFLAGS) -mkernel -nostdinc -I$(KHEADERS) -I$(LILU) \
             -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
             -DPRODUCT_NAME=$(NAME) -DMODULE_VERSION=$(VERSION) \
             -fno-builtin -fno-common -O2 -g -Wall -Wno-unused-parameter \
             -Wno-deprecated-declarations -Wno-inconsistent-missing-override
CXXFLAGS  := $(KFLAGS) -std=gnu++17 -fapple-kext -fno-exceptions -fno-rtti
CFLAGS    := $(KFLAGS) -std=gnu11
LDFLAGS   := $(ARCHFLAGS) -nostdlib -L$(KSDK)/Library/x86_64 -Xlinker -kext -Xlinker -export_dynamic \
             -lkmodc++ -lkmod -lcc_kext

OBJS := $(BUILD)/NVHDAPatch.o $(BUILD)/plugin_start.o $(BUILD)/kmod_info.o

.PHONY: all check clean
.DELETE_ON_ERROR:
all: $(KEXT)

$(BUILD)/NVHDAPatch.o: src/NVHDAPatch.cpp | $(BUILD)
	clang++ $(CXXFLAGS) -c $< -o $@

$(BUILD)/plugin_start.o: $(LILU)/Library/plugin_start.cpp | $(BUILD)
	clang++ $(CXXFLAGS) -c $< -o $@

$(BUILD)/kmod_info.o: src/kmod_info.c | $(BUILD)
	clang $(CFLAGS) -c $< -o $@

$(KEXT): $(OBJS) Info.plist
	mkdir -p $(KEXT)/Contents/MacOS
	clang++ $(LDFLAGS) $(OBJS) -o $(KEXT)/Contents/MacOS/$(NAME)
	cp Info.plist $(KEXT)/Contents/Info.plist
	find $(KEXT) -name '._*' -delete; xattr -cr $(KEXT)
	codesign --force --sign - $(KEXT)

check: $(KEXT)
	kmutil libraries -p $(KEXT) --undef-symbols > /dev/null
	../tools/check_kext_libs.sh $(KEXT)

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)
