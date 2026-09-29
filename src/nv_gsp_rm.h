// GSP-RM r570.144 interface structures (Turing, LibOS2), plus the pure helpers
// that build and check them. No IOKit; shared by the kext and the host tool.
//
// Layouts transcribed from NVIDIA open-gpu-kernel-modules tag 570.144
// (MIT license, Copyright (c) NVIDIA CORPORATION & AFFILIATES). Names are kept
// as NVIDIA's so each struct can be traced back to its header, noted above it.
// The GSP is a little-endian RV64 core with natural alignment, the same ABI as
// x86-64, so these plain structs match its view byte for byte; the
// static_asserts pin every size and offset the firmware depends on.
#pragma once

#include <stddef.h>
#include <stdint.h>

// ---- arch/nvalloc/common/inc/gsp/gsp_fw_wpr_meta.h ---------------------------

#define GSP_FW_WPR_META_MAGIC     0xdc3aae21371a60b3ULL
#define GSP_FW_WPR_META_REVISION  1
#define GSP_FW_WPR_META_VERIFIED  0xa0a0a0a0a0a0a0a0ULL

struct GspFwWprMeta {
    uint64_t magic;
    uint64_t revision;
    uint64_t sysmemAddrOfRadix3Elf;
    uint64_t sizeOfRadix3Elf;
    uint64_t sysmemAddrOfBootloader;
    uint64_t sizeOfBootloader;
    uint64_t bootloaderCodeOffset;
    uint64_t bootloaderDataOffset;
    uint64_t bootloaderManifestOffset;
    uint64_t sysmemAddrOfSignature;     // union with gspFwHeapFreeListWprOffset (unused here)
    uint64_t sizeOfSignature;
    uint64_t gspFwRsvdStart;
    uint64_t nonWprHeapOffset;
    uint64_t nonWprHeapSize;
    uint64_t gspFwWprStart;
    uint64_t gspFwHeapOffset;
    uint64_t gspFwHeapSize;
    uint64_t gspFwOffset;
    uint64_t bootBinOffset;
    uint64_t frtsOffset;
    uint64_t frtsSize;
    uint64_t gspFwWprEnd;
    uint64_t fbSize;
    uint64_t vgaWorkspaceOffset;
    uint64_t vgaWorkspaceSize;
    uint64_t bootCount;
    uint64_t partitionRpcAddr;          // union with the crash-report queue fields
    uint16_t partitionRpcRequestOffset;
    uint16_t partitionRpcReplyOffset;
    uint32_t elfCodeOffset;
    uint32_t elfDataOffset;
    uint32_t elfCodeSize;
    uint32_t elfDataSize;
    uint32_t lsUcodeVersion;
    uint8_t  gspFwHeapVfPartitionCount;
    uint8_t  flags;
    uint8_t  padding[2];
    uint32_t pmuReservedSize;
    uint64_t verified;
};
static_assert(sizeof(GspFwWprMeta) == 256, "GspFwWprMeta");   // NVIDIA's own ct_assert
static_assert(offsetof(GspFwWprMeta, gspFwRsvdStart) == 0x58, "GspFwWprMeta");
static_assert(offsetof(GspFwWprMeta, bootCount) == 0xc8, "GspFwWprMeta");
static_assert(offsetof(GspFwWprMeta, gspFwHeapVfPartitionCount) == 0xf0, "GspFwWprMeta");
static_assert(offsetof(GspFwWprMeta, verified) == 0xf8, "GspFwWprMeta");

// ---- uproc/os/common/include/libos_init_args.h -------------------------------

enum { LIBOS_MEMORY_REGION_NONE, LIBOS_MEMORY_REGION_CONTIGUOUS, LIBOS_MEMORY_REGION_RADIX3 };
enum { LIBOS_MEMORY_REGION_LOC_NONE, LIBOS_MEMORY_REGION_LOC_SYSMEM, LIBOS_MEMORY_REGION_LOC_FB };

