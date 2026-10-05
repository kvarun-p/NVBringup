// libnvmac: user-space access to NVBringup's GPU interface (src/nv_uapi.h). A thin C
// wrapper over IOConnectCallMethod; the future nvkmd_macos backend of NVK sits on it.
// All calls return 0 or an IOReturn (kern_return_t) error.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "nv_uapi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nvmac_dev nvmac_dev;

int  nvmac_open(nvmac_dev **out);
int  nvmac_open_info(nvmac_dev **out);      // info only (control client): doesn't power the GPU on
void nvmac_close(nvmac_dev *d);
const struct nvmac_info *nvmac_info(nvmac_dev *d);
int  nvmac_refresh_info(nvmac_dev *d);     // re-reads GET_INFO (memory usage)
const char *nvmac_strerror(int err);

int nvmac_mem_alloc(nvmac_dev *d, uint64_t size, uint64_t align, uint32_t flags, uint8_t kind,
                    uint32_t *mem, uint64_t *size_out);
int nvmac_mem_free(nvmac_dev *d, uint32_t mem);
int nvmac_mem_import(nvmac_dev *d, void *ptr, uint64_t size, uint32_t *mem, uint64_t *size_out);
int nvmac_mem_map(nvmac_dev *d, uint32_t mem, void **ptr, uint64_t *size);
int nvmac_mem_unmap(nvmac_dev *d, uint32_t mem);

int nvmac_vm_bind(nvmac_dev *d, const struct nvmac_bind_op *ops, uint32_t count);
int nvmac_bind(nvmac_dev *d, uint64_t va, uint32_t mem, uint64_t mem_offset, uint64_t range);
int nvmac_unbind(nvmac_dev *d, uint64_t va, uint64_t range);

int nvmac_ctx_create(nvmac_dev *d, uint32_t engines, uint32_t *ctx, uint32_t *seq_sync);
int nvmac_ctx_destroy(nvmac_dev *d, uint32_t ctx);
int nvmac_ctx_status(nvmac_dev *d, uint32_t ctx, int *lost, uint32_t *except_type, uint64_t *completed);
int nvmac_exec(nvmac_dev *d, uint32_t ctx,
               const struct nvmac_sync_point *waits, uint32_t n_wait,
               const struct nvmac_push *pushes, uint32_t n_push,
               const struct nvmac_sync_point *signals, uint32_t n_signal, uint64_t *seqno);

int nvmac_sync_create(nvmac_dev *d, uint64_t initial, uint32_t *sync);
int nvmac_sync_destroy(nvmac_dev *d, uint32_t sync);
int nvmac_sync_signal(nvmac_dev *d, uint32_t sync, uint64_t value);
// Current value, read from the mapped sync page (no system call).
uint64_t nvmac_sync_value(nvmac_dev *d, uint32_t sync);
// Waits until all (or any) entries reach their values; timeout_ns relative (UINT64_MAX: forever).
int nvmac_sync_wait(nvmac_dev *d, const struct nvmac_sync_point *pts, uint32_t count, int all,
                    uint64_t timeout_ns, uint32_t *signaled);

uint64_t nvmac_gpu_timestamp(nvmac_dev *d);

#ifdef __cplusplus
}
#endif
