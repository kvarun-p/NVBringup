#include "libnvmac.h"

#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdlib.h>
#include <string.h>

struct nvmac_dev {
    io_connect_t conn;
    struct nvmac_info info;
    const volatile uint64_t *syncs;     // read-only sync page
    int control;                        // nvmac_open_info: control client, info only
};

int nvmac_open(nvmac_dev **out)
{
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVBringup"));
    if (!s)
        return kIOReturnNotFound;
    nvmac_dev *d = calloc(1, sizeof(*d));
    if (!d) {
        IOObjectRelease(s);
        return kIOReturnNoMemory;
    }
    kern_return_t kr = IOServiceOpen(s, mach_task_self(), NVMAC_CLIENT_TYPE, &d->conn);
    IOObjectRelease(s);
    if (kr) {
        free(d);
        return kr;
    }
    size_t n = sizeof(d->info);
    kr = IOConnectCallStructMethod(d->conn, NVMAC_GET_INFO, NULL, 0, &d->info, &n);
    if (!kr && (n != sizeof(d->info) || d->info.version != NVMAC_ABI_VERSION))
        kr = kIOReturnUnsupported;
    uint64_t out2[2] = {0};
    uint32_t cnt = 2;
    if (!kr)
        kr = IOConnectCallScalarMethod(d->conn, NVMAC_MAP_SYNC_PAGE, NULL, 0, out2, &cnt);
    if (kr) {
        IOServiceClose(d->conn);
        free(d);
        return kr;
    }
    d->syncs = (const volatile uint64_t *)(uintptr_t)out2[0];
    *out = d;
    return 0;
}

// Device info only, through the control client (NVMAC_CONTROL_TYPE): doesn't open the GPU, so it
// doesn't power it on. kIOReturnNotReady before its first boot: use nvmac_open then.
int nvmac_open_info(nvmac_dev **out)
{
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVBringup"));
    if (!s)
        return kIOReturnNotFound;
    nvmac_dev *d = calloc(1, sizeof(*d));
    if (!d) {
        IOObjectRelease(s);
        return kIOReturnNoMemory;
    }
    kern_return_t kr = IOServiceOpen(s, mach_task_self(), NVMAC_CONTROL_TYPE, &d->conn);
    IOObjectRelease(s);
    if (kr) {
        free(d);
        return kr;
    }
    d->control = 1;
    kr = nvmac_refresh_info(d);
    if (!kr && d->info.version != NVMAC_ABI_VERSION)
        kr = kIOReturnUnsupported;
    if (kr) {
        IOServiceClose(d->conn);
        free(d);
        return kr;
    }
    *out = d;
    return 0;
}

void nvmac_close(nvmac_dev *d)
{
    if (!d)
        return;
    IOServiceClose(d->conn);
    free(d);
}

const struct nvmac_info *nvmac_info(nvmac_dev *d) { return &d->info; }

int nvmac_refresh_info(nvmac_dev *d)
{
    size_t n = sizeof(d->info);
    kern_return_t kr = IOConnectCallStructMethod(d->conn, d->control ? NVMAC_POWER_INFO : NVMAC_GET_INFO,
                                                 NULL, 0, &d->info, &n);
    return !kr && n != sizeof(d->info) ? kIOReturnUnsupported : kr;
}

const char *nvmac_strerror(int err)
{
    switch (err) {
    case 0: return "ok";
    case kIOReturnBadArgument: return "bad argument";
    case kIOReturnNoMemory: return "out of memory";
    case kIOReturnNoDevice: return "device lost / not running";
    case kIOReturnTimeout: return "timeout";
    case kIOReturnAborted: return "interrupted";
    case kIOReturnBusy: return "busy";
    case kIOReturnNotPrivileged: return "not privileged";
    case kIOReturnNoResources: return "no resources";
    case kIOReturnNotFound: return "driver not found";
    case kIOReturnUnsupported: return "unsupported";
    case kIOReturnNotReady: return "not ready";
    default: return "error";
    }
}