struct LibosMemoryRegionInitArgument {
    uint64_t id8;       // name packed big-end-first, see nv_libos_id8
    uint64_t pa;
    uint64_t size;
    uint8_t  kind;
    uint8_t  loc;
};
static_assert(sizeof(LibosMemoryRegionInitArgument) == 32, "LibosMemoryRegionInitArgument");

#define LIBOS_INIT_ARGUMENTS_SIZE  0x1000   // g_kernel_gsp_nvoc.h: one page of the above

// ---- inc/kernel/gpu/gsp/gsp_init_args.h --------------------------------------

struct MESSAGE_QUEUE_INIT_ARGUMENTS {
    uint64_t sharedMemPhysAddr;     // PA of the shared area's first page (the PTE array)
    uint32_t pageTableEntryCount;
    uint64_t cmdQueueOffset;        // NvLength
    uint64_t statQueueOffset;
};
static_assert(sizeof(MESSAGE_QUEUE_INIT_ARGUMENTS) == 32, "MESSAGE_QUEUE_INIT_ARGUMENTS");

struct GSP_SR_INIT_ARGUMENTS {
    uint32_t oldLevel;
    uint32_t flags;
    uint8_t  bInPMTransition;
};
static_assert(sizeof(GSP_SR_INIT_ARGUMENTS) == 12, "GSP_SR_INIT_ARGUMENTS");

struct GSP_ARGUMENTS_CACHED {
    MESSAGE_QUEUE_INIT_ARGUMENTS messageQueueInitArguments;
    GSP_SR_INIT_ARGUMENTS        srInitArguments;
    uint32_t                     gpuInstance;
    uint8_t                      bDmemStack;
    struct { uint64_t pa, size; } profilerArgs;
};
static_assert(offsetof(GSP_ARGUMENTS_CACHED, gpuInstance) == 0x2c, "GSP_ARGUMENTS_CACHED");
static_assert(offsetof(GSP_ARGUMENTS_CACHED, bDmemStack) == 0x30, "GSP_ARGUMENTS_CACHED");
static_assert(sizeof(GSP_ARGUMENTS_CACHED) == 0x48, "GSP_ARGUMENTS_CACHED");

// ---- common/shared/msgq/inc/msgq/msgq_priv.h, msgq.h -------------------------

#define MSGQ_VERSION        0
#define MSGQ_FLAGS_SWAP_RX  1

struct msgqTxHeader {
    uint32_t version;
    uint32_t size;          // bytes, page aligned
    uint32_t msgSize;       // entry size
    uint32_t msgCount;
    uint32_t writePtr;      // index of the next slot the writer fills
    uint32_t flags;
    uint32_t rxHdrOff;      // offset of msgqRxHeader in this queue
    uint32_t entryOff;      // offset of entry 0 in this queue
};
static_assert(sizeof(msgqTxHeader) == 32, "msgqTxHeader");

struct msgqRxHeader {
    uint32_t readPtr;
};

// ---- generated/g_rpc-message-header.h, inc/kernel/vgpu/rpc_headers.h ---------

#define NV_VGPU_MSG_HEADER_VERSION   0x03000000u   // major 3, minor 0
#define NV_VGPU_MSG_SIGNATURE_VALID  0x43505256u   // "VRPC"
#define NV_VGPU_MSG_RESULT_RPC_PENDING 0xffffffffu

struct rpc_message_header_v {
    uint32_t header_version;
    uint32_t signature;
    uint32_t length;            // header + payload bytes
    uint32_t function;
    uint32_t rpc_result;
    uint32_t rpc_result_private;
    uint32_t sequence;
    uint32_t spare;             // union { spare; cpuRmGfid; }
    // payload follows
};
static_assert(sizeof(rpc_message_header_v) == 32, "rpc_message_header_v");

// ---- inc/kernel/gpu/gsp/message_queue_priv.h ---------------------------------

