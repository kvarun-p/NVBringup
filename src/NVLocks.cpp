// The driver's locks: gspLock_ alone, or with dispLock_ in front of it (boot-arg nvlocksplit=1), with hold and wait
// statistics for each (NVLockStats) and kdebug points (nv_trace.h LOCK_WAIT, LOCK_HOLD). The layering and the order
// are in NVBringup.hpp ("Locks").
//
// Why: under one lock the display's modeset (relight retries sleeping 200 ms up to five times, channel and
// notifier waits, the SCDC settle) held gspLock_ for seconds, and every EXEC, memory call and the message poller
// waited behind it. With the split the display flows give gspLock_ up across their sleeps (dispSleep) and keep
// dispLock_, so they exclude each other but not the GPU work. The GSP queues stay under gspLock_ (one RPC at a
// time, as in NVIDIA's RM), so no RPC or event handler ever runs concurrently with another.

#include "nv_gsp_state.h"
#include "nv_trace.h"

#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>

#define LOG(fmt, ...) log(fmt "\n", ##__VA_ARGS__)

static const char *const kLockNames[NVBringup::kLockCount] = { "gsp", "disp" };

static uint64_t absUs(uint64_t abs)
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(abs, &ns);
    return ns / 1000;
}

// start(), after bootGuard (safe mode turns the split off) and before anything can boot GSP-RM.
void NVBringup::lockInit()
{
    uint32_t arg = 0;
    lockSplit_ = PE_parse_boot_argn("nvlocksplit", &arg, sizeof(arg)) && arg == 1;
    if (lockSplit_ && safeMode_) {
        LOG("locks: nvlocksplit=1 ignored in safe mode");
        lockSplit_ = false;
    }
    if (lockSplit_ && !(dispLock_ = IOLockAlloc())) {
        LOG("locks: no memory for the display lock: one GSP lock");
        lockSplit_ = false;
    }
    LOG("locks: %s", lockSplit_ ? "split (nvlocksplit=1): display lock, then GSP lock" : "one GSP lock");
    setProperty("NVLockSplit", lockSplit_);
}

void NVBringup::lockFree()
{
    if (dispLock_) {
        IOLock *l = dispLock_;
        dispLock_ = nullptr;
        IOLockFree(l);
    }
}

void NVBringup::lockTake(uint32_t id, const char *site)
{
    IOLock *l = lockOf(id);
    LockState &s = lockState_[id];
    const uint64_t t0 = mach_absolute_time();
    const bool waited = !IOLockTryLock(l);
    const char *holder = waited ? s.site : nullptr;     // racy read; for the statistics only
    if (waited) {
        __atomic_add_fetch(&s.waiters, 1, __ATOMIC_RELAXED);
        IOLockLock(l);
        __atomic_sub_fetch(&s.waiters, 1, __ATOMIC_RELAXED);
    }
    const uint64_t t1 = mach_absolute_time();
    s.owner = current_thread();
    s.since = t1;
    s.site = site;
    s.st.acquired++;
    if (waited) {
        const uint64_t w = t1 - t0;
        s.st.contended++;
        s.st.waitAbs += w;
        if (w > s.st.maxWaitAbs) {
            s.st.maxWaitAbs = w;
            s.st.maxWaitSite = site;
            s.st.maxWaitHolder = holder ? holder : "";
        }
        NV_TRACE(NV_TRACE_LOCK_WAIT, DBG_FUNC_NONE, id, w, 0, 0);
    }
}

void NVBringup::lockDrop(uint32_t id)
{
    LockState &s = lockState_[id];
    const uint64_t h = mach_absolute_time() - s.since;
    if (h > s.st.maxHoldAbs) {
        s.st.maxHoldAbs = h;
        s.st.maxHoldSite = s.site;
    }
    const uint64_t us = absUs(h);
    if (us > 10000) {
        s.st.slowHolds++;
        LockStat::Slow *e = nullptr;
        for (LockStat::Slow &x : s.st.slow)
            if (!x.site || !strcmp(x.site, s.site)) {
                e = &x;
                break;
            }
        if (e) {                                // the first eight sites; later ones only in slowHolds
            e->site = s.site;
            e->count++;
            if (h > e->maxAbs)
                e->maxAbs = h;
        }
    }
    if (us > 1000)
        NV_TRACE(NV_TRACE_LOCK_HOLD, DBG_FUNC_NONE, id, h, 0, 0);
    s.owner = nullptr;
    IOLockUnlock(lockOf(id));
}