static int scalar(nvmac_dev *d, uint32_t sel, const uint64_t *in, uint32_t nin, uint64_t *out, uint32_t nout)
{
    uint32_t cnt = nout;
    return IOConnectCallScalarMethod(d->conn, sel, in, nin, out, out ? &cnt : NULL);
}

int nvmac_mem_alloc(nvmac_dev *d, uint64_t size, uint64_t align, uint32_t flags, uint8_t kind,
                    uint32_t *mem, uint64_t *size_out)
{
    uint64_t in[4] = { size, align, flags, kind }, out[2] = {0};
    int r = scalar(d, NVMAC_MEM_ALLOC, in, 4, out, 2);
    if (!r) {
        *mem = (uint32_t)out[0];
        if (size_out)
            *size_out = out[1];
    }
    return r;
}

int nvmac_mem_free(nvmac_dev *d, uint32_t mem)
{
    uint64_t in = mem;
    return scalar(d, NVMAC_MEM_FREE, &in, 1, NULL, 0);
}

int nvmac_mem_map(nvmac_dev *d, uint32_t mem, void **ptr, uint64_t *size)
{
    uint64_t in = mem, out[2] = {0};
    int r = scalar(d, NVMAC_MEM_MAP, &in, 1, out, 2);
    if (!r) {
        *ptr = (void *)(uintptr_t)out[0];
        if (size)
            *size = out[1];
    }
    return r;
}

int nvmac_mem_unmap(nvmac_dev *d, uint32_t mem)
{
    uint64_t in = mem;
    return scalar(d, NVMAC_MEM_UNMAP, &in, 1, NULL, 0);
}

int nvmac_vm_bind(nvmac_dev *d, const struct nvmac_bind_op *ops, uint32_t count)
{
    size_t n = sizeof(struct nvmac_bind_hdr) + count * sizeof(*ops);
    uint8_t *b = malloc(n);
    if (!b)
        return kIOReturnNoMemory;
    struct nvmac_bind_hdr h = { count, 0 };
    memcpy(b, &h, sizeof(h));
    memcpy(b + sizeof(h), ops, count * sizeof(*ops));
    int r = IOConnectCallStructMethod(d->conn, NVMAC_VM_BIND, b, n, NULL, NULL);
    free(b);
    return r;
}

int nvmac_bind(nvmac_dev *d, uint64_t va, uint32_t mem, uint64_t mem_offset, uint64_t range)
{
    struct nvmac_bind_op o = { NVMAC_BIND, mem, va, range, mem_offset };
    return nvmac_vm_bind(d, &o, 1);
}

int nvmac_unbind(nvmac_dev *d, uint64_t va, uint64_t range)
{
    struct nvmac_bind_op o = { NVMAC_UNBIND, 0, va, range, 0 };
    return nvmac_vm_bind(d, &o, 1);
}

int nvmac_ctx_create(nvmac_dev *d, uint32_t engines, uint32_t *ctx, uint32_t *seq_sync)
{
    uint64_t in = engines, out[2] = {0};
    int r = scalar(d, NVMAC_CTX_CREATE, &in, 1, out, 2);
    if (!r) {
        *ctx = (uint32_t)out[0];
        if (seq_sync)
            *seq_sync = (uint32_t)out[1];
    }
    return r;
}

int nvmac_ctx_destroy(nvmac_dev *d, uint32_t ctx)
{
    uint64_t in = ctx;
    return scalar(d, NVMAC_CTX_DESTROY, &in, 1, NULL, 0);
}

int nvmac_ctx_status(nvmac_dev *d, uint32_t ctx, int *lost, uint32_t *except_type, uint64_t *completed)
{
    uint64_t in = ctx, out[3] = {0};
    int r = scalar(d, NVMAC_CTX_STATUS, &in, 1, out, 3);
    if (!r) {
        if (lost)
            *lost = (int)out[0];
        if (except_type)
            *except_type = (uint32_t)out[1];
        if (completed)
            *completed = out[2];
    }
    return r;
}