#define GSP_MSG_QUEUE_ELEMENT_SIZE_MIN  0x1000u
#define GSP_MSG_QUEUE_ELEMENT_SIZE_MAX  0x10000u
#define GSP_MSG_QUEUE_HEADER_ALIGN      4           // log2: 16 bytes
#define GSP_MSG_QUEUE_ELEMENT_ALIGN     12          // log2: 4 KiB

struct GSP_MSG_QUEUE_ELEMENT {
    uint8_t  authTagBuffer[16];     // confidential compute only; zero
    uint8_t  aadBuffer[16];
    uint32_t checkSum;              // makes the XOR checksum of the element zero
    uint32_t seqNum;
    uint32_t elemCount;             // number of 4 KiB queue entries this message spans
    alignas(8) rpc_message_header_v rpc;
};
static_assert(offsetof(GSP_MSG_QUEUE_ELEMENT, checkSum) == 0x20, "GSP_MSG_QUEUE_ELEMENT");
static_assert(offsetof(GSP_MSG_QUEUE_ELEMENT, rpc) == 0x30, "GSP_MSG_QUEUE_ELEMENT");
static_assert(sizeof(GSP_MSG_QUEUE_ELEMENT) == 0x50, "GSP_MSG_QUEUE_ELEMENT");
#define GSP_MSG_QUEUE_ELEMENT_HDR_SIZE  offsetof(GSP_MSG_QUEUE_ELEMENT, rpc)

// ---- inc/kernel/vgpu/rpc_global_enums.h (the ones used for boot) -------------

enum {
    NV_VGPU_MSG_FUNCTION_NOP                    = 0,
    NV_VGPU_MSG_FUNCTION_UNLOADING_GUEST_DRIVER = 47,
    NV_VGPU_MSG_FUNCTION_GET_GSP_STATIC_INFO    = 65,
    NV_VGPU_MSG_FUNCTION_CONTINUATION_RECORD    = 71,
    NV_VGPU_MSG_FUNCTION_GSP_SET_SYSTEM_INFO    = 72,
    NV_VGPU_MSG_FUNCTION_SET_REGISTRY           = 73,
    NV_VGPU_MSG_FUNCTION_FREE                   = 10,
    NV_VGPU_MSG_FUNCTION_GSP_RM_CONTROL         = 76,
    NV_VGPU_MSG_FUNCTION_GSP_RM_ALLOC           = 103,
    NV_VGPU_MSG_FUNCTION_NUM_FUNCTIONS          = 227,

    NV_VGPU_MSG_EVENT_FIRST_EVENT               = 0x1000,
    NV_VGPU_MSG_EVENT_GSP_INIT_DONE             = 0x1001,
    NV_VGPU_MSG_EVENT_GSP_RUN_CPU_SEQUENCER     = 0x1002,
    NV_VGPU_MSG_EVENT_POST_EVENT                = 0x1003,
    NV_VGPU_MSG_EVENT_RC_TRIGGERED              = 0x1004,
    NV_VGPU_MSG_EVENT_MMU_FAULT_QUEUED          = 0x1005,
    NV_VGPU_MSG_EVENT_OS_ERROR_LOG              = 0x1006,
    NV_VGPU_MSG_EVENT_UCODE_LIBOS_PRINT         = 0x100c,
    NV_VGPU_MSG_EVENT_PERF_BRIDGELESS_INFO_UPDATE = 0x100f,
    NV_VGPU_MSG_EVENT_GSP_LOCKDOWN_NOTICE       = 0x101c,
    NV_VGPU_MSG_EVENT_GSP_POST_NOCAT_RECORD     = 0x1020,
    NV_VGPU_MSG_EVENT_NUM_EVENTS                = 0x1023,
};

// ---- generated/g_rpc-structures.h (boot-time payloads) -----------------------

