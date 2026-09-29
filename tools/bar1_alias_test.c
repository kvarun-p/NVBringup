// Security test: does a CPU mapping of VRAM outlive NVMAC_MEM_FREE when the process has
// duplicated it? And do the driver's defences (held BAR1 slices, per-connection reuse, the
// hold cap) behave?
//
//   build/bar1_alias_test            remap, fork, reuse, remap-gart and fork-gart
//   build/bar1_alias_test remap|fork|reuse|cap|remap-gart|fork-gart
//
// remap: this process allocates 64 KiB of CPU-mappable VRAM, maps it, fills it with pattern
//   A, duplicates the mapping with mach_vm_remap (shared, not copied) and frees the memory. A
//   second process (this program, spawned with "victim") allocates the same size and fills it
//   with pattern B. This process then probes the duplicate (below); the victim checks
//   whether pattern X reached its memory.
// fork: the same with the duplicate in a forked child (vm_inherit VM_INHERIT_SHARE). The
//   parent frees the memory, closes its connection, and allocates pattern B through a new
//   connection before the child probes; the parent checks its B afterwards.
// reuse: 200 rounds of alloc / map / fill / free on one connection, alternating 64 and
//   128 KiB. Each new mapping must read zeroes (fresh, scrubbed VRAM) and keep what is
//   written (not the dummy page); BAR1 in use (GET_INFO bar1_used) must not grow after the
//   first two rounds, which is what reusing the connection's own held slices gives.
// cap: maps VRAM without freeing until MEM_MAP refuses (kIOReturnNoResources), which must
//   happen by half the BAR1 window; after freeing it all, a new mapping must still work
//   (reuse). NOT run by default: the held BAR1 stays held, so every other process's CPU
//   mappings of VRAM fail until the driver is reloaded.
// remap-gart, fork-gart: remap and fork with system memory (NVMAC_MEM_GART) instead of
//   VRAM. The other side allocates 256 buffers (16 MiB) of B, so freed pages likely come
//   back to it if the kernel releases them while a duplicate still maps them.
//
// Probing a duplicate: read it, write X through it, read it again. VRAM: SAFE when it faults
// or reads only zeroes both times (the driver's read-only dummy page); VULNERABLE when it
// shows B (another connection's memory) or X sticks (the dummy page is writable, a channel
// between processes); INCONCLUSIVE otherwise. A duplicate is never touched before the other
// allocation exists, so no access goes to an unmapped part of BAR1. System memory: SAFE when
// it faults, reads zeroes, or still reads its own A (the duplicate keeps the old pages alive;
// then X sticking is fine, those pages are its own); VULNERABLE when it shows B or anything
// else (the pages went back to the system while still mapped; X is then not written, the
// pages could be the kernel's); INCONCLUSIVE for a mix of A and zeroes. In both cases the other side's memory must still hold all its B and no X.
//
// Exit status, worst over the tests run: 0 safe / pass, 1 vulnerable / fail, 2 inconclusive
// or error.

#include "libnvmac.h"

#include <IOKit/IOReturn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <setjmp.h>
#include <signal.h>
#include <stdlib.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define SIZE    0x10000ull
#define PAT_A   0xaaaaaaaaaaaaaaaaull
#define PAT_B   0xb0b0b0b0b0b0b0b0ull
#define PAT_X   0x5858585858585858ull
#define FLAGS   (NVMAC_MEM_VRAM | NVMAC_MEM_CAN_MAP)
#define SPRAY   256                     // the other side's buffers for system memory

static int gart;                        // remap-gart / fork-gart: system memory, not VRAM

static sigjmp_buf fault_jmp;

static void on_fault(int sig)
{
    siglongjmp(fault_jmp, sig);
}

static void fill(volatile uint64_t *p, uint64_t bytes, uint64_t v)
{
    for (uint64_t i = 0; i < bytes / 8; i++)
        p[i] = v;
}

static uint64_t count(volatile uint64_t *p, uint64_t bytes, uint64_t v)
{
    uint64_t n = 0;
    for (uint64_t i = 0; i < bytes / 8; i++)
        n += p[i] == v;
    return n;
}

static int worst(int a, int b)
{
    return a == 1 || b == 1 ? 1 : a > b ? a : b;
}

