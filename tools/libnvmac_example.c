// libnvmac example: copy 64 KiB with the GPU's copy engine, from one system-memory buffer to
// another, and check the result on the CPU. Shows the whole cycle: open, allocate, map, bind,
// build a push buffer, submit, wait, free.
//
//   clang -std=c11 -Wall -Wextra -Isrc tools/libnvmac_example.c tools/libnvmac.c \
//         -framework IOKit -framework CoreFoundation -o build/libnvmac_example
//   build/libnvmac_example
#include "libnvmac.h"

#include <stdio.h>
#include <string.h>

#define SIZE     (64u << 10)
#define CLS_COPY 0xc5b5                 // TURING_DMA_COPY_A
#define SUBC     4                      // the copy object's subchannel

// Volta+ incrementing method header, then the method's data words.
static uint32_t *mthd(uint32_t *p, uint32_t method, uint32_t count, const uint32_t *data)
{
    *p++ = (1u << 29) | (count << 16) | (SUBC << 13) | (method >> 2);
    memcpy(p, data, count * 4);
    return p + count;
}

int main(void)
{
    nvmac_dev *d;
    int err = nvmac_open(&d);
    if (err) {
        fprintf(stderr, "open: %s\n", nvmac_strerror(err));
        return 1;
    }
    const struct nvmac_info *info = nvmac_info(d);
    printf("%s, %llu MiB VRAM\n", info->name, (unsigned long long)(info->vram_size >> 20));

    // Three system-memory buffers (always CPU-mappable): source, destination, push buffer.
    uint32_t mem[3];
    void *cpu[3];
    uint64_t va[3];
    for (int i = 0; i < 3; i++) {
        if ((err = nvmac_mem_alloc(d, SIZE, 0, NVMAC_MEM_GART, 0, &mem[i], NULL)) ||
            (err = nvmac_mem_map(d, mem[i], &cpu[i], NULL)))
            goto out;
        va[i] = info->va_start + i * SIZE;          // user space picks GPU VAs itself
        if ((err = nvmac_bind(d, va[i], mem[i], 0, SIZE)))
            goto out;
    }
    uint32_t *src = cpu[0], *dst = cpu[1];
    for (uint32_t i = 0; i < SIZE / 4; i++)
        src[i] = i * 2654435761u;

    // A copy-engine context: a GPU channel plus its completion timeline (seq_sync).
    uint32_t ctx, seq_sync;
    if ((err = nvmac_ctx_create(d, NVMAC_ENGINE_COPY, &ctx, &seq_sync)))
        goto out;

    uint32_t *p = cpu[2];
    p = mthd(p, 0x000, 1, (uint32_t[]){ CLS_COPY });                        // SET_OBJECT
    p = mthd(p, 0x400, 4, (uint32_t[]){ (uint32_t)(va[0] >> 32), (uint32_t)va[0],
                                         (uint32_t)(va[1] >> 32), (uint32_t)va[1] });  // OFFSET_IN/OUT
    p = mthd(p, 0x418, 2, (uint32_t[]){ SIZE, 1 });                         // LINE_LENGTH_IN, LINE_COUNT
    p = mthd(p, 0x300, 1, (uint32_t[]){ 2u | 1u << 2 | 1u << 7 | 1u << 8 }); // LAUNCH_DMA: non-pipelined,
                                                                            // flush, pitch, virtual
    struct nvmac_push push = { va[2], (uint32_t)((uint8_t *)p - (uint8_t *)cpu[2]), 0 };
    uint64_t seqno;
    if ((err = nvmac_exec(d, ctx, NULL, 0, &push, 1, NULL, 0, &seqno)))
        goto out_ctx;

    // Each EXEC advances the context's timeline; wait until it reaches this one (1 s timeout).
    struct nvmac_sync_point done = { seq_sync, 0, seqno };
    if ((err = nvmac_sync_wait(d, &done, 1, 1, 1000000000ull, NULL)))
        goto out_ctx;
    printf("copy %s\n", memcmp(src, dst, SIZE) ? "WRONG" : "ok");

out_ctx:
    nvmac_ctx_destroy(d, ctx);
out:
    if (err)
        fprintf(stderr, "error: 0x%x (%s)\n", err, nvmac_strerror(err));
    nvmac_close(d);                      // frees everything the connection still owns
    return err != 0;
}