struct rpc_run_cpu_sequencer_v17_00 {
    uint32_t bufferSizeDWord;
    uint32_t cmdIndex;              // dwords used in commandBuffer
    uint32_t regSaveArea[8];
    // uint32_t commandBuffer[] follows
};
static_assert(sizeof(rpc_run_cpu_sequencer_v17_00) == 40, "rpc_run_cpu_sequencer_v17_00");

// RM object API over RPC: the payload follows the fixed part.
struct rpc_gsp_rm_alloc_v03_00 {
    uint32_t hClient, hParent, hObject, hClass;
    uint32_t status;            // NV_STATUS of the allocation (reply)
    uint32_t paramsSize;
    uint32_t flags;             // RMAPI_RPC_FLAGS_NONE (params not serialized)
    uint8_t  reserved[4];
    // uint8_t params[] follows
};
static_assert(sizeof(rpc_gsp_rm_alloc_v03_00) == 32, "rpc_gsp_rm_alloc_v03_00");

struct rpc_gsp_rm_control_v03_00 {
    uint32_t hClient, hObject, cmd;
    uint32_t status;
    uint32_t paramsSize;
    uint32_t flags;
    // uint8_t params[] follows
};
static_assert(sizeof(rpc_gsp_rm_control_v03_00) == 24, "rpc_gsp_rm_control_v03_00");

struct rpc_free_v03_00 {        // NVOS00_PARAMETERS_v03_00 (g_sdk-structures.h)
    uint32_t hRoot, hObjectParent, hObjectOld;
    uint32_t status;
};
static_assert(sizeof(rpc_free_v03_00) == 16, "rpc_free_v03_00");

struct rpc_os_error_log_v17_00 {
    uint32_t exceptType;
    uint32_t runlistId;
    uint32_t chid;
    char     errString[0x100];
    uint32_t preemptiveRemovalPreviousXid;
};
static_assert(sizeof(rpc_os_error_log_v17_00) == 0x110, "rpc_os_error_log_v17_00");

struct rpc_ucode_libos_print_v1E_08 {
    uint32_t ucodeEngDesc;
    uint32_t libosPrintBufSize;
    // uint8_t libosPrintBuf[] follows
};

struct rpc_gsp_lockdown_notice_v17_00 {
    uint8_t bLockdownEngaging;
};

// ---- sdk ctrl2080nvd.h: GSP_POST_NOCAT_RECORD payload -------------------------
// NVIDIA's error journal ("NOCAT"): bugchecks, asserts, engine errors, RC events.

#define NV2080_NOCAT_JOURNAL_MAX_STR_LEN      65
#define NV2080_NOCAT_JOURNAL_MAX_DIAG_BUFFER  1024

struct NV2080CtrlNocatJournalInsertRecord {
    uint32_t flags;
    uint64_t timestamp;
    uint8_t  recType;       // 1 bugcheck, 2 engine, 3 TDR, 4 RC, 5 assert
    uint32_t bugcheck;
    char     source[NV2080_NOCAT_JOURNAL_MAX_STR_LEN];
    uint32_t subsystem;
    uint64_t errorCode;
    char     faultingEngine[NV2080_NOCAT_JOURNAL_MAX_STR_LEN];
    uint32_t tdrReason;
    uint32_t diagBufferLen;
    uint8_t  diagBuffer[NV2080_NOCAT_JOURNAL_MAX_DIAG_BUFFER];
};
static_assert(offsetof(NV2080CtrlNocatJournalInsertRecord, source) == 24, "NOCAT record");
static_assert(offsetof(NV2080CtrlNocatJournalInsertRecord, errorCode) == 96, "NOCAT record");
static_assert(offsetof(NV2080CtrlNocatJournalInsertRecord, diagBuffer) == 180, "NOCAT record");
static_assert(sizeof(NV2080CtrlNocatJournalInsertRecord) == 1208, "NOCAT record");

const char *nv_nocat_type_name(uint8_t recType);

