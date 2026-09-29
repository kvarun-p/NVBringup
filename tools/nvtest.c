// nvtest: Phase 5 tests of the GPU interface from user space (libnvmac). Run as root:
//   sudo build/nvtest            all tests
//   sudo build/nvtest -v         also print details
// Groups: 5a memory and bindings, 5b contexts / submission / syncs, 5c robustness.
// Push buffers live in system memory (GART) bound into the GPU VA space.
#include "libnvmac.h"

#include <IOKit/IOReturn.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static int verbose, fails, passes;

#define CHECK(cond, ...) do { \
        if (cond) { passes++; if (verbose) { printf("    ok: "); printf(__VA_ARGS__); printf("\n"); } } \
        else { fails++; printf("    FAIL (%s:%d): ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
    } while (0)
#define TRY(call) do { int r_ = (call); if (r_) { fails++; \
        printf("    FAIL (%s:%d): %s -> 0x%x (%s)\n", __FILE__, __LINE__, #call, r_, nvmac_strerror(r_)); \
        return; } } while (0)

static double now_us(void)
{
    static mach_timebase_info_data_t tb;
    if (!tb.denom)
        mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1000.0;
}

// ---- Push buffer building (Volta+ incrementing method headers) ----------------------------

enum { SUBC_3D = 0, SUBC_COMPUTE = 1, SUBC_COPY = 4 };
enum { CLS_3D = 0xc597, CLS_COMPUTE = 0xc5c0, CLS_COPY = 0xc5b5 };

struct pb { uint32_t *p; uint32_t n; };

static void mthd(struct pb *b, uint32_t subc, uint32_t m, uint32_t count, ...)
{
    b->p[b->n++] = (1u << 29) | (count << 16) | (subc << 13) | (m >> 2);
    va_list ap;
    va_start(ap, count);
    for (uint32_t i = 0; i < count; i++)
        b->p[b->n++] = va_arg(ap, uint32_t);
    va_end(ap);
}

static void copy_cmd(struct pb *b, uint64_t src, uint64_t dst, uint32_t bytes)
{
    mthd(b, SUBC_COPY, 0x400, 4, (uint32_t)(src >> 32), (uint32_t)src, (uint32_t)(dst >> 32), (uint32_t)dst);
    mthd(b, SUBC_COPY, 0x418, 2, bytes, 1);                          // LINE_LENGTH_IN, LINE_COUNT
    mthd(b, SUBC_COPY, 0x300, 1, 2u | (1u << 2) | (1u << 7) | (1u << 8));   // non-pipelined, flush, pitch, virtual
}

// SET_REPORT_SEMAPHORE_A..D (same offsets in the compute and 3D classes): one-word release.
static void report_sem(struct pb *b, uint32_t subc, uint64_t va, uint32_t value)
{
    mthd(b, subc, 0x1b00, 4, (uint32_t)(va >> 32), (uint32_t)va, value, 0x10000000u);
}

// ---- A test device: one connection, a GART scratch area bound at the start of user VA ----

struct tdev {
    nvmac_dev *d;
    uint32_t scratch;           // GART, 4 MiB: pushes at 0, semaphores at 3 MiB
    uint8_t *cpu;
    uint64_t va, vaNext;
    uint32_t pushOff;
};

#define SCRATCH_SIZE (4u << 20)
#define SEM_OFF      (3u << 20)

static int tdev_open(struct tdev *t)
{
    memset(t, 0, sizeof(*t));
    int r = nvmac_open(&t->d);
    if (r)
        return r;
    t->va = nvmac_info(t->d)->va_start;
    uint64_t size;
    if ((r = nvmac_mem_alloc(t->d, SCRATCH_SIZE, 0, NVMAC_MEM_GART, 0, &t->scratch, &size)) ||
        (r = nvmac_mem_map(t->d, t->scratch, (void **)&t->cpu, NULL)) ||
        (r = nvmac_bind(t->d, t->va, t->scratch, 0, SCRATCH_SIZE)))
        return r;
    t->vaNext = t->va + SCRATCH_SIZE + 0x100000;
    return 0;
}

static uint64_t tdev_va(struct tdev *t, uint64_t size)
{
    uint64_t v = (t->vaNext + 0xffff) & ~0xffffull;
    t->vaNext = v + size + 0x10000;            // leave holes between buffers
    return v;
}

// A push buffer (up to 64 words) in the scratch area; 12288 slots wrap around within the
// first 3 MiB, far more than the kernel lets be in flight (63 EXECs per context).
static struct pb tdev_pb(struct tdev *t, uint64_t *va)
{
    if (t->pushOff + 0x100 > SEM_OFF)
        t->pushOff = 0;
    struct pb b = { (uint32_t *)(t->cpu + t->pushOff), 0 };
    *va = t->va + t->pushOff;
    t->pushOff += 0x100;
    return b;
}