static int alloc_map(nvmac_dev *d, uint64_t size, uint32_t *mem, void **ptr, const char *who)
{
    uint64_t got;
    int err = nvmac_mem_alloc(d, size, 0x10000, gart ? NVMAC_MEM_GART : FLAGS, 0, mem, &got);
    if (!err && (err = nvmac_mem_map(d, *mem, ptr, NULL)))
        nvmac_mem_free(d, *mem);
    if (err)
        fprintf(stderr, "%s: alloc/map: 0x%x (%s)\n", who, err, nvmac_strerror(err));
    return err;
}

// Reads the duplicate, writes X through it, reads it again (see the top of the file).
static int probe(volatile uint64_t *p, const char *who)
{
    static uint64_t na, nb, nx, nz, other, first;
    static volatile int phase;
    na = nb = nx = nz = other = first = 0;
    phase = 0;
    signal(SIGBUS, on_fault);
    signal(SIGSEGV, on_fault);
    signal(SIGALRM, on_fault);          // an access blocked in vm_fault (e.g. redirected to nothing)
    int result;
    int sig = sigsetjmp(fault_jmp, 1);
    if (sig) {
        printf("%s: duplicate %s %s: SAFE\n", who, sig == SIGALRM ? "blocked for 10 s" : "faulted",
               phase == 0 ? "reading" : phase == 1 ? "writing X" : "re-reading");
        result = 0;
        goto out;
    }
    alarm(10);
    for (uint64_t i = 0; i < SIZE / 8; i++) {
        uint64_t v = p[i];
        if (v == PAT_A)
            na++;
        else if (v == PAT_B)
            nb++;
        else if (v == 0)
            nz++;
        else if (!other++)
            first = v;
    }
    printf("%s: duplicate reads %llu words A, %llu B (another connection's), %llu zero, "
           "%llu other (first 0x%llx)\n", who, (unsigned long long)na, (unsigned long long)nb,
           (unsigned long long)nz, (unsigned long long)other, (unsigned long long)first);
    if (gart && (nb || other)) {       // pages that may be someone else's now: don't write
        printf("%s: a freed mapping of system memory reaches pages that aren't its own\n", who);
        result = 1;
        goto out;
    }
    phase = 1;
    fill(p, SIZE, PAT_X);
    phase = 2;
    nx = count(p, SIZE, PAT_X);
    nz = count(p, SIZE, 0);
    printf("%s: after writing X it reads %llu words X, %llu zero\n", who, (unsigned long long)nx,
           (unsigned long long)nz);
    if (gart)
        result = na == SIZE / 8 || nz == SIZE / 8 ? 0 : 2;
    else if (nb)
        result = 1;
    else if (nx) {
        printf("%s: writes through a freed mapping stick (the dummy page is writable?)\n", who);
        result = 1;
    } else
        result = nz == SIZE / 8 && !na && !other ? 0 : 2;
out:
    alarm(0);
    signal(SIGBUS, SIG_DFL);
    signal(SIGSEGV, SIG_DFL);
    signal(SIGALRM, SIG_DFL);
    return result;
}

static const char *verdict(int r)
{
    return r == 1 ? "VULNERABLE" : r == 0 ? "SAFE" : "INCONCLUSIVE";
}

// ---- remap --------------------------------------------------------------------------------

// The other side's memory, filled with B: one buffer of VRAM, or SPRAY buffers of system
// memory (see the top of the file).
struct side {
    uint32_t mem[SPRAY];
    void    *ptr[SPRAY];
    int      n;
};

static int side_alloc(nvmac_dev *d, struct side *s, const char *who)
{
    int want = gart ? SPRAY : 1;
    for (s->n = 0; s->n < want; s->n++) {
        if (alloc_map(d, SIZE, &s->mem[s->n], &s->ptr[s->n], who))
            break;
        fill(s->ptr[s->n], SIZE, PAT_B);
    }
    return s->n ? 0 : 2;
}

// 1 when some of it lost its B or took X.
static int side_check(nvmac_dev *d, struct side *s, const char *who)
{
    uint64_t b = 0, x = 0;
    for (int i = 0; i < s->n; i++) {
        b += count(s->ptr[i], SIZE, PAT_B);
        x += count(s->ptr[i], SIZE, PAT_X);
        nvmac_mem_free(d, s->mem[i]);
    }
    printf("%s: %llu of %llu words still B, %llu are X\n", who, (unsigned long long)b,
           (unsigned long long)(s->n * SIZE / 8), (unsigned long long)x);
    return x || b != s->n * SIZE / 8 ? 1 : 0;
}