// ---- generated/g_os_nvoc.h: SET_REGISTRY payload -----------------------------

#define REGISTRY_TABLE_ENTRY_TYPE_DWORD   1
#define REGISTRY_TABLE_ENTRY_TYPE_BINARY  2
#define REGISTRY_TABLE_ENTRY_TYPE_STRING  3

struct PACKED_REGISTRY_ENTRY {      // "packed" is NVIDIA's name; the layout is natural
    uint32_t nameOffset;            // from the start of the table
    uint8_t  type;
    uint32_t data;                  // DWORD value, or offset of BINARY/STRING data
    uint32_t length;
};
static_assert(sizeof(PACKED_REGISTRY_ENTRY) == 16, "PACKED_REGISTRY_ENTRY");

struct PACKED_REGISTRY_TABLE {
    uint32_t size;                  // whole table including names
    uint32_t numEntries;
    // PACKED_REGISTRY_ENTRY entries[] follow, then the NUL-terminated names
};

// ---- arch/nvalloc/common/inc/rmgspseq.h --------------------------------------

enum {
    GSP_SEQ_BUF_OPCODE_REG_WRITE = 0,       // addr, val
    GSP_SEQ_BUF_OPCODE_REG_MODIFY,          // addr, mask, val: (rd & ~mask) | val
    GSP_SEQ_BUF_OPCODE_REG_POLL,            // addr, mask, val, timeout (us), error
    GSP_SEQ_BUF_OPCODE_DELAY_US,            // val
    GSP_SEQ_BUF_OPCODE_REG_STORE,           // addr, index -> regSaveArea[index]
    GSP_SEQ_BUF_OPCODE_CORE_RESET,          // reset the GSP falcon
    GSP_SEQ_BUF_OPCODE_CORE_START,          // start the GSP falcon
    GSP_SEQ_BUF_OPCODE_CORE_WAIT_FOR_HALT,
    GSP_SEQ_BUF_OPCODE_CORE_RESUME,         // Turing: reset GSP into RISC-V, rerun SEC2
};
#define GSP_SEQ_BUF_REG_SAVE_SIZE  8

// ---- inc/kernel/gpu/gsp/gsp_static_config.h: GSP_SET_SYSTEM_INFO payload -----

struct BUSINFO {                    // generated/g_chipset_nvoc.h
    uint16_t deviceID, vendorID, subdeviceID, subvendorID;
    uint8_t  revisionID;
};
static_assert(sizeof(BUSINFO) == 10, "BUSINFO");

#define NV0073_CTRL_SYSTEM_ACPI_ID_MAP_MAX_DISPLAYS 16

// inc/kernel/gpu/gpu_acpi_data.h (NV_STATUS is a u32)
struct DOD_METHOD_DATA { uint32_t status, acpiIdListLen, acpiIdList[NV0073_CTRL_SYSTEM_ACPI_ID_MAP_MAX_DISPLAYS]; };
struct JT_METHOD_DATA  { uint32_t status, jtCaps; uint16_t jtRevId; uint8_t bSBIOSCaps; };
struct MUX_METHOD_DATA_ELEMENT { uint32_t acpiId, mode, status; };
struct MUX_METHOD_DATA {
    uint32_t tableLen;
    MUX_METHOD_DATA_ELEMENT acpiIdMuxModeTable[NV0073_CTRL_SYSTEM_ACPI_ID_MAP_MAX_DISPLAYS];
    MUX_METHOD_DATA_ELEMENT acpiIdMuxPartTable[NV0073_CTRL_SYSTEM_ACPI_ID_MAP_MAX_DISPLAYS];
    MUX_METHOD_DATA_ELEMENT acpiIdMuxStateTable[NV0073_CTRL_SYSTEM_ACPI_ID_MAP_MAX_DISPLAYS];
};
struct CAPS_METHOD_DATA { uint32_t status, optimusCaps; };
struct ACPI_METHOD_DATA {
    uint8_t          bValid;
    DOD_METHOD_DATA  dodMethodData;
    JT_METHOD_DATA   jtMethodData;
    MUX_METHOD_DATA  muxMethodData;
    CAPS_METHOD_DATA capsMethodData;
};
static_assert(sizeof(JT_METHOD_DATA) == 12, "JT_METHOD_DATA");
static_assert(sizeof(MUX_METHOD_DATA) == 580, "MUX_METHOD_DATA");
static_assert(sizeof(ACPI_METHOD_DATA) == 676, "ACPI_METHOD_DATA");