static int submit(struct tdev *t, uint32_t ctx, struct pb *b, uint64_t va,
                  const struct nvmac_sync_point *waits, uint32_t nw,
                  const struct nvmac_sync_point *sigs, uint32_t ns, uint64_t *seq)
{
    struct nvmac_push p = { va, b->n * 4, 0 };
    return nvmac_exec(t->d, ctx, waits, nw, &p, 1, sigs, ns, seq);
}

static int wait_seq(struct tdev *t, uint32_t seqSync, uint64_t seq, uint64_t timeout_ms)
{
    struct nvmac_sync_point w = { seqSync, 0, seq };
    return nvmac_sync_wait(t->d, &w, 1, 1, timeout_ms * 1000000ull, NULL);
}

// ---- 5a: memory and bindings -----------------------------------------------------------------

static void test_info(struct tdev *t)
{
    printf("[5a] device info\n");
    const struct nvmac_info *i = nvmac_info(t->d);
    printf("    %s: device %04x chipset 0x%x rev %02x, sm%u, %u GPC / %u TPC / %u SM per TPC, %u warps/SM\n",
           i->name, i->device_id, i->chipset, i->revision, i->sm, i->gpc_count, i->tpc_count, i->mp_per_tpc,
           i->max_warps_per_mp);
    printf("    VRAM %llu MiB (%llu MiB used), BAR1 window %llu MiB, user VA 0x%llx..0x%llx\n",
           (unsigned long long)(i->vram_size >> 20), (unsigned long long)(i->vram_used >> 20),
           (unsigned long long)(i->bar1_size >> 20), (unsigned long long)i->va_start, (unsigned long long)i->va_end);
    CHECK(i->sm == 75, "shader model 75 (got %u)", i->sm);
    CHECK(i->tpc_count > 0 && i->gpc_count > 0 && i->mp_per_tpc > 0, "GR unit counts present");
    CHECK(i->vram_size > (3ull << 30), "VRAM heap > 3 GiB");
    CHECK(i->bar1_size > (64ull << 20), "BAR1 window > 64 MiB");
    uint64_t t0 = nvmac_gpu_timestamp(t->d);
    usleep(10000);
    uint64_t t1 = nvmac_gpu_timestamp(t->d);
    CHECK(t1 > t0 && t1 - t0 > 5000000 && t1 - t0 < 100000000, "GPU timestamp advances ~10 ms (%.2f ms)",
          (double)(t1 - t0) / 1e6);
}

static void test_memory(struct tdev *t)
{
    printf("[5a] memory: GART, BAR1-mapped VRAM, scrubbing\n");
    uint32_t g, v;
    uint64_t size;
    uint8_t *pg, *pv;
    TRY(nvmac_mem_alloc(t->d, 1 << 20, 0, NVMAC_MEM_GART, 0, &g, &size));
    TRY(nvmac_mem_map(t->d, g, (void **)&pg, NULL));
    for (uint32_t i = 0; i < (1u << 20); i += 4)
        *(uint32_t *)(pg + i) = i ^ 0xa5a5a5a5;
    uint32_t bad = 0;
    for (uint32_t i = 0; i < (1u << 20); i += 4)
        bad += *(uint32_t *)(pg + i) != (i ^ 0xa5a5a5a5);
    CHECK(!bad, "GART 1 MiB written and read back by the CPU");

    TRY(nvmac_mem_alloc(t->d, 1 << 20, 0, NVMAC_MEM_VRAM | NVMAC_MEM_CAN_MAP, 0, &v, &size));
    TRY(nvmac_mem_map(t->d, v, (void **)&pv, NULL));
    uint32_t nz = 0;
    for (uint32_t i = 0; i < (1u << 20); i += 4)
        nz += *(volatile uint32_t *)(pv + i) != 0;
    CHECK(!nz, "new VRAM reads zero through BAR1 (scrubbed; %u non-zero words)", nz);
    double t0 = now_us();
    for (uint32_t i = 0; i < (1u << 20); i += 4)
        *(volatile uint32_t *)(pv + i) = i * 2654435761u;
    double t1 = now_us();
    bad = 0;
    for (uint32_t i = 0; i < (1u << 20); i += 4)
        bad += *(volatile uint32_t *)(pv + i) != i * 2654435761u;
    double t2 = now_us();
    CHECK(!bad, "VRAM 1 MiB through BAR1: write %.0f MB/s, read %.0f MB/s", 1.048576e6 / (t1 - t0) * 1e0,
          1.048576e6 / (t2 - t1));
    void *again;
    TRY(nvmac_mem_map(t->d, v, &again, NULL));
    CHECK(again == pv, "second map returns the same address");
    uint32_t plain;
    TRY(nvmac_mem_alloc(t->d, 1 << 20, 0, NVMAC_MEM_VRAM, 0, &plain, &size));
    void *pp;
    CHECK(nvmac_mem_map(t->d, plain, &pp, NULL) == kIOReturnBadArgument, "VRAM without CAN_MAP is not mappable");
    CHECK(nvmac_mem_alloc(t->d, 4096, 0, NVMAC_MEM_VRAM | NVMAC_MEM_GART, 0, &plain, &size) == kIOReturnBadArgument,
          "VRAM|GART rejected");
    CHECK(nvmac_mem_free(t->d, 9999) == kIOReturnBadArgument, "free of a bad handle rejected");
    TRY(nvmac_mem_free(t->d, g));
    TRY(nvmac_mem_free(t->d, v));
}

