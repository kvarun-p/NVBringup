#include "NVGpuUserClient.hpp"
#include "nv_uapi.h"

#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>

#define super IOUserClient
OSDefineMetaClassAndStructors(NVGpuUserClient, IOUserClient)

bool NVGpuUserClient::initWithTask(task_t owningTask, void *securityToken, UInt32 type,
                                   OSDictionary *properties)
{
    // Root, or the user logged in at the console (connections are isolated from each other:
    // own VA space, unprivileged channels, scrubbed VRAM). nvgpu_users=1 lets anyone in.
    uint32_t users = 0;
    PE_parse_boot_argn("nvgpu_users", &users, sizeof(users));
    if (users != 1 && clientHasPrivilege(securityToken, kIOClientPrivilegeAdministrator) != kIOReturnSuccess &&
        clientHasPrivilege(securityToken, kIOClientPrivilegeLocalUser) != kIOReturnSuccess)
        return false;
    if (!super::initWithTask(owningTask, securityToken, type, properties))
        return false;
    task_ = owningTask;
    control_ = type == NVMAC_CONTROL_TYPE;
    return true;
}

bool NVGpuUserClient::start(IOService *provider)
{
    drv_ = OSDynamicCast(NVBringup, provider);
    if (!drv_ || !super::start(provider))
        return false;
    return control_ || drv_->gpuOpen(task_, &conn_) == kIOReturnSuccess;
}

IOReturn NVGpuUserClient::clientClose()
{
    if (conn_ && !closed_) {
        closed_ = true;
        drv_->gpuClose(conn_);
    }
    terminate();
    return kIOReturnSuccess;
}

void NVGpuUserClient::free()
{
    if (conn_)
        drv_->gpuRelease(conn_);
    conn_ = nullptr;
    super::free();
}

IOReturn NVGpuUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *a,
                                         IOExternalMethodDispatch *, OSObject *, void *)
{
    if (control_)
        return drv_->powerCall(selector, a);    // power status/mode; the GPU isn't opened
    if (!conn_ || closed_)
        return kIOReturnNotOpen;
    if (selector >= NVMAC_SELECTOR_COUNT)
        return kIOReturnBadArgument;
    return drv_->gpuCall(conn_, selector, a);
}
