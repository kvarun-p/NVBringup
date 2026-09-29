#include "NVBringupUserClient.hpp"
#include "NVBringup.hpp"

#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>

#define super IOUserClient
OSDefineMetaClassAndStructors(NVBringupUserClient, IOUserClient)

bool NVBringupUserClient::initWithTask(task_t owningTask, void *securityToken, UInt32 type,
                                       OSDictionary *properties)
{
    // Booting GSP-RM drives the GPU's security processors: administrators only.
    if (clientHasPrivilege(securityToken, kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
        return false;
    return super::initWithTask(owningTask, securityToken, type, properties);
}

bool NVBringupUserClient::start(IOService *provider)
{
    drv_ = OSDynamicCast(NVBringup, provider);
    return drv_ && super::start(provider);
}

IOReturn NVBringupUserClient::clientClose()
{
    terminate();
    return kIOReturnSuccess;
}

IOReturn NVBringupUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *a,
                                             IOExternalMethodDispatch *, OSObject *, void *)
{
    switch (selector) {
    case kNVUCSetFirmware: {
        if (a->scalarInputCount != 1)
            return kIOReturnBadArgument;
        // Inputs over 4 KiB arrive as a descriptor; small ones inline.
        IOMemoryDescriptor *md = a->structureInputDescriptor;
        uint64_t len = md ? md->getLength() : a->structureInputSize;
        bool own = false;
        if (!md) {
            if (!a->structureInput || !len)
                return kIOReturnBadArgument;
            md = IOMemoryDescriptor::withAddress((void *)a->structureInput, len, kIODirectionOut);
            if (!md)
                return kIOReturnNoMemory;
            own = true;
        }
        IOReturn r = md->prepare(kIODirectionOut);
        if (r == kIOReturnSuccess) {
            r = drv_->setFirmware((uint32_t)a->scalarInput[0], md, len);
            md->complete(kIODirectionOut);
        }
        if (own)
            md->release();
        return r;
    }
    case kNVUCBootGsp:
        if (a->scalarOutputCount != 1)
            return kIOReturnBadArgument;
        a->scalarOutput[0] = (uint32_t)drv_->bootGsp();
        return kIOReturnSuccess;
    case kNVUCUnloadGsp:
        if (a->scalarOutputCount != 1)
            return kIOReturnBadArgument;
        a->scalarOutput[0] = (uint32_t)drv_->unloadGsp("nvgsp unload");
        return kIOReturnSuccess;
    case kNVUCIntr: {
        if (a->scalarInputCount != 1 || a->scalarOutputCount != 4)
            return kIOReturnBadArgument;
        uint64_t out[4] = {};
        IOReturn r = drv_->setIntr((uint32_t)a->scalarInput[0], out);
        for (int i = 0; i < 4; i++)
            a->scalarOutput[i] = out[i];
        return r;
    }
    case kNVUCReadLog: {
        IOMemoryDescriptor *md = a->structureOutputDescriptor;
        if (a->scalarInputCount != 1 || !md)
            return kIOReturnBadArgument;    // the 64 KiB output always comes as a descriptor
        IOReturn r = md->prepare(kIODirectionIn);
        if (r != kIOReturnSuccess)
            return r;
        uint64_t len = 0;
        r = drv_->readGspLog((uint32_t)a->scalarInput[0], md, &len);
        md->complete(kIODirectionIn);
        a->structureOutputDescriptorSize = (uint32_t)len;
        return r;
    }
    default:
        return kIOReturnBadArgument;
    }
}