static void test_bind_rules(struct tdev *t)
{
    printf("[5a] bindings: ranges, overlap, split\n");
    const struct nvmac_info *i = nvmac_info(t->d);
    uint32_t m;
    uint64_t size;
    TRY(nvmac_mem_alloc(t->d, 1 << 20, 0, NVMAC_MEM_VRAM, 0, &m, &size));
    CHECK(nvmac_bind(t->d, 0x04000000, m, 0, 0x1000) == kIOReturnBadArgument, "kernel region rejected");
    CHECK(nvmac_bind(t->d, 0x100000000ull, m, 0, 0x1000) == kIOReturnBadArgument, "GSP-RM region rejected");
    CHECK(nvmac_bind(t->d, i->va_end - 0x1000, m, 0, 0x2000) == kIOReturnBadArgument, "end of VA rejected");
    uint64_t va = tdev_va(t, 1 << 20);
    CHECK(nvmac_bind(t->d, va + 0x800, m, 0, 0x1000) == kIOReturnBadArgument, "unaligned rejected");
    CHECK(nvmac_bind(t->d, va, m, 0x1000, 1 << 20) == kIOReturnBadArgument, "beyond the memory rejected");
    TRY(nvmac_bind(t->d, va, m, 0, 1 << 20));
    CHECK(nvmac_bind(t->d, va + 0x10000, m, 0, 0x1000) == kIOReturnBadArgument, "overlap rejected");
    CHECK(nvmac_unbind(t->d, va + 0x11000, 0x1000) == kIOReturnBadArgument,
          "unbind inside a 64 KiB page rejected");
    TRY(nvmac_unbind(t->d, va + 0x10000, 0x10000));                 // split in two
    TRY(nvmac_bind(t->d, va + 0x10000, m, 0x80000, 0x10000));        // hole filled with other pages
    TRY(nvmac_unbind(t->d, va, 1 << 20));
    TRY(nvmac_bind(t->d, va, m, 0, 1 << 20));                        // everything unbound again
    TRY(nvmac_mem_free(t->d, m));                                    // unbinds implicitly
    uint32_t m2;
    TRY(nvmac_mem_alloc(t->d, 1 << 20, 0, NVMAC_MEM_VRAM, 0, &m2, &size));
    CHECK(nvmac_bind(t->d, va, m2, 0, 1 << 20) == 0, "range free again after MEM_FREE");
    TRY(nvmac_mem_free(t->d, m2));
}

// ---- 5b: contexts, submission, syncs ---------------------------------------------------------

// Copy engine: GART -> VRAM -> BAR1-visible VRAM, checked by the CPU. Returns 0 on success.
static int copy_roundtrip(struct tdev *t, uint32_t ctx, uint32_t seqSync, uint32_t bytes, int quiet)
{
    uint32_t src, mid, dst;
    uint64_t size;
    uint8_t *ps, *pd;
    int r;
    if ((r = nvmac_mem_alloc(t->d, bytes, 0, NVMAC_MEM_GART, 0, &src, &size)) ||
        (r = nvmac_mem_alloc(t->d, bytes, 0, NVMAC_MEM_VRAM, 0, &mid, &size)) ||
        (r = nvmac_mem_alloc(t->d, bytes, 0, NVMAC_MEM_VRAM | NVMAC_MEM_CAN_MAP, 0, &dst, &size)) ||
        (r = nvmac_mem_map(t->d, src, (void **)&ps, NULL)) || (r = nvmac_mem_map(t->d, dst, (void **)&pd, NULL)))
        return r;
    uint64_t vs = tdev_va(t, bytes), vm = tdev_va(t, bytes), vd = tdev_va(t, bytes);
    if ((r = nvmac_bind(t->d, vs, src, 0, bytes)) || (r = nvmac_bind(t->d, vm, mid, 0, bytes)) ||
        (r = nvmac_bind(t->d, vd, dst, 0, bytes)))
        return r;
    uint32_t salt = (uint32_t)rand();
    for (uint32_t i = 0; i < bytes; i += 4)
        *(uint32_t *)(ps + i) = (i * 0x01000193u) ^ salt;
    uint64_t pva;
    struct pb b = tdev_pb(t, &pva);
    mthd(&b, SUBC_COPY, 0, 1, CLS_COPY);
    copy_cmd(&b, vs, vm, bytes);
    copy_cmd(&b, vm, vd, bytes);
    uint64_t seq;
    if ((r = submit(t, ctx, &b, pva, NULL, 0, NULL, 0, &seq)) || (r = wait_seq(t, seqSync, seq, 5000)))
        return r;
    uint32_t bad = 0;
    for (uint32_t i = 0; i < bytes; i += 4)
        bad += *(volatile uint32_t *)(pd + i) != ((i * 0x01000193u) ^ salt);
    if (!quiet || bad)
        printf("    copy %u KiB GART -> VRAM -> VRAM(BAR1): %u bad words\n", bytes >> 10, bad);
    nvmac_mem_free(t->d, src);
    nvmac_mem_free(t->d, mid);
    nvmac_mem_free(t->d, dst);
    return bad ? kIOReturnIOError : 0;
}