int nvmac_exec(nvmac_dev *d, uint32_t ctx,
               const struct nvmac_sync_point *waits, uint32_t n_wait,
               const struct nvmac_push *pushes, uint32_t n_push,
               const struct nvmac_sync_point *signals, uint32_t n_signal, uint64_t *seqno)
{
    size_t n = sizeof(struct nvmac_exec_hdr) + (n_wait + n_signal) * sizeof(struct nvmac_sync_point) +
               n_push * sizeof(struct nvmac_push);
    uint8_t *b = malloc(n), *p = b;
    if (!b)
        return kIOReturnNoMemory;
    struct nvmac_exec_hdr h = { ctx, n_wait, n_push, n_signal };
    memcpy(p, &h, sizeof(h));
    p += sizeof(h);
    memcpy(p, waits, n_wait * sizeof(*waits));
    p += n_wait * sizeof(*waits);
    memcpy(p, pushes, n_push * sizeof(*pushes));
    p += n_push * sizeof(*pushes);
    memcpy(p, signals, n_signal * sizeof(*signals));
    uint64_t seq = 0;
    uint32_t cnt = 1;
    int r = IOConnectCallMethod(d->conn, NVMAC_EXEC, NULL, 0, b, n, &seq, &cnt, NULL, NULL);
    free(b);
    if (!r && seqno)
        *seqno = seq;
    return r;
}

int nvmac_sync_create(nvmac_dev *d, uint64_t initial, uint32_t *sync)
{
    uint64_t in = initial, out[2] = {0};
    int r = scalar(d, NVMAC_SYNC_CREATE, &in, 1, out, 2);
    if (!r)
        *sync = (uint32_t)out[0];
    return r;
}

int nvmac_sync_destroy(nvmac_dev *d, uint32_t sync)
{
    uint64_t in = sync;
    return scalar(d, NVMAC_SYNC_DESTROY, &in, 1, NULL, 0);
}

int nvmac_sync_signal(nvmac_dev *d, uint32_t sync, uint64_t value)
{
    uint64_t in[2] = { sync, value };
    return scalar(d, NVMAC_SYNC_SIGNAL, in, 2, NULL, 0);
}

uint64_t nvmac_sync_value(nvmac_dev *d, uint32_t sync)
{
    return sync && sync <= d->info.sync_count ? d->syncs[sync - 1] : 0;
}

int nvmac_sync_wait(nvmac_dev *d, const struct nvmac_sync_point *pts, uint32_t count, int all,
                    uint64_t timeout_ns, uint32_t *signaled)
{
    // Fast path: already there (no system call).
    uint32_t met = 0, first = count;
    for (uint32_t i = 0; i < count; i++)
        if (nvmac_sync_value(d, pts[i].sync) >= pts[i].value) {
            met++;
            if (first == count)
                first = i;
        }
    if (count && (all ? met == count : met > 0)) {
        if (signaled)
            *signaled = all ? 0 : first;
        return 0;
    }
    size_t n = sizeof(struct nvmac_wait_hdr) + count * sizeof(*pts);
    uint8_t *b = malloc(n);
    if (!b)
        return kIOReturnNoMemory;
    struct nvmac_wait_hdr h = { count, all ? NVMAC_WAIT_ALL : 0, timeout_ns };
    memcpy(b, &h, sizeof(h));
    memcpy(b + sizeof(h), pts, count * sizeof(*pts));
    uint64_t idx = 0;
    uint32_t cnt = 1;
    int r = IOConnectCallMethod(d->conn, NVMAC_SYNC_WAIT, NULL, 0, b, n, &idx, &cnt, NULL, NULL);
    free(b);
    if (!r && signaled)
        *signaled = (uint32_t)idx;
    return r;
}

uint64_t nvmac_gpu_timestamp(nvmac_dev *d)
{
    uint64_t t = 0;
    scalar(d, NVMAC_GET_TIMESTAMP, NULL, 0, &t, 1);
    return t;
}
