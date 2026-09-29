// User client for GPU clients (IOServiceOpen type NVMAC_CLIENT_TYPE, src/nv_uapi.h): one
// connection = one GPU VA space with its memory, contexts and syncs (NVGpu.cpp). Type
// NVMAC_CONTROL_TYPE opens no GPU connection: GPU power status and mode only (NVPower.cpp).
// Administrators only, unless boot-arg nvgpu_users=1.
#pragma once

#include <IOKit/IOUserClient.h>
#include "NVBringup.hpp"

class NVGpuUserClient : public IOUserClient {
    OSDeclareDefaultStructors(NVGpuUserClient)

public:
    bool initWithTask(task_t owningTask, void *securityToken, UInt32 type,
                      OSDictionary *properties) override;
    bool start(IOService *provider) override;
    IOReturn clientClose() override;
    void free() override;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                            IOExternalMethodDispatch *dispatch, OSObject *target,
                            void *reference) override;

private:
    NVBringup *drv_ = nullptr;
    NVBringup::GpuConn *conn_ = nullptr;
    task_t task_ = nullptr;
    bool closed_ = false;
    bool control_ = false;          // NVMAC_CONTROL_TYPE: power control only, no GPU connection
};