static void test_copy_ctx(struct tdev *t)
{
    printf("[5b] copy-engine context\n");
    uint32_t ctx, ss;
    TRY(nvmac_ctx_create(t->d, NVMAC_ENGINE_COPY, &ctx, &ss));
    CHECK(copy_roundtrip(t, ctx, ss, 256 << 10, 0) == 0, "copy through a CE context");
    int lost;
    uint32_t ex;
    uint64_t done;
    TRY(nvmac_ctx_status(t->d, ctx, &lost, &ex, &done));
    CHECK(!lost && done == 1, "status: not lost, seqno 1 completed (got %llu)", (unsigned long long)done);
    TRY(nvmac_ctx_destroy(t->d, ctx));
    CHECK(nvmac_ctx_destroy(t->d, ctx) == kIOReturnBadArgument, "double destroy rejected");
}

static void test_gr_ctx(struct tdev *t)
{
    printf("[5b] graphics context: 3D + compute + copy (GRCE)\n");
    uint32_t ctx, ss;
    TRY(nvmac_ctx_create(t->d, NVMAC_ENGINE_3D | NVMAC_ENGINE_COMPUTE | NVMAC_ENGINE_COPY, &ctx, &ss));
    uint64_t semVa = t->va + SEM_OFF;
    volatile uint32_t *sem = (volatile uint32_t *)(t->cpu + SEM_OFF);
    sem[0] = sem[1] = 0;
    uint64_t pva, seq;
    struct pb b = tdev_pb(t, &pva);
    mthd(&b, SUBC_3D, 0, 1, CLS_3D);
    mthd(&b, SUBC_COMPUTE, 0, 1, CLS_COMPUTE);
    report_sem(&b, SUBC_COMPUTE, semVa, 0xc0de0001);
    report_sem(&b, SUBC_3D, semVa + 16, 0x3d000001);
    TRY(submit(t, ctx, &b, pva, NULL, 0, NULL, 0, &seq));
    int r = wait_seq(t, ss, seq, 5000);
    CHECK(r == 0, "exec completed (0x%x)", r);
    CHECK(sem[0] == 0xc0de0001, "compute class released its semaphore (0x%08x)", sem[0]);
    CHECK(sem[4] == 0x3d000001, "3D class released its semaphore (0x%08x)", sem[4]);
    CHECK(copy_roundtrip(t, ctx, ss, 64 << 10, 0) == 0, "copy class on the graphics channel");
    TRY(nvmac_ctx_destroy(t->d, ctx));
}