struct GSP_VF_INFO {
    uint32_t totalVFs, firstVFOffset;
    uint64_t FirstVFBar0Address, FirstVFBar1Address, FirstVFBar2Address;
    uint8_t  b64bitBar0, b64bitBar1, b64bitBar2;
};
static_assert(sizeof(GSP_VF_INFO) == 40, "GSP_VF_INFO");

struct GspSystemInfo {
    uint64_t gpuPhysAddr;           // BAR0
    uint64_t gpuPhysFbAddr;         // BAR1
    uint64_t gpuPhysInstAddr;       // BAR2/3 (instance memory)
    uint64_t gpuPhysIoAddr;
    uint64_t nvDomainBusDeviceFunc;
    uint64_t simAccessBufPhysAddr;
    uint64_t notifyOpSharedSurfacePhysAddr;
    uint64_t pcieAtomicsOpMask;
    uint64_t consoleMemSize;
    uint64_t maxUserVa;
    uint32_t pciConfigMirrorBase;
    uint32_t pciConfigMirrorSize;
    uint32_t PCIDeviceID;           // device << 16 | vendor
    uint32_t PCISubDeviceID;        // subsystem << 16 | subsystem vendor
    uint32_t PCIRevisionID;
    uint32_t pcieAtomicsCplDeviceCapMask;
    uint8_t  oorArch;
    uint64_t clPdbProperties;
    uint32_t Chipset;
    uint8_t  bGpuBehindBridge;
    uint8_t  bFlrSupported;
    uint8_t  b64bBar0Supported;
    uint8_t  bMnocAvailable;
    uint32_t chipsetL1ssEnable;
    uint8_t  bUpstreamL0sUnsupported;
    uint8_t  bUpstreamL1Unsupported;
    uint8_t  bUpstreamL1PorSupported;
    uint8_t  bUpstreamL1PorMobileOnly;
    uint8_t  bSystemHasMux;
    uint8_t  upstreamAddressValid;
    BUSINFO  FHBBusInfo;
    BUSINFO  chipsetIDInfo;
    ACPI_METHOD_DATA acpiMethodData;
    uint32_t hypervisorType;
    uint8_t  bIsPassthru;
    uint64_t sysTimerOffsetNs;
    GSP_VF_INFO gspVFInfo;
    uint8_t  bIsPrimary;
    uint8_t  isGridBuild;
    uint32_t pcieConfigReg_linkCap; // GSP_PCIE_CONFIG_REG
    uint32_t gridBuildCsp;
    uint8_t  bPreserveVideoMemoryAllocations;
    uint8_t  bTdrEventSupported;
    uint8_t  bFeatureStretchVblankCapable;
    uint8_t  bEnableDynamicGranularityPageArrays;
    uint8_t  bClockBoostSupported;
    uint8_t  bRouteDispIntrsToCPU;
    uint64_t hostPageSize;
};
static_assert(offsetof(GspSystemInfo, pciConfigMirrorBase) == 0x50, "GspSystemInfo");
static_assert(offsetof(GspSystemInfo, clPdbProperties) == 0x70, "GspSystemInfo");
static_assert(offsetof(GspSystemInfo, chipsetL1ssEnable) == 0x80, "GspSystemInfo");
static_assert(offsetof(GspSystemInfo, FHBBusInfo) == 0x8a, "GspSystemInfo");
static_assert(offsetof(GspSystemInfo, acpiMethodData) == 0xa0, "GspSystemInfo");
static_assert(offsetof(GspSystemInfo, hypervisorType) == 0x344, "GspSystemInfo");
static_assert(offsetof(GspSystemInfo, sysTimerOffsetNs) == 0x350, "GspSystemInfo");
static_assert(offsetof(GspSystemInfo, gspVFInfo) == 0x358, "GspSystemInfo");
static_assert(offsetof(GspSystemInfo, pcieConfigReg_linkCap) == 0x384, "GspSystemInfo");
static_assert(offsetof(GspSystemInfo, hostPageSize) == 0x398, "GspSystemInfo");
static_assert(sizeof(GspSystemInfo) == 0x3a0, "GspSystemInfo");