// A display flow's entry: dispLock_, then gspLock_ (split), or gspLock_ alone. Not nested, not under gspLock_.
void NVBringup::dispEnter(const char *site)
{
    if (lockSplit_) {
        lockTake(kLockDisp, site);
        dispYield_ = true;
    }
    lockTake(kLockGsp, site);
}

void NVBringup::dispExit()
{
    lockDrop(kLockGsp);
    if (lockSplit_) {
        dispYield_ = false;
        lockDrop(kLockDisp);
    }
}

// A display flow's sleep: gspLock_ given up for it when this thread entered through dispEnter (split), so GPU work
// goes on meanwhile. The flow's own state is dispLock_'s; anything it read from GPU state before the sleep may
// have changed (contexts, heaps), but gsp_ and booted can't: boot and unload take dispLock_ too.
void NVBringup::dispSleep(uint32_t ms)
{
    if (lockSplit_ && dispYield_ && lockState_[kLockDisp].owner == current_thread()) {
        lockDrop(kLockGsp);
        IOSleep(ms);
        lockTake(kLockGsp, "dispSleep");
        return;
    }
    IOSleep(ms);
}

// GSP-RM boot, unload and free: gsp_ is replaced or goes, and nothing may be in the middle of a flow.
void NVBringup::allLock(const char *site)
{
    if (lockSplit_) {
        lockTake(kLockDisp, site);
        dispYield_ = false;
    }
    lockTake(kLockGsp, site);
}

void NVBringup::allUnlock()
{
    lockDrop(kLockGsp);
    if (lockSplit_)
        lockDrop(kLockDisp);
}

// NVLockStats, per lock: acquisitions, how many waited (the longest wait and where, the total), the longest hold
// and where, and holds over 10 ms. Racy reads of counters the holders update: fine for a monitor.
// Called after allUnlock at the end of a successful boot, so the boot's own multi-second hold doesn't stay the
// maximum and hide the steady state. Counts stay cumulative. Unlocked: these are statistics.
void NVBringup::lockResetPeaks()
{
    for (LockState &s : lockState_) {
        s.st.maxWaitAbs = s.st.maxHoldAbs = s.st.slowHolds = 0;
        s.st.maxWaitSite = s.st.maxHoldSite = s.st.maxWaitHolder = "";
        for (LockStat::Slow &x : s.st.slow)
            x = LockStat::Slow();
    }
}

// For a long job done in pieces under gspLock_ (memAlloc's VRAM scrub): when other threads wait, drop the lock,
// let them take it (up to 1 ms for the first to get it), and take it back behind them. The caller rechecks
// whatever it relies on afterwards: anything gspLock_ guards may have changed. Not for display flows (dispSleep).
bool NVBringup::lockYield()
{
    LockState &s = lockState_[kLockGsp];
    const uint32_t w = __atomic_load_n(&s.waiters, __ATOMIC_RELAXED);
    if (!w)
        return false;
    const char *site = s.site;
    lockDrop(kLockGsp);
    for (int i = 0; i < 1000 && __atomic_load_n(&s.waiters, __ATOMIC_RELAXED) >= w; i++)
        IODelay(1);
    lockTake(kLockGsp, site);
    return true;
}

void NVBringup::lockPublish()
{
    char m[1024];
    int o = snprintf(m, sizeof(m), "%s: ", lockSplit_ ? "split" : "one lock");
    for (uint32_t id = 0; id < kLockCount && o < (int)sizeof(m); id++) {
        if (id != kLockGsp && !lockSplit_)
            break;
        const LockStat &st = lockState_[id].st;
        o += snprintf(m + o, sizeof(m) - (size_t)o,
                      "%s%s %llu taken, %llu waited (max %llu us in %s behind %s, %llu ms in all), held max %llu us "
                      "in %s, %llu over 10 ms",
                      id ? "; " : "", kLockNames[id], st.acquired, st.contended, absUs(st.maxWaitAbs), st.maxWaitSite,
                      *st.maxWaitHolder ? st.maxWaitHolder : "?", absUs(st.waitAbs) / 1000, absUs(st.maxHoldAbs),
                      st.maxHoldSite, st.slowHolds);
        for (uint32_t i = 0; i < 8 && st.slow[i].site && o < (int)sizeof(m); i++)
            o += snprintf(m + o, sizeof(m) - (size_t)o, "%s%s %ux max %llu ms", i ? ", " : " (", st.slow[i].site,
                          st.slow[i].count, absUs(st.slow[i].maxAbs) / 1000);
        if (st.slow[0].site && o < (int)sizeof(m))
            o += snprintf(m + o, sizeof(m) - (size_t)o, ")");
    }
    setProperty("NVLockStats", m);
}