// Spawned victim: allocate, fill with B, tell the parent, wait for it, report whether X arrived.
static int victim(int rd, int wr)
{
    nvmac_dev *d;
    static struct side s;
    int err = nvmac_open(&d);
    if (!err)
        err = side_alloc(d, &s, "victim");
    char c = err ? 'E' : 'R';
    if (write(wr, &c, 1) != 1 || err)
        return 2;
    if (read(rd, &c, 1) != 1)                   // parent has probed the duplicate
        return 2;
    int r = side_check(d, &s, "victim");
    nvmac_close(d);
    return r;
}

static int test_remap(const char *argv0)
{
    printf("== remap%s: duplicate made with mach_vm_remap, victim in another process\n", gart ? "-gart" : "");
    nvmac_dev *d;
    uint32_t mem;
    void *ptr;
    int err = nvmac_open(&d);
    if (err) {
        fprintf(stderr, "remap: open: 0x%x (%s)\n", err, nvmac_strerror(err));
        return 2;
    }
    if (alloc_map(d, SIZE, &mem, &ptr, "remap")) {
        nvmac_close(d);
        return 2;
    }
    fill(ptr, SIZE, PAT_A);

    mach_vm_address_t alias = 0;
    vm_prot_t cur, max;
    kern_return_t kr = mach_vm_remap(mach_task_self(), &alias, SIZE, 0, VM_FLAGS_ANYWHERE,
                                     mach_task_self(), (mach_vm_address_t)ptr, FALSE, &cur, &max,
                                     VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS) {
        printf("remap: mach_vm_remap refused (%s): SAFE\n", mach_error_string(kr));
        nvmac_close(d);
        return 0;
    }
    printf("remap: duplicate at 0x%llx reads %llu words of A\n", (unsigned long long)alias,
           (unsigned long long)count((volatile uint64_t *)alias, SIZE, PAT_A));
    if ((err = nvmac_mem_free(d, mem))) {
        fprintf(stderr, "remap: free: 0x%x (%s)\n", err, nvmac_strerror(err));
        nvmac_close(d);
        return 2;
    }

    int to[2], from[2];
    if (pipe(to) || pipe(from))
        return 2;
    char a1[16], a2[16];
    snprintf(a1, sizeof(a1), "%d", to[0]);
    snprintf(a2, sizeof(a2), "%d", from[1]);
    char *args[] = { (char *)argv0, gart ? "victim-gart" : "victim", a1, a2, NULL };
    pid_t pid;
    fflush(NULL);
    if (posix_spawn(&pid, argv0, NULL, NULL, args, environ)) {
        perror("remap: posix_spawn");
        return 2;
    }
    char c;
    if (read(from[0], &c, 1) != 1 || c != 'R') {
        fprintf(stderr, "remap: victim failed\n");
        waitpid(pid, NULL, 0);
        return 2;
    }
    int result = probe((volatile uint64_t *)alias, "remap");
    c = 'W';
    if (write(to[1], &c, 1) != 1)
        return 2;
    int st = 0;
    waitpid(pid, &st, 0);
    int vr = WIFEXITED(st) ? WEXITSTATUS(st) : 2;
    result = worst(result, vr);
    printf("remap: %s\n", verdict(result));
    nvmac_close(d);
    return result;
}

// ---- fork ---------------------------------------------------------------------------------

// Forked child: holds the inherited mapping; never touches IOKit.
static void fork_child(volatile uint64_t *p, int rd, int wr)
{
    char c = count(p, SIZE, PAT_A) == SIZE / 8 ? 'A' : 'N';
    if (write(wr, &c, 1) != 1 || c != 'A')
        _exit(2);
    if (read(rd, &c, 1) != 1 || c != 'G')      // parent freed, reconnected, allocated B
        _exit(2);
    int r = probe(p, "fork child");
    fflush(NULL);
    c = 'D';
    if (write(wr, &c, 1) != 1)
        _exit(2);
    _exit(r);
}

