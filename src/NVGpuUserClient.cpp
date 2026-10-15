#include "NVGpuUserClient.hpp"
#include <sys/kauth.h>
#include <sys/proc.h>
#include "nv_uapi.h"

#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>

#define super IOUserClient
OSDefineMetaClassAndStructors(NVGpuUserClient, IOUserClient)

OSDefineMetaClassAndStructors(IOAccelNVGpuUserClient, NVGpuUserClient)

// The calling process is WindowServer: uid 88 (_windowserver) and that process name. newUserClient and initWithTask run
// in the caller.
bool NVGpuUserClient::callerIsWindowServer()
{
    char name[32] = {};
    proc_selfname(name, sizeof(name));
    return kauth_getuid() == 88 && strcmp(name, "WindowServer") == 0;
}

bool NVGpuUserClient::initWithTask(task_t owningTask, void *securityToken, UInt32 type,
                                   OSDictionary *properties)
{
    // Root, or the user logged in at the console (connections are isolated from each other:
    // own VA space, unprivileged channels, scrubbed VRAM). nvgpu_users=1 lets anyone in.
    // On a display-pipe boot, also WindowServer: it runs as _windowserver (uid 88), neither admin nor the console
    // user, and NVMetal in it opens the GPU through NVK (refused, NVK found no GPU and CoreDisplay's Metal device had
    // no library: the WindowServer crashes of 2026-10-11 20:18). Both types: NVK lists the GPU through the control
    // client before it opens the GPU client (refused at 20:26 when only the GPU client admitted it).
    uint32_t users = 0;
    PE_parse_boot_argn("nvgpu_users", &users, sizeof(users));
    if (users != 1 && clientHasPrivilege(securityToken, kIOClientPrivilegeAdministrator) != kIOReturnSuccess &&
        clientHasPrivilege(securityToken, kIOClientPrivilegeLocalUser) != kIOReturnSuccess &&
        !(allowWindowServer && callerIsWindowServer()))
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
    // Zero-copy scanout registrations are WindowServer's (or root's, for tests): the display shows what they name.
    if (selector == NVMAC_SURFACE_SCANOUT && kauth_getuid() != 0 && !callerIsWindowServer())
        return kIOReturnNotPermitted;
    return drv_->gpuCall(conn_, selector, a);
}