static void test_syncs(struct tdev *t)
{
    printf("[5b] syncs: CPU signal, GPU wait across contexts, timeout\n");
    uint32_t a, b2, sa, sb, x, y;
    TRY(nvmac_ctx_create(t->d, NVMAC_ENGINE_COPY, &a, &sa));
    TRY(nvmac_ctx_create(t->d, NVMAC_ENGINE_COMPUTE | NVMAC_ENGINE_3D, &b2, &sb));
    TRY(nvmac_sync_create(t->d, 0, &x));
    TRY(nvmac_sync_create(t->d, 0, &y));
    struct nvmac_sync_point wx = { x, 0, 5 }, sy = { y, 0, 7 }, wy = { y, 0, 7 };
    uint64_t pva, seq;
    struct pb pb1 = tdev_pb(t, &pva);
    mthd(&pb1, SUBC_COPY, 0, 1, CLS_COPY);
    TRY(submit(t, a, &pb1, pva, &wx, 1, &sy, 1, &seq));             // A: wait x >= 5, then y = 7
    int r = nvmac_sync_wait(t->d, &wy, 1, 1, 50000000ull, NULL);
    CHECK(r == kIOReturnTimeout, "y not signaled while x = 0 (wait timed out: 0x%x)", r);
    CHECK(nvmac_sync_value(t->d, y) == 0, "y still 0");
    // B: signals x = 5 from the GPU (a different context and engine)
    struct nvmac_sync_point sx = { x, 0, 5 };
    struct pb pb2 = tdev_pb(t, &pva);
    mthd(&pb2, SUBC_COMPUTE, 0, 1, CLS_COMPUTE);
    uint64_t seqB;
    TRY(submit(t, b2, &pb2, pva, NULL, 0, &sx, 1, &seqB));
    double t0 = now_us();
    r = nvmac_sync_wait(t->d, &wy, 1, 1, 2000000000ull, NULL);
    CHECK(r == 0 && nvmac_sync_value(t->d, y) == 7, "B's signal released A's GPU wait (%.0f us)", now_us() - t0);
    // CPU signal
    uint32_t z;
    TRY(nvmac_sync_create(t->d, 0, &z));
    struct nvmac_sync_point wz = { z, 0, 3 }, sz = { y, 0, 9 };
    struct pb pb3 = tdev_pb(t, &pva);
    mthd(&pb3, SUBC_COPY, 0, 1, CLS_COPY);
    TRY(submit(t, a, &pb3, pva, &wz, 1, &sz, 1, &seq));
    usleep(20000);
    CHECK(nvmac_sync_value(t->d, y) == 7, "A waits for the CPU");
    TRY(nvmac_sync_signal(t->d, z, 3));
    struct nvmac_sync_point wy9 = { y, 0, 9 };
    CHECK(nvmac_sync_wait(t->d, &wy9, 1, 1, 2000000000ull, NULL) == 0, "CPU signal released the GPU wait");
    TRY(nvmac_sync_signal(t->d, z, 1));
    CHECK(nvmac_sync_value(t->d, z) == 3, "CPU signals never move a sync backwards");
    // any-of wait
    struct nvmac_sync_point any[2] = { { x, 0, 100 }, { y, 0, 9 } };
    uint32_t which = 99;
    CHECK(nvmac_sync_wait(t->d, any, 2, 0, 1000000ull, &which) == 0 && which == 1, "any-of wait reports entry 1");
    CHECK(nvmac_sync_wait(t->d, any, 2, 1, 1000000ull, NULL) == kIOReturnTimeout, "all-of wait times out");
    nvmac_sync_destroy(t->d, x);
    nvmac_sync_destroy(t->d, y);
    nvmac_sync_destroy(t->d, z);
    nvmac_ctx_destroy(t->d, a);
    nvmac_ctx_destroy(t->d, b2);
}

static void test_ring(struct tdev *t)
{
    printf("[5b] 2000 submissions (ring and kernel push-slot wrap), latency\n");
    uint32_t ctx, ss;
    TRY(nvmac_ctx_create(t->d, NVMAC_ENGINE_COMPUTE | NVMAC_ENGINE_3D, &ctx, &ss));
    volatile uint32_t *sem = (volatile uint32_t *)(t->cpu + SEM_OFF + 64);
    uint64_t semVa = t->va + SEM_OFF + 64, seq = 0, pva;
    *sem = 0;
    struct pb init = tdev_pb(t, &pva);
    mthd(&init, SUBC_COMPUTE, 0, 1, CLS_COMPUTE);
    TRY(submit(t, ctx, &init, pva, NULL, 0, NULL, 0, &seq));
    // Throughput: 2000 execs, 4 pushes each, without waiting in between.
    double t0 = now_us();
    for (uint32_t i = 1; i <= 2000; i++) {
        struct nvmac_push p[4];
        for (int k = 0; k < 4; k++) {
            struct pb b = tdev_pb(t, &pva);
            report_sem(&b, SUBC_COMPUTE, semVa, i);
            p[k] = (struct nvmac_push){ pva, b.n * 4, 0 };
        }
        int r = nvmac_exec(t->d, ctx, NULL, 0, p, 4, NULL, 0, &seq);
        if (r) {
            CHECK(0, "exec %u: 0x%x (%s)", i, r, nvmac_strerror(r));
            break;
        }
    }
    double t1 = now_us();
    int r = wait_seq(t, ss, seq, 10000);
    double t2 = now_us();
    CHECK(r == 0 && *sem == 2000, "all 2000 done, semaphore %u, seqno %llu", *sem, (unsigned long long)seq);
    printf("    submit %.1f us/exec, drain %.0f us\n", (t1 - t0) / 2000, t2 - t1);
    // Latency: submit + wait, one at a time.
    double lat = 0, worst = 0;
    for (uint32_t i = 0; i < 200; i++) {
        struct pb b = tdev_pb(t, &pva);
        report_sem(&b, SUBC_COMPUTE, semVa, 5000 + i);
        double a0 = now_us();
        submit(t, ctx, &b, pva, NULL, 0, NULL, 0, &seq);
        wait_seq(t, ss, seq, 1000);
        double d = now_us() - a0;
        lat += d;
        if (d > worst)
            worst = d;
    }
    CHECK(*sem == 5199, "round trips complete");
    printf("    round trip (exec + wait): avg %.0f us, worst %.0f us\n", lat / 200, worst);
    nvmac_ctx_destroy(t->d, ctx);
}