// =============================================================================
// Helpers (nv_gsp_rm.cpp)
// =============================================================================

// LibOS region name -> id8: up to 8 chars, first char in the highest used byte.
uint64_t nv_libos_id8(const char *name);

// ---- Radix3 page tables for the GSP-RM ELF image ------------------------------
// Level 2 holds the PA of every 4 KiB data page, level 1 the PAs of the level-2
// pages, level 0 (one page) the PAs of the level-1 pages. The meta's
// sysmemAddrOfRadix3Elf points at level 0.
struct nv_radix3_sizes {
    uint64_t data_pages;
    uint64_t lvl0_pages, lvl1_pages, lvl2_pages;    // table pages per level
    uint64_t table_pages;                           // sum, laid out lvl0, lvl1, lvl2
};
bool nv_radix3_plan(uint64_t data_bytes, nv_radix3_sizes *s);

// Fills `tables` (s->table_pages * 512 u64s). table_pa[i] is the PA of table page i
// (same order), data_pa[i] of data page i.
void nv_radix3_fill(const nv_radix3_sizes *s, uint64_t *tables,
                    const uint64_t *table_pa, const uint64_t *data_pa);

// ---- Shared memory: PTE array + command queue + status queue ------------------
struct nv_gsp_shm_layout {
    uint64_t cmdq_size, msgq_size;      // 256 KiB each (r570 default)
    uint32_t pte_count;                 // pages of the whole area, PTE pages included
    uint64_t pte_size;                  // page-aligned bytes of the PTE array
    uint64_t cmdq_off, msgq_off;        // == cmdQueueOffset / statQueueOffset
    uint64_t total;
    // Within each queue (msgqTxCreate with header align 16, entry align 4 KiB):
    uint32_t rx_hdr_off, entry_off, msg_count;
};
void nv_gsp_shm_plan(nv_gsp_shm_layout *l);

// Writes the CPU's command-queue TX header (the GSP creates the status queue).
void nv_gsp_cmdq_init(const nv_gsp_shm_layout *l, msgqTxHeader *tx);

// ---- Messages ------------------------------------------------------------------
// XOR of the 64-bit words, folded to 32 bits (_checkSum32). len must be a
// multiple of 8 (callers pad with zeros).
uint32_t nv_gsp_checksum(const void *data, uint32_t len);

// Builds one queue message into `elem` (cap bytes, zeroed by this call):
// element header + RPC header + payload, checksum set. Returns the number of
// 4 KiB queue entries it spans, or 0 if it does not fit.
uint32_t nv_gsp_msg_build(void *elem, uint32_t cap, uint32_t seq, uint32_t function,
                          const void *payload, uint32_t payload_len);

// Ring access (msgq.c). A queue is `size` bytes: TX header, RX header at
// rxHdrOff, entries from entryOff. With SWAP_RX each side's read pointer lives in
// the RX header of its *own* queue: the CPU's status-queue read pointer is in the
// command queue at rxHdrOff, and the GSP's command-queue read pointer in the
// status queue at rxHdrOff.

