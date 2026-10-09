// AppleGraphicsDeviceControl (AGDC), declared for layout only. The class is in the kernel: com.apple.AppleGraphicsDeviceControl
// 8.1.9 (AppleGraphicsControl.kext/PlugIns, no personalities, a base for the vendor providers IOPresentment and WindowServer
// talk to). NVAGDCControl.cpp subclasses it. Nothing here is defined in this kext: every method below is the kernel's.
//
// The layout is what `make verify` checks against the running kernel's collections (display/tools/verify_vtables.py, and
// display/tools/verify_sizes.py for the size), the way accel/tools/gen_decl.py generates its declarations:
//   vtable  0..265  IOService's (266 entries in the kernel; IOService's own declarations give the same count)
//           266     vendor_doDeviceAttribute(..., IOExternalMethodArguments *)  pure: the vendor implements it
//           267     vendor_doDeviceAttribute(..., AGDCClientState_t *)           the kernel's; forwards to 266
//           268-275 _RESERVEDAppleGraphicsDeviceControl0..7
//   The overrides of IOService's virtuals that AGDC makes, at their IOService slots: ~ (0, 1), getMetaClass (7), start (184),
//   stop (185), terminate (192), message (230), newUserClient (238). They are redeclared so that a subclass's vtable points at
//   the kernel's versions, not IOService's.
//   Size 272 (the MetaClass constructor's classSize): the kernel's data members follow IOService and are padded here.

#ifndef AppleGraphicsDeviceControl_decl_h
#define AppleGraphicsDeviceControl_decl_h

#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSDictionary.h>

struct AGDCClientState_t;   // the user client's state; the kernel's slot 267 passes its args on to slot 266. Opaque here.

class AppleGraphicsDeviceControl : public IOService {
    OSDeclareDefaultStructors(AppleGraphicsDeviceControl)

private:
    uint8_t kernelFields_[272 - sizeof(IOService)];      // the kernel's data members (not used here)

public:
    // 0, 1 (the destructor) and 7 (getMetaClass) come from OSDeclareDefaultStructors above.
    virtual bool start(IOService *provider) override;                                   // 184
    virtual void stop(IOService *provider) override;                                    // 185
    virtual bool terminate(IOOptionBits options = 0) override;                          // 192
    virtual IOReturn message(UInt32 type, IOService *provider, void *argument = 0) override;   // 230
    virtual IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
                                   OSDictionary *properties, IOUserClient **handler) override;  // 238

    // 266: the vendor hook. The kernel implements nothing here; a subclass must (IntelFBClientControl does).
    virtual IOReturn vendor_doDeviceAttribute(UInt32 cmd, unsigned long *in, unsigned long inCount, unsigned long *out,
                                              unsigned long *outCount, IOExternalMethodArguments *args) = 0;
    // 267: the kernel's, with the client state; it calls slot 266 with the state's IOExternalMethodArguments.
    virtual IOReturn vendor_doDeviceAttribute(UInt32 cmd, unsigned long *in, unsigned long inCount, unsigned long *out,
                                              unsigned long *outCount, AGDCClientState_t *state);

    // 268-275: reserved in the kernel.
    virtual void _RESERVEDAppleGraphicsDeviceControl0();
    virtual void _RESERVEDAppleGraphicsDeviceControl1();
    virtual void _RESERVEDAppleGraphicsDeviceControl2();
    virtual void _RESERVEDAppleGraphicsDeviceControl3();
    virtual void _RESERVEDAppleGraphicsDeviceControl4();
    virtual void _RESERVEDAppleGraphicsDeviceControl5();
    virtual void _RESERVEDAppleGraphicsDeviceControl6();
    virtual void _RESERVEDAppleGraphicsDeviceControl7();
};

static_assert(sizeof(AppleGraphicsDeviceControl) == 272, "AppleGraphicsDeviceControl is 272 bytes in the kernel");

#endif