static void test_bandwidth(struct tdev *t)
{
    printf("[5b] copy-engine bandwidth\n");
    uint32_t ctx, ss, a, b, g;
    uint64_t size, pva, seq;
    const uint32_t big = 256u << 20, gsz = 64u << 20;
    TRY(nvmac_ctx_create(t->d, NVMAC_ENGINE_COPY, &ctx, &ss));
    TRY(nvmac_mem_alloc(t->d, big, 0, NVMAC_MEM_VRAM, 0, &a, &size));
    TRY(nvmac_mem_alloc(t->d, big, 0, NVMAC_MEM_VRAM, 0, &b, &size));
    TRY(nvmac_mem_alloc(t->d, gsz, 0, NVMAC_MEM_GART, 0, &g, &size));
    uint64_t va = tdev_va(t, big), vb = tdev_va(t, big), vg = tdev_va(t, gsz);
    double t0 = now_us();
    TRY(nvmac_bind(t->d, va, a, 0, big));
    TRY(nvmac_bind(t->d, vb, b, 0, big));
    TRY(nvmac_bind(t->d, vg, g, 0, gsz));
    printf("    binding 576 MiB took %.0f ms\n", (now_us() - t0) / 1000);
    struct pb p = tdev_pb(t, &pva);
    mthd(&p, SUBC_COPY, 0, 1, CLS_COPY);
    copy_cmd(&p, va, vb, big);
    t0 = now_us();
    TRY(submit(t, ctx, &p, pva, NULL, 0, NULL, 0, &seq));
    TRY(wait_seq(t, ss, seq, 10000));
    double dt = now_us() - t0;
    printf("    VRAM -> VRAM 256 MiB: %.1f GB/s\n", big / dt / 1e3);
    p = tdev_pb(t, &pva);
    copy_cmd(&p, vg, va, gsz);
    t0 = now_us();
    TRY(submit(t, ctx, &p, pva, NULL, 0, NULL, 0, &seq));
    TRY(wait_seq(t, ss, seq, 10000));
    dt = now_us() - t0;
    printf("    system memory -> VRAM 64 MiB: %.1f GB/s\n", gsz / dt / 1e3);
    p = tdev_pb(t, &pva);
    copy_cmd(&p, va, vg, gsz);
    t0 = now_us();
    TRY(submit(t, ctx, &p, pva, NULL, 0, NULL, 0, &seq));
    TRY(wait_seq(t, ss, seq, 10000));
    dt = now_us() - t0;
    printf("    VRAM -> system memory 64 MiB: %.1f GB/s\n", gsz / dt / 1e3);
    CHECK(1, "bandwidth measured");
    t0 = now_us();
    nvmac_mem_free(t->d, a);
    nvmac_mem_free(t->d, b);
    nvmac_mem_free(t->d, g);
    printf("    freeing (unbind) took %.0f ms\n", (now_us() - t0) / 1000);
    nvmac_ctx_destroy(t->d, ctx);
}

// ---- 5c: robustness ------------------------------------------------------------------------------

static void test_invalid(struct tdev *t)
{
    printf("[5c] invalid submissions\n");
    uint32_t ctx, ss;
    TRY(nvmac_ctx_create(t->d, NVMAC_ENGINE_COPY, &ctx, &ss));
    struct nvmac_push p = { 0x04000000, 64, 0 };
    CHECK(nvmac_exec(t->d, ctx, NULL, 0, &p, 1, NULL, 0, NULL) == kIOReturnBadArgument, "push in the kernel region");
    p.va = t->va + 2;
    CHECK(nvmac_exec(t->d, ctx, NULL, 0, &p, 1, NULL, 0, NULL) == kIOReturnBadArgument, "unaligned push");
    p = (struct nvmac_push){ t->va, 64, 0x80 };
    CHECK(nvmac_exec(t->d, ctx, NULL, 0, &p, 1, NULL, 0, NULL) == kIOReturnBadArgument, "unknown push flag");
    struct nvmac_sync_point bogus = { 1999, 0, 1 };
    p.flags = 0;
    CHECK(nvmac_exec(t->d, ctx, &bogus, 1, &p, 1, NULL, 0, NULL) == kIOReturnBadArgument, "unknown sync");
    struct nvmac_sync_point seqsig = { ss, 0, 100 };
    CHECK(nvmac_exec(t->d, ctx, NULL, 0, &p, 1, &seqsig, 1, NULL) == kIOReturnBadArgument,
          "signaling a context's seqno sync");
    CHECK(nvmac_exec(t->d, 77, NULL, 0, &p, 1, NULL, 0, NULL) == kIOReturnBadArgument, "unknown context");
    CHECK(nvmac_ctx_create(t->d, 1u << 6, &ctx, &ss) == kIOReturnBadArgument, "unknown engine");
    nvmac_ctx_destroy(t->d, ctx);
}

