// User client for tools/nvgsp: hands firmware files to the driver, triggers the
// GSP-RM boot, and copies the GSP log buffers out. Root only.
#pragma once

#include <IOKit/IOUserClient.h>

class NVBringup;

// Selectors (keep in sync with tools/nvgsp.cpp)
enum {
    kNVUCSetFirmware = 0,   // scalar in: kind; struct in: file bytes
    kNVUCBootGsp     = 1,   // scalar out: IOReturn of the boot
    kNVUCReadLog     = 2,   // scalar in: log index (0..3); struct out: 64 KiB
    kNVUCUnloadGsp   = 3,   // scalar out: IOReturn of the teardown
    kNVUCIntr        = 4,   // scalar in: 0 off, 1 on, 2 query; scalar out: on, count, spurious, storms
    kNVUCCount
};

class NVBringupUserClient : public IOUserClient {
    OSDeclareDefaultStructors(NVBringupUserClient)

public:
    bool initWithTask(task_t owningTask, void *securityToken, UInt32 type,
                      OSDictionary *properties) override;
    bool start(IOService *provider) override;
    IOReturn clientClose() override;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                            IOExternalMethodDispatch *dispatch, OSObject *target,
                            void *reference) override;

private:
    NVBringup *drv_ = nullptr;
};
