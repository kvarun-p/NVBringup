// Bring-up driver: attach to an NVIDIA GPU, power it on via ACPI if needed,
// map BAR0, identify the chip, and read + parse the VBIOS to locate FWSEC.
// Read-only unless boot-arg nvfwsec=1: then it runs FWSEC-FRTS on the GSP
// falcon, which writes falcon registers and uses DMA (see runFwsecFrts).
#pragma once

#include <IOKit/IOService.h>
#include "nv_uapi.h"
#include <IOKit/IOFilterInterruptEventSource.h>
#include <kern/thread_call.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/acpi/IOACPIPlatformDevice.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOUserClient.h>

#include "nv_fwsec.h"

class NVBringup : public IOService {
    OSDeclareDefaultStructors(NVBringup)

public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    bool didTerminate(IOService *provider, IOOptionBits options, bool *defer) override;
    void stop(IOService *provider) override;
    void free() override;

private:
    IOPCIDevice          *pci_  = nullptr;
    IOACPIPlatformDevice *acpi_ = nullptr;
    IOMemoryMap          *bar0_ = nullptr;
    bool opened_        = false;
    bool memWasEnabled_ = false;
    uint32_t chipset_   = 0;

    // Boot-time IOLog output is lost before logd starts, so every log line is
    // also kept here and published as the NVLog property (ioreg, no root needed).
    char     log_[65536];
    uint32_t logLen_ = 0;
    void log(const char *fmt, ...) __printflike(2, 3);

    // Phase 2
    void logConfigSpace();
    void logBars();
    bool identifyChip();
    uint32_t rd32(uint32_t offset) const;

    // Phase 3a: power
    void findAcpiNode();
    bool acpiHas(const char *method);
    int  pciPowerState();
    bool powerOn();

    // Phase 3b: VBIOS
    typedef bool (NVBringup::*RomFetch)(uint32_t offset, uint32_t length, uint8_t *out);
    bool fetchAcpiRom(uint32_t offset, uint32_t length, uint8_t *out);
    bool fetchProm(uint32_t offset, uint32_t length, uint8_t *out);
    bool readRom(const char *name, RomFetch fetch, uint8_t *buf, uint32_t *romLen);
    bool analyzeRom(const char *source, const uint8_t *buf, uint32_t len);
    void publishRom(const char *source, const uint8_t *buf, uint32_t len);
    void readVbios();

    // Phase 3c preparation: read-only. Parses FWSEC, reads VRAM size and WPR2
    // state, and logs the FRTS plan. Nothing is loaded or started.
    void prepareFwsec(const uint8_t *buf, uint32_t len);

    // Phase 3c step 2: FWSEC-FRTS on the GSP falcon via the generic bootloader.
    // Only runs with boot-arg nvfwsec=1 and after every check in prepareFwsec.
    // The falcon helpers take the falcon's register base: GSP (default) or SEC2.
    static const uint32_t NV_PGSP_BASE = 0x00110000;
    static const uint32_t NV_PSEC_BASE = 0x00840000;
    void wr32(uint32_t offset, uint32_t value);
    uint32_t grd(uint32_t off, uint32_t base = NV_PGSP_BASE) const { return rd32(base + off); }
    void gwr(uint32_t off, uint32_t value, uint32_t base = NV_PGSP_BASE) { wr32(base + off, value); }
    bool falconReset(uint32_t base = NV_PGSP_BASE);
    bool falconWaitHalted(uint32_t ms, uint32_t base = NV_PGSP_BASE);
    void falconStart(uint32_t base = NV_PGSP_BASE);
    // tag is in 256-byte units (IMEM block tags)
    void falconPioImem(const uint8_t *data, uint32_t len, uint32_t dst, uint32_t tag,
                       bool secure = false, uint32_t base = NV_PGSP_BASE);
    void falconPioDmem(const uint8_t *data, uint32_t len, uint32_t dst, uint32_t base = NV_PGSP_BASE);
    // cmd: NV_DMEMMAPPER_CMD_FRTS (boot, needs frts) or NV_DMEMMAPPER_CMD_SB (unload)
    bool runFwsec(const uint8_t *rom, const nv_fwsec &f, uint32_t cmd, uint64_t frts);
    bool frtsOk_ = false;       // FRTS succeeded this boot; GSP-RM boot requires it
    uint64_t frtsAddr_ = 0, vgaAddr_ = 0, vramSize_ = 0;