// A context that faults (copy to an unbound VA) is lost; its connection reports device lost;
// other connections carry on.
static void test_fault(struct tdev *main)
{
    printf("[5c] GPU fault in another connection\n");
    struct tdev t;
    TRY(tdev_open(&t));
    uint32_t ctx, ss;
    TRY(nvmac_ctx_create(t.d, NVMAC_ENGINE_COPY, &ctx, &ss));
    uint64_t pva, seq;
    struct pb b = tdev_pb(&t, &pva);
    mthd(&b, SUBC_COPY, 0, 1, CLS_COPY);
    copy_cmd(&b, t.va, 0x7000000000ull, 4096);                     // unbound destination
    TRY(submit(&t, ctx, &b, pva, NULL, 0, NULL, 0, &seq));
    double t0 = now_us();
    int r = wait_seq(&t, ss, seq, 10000);
    CHECK(r == kIOReturnNoDevice, "wait reports device lost after %.0f ms (0x%x %s)", (now_us() - t0) / 1000, r,
          nvmac_strerror(r));
    int lost = 0;
    uint32_t ex = 0;
    nvmac_ctx_status(t.d, ctx, &lost, &ex, NULL);
    CHECK(lost, "context marked lost (exception type 0x%x)", ex);
    r = submit(&t, ctx, &b, pva, NULL, 0, NULL, 0, &seq);
    CHECK(r == kIOReturnNoDevice, "exec on a lost context refused (0x%x)", r);
    nvmac_close(t.d);
    uint32_t c2, s2;
    TRY(nvmac_ctx_create(main->d, NVMAC_ENGINE_COPY, &c2, &s2));
    CHECK(copy_roundtrip(main, c2, s2, 64 << 10, 1) == 0, "other connection still works");
    nvmac_ctx_destroy(main->d, c2);
}

// Child process for test_kill: allocates, submits work that blocks on a sync nobody signals,
// then sleeps until killed.
static int child_hang(void)
{
    struct tdev t;
    if (tdev_open(&t))
        return 1;
    uint32_t m, ctx, ss, x;
    uint64_t size, pva, seq;
    if (nvmac_mem_alloc(t.d, 64 << 20, 0, NVMAC_MEM_VRAM, 0, &m, &size) ||
        nvmac_bind(t.d, tdev_va(&t, 64 << 20), m, 0, 64 << 20) ||
        nvmac_ctx_create(t.d, NVMAC_ENGINE_COMPUTE | NVMAC_ENGINE_3D, &ctx, &ss) || nvmac_sync_create(t.d, 0, &x))
        return 2;
    struct nvmac_sync_point w = { x, 0, 1 };
    struct pb b = tdev_pb(&t, &pva);
    mthd(&b, SUBC_COMPUTE, 0, 1, CLS_COMPUTE);
    if (submit(&t, ctx, &b, pva, &w, 1, NULL, 0, &seq))
        return 3;
    printf("    child: 64 MiB allocated, context blocked on a GPU wait\n");
    fflush(stdout);
    for (;;)
        pause();
}

static void test_kill(struct tdev *main, const char *self)
{
    printf("[5c] process killed with a blocked context\n");
    nvmac_refresh_info(main->d);
    uint64_t before = nvmac_info(main->d)->vram_used;
    pid_t pid;
    char *argv[] = { (char *)self, (char *)"--child-hang", NULL };
    if (posix_spawn(&pid, self, NULL, NULL, argv, environ)) {
        CHECK(0, "spawn");
        return;
    }
    sleep(3);
    nvmac_refresh_info(main->d);
    uint64_t during = nvmac_info(main->d)->vram_used;
    kill(pid, SIGKILL);
    int st;
    waitpid(pid, &st, 0);
    uint64_t after = 0;
    for (int i = 0; i < 50; i++) {
        usleep(100000);
        nvmac_refresh_info(main->d);
        after = nvmac_info(main->d)->vram_used;
        if (after == before)
            break;
    }
    CHECK(during >= before + (64 << 20), "child held its VRAM (%+lld MiB)", (long long)(during - before) >> 20);
    CHECK(after == before, "all of it freed after SIGKILL (%+lld KiB left)", (long long)(after - before) >> 10);
    uint32_t c2, s2;
    TRY(nvmac_ctx_create(main->d, NVMAC_ENGINE_COMPUTE | NVMAC_ENGINE_3D | NVMAC_ENGINE_COPY, &c2, &s2));
    CHECK(copy_roundtrip(main, c2, s2, 64 << 10, 1) == 0, "GPU still works");
    nvmac_ctx_destroy(main->d, c2);
}