static int test_fork(void)
{
    printf("== fork%s: duplicate inherited by a child (VM_INHERIT_SHARE), owner's connection closed\n",
           gart ? "-gart" : "");
    nvmac_dev *d;
    uint32_t mem;
    void *ptr;
    int err = nvmac_open(&d);
    if (err) {
        fprintf(stderr, "fork: open: 0x%x (%s)\n", err, nvmac_strerror(err));
        return 2;
    }
    if (alloc_map(d, SIZE, &mem, &ptr, "fork")) {
        nvmac_close(d);
        return 2;
    }
    fill(ptr, SIZE, PAT_A);
    kern_return_t kr = vm_inherit(mach_task_self(), (vm_address_t)ptr, SIZE, VM_INHERIT_SHARE);
    if (kr != KERN_SUCCESS) {
        printf("fork: vm_inherit refused (%s): SAFE\n", mach_error_string(kr));
        nvmac_close(d);
        return 0;
    }

    int to[2], from[2];
    if (pipe(to) || pipe(from))
        return 2;
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork: fork");
        return 2;
    }
    if (pid == 0)
        fork_child((volatile uint64_t *)ptr, to[0], from[1]);

    char c;
    if (read(from[0], &c, 1) != 1 || c != 'A') {
        printf("fork: the child's copy doesn't show A (not shared): INCONCLUSIVE\n");
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        nvmac_close(d);
        return 2;
    }
    printf("fork: child shares the mapping; freeing it and closing the connection\n");
    nvmac_mem_free(d, mem);
    nvmac_close(d);

    int result = 2;
    static struct side s;
    if ((err = nvmac_open(&d))) {
        fprintf(stderr, "fork: reopen: 0x%x (%s)\n", err, nvmac_strerror(err));
        d = NULL;
    } else if (!side_alloc(d, &s, "fork")) {
        c = 'G';
        int done = write(to[1], &c, 1) == 1 && read(from[0], &c, 1) == 1 && c == 'D';
        int r = side_check(d, &s, "fork: new connection");
        if (done)
            result = r;
    }
    if (result == 2)
        kill(pid, SIGKILL);
    int st = 0;
    waitpid(pid, &st, 0);
    result = worst(result, WIFEXITED(st) ? WEXITSTATUS(st) : 2);
    printf("fork: %s\n", verdict(result));
    if (d)
        nvmac_close(d);
    return result;
}

// ---- reuse --------------------------------------------------------------------------------

static int test_reuse(void)
{
    enum { ROUNDS = 200 };
    printf("== reuse: %d rounds of alloc/map/free on one connection\n", ROUNDS);
    nvmac_dev *d;
    int err = nvmac_open(&d);
    if (err) {
        fprintf(stderr, "reuse: open: 0x%x (%s)\n", err, nvmac_strerror(err));
        return 2;
    }
    int result = 0;
    uint64_t base = 0;
    for (int i = 0; i < ROUNDS; i++) {
        uint64_t size = i & 1 ? 2 * SIZE : SIZE;
        uint32_t mem;
        void *ptr;
        if (alloc_map(d, size, &mem, &ptr, "reuse")) {
            printf("reuse: round %d failed\n", i);
            result = 1;
            break;
        }
        volatile uint64_t *p = ptr;
        uint64_t zero = count(p, size, 0);
        uint64_t pat = 0x1000000000000000ull | (uint64_t)i;
        fill(p, size, pat);
        uint64_t kept = count(p, size, pat);
        nvmac_mem_free(d, mem);
        if (zero != size / 8 || kept != size / 8) {
            printf("reuse: round %d (%llu KiB): %llu of %llu words zero when mapped, %llu kept "
                   "what was written\n", i, (unsigned long long)(size >> 10), (unsigned long long)zero,
                   (unsigned long long)(size / 8), (unsigned long long)kept);
            result = 1;
            break;
        }
        if (i == 1) {
            if ((err = nvmac_refresh_info(d)))
                break;
            base = nvmac_info(d)->bar1_used;
        }
    }
    if (result == 0 && !err && !(err = nvmac_refresh_info(d))) {
        uint64_t used = nvmac_info(d)->bar1_used;
        int64_t grew = (int64_t)(used - base);
        printf("reuse: BAR1 in use after round 1: %llu KiB, after round %d: %llu KiB\n",
               (unsigned long long)(base >> 10), ROUNDS - 1, (unsigned long long)(used >> 10));
        if (grew <= 0)
            result = 0;
        else if ((uint64_t)grew >= (ROUNDS - 2) / 2 * 3 * SIZE / 2)
            result = 1;                         // most rounds took a new slice: no reuse
        else
            result = 2;                         // other processes using BAR1 meanwhile?
    }
    if (err) {
        fprintf(stderr, "reuse: info: 0x%x (%s)\n", err, nvmac_strerror(err));
        result = 2;
    }
    printf("reuse: %s\n", result == 0 ? "PASS" : result == 1 ? "FAIL" : "INCONCLUSIVE");
    nvmac_close(d);
    return result;
}

