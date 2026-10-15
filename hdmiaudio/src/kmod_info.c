// Kext module descriptor (what Xcode normally generates as <target>_info.c). libkmod's _start runs the C++
// constructors and then _realmain, which for a Lilu plugin is <product>_kern_start (Lilu's plugin_start.cpp).
#include <mach/mach_types.h>

extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);
extern kern_return_t NVHDAPatch_kern_start(kmod_info_t *ki, void *data);
extern kern_return_t NVHDAPatch_kern_stop(kmod_info_t *ki, void *data);

__attribute__((visibility("default")))
KMOD_EXPLICIT_DECL(io.github.kvarun-p.nvhdapatch, "0.1.0", _start, _stop)

__private_extern__ kmod_start_func_t *_realmain = NVHDAPatch_kern_start;
__private_extern__ kmod_stop_func_t *_antimain = NVHDAPatch_kern_stop;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
