# Builds NVBringup.kext with the Command Line Tools (no Xcode needed).

NAME     := NVBringup
BUILD    := build
KEXT     := $(BUILD)/$(NAME).kext
SDK      := $(shell xcrun --sdk macosx --show-sdk-path)
KHEADERS := $(SDK)/System/Library/Frameworks/Kernel.framework/Headers

ARCHFLAGS := -arch x86_64 -mmacosx-version-min=14.0 -isysroot $(SDK)
# Experiment only: 1/2 revoke freed CPU mappings with IOMemoryMap::redirect instead of
# holding their BAR1 slices (src/NVGpu.cpp). Rebuild from clean when changing it.
NV_BAR1_REDIRECT ?= 0
KFLAGS    := $(ARCHFLAGS) -mkernel -nostdinc -I$(KHEADERS) \
             -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
             -DNV_BAR1_REDIRECT=$(NV_BAR1_REDIRECT) \
             -fno-builtin -fno-common -O2 -g -Wall -Wextra -Wno-unused-parameter \
             -Wno-deprecated-declarations -Wno-inconsistent-missing-override
CXXFLAGS  := $(KFLAGS) -std=gnu++17 -fapple-kext -fno-exceptions -fno-rtti
CFLAGS    := $(KFLAGS) -std=gnu11
LDFLAGS   := $(ARCHFLAGS) -nostdlib -Xlinker -kext -Xlinker -export_dynamic \
             -lkmodc++ -lkmod -lcc_kext

OBJS := $(BUILD)/NVBringup.o $(BUILD)/nv_hal.o $(BUILD)/hal_tu1xx.o $(BUILD)/hal_ga10x.o $(BUILD)/nv_vbios.o $(BUILD)/nv_fwsec.o $(BUILD)/nv_gsp.o $(BUILD)/nv_gsp_rm.o $(BUILD)/nv_vram.o $(BUILD)/nv_mmu.o $(BUILD)/NVGsp.o $(BUILD)/NVBringupUserClient.o $(BUILD)/NVGpu.o $(BUILD)/NVGpuUserClient.o $(BUILD)/NVPower.o $(BUILD)/kmod_info.o
TOOL := $(BUILD)/vbios_tool
NVGSP := $(BUILD)/nvgsp
NVTEST := $(BUILD)/nvtest
VKTEST := $(BUILD)/vktest
ALIASTEST := $(BUILD)/bar1_alias_test
# Machine-specific paths (NVB_*) can go in local.env, which git ignores.
-include local.env
# Vulkan headers for vktest: the Vulkan-Headers install prefix
NVB_VULKAN_PREFIX ?= /usr/local
VULKAN_INC ?= $(NVB_VULKAN_PREFIX)/include
GENBL_BIN := firmware/nvidia/tu102/gsp/gen_bootloader-570.144.bin
GENBL_H   := $(BUILD)/gen_bootloader.h

.PHONY: all clean check test gsp_static_h
all: $(KEXT) $(TOOL) $(NVGSP) $(NVTEST)

$(GENBL_H): $(GENBL_BIN) tools/bin2h.py | $(BUILD)
	python3 tools/bin2h.py $< nv_gen_bootloader > $@

$(BUILD)/%.o: src/%.cpp src/*.hpp src/*.h $(GENBL_H) | $(BUILD)
	clang++ $(CXXFLAGS) -I$(BUILD) -c $< -o $@

$(BUILD)/%.o: src/%.c | $(BUILD)
	clang $(CFLAGS) -c $< -o $@

$(KEXT): $(OBJS) Info.plist
	mkdir -p $(KEXT)/Contents/MacOS
	clang++ $(LDFLAGS) $(OBJS) -o $(KEXT)/Contents/MacOS/$(NAME)
	cp Info.plist $(KEXT)/Contents/Info.plist
	# exFAT stores extended attributes in ._* files, which break codesign.
	find $(KEXT) -name '._*' -delete; xattr -cr $(KEXT)
	codesign --force --sign - $(KEXT)

# Resolves the kext's symbols against the running kernel's libraries without loading it.
check: $(KEXT)
	kmutil libraries -p $(KEXT) --undef-symbols

# Host build of the VBIOS parser, with sanitizers.
$(TOOL): tools/vbios_tool.cpp src/nv_vbios.cpp src/nv_fwsec.cpp src/nv_gsp.cpp src/nv_gsp_rm.cpp src/nv_vram.cpp src/nv_mmu.cpp src/nv_hal.cpp src/*.h $(GENBL_H) | $(BUILD)
	clang++ -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined -I$(BUILD) \
		tools/vbios_tool.cpp src/nv_vbios.cpp src/nv_fwsec.cpp src/nv_gsp.cpp src/nv_gsp_rm.cpp src/nv_vram.cpp src/nv_mmu.cpp src/nv_hal.cpp -o $@

# User-space tool that boots GSP-RM through the kext's user client.
$(NVGSP): tools/nvgsp.cpp src/nv_hal.cpp src/nv_gsp.cpp src/nv_hal.h src/nv_gsp.h | $(BUILD)
	clang++ -std=c++17 -O2 -Wall -Wextra tools/nvgsp.cpp src/nv_hal.cpp src/nv_gsp.cpp -framework IOKit -framework CoreFoundation -o $@

$(NVTEST): tools/nvtest.c tools/libnvmac.c tools/libnvmac.h src/nv_uapi.h | $(BUILD)
	clang -std=c11 -O2 -Wall -Wextra -Isrc tools/nvtest.c tools/libnvmac.c -framework IOKit -framework CoreFoundation -o $@

# Security test: does a duplicated CPU mapping of VRAM survive MEM_FREE? (+ BAR1 hold reuse / cap)
$(ALIASTEST): tools/bar1_alias_test.c tools/libnvmac.c tools/libnvmac.h src/nv_uapi.h | $(BUILD)
	clang -std=c11 -O2 -Wall -Wextra -Isrc tools/bar1_alias_test.c tools/libnvmac.c -framework IOKit -framework CoreFoundation -o $@

$(VKTEST): tools/vktest.c | $(BUILD)
	clang -std=c11 -O2 -Wall -Wextra -I$(VULKAN_INC) tools/vktest.c -o $@

# Regenerates src/nv_gsp_static.h from NVIDIA's headers (not needed for a normal build).
NVSRC ?= temp/ogkm/src
gsp_static_h:
	clang -std=c11 -I$(NVSRC)/common/sdk/nvidia/inc tools/gsp_layout_probe.c -o $(BUILD)/gsp_layout_probe
	$(BUILD)/gsp_layout_probe > src/nv_gsp_static.h

test: $(TOOL)
	$(TOOL) --selftest
	$(TOOL) --gsp firmware/nvidia

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)