// ---- cap ----------------------------------------------------------------------------------

static int test_cap(void)
{
    printf("== cap: mapping VRAM until the hold cap refuses\n"
           "   (held BAR1 stays held: other processes' CPU mappings of VRAM fail until the driver reloads)\n");
    nvmac_dev *d;
    int err = nvmac_open(&d);
    if (err) {
        fprintf(stderr, "cap: open: 0x%x (%s)\n", err, nvmac_strerror(err));
        return 2;
    }
    uint64_t window = nvmac_info(d)->bar1_size;
    uint64_t chunk = (window / 256 + 0xfffff) & ~0xfffffull;
    if (chunk < (1ull << 20))
        chunk = 1ull << 20;
    uint32_t max = (uint32_t)(window / chunk) + 2, n = 0;
    uint32_t *mems = calloc(max, sizeof(*mems));
    if (!mems)
        return 2;
    printf("cap: BAR1 window %llu MiB, mapping %llu MiB at a time\n", (unsigned long long)(window >> 20),
           (unsigned long long)(chunk >> 20));

    int result = 2;
    while (n < max) {
        uint64_t got;
        void *ptr;
        if ((err = nvmac_mem_alloc(d, chunk, 0x10000, FLAGS, 0, &mems[n], &got))) {
            printf("cap: allocation %u failed first: 0x%x (%s)\n", n, err, nvmac_strerror(err));
            break;
        }
        if ((err = nvmac_mem_map(d, mems[n], &ptr, NULL))) {
            nvmac_mem_free(d, mems[n]);
            printf("cap: mapping refused after %llu MiB: 0x%x (%s)\n",
                   (unsigned long long)((n * chunk) >> 20), err, nvmac_strerror(err));
            if (err == kIOReturnNoResources)
                result = n * chunk <= window / 2 ? 0 : 1;
            break;
        }
        n++;
    }
    if (n == max) {
        printf("cap: mapped the whole window without being refused\n");
        result = 1;
    }
    for (uint32_t i = 0; i < n; i++)
        nvmac_mem_free(d, mems[i]);
    free(mems);

    if (result == 0 && n) {                     // freed slices are this connection's to reuse
        uint32_t mem;
        void *ptr;
        if (alloc_map(d, chunk, &mem, &ptr, "cap")) {
            printf("cap: mapping after freeing everything failed: no reuse under the cap\n");
            result = 1;
        } else {
            printf("cap: mapping after freeing everything works (reused a held slice)\n");
            nvmac_mem_free(d, mem);
        }
    }
    printf("cap: %s\n", result == 0 ? "PASS" : result == 1 ? "FAIL" : "INCONCLUSIVE");
    nvmac_close(d);
    return result;
}

int main(int argc, char **argv)
{
    if (argc == 4 && !strncmp(argv[1], "victim", 6)) {
        gart = !strcmp(argv[1], "victim-gart");
        return victim(atoi(argv[2]), atoi(argv[3]));
    }
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (argc == 2) {
        if (!strcmp(argv[1], "remap"))
            return test_remap(argv[0]);
        if (!strcmp(argv[1], "fork"))
            return test_fork();
        if (!strcmp(argv[1], "reuse"))
            return test_reuse();
        if (!strcmp(argv[1], "cap"))
            return test_cap();
        gart = 1;
        if (!strcmp(argv[1], "remap-gart"))
            return test_remap(argv[0]);
        if (!strcmp(argv[1], "fork-gart"))
            return test_fork();
    }
    if (argc != 1) {
        fprintf(stderr, "usage: %s [remap|fork|reuse|cap|remap-gart|fork-gart]\n", argv[0]);
        return 2;
    }
    int r = test_remap(argv[0]);
    r = worst(r, test_fork());
    r = worst(r, test_reuse());
    gart = 1;
    r = worst(r, test_remap(argv[0]));
    r = worst(r, test_fork());
    printf("overall: %s\n", r == 0 ? "SAFE / PASS" : r == 1 ? "VULNERABLE / FAIL" : "INCONCLUSIVE");
    return r;
}