    // Phase 3d: GSP-RM boot, triggered through the user client (NVGsp.cpp).
public:
    enum { kFwGspElf, kFwGspBootloader, kFwBooterLoad, kFwBooterUnload, kFwCount };
    IOReturn setFirmware(uint32_t kind, IOMemoryDescriptor *src, uint64_t len);
    IOReturn bootGsp();
    IOReturn readGspLog(uint32_t index, IOMemoryDescriptor *dst, uint64_t *len);
    // Tears GSP-RM down and clears WPR2 (r570 kgspUnloadRm + kgspTeardown_TU102).
    IOReturn unloadGsp(const char *why);
    // Item 4: non-stall interrupts. mode 0 off, 1 on, 2 query; out: on, interrupts, spurious, storms.
    IOReturn setIntr(uint32_t mode, uint64_t out[4]);
    void systemWillShutdown(IOOptionBits specifier) override;

    // Phase 5: GPU client connections (NVGpu.cpp), one per NVGpuUserClient (open type 1).
    struct GpuConn;
    struct GpuCtx;
    struct GpuMem;
    IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
                           OSDictionary *properties, IOUserClient **handler) override;
    IOReturn gpuOpen(task_t task, GpuConn **out);   // powers the GPU on first if it is off (auto)
    // Runtime power (control client, IOServiceOpen type NVMAC_CONTROL_TYPE): see NVPower.cpp.
    IOReturn powerCall(uint32_t selector, IOExternalMethodArguments *a);
    void     gpuClose(GpuConn *c);      // frees the connection's GPU state (clientClose)
    void     gpuRelease(GpuConn *c);    // frees the connection itself (user client free)
    IOReturn gpuCall(GpuConn *c, uint32_t selector, IOExternalMethodArguments *a);