// Copies `count` entries of a built message into the command queue at its
// writePtr and advances writePtr. `their_rptr` is the GSP's read pointer. Returns
// false if the queue lacks room (one slot always stays empty).
bool nv_gsp_cmdq_push(uint8_t *cmdq, const void *msg, uint32_t count, uint32_t their_rptr);

// True once the GSP has written a sane TX header for the status queue.
bool nv_gsp_msgq_linked(const msgqTxHeader *h, uint32_t size);

// Copies the message at `rptr` out of the status queue into `out` (cap bytes),
// joining entries that wrap. Returns its entry count, or 0 if the element header
// is inconsistent. The caller checks it with nv_gsp_msg_check and then sets its
// read pointer to (rptr + count) % msgCount.
uint32_t nv_gsp_msgq_read(const uint8_t *msgq, uint32_t rptr, void *out, uint32_t cap);

// Checks a received message (elemCount * 4 KiB bytes at `elem`): checksum,
// RPC signature and length. Returns nullptr if fine, else the reason.
const char *nv_gsp_msg_check(const void *elem, uint32_t len);

// ---- SET_REGISTRY ----------------------------------------------------------------
struct nv_reg_dword { const char *name; uint32_t value; };

// DWORD entries only. Returns the table size, or 0 if it does not fit in cap.
uint32_t nv_gsp_registry_pack(void *out, uint32_t cap, const nv_reg_dword *e, uint32_t n);

// The entries nouveau and nova-core send on every boot.
extern const nv_reg_dword nv_gsp_default_registry[];
extern const uint32_t nv_gsp_default_registry_count;

// ---- Sequencer (GSP_RUN_CPU_SEQUENCER) -------------------------------------------
struct nv_seq_op {
    uint32_t opcode;
    uint32_t args[5];
    uint32_t nargs;
};
// Decodes op `*pos` from a command buffer of `used` dwords and advances *pos.
// Returns false on an unknown opcode or truncated payload.
bool nv_gsp_seq_next(const uint32_t *cmds, uint32_t used, uint32_t *pos, nv_seq_op *op);
const char *nv_gsp_seq_name(uint32_t opcode);

// ---- Filling the boot structures ---------------------------------------------------
struct nv_wpr2_layout;

struct nv_gsp_sysmem {
    uint64_t radix3_pa;                 // level-0 page
    uint64_t elf_size;                  // .fwimage bytes
    uint64_t bl_pa, bl_size;            // GSP bootloader image
    uint32_t bl_code_off, bl_data_off, bl_manifest_off;
    uint64_t sig_pa, sig_size;          // .fwsignature_tu11x copy
};
void nv_gsp_wpr_meta_fill(GspFwWprMeta *m, const nv_wpr2_layout *l, const nv_gsp_sysmem *s);

// LOGINIT, LOGINTR, LOGRM, LOGMNOC (64 KiB each), then RMARGS. log_pa[i] is the
// PA of log buffer i's first page. Returns the number of regions (5).
enum { NV_GSP_LOG_COUNT = 4, NV_GSP_LOG_SIZE = 0x10000 };
extern const char *const nv_gsp_log_names[NV_GSP_LOG_COUNT];
uint32_t nv_gsp_libos_args_fill(LibosMemoryRegionInitArgument *a, const uint64_t *log_pa,
                                uint64_t rmargs_pa, uint64_t rmargs_size);

void nv_gsp_rmargs_fill(GSP_ARGUMENTS_CACHED *g, const nv_gsp_shm_layout *l, uint64_t shm_pa);

struct nv_gsp_pci_info {
    uint64_t bar0_pa, bar1_pa, bar3_pa;
    uint8_t  bus, dev, fn;
    uint16_t vendor, device, subvendor, subdevice;
    uint8_t  revision;
};
void nv_gsp_sysinfo_fill(GspSystemInfo *si, const nv_gsp_pci_info *p);