static void *thread_main(void *arg)
{
    int *result = arg;
    struct tdev t;
    uint32_t ctx, ss;
    if (tdev_open(&t) || nvmac_ctx_create(t.d, NVMAC_ENGINE_COPY, &ctx, &ss)) {
        *result = -1;
        return NULL;
    }
    for (int i = 0; i < 40 && !*result; i++)
        if (copy_roundtrip(&t, ctx, ss, 64 << 10, 1))
            *result = i + 1;
    nvmac_close(t.d);
    return NULL;
}

static void test_threads(void)
{
    printf("[5c] 6 connections in parallel threads, 40 copies each\n");
    pthread_t th[6];
    int res[6] = {0};
    for (int i = 0; i < 6; i++)
        pthread_create(&th[i], NULL, thread_main, &res[i]);
    int bad = 0;
    for (int i = 0; i < 6; i++) {
        pthread_join(th[i], NULL);
        bad += res[i] != 0;
    }
    CHECK(!bad, "all threads verified their copies (%d failed)", bad);
}

// Churn in a connection of its own: page tables stay with a VA space until it closes, so
// the VRAM check is made after closing it.
static void test_churn(struct tdev *main)
{
    printf("[5c] 400 allocate/bind/free cycles (own connection)\n");
    nvmac_refresh_info(main->d);
    uint64_t before = nvmac_info(main->d)->vram_used;
    struct tdev tt, *t = &tt;
    TRY(tdev_open(t));
    uint32_t live[16] = {0};
    int err = 0;
    for (int i = 0; i < 400 && !err; i++) {
        int k = rand() % 16;
        if (live[k]) {
            err = nvmac_mem_free(t->d, live[k]);
            live[k] = 0;
            continue;
        }
        uint64_t size = (uint64_t)((rand() % 64) + 1) << 14, got;
        uint32_t flags = rand() % 3 == 0 ? NVMAC_MEM_GART : rand() % 2 ? NVMAC_MEM_VRAM : NVMAC_MEM_VRAM | NVMAC_MEM_CAN_MAP;
        if ((err = nvmac_mem_alloc(t->d, size, 0, flags, 0, &live[k], &got)))
            break;
        err = nvmac_bind(t->d, tdev_va(t, got), live[k], 0, got);
    }
    nvmac_refresh_info(main->d);
    uint64_t during = nvmac_info(main->d)->vram_used;
    nvmac_close(t->d);                          // live objects are freed by the close
    nvmac_refresh_info(main->d);
    uint64_t after = nvmac_info(main->d)->vram_used;
    CHECK(!err, "no errors (0x%x)", err);
    // CAN_MAP VRAM is also mapped into BAR1, whose page tables are shared by all connections
    // and kept until unload: one 4 KiB table per 2 MiB of BAR1 window at most may appear.
    uint64_t bar1Pts = (nvmac_info(main->d)->bar1_size >> 21) << 12;
    CHECK(after >= before && after - before <= bar1Pts && !((after - before) & 0xfff),
          "VRAM usage back to where it was after close (%+lld KiB while open, %+lld KiB after; "
          "up to %llu KiB of shared BAR1 page tables allowed)",
          (long long)(during - before) >> 10, (long long)(after - before) >> 10,
          (unsigned long long)bar1Pts >> 10);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--child-hang"))
        return child_hang();
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    setvbuf(stdout, NULL, _IOLBF, 0);
    srand(1234);
    struct tdev t;
    int r = tdev_open(&t);
    if (r) {
        fprintf(stderr, "nvtest: open failed: 0x%x (%s)%s\n", r, nvmac_strerror(r),
                r == (int)kIOReturnNotPrivileged ? " - run with sudo" : "");
        return 1;
    }
    test_info(&t);
    test_memory(&t);
    test_bind_rules(&t);
    test_copy_ctx(&t);
    test_gr_ctx(&t);
    test_syncs(&t);
    test_ring(&t);
    test_bandwidth(&t);
    test_invalid(&t);
    test_fault(&t);
    test_kill(&t, argv[0]);
    test_threads();
    test_churn(&t);
    nvmac_close(t.d);
    printf("\nnvtest: %d checks passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