private:
    struct GspState;
    GspState *gsp_ = nullptr;
    IOLock   *gspLock_ = nullptr;
    void freeGsp(bool stopFalcons);
    IONotifier *sleepNotifier_ = nullptr;
    static IOReturn sleepHandler(void *target, void *refCon, UInt32 messageType, IOService *provider,
                                 void *messageArgument, vm_size_t argSize);
    IOReturn unloadGspLocked(const char *why);
    bool sendGspRpc(uint32_t function, const void *payload, uint32_t len);
    bool waitGspReply(uint32_t function, uint32_t ms, uint32_t *result,
                      uint8_t *buf, uint32_t cap, uint32_t *len);
    bool getGspStaticInfo();
    bool gspRmAlloc(uint32_t hClient, uint32_t hParent, uint32_t hObject, uint32_t hClass,
                    void *params, uint32_t size, uint32_t *status);
    bool gspRmControl(uint32_t hClient, uint32_t hObject, uint32_t cmd,
                      void *params, uint32_t size, uint32_t *status);
    bool gspRmFree(uint32_t hRoot, uint32_t hParent, uint32_t hObject, uint32_t *status);
    bool createRmObjects();
    // PRAMIN: CPU access to VRAM through BAR0 (Phase 4 step 3)
    bool     praminSaved_ = false;
    uint32_t praminOrig_ = 0, praminCur_ = 0;
    void     praminSelect(uint64_t vram);
    uint32_t praminRd32(uint64_t vram);
    void     praminWr32(uint64_t vram, uint32_t v);
    void     praminRestore();
    bool     testVram();
    // GPU VA space with host-owned page tables (Phase 4 step 4); nv_mmu callbacks via PRAMIN
    static uint64_t mmuAlloc(void *ctx, uint32_t bytes);
    static uint64_t mmuRd64(void *ctx, uint64_t addr);
    static void     mmuWr64(void *ctx, uint64_t addr, uint64_t value);
    bool     createVaSpace();
    // GPFIFO channel plus copy engine test (Phase 4 step 5)
    bool     tlbFlush(uint64_t pdb, bool hubOnly = false);
    bool     testCopyEngine();
    struct TestChan;
    bool     createChannel(TestChan &c, uint32_t handle, uint32_t chid, uint32_t engine,
                           uint64_t vaBase, uint32_t mthdSize);
    bool     scheduleChannel(TestChan &c);
    bool     submitAndWait(TestChan &c, const uint32_t *push, uint32_t words, uint32_t semPayload, uint32_t ms,
                           bool verbose = true);
    // Graphics context buffers plus TURING_COMPUTE_A (Phase 4 step 7)
    bool     testCompute();
    // BAR1 through GSP-RM's BAR1 root (Phase 4 step 6)
    bool     testBar1();

    // Phase 5 (NVGpu.cpp): shared GPU setup after the RM objects exist
    bool     initEngines();
    bool     createUtilChannel();
    bool     initGrGlobal();
    void     initNvdec();
    void     queryGrInfo();
    void     probeIntr();               // read-only: CPU interrupt table and tree state
    bool     initBar1();
    void     bar1Cleanup();             // removes every BAR1 mapping of ours (before unload)
    bool     ptPoolInit();
    void     ptPoolFini();
    uint64_t ptPoolAlloc();             // a zeroed 4 KiB table from the pool, or 0
    void     tableFree(uint64_t addr);  // pool or heap
    bool     bar1Flush();               // BAR1 writes have reached VRAM
    bool     bar1Map(uint64_t pa, uint64_t size, uint64_t *va);
    void     bar1Unmap(uint64_t va, uint64_t size);
    // BAR1 slices held past a CPU-mapped free (memFree): a CPU mapping created through memMap
    // may have been duplicated into any task (fork with VM_INHERIT_SHARE, mach_vm_remap, a
    // memory entry sent over a port), and nothing tells us when the last duplicate is gone, so
    // their BAR1 range never maps another connection's memory while the driver is loaded. The
    // connection that held a slice may reuse it (bar1Reuse): whoever has a duplicate got it
    // from that process. Kept on NVBringup itself, not GspState, so a slice stays held across
    // a GSP-RM unload and reboot.
    struct Bar1Held { uint64_t va = 0, size = 0, owner = 0; };     // owner: GpuConn::serial
    Bar1Held *bar1Held_ = nullptr;
    uint32_t  bar1HeldN_ = 0, bar1HeldCap_ = 0;
    uint32_t  bar1HeldRes_ = 0;         // entries promised to CPU-mapped memory not yet freed
    uint64_t  bar1HoldBytes_ = 0;       // held + promised bytes; capped at half the BAR1 window
    bool      bar1CapLogged_ = false;
    uint64_t  connSerial_ = 0;          // last GpuConn::serial handed out
    IOReturn bar1HoldCommit(uint64_t size);                   // promises an entry + bytes (memMap)
    void     bar1HoldUncommit(uint64_t size);                 // takes back an unused promise
    void     bar1Hold(uint64_t va, uint64_t size, uint64_t owner);    // dummy PTEs, tracks the slice
    bool     bar1Reuse(uint64_t owner, uint64_t pa, uint64_t size, uint64_t *va, uint64_t *sliceSize);
    bool     bar1ReserveHeld();                               // after a GSP-RM boot: reserve + re-point every held slice
    bool     scrubVram(uint64_t pa, uint64_t size);
    bool     testScrub();
    uint64_t vasCreate(uint32_t hVas, const struct nv_mmu_ops *ops);
    bool     rmAllocChannel(uint32_t handle, uint32_t chid, uint32_t engine, uint32_t hVas,
                            uint64_t ringVa, uint32_t ringEntries, uint64_t inst, uint64_t userd,
                            uint64_t mthd, bool kernel);
    bool     rmScheduleChannel(uint32_t handle, uint32_t engine, uint32_t *token);
    int      allocChid();
    void     markChidLost(uint32_t chid, uint32_t exceptType);
    uint64_t gpuTime();
    // Connections
    void     gpuTeardown(GpuConn *c, const char *why);
    void     gpuTeardownAll(const char *why);
    IOReturn memAlloc(GpuConn *c, uint64_t size, uint64_t align, uint32_t flags, uint32_t kind,
                      uint32_t *handle, uint64_t *outSize);
    void     memFree(GpuConn *c, GpuMem *m, bool unbind);
    IOReturn memMap(GpuConn *c, GpuMem *m, uint64_t *addr);
    IOReturn vmBind(GpuConn *c, const uint8_t *buf, uint32_t len);
    IOReturn unbindRange(GpuConn *c, uint64_t va, uint64_t size, GpuMem *only);
    IOReturn ctxCreate(GpuConn *c, uint32_t engines, uint32_t *handle);
    void     ctxDestroy(GpuCtx *x);
    IOReturn exec(GpuConn *c, const uint8_t *buf, uint32_t len, uint64_t *seq);
    IOReturn syncWait(GpuConn *c, const uint8_t *buf, uint32_t len, uint64_t *index);
    static uint64_t connMmuAlloc(void *ctx, uint32_t bytes);
    static uint64_t connMmuRd64(void *ctx, uint64_t addr);
    static void     connMmuWr64(void *ctx, uint64_t addr, uint64_t value);
    static uint64_t bar1MmuAlloc(void *ctx, uint32_t bytes);
    static void     bar1MmuLinked(void *ctx, uint64_t entry);
    IOWorkLoop         *gspWorkLoop_ = nullptr;
    IOTimerEventSource *gspTimer_ = nullptr;
    int  serviceStatusQueue();
    void startGspPoller();
    void stopGspPoller();
    void shutdownHw();
    void recordTeardown(const char *why, IOReturn r);
    void resumeGsp(const char *why = "wake");
    static void resumeThunk(thread_call_param_t self, thread_call_param_t);
    thread_call_t resumeCall_ = nullptr;
    bool resumePending_ = false;    // GSP-RM was unloaded for sleep: boot it again on wake
    void logLastTeardown();
    void gspPoll();
    // Monitoring (NVGpu.cpp): sampled by gspPoll every 100 ms, published once a second as
    // the NVStats property (read-only: one PTHERM register read, the rest is our own state).
    uint32_t statTicks_ = 0, statBusy_ = 0, statSeq_ = 0;
    void updateStats();

    // Item 4: non-stall interrupts (NVGpu.cpp). On after each GSP-RM boot unless boot-arg nvintr=0;
    // `sudo nvgsp intr on|off` switches at runtime. The channel adds
    // NON_STALL_INTERRUPT after each EXEC's seqno release; GR0/CE non-stall vectors raise the
    // CPU tree's MSI; the filter clears and re-arms, the action wakes SYNC_WAIT.
    IOLock   *waitLock_ = nullptr;          // SYNC_WAIT sleeps here; every waker bumps waitGen_
    volatile uint64_t waitGen_ = 0;
    void      wakeWaiters();
    uint32_t  intrLeaf_[8] = {};            // our non-stall vectors per leaf register (probeIntr)
    uint32_t  intrTop_[2] = {};             // their subtrees
    IOWorkLoop *intrWL_ = nullptr;
    IOFilterInterruptEventSource *intrES_ = nullptr;
    bool      intrESOn_ = false;
    volatile bool intrWanted_ = false;      // the switch (on unless nvintr=0); survives GSP-RM reboots
    volatile bool intrOn_ = false;          // enabled in hardware right now
    volatile uint32_t intrCount_ = 0, intrSpurious_ = 0, intrStorms_ = 0;
    uint32_t  intrStormsLogged_ = 0;
    uint64_t  intrWinStart_ = 0, intrWinLen_ = 0;
    uint32_t  intrWinCount_ = 0;
    bool      intrFilter(IOFilterInterruptEventSource *src);
    void      intrAction(IOInterruptEventSource *src, int count);
    bool      intrHwOn();                   // caller holds gspLock_, GSP-RM running
    void      intrHwOff();
    void      intrRelease();

    // Runtime power (NVPower.cpp): with no GPU connection for idleSec_ seconds (auto mode), GSP-RM
    // is unloaded and the GPU cut off with ACPI _OFF on its node (laptops with switchable graphics;
    // desktop cards usually have none and stay on); the next open powers it on (_ON),
    // restores its PCI state, reruns FRTS and boots GSP-RM. powerLock_ serializes transitions
    // and is held across gpuOpen, so an open never races an idle power-off.
    enum { kPwrOn = 0, kPwrOff = 1, kPwrSwitching = 2, kPwrFailed = 3 };
    enum { kModeOff = 0, kModeOn = 1, kModeAuto = 2 };
    IOLock   *powerLock_ = nullptr;
    volatile int pwrState_ = kPwrOn;
    int       pwrMode_ = kModeAuto;
    uint32_t  idleSec_ = 30;
    uint64_t  idleSince_ = 0;               // mach time the GPU became idle; 0 = in use
    thread_call_t idleCall_ = nullptr, rawOffCall_ = nullptr;
    volatile bool idleQueued_ = false;
    bool      rawOn_ = false;               // powered on around sleep/restart without GSP-RM
    uint32_t  pwrOffCount_ = 0, pwrOnCount_ = 0, lastOnMs_ = 0;
    uint8_t   msiCap_ = 0;
    uint32_t  msiSave_[4] = {};             // control word, address lo, address hi, data
    IOReturn  gpuOpenLocked(task_t task, GpuConn **out);
    nvmac_info infoCache_ = {};             // device info from the last GSP-RM boot (NVMAC_POWER_INFO)
    bool      infoCached_ = false;
    void      fillInfo(nvmac_info *i);       // gspLock_ held, GPU interface ready
    void      powerInit();
    void      powerProbe();                 // after findAcpiNode: can ACPI cut the GPU off?
    bool      pwrCapable_ = false;          // ACPI _OFF plus _ON or _PS0 on the GPU's node
    void      powerFree();
    void      idleTick();                   // gspPoll, gspLock_ held
    bool      gpuPowerOff(const char *why); // powerLock_ held
    bool      gpuPowerOn(const char *why);  // powerLock_ held
    bool      rawPowerOff(const char *why); // no GSP-RM: save PCI state, _OFF
    bool      rawPowerOn(const char *why);  // _ON, restore PCI state
    void      publishPower();
    static void idleThunk(thread_call_param_t self, thread_call_param_t);
    static void rawOffThunk(thread_call_param_t self, thread_call_param_t);
    void      powerSleep(UInt32 messageType);   // sleepHandler: WillSleep/PowerOff/Restart
    void      powerWake();                      // sleepHandler: HasPoweredOn
    int  handleGspMessage(const struct GSP_MSG_QUEUE_ELEMENT *e, const uint8_t *payload, uint32_t len);
    bool runSequencer(const uint8_t *payload, uint32_t len);
};
