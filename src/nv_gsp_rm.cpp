#include "nv_gsp_rm.h"
#include "nv_gsp.h"

#include <string.h>

static uint64_t align_up(uint64_t x, uint64_t a) { return (x + a - 1) & ~(a - 1); }

// ---- LibOS ids ----------------------------------------------------------------

uint64_t nv_libos_id8(const char *name)
{
    uint64_t id = 0;
    for (int i = 0; i < 8 && name[i]; i++)
        id = (id << 8) | (uint8_t)name[i];
    return id;
}

// ---- Radix3 ---------------------------------------------------------------------

static const uint64_t PAGE = 0x1000, PTES_PER_PAGE = PAGE / 8;

bool nv_radix3_plan(uint64_t data_bytes, nv_radix3_sizes *s)
{
    memset(s, 0, sizeof(*s));
    if (!data_bytes)
        return false;
    s->data_pages = align_up(data_bytes, PAGE) / PAGE;
    s->lvl2_pages = align_up(s->data_pages, PTES_PER_PAGE) / PTES_PER_PAGE;
    s->lvl1_pages = align_up(s->lvl2_pages, PTES_PER_PAGE) / PTES_PER_PAGE;
    s->lvl0_pages = 1;
    if (s->lvl1_pages > PTES_PER_PAGE)      // > 512 GiB: cannot happen for a 28 MiB image
        return false;
    s->table_pages = s->lvl0_pages + s->lvl1_pages + s->lvl2_pages;
    return true;
}

void nv_radix3_fill(const nv_radix3_sizes *s, uint64_t *tables,
                    const uint64_t *table_pa, const uint64_t *data_pa)
{
    memset(tables, 0, s->table_pages * PAGE);
    uint64_t *lvl0 = tables;
    uint64_t *lvl1 = tables + s->lvl0_pages * PTES_PER_PAGE;
    uint64_t *lvl2 = lvl1 + s->lvl1_pages * PTES_PER_PAGE;
    const uint64_t *lvl1_pa = table_pa + s->lvl0_pages;
    const uint64_t *lvl2_pa = lvl1_pa + s->lvl1_pages;

    for (uint64_t i = 0; i < s->lvl1_pages; i++)
        lvl0[i] = lvl1_pa[i];
    for (uint64_t i = 0; i < s->lvl2_pages; i++)
        lvl1[i] = lvl2_pa[i];
    for (uint64_t i = 0; i < s->data_pages; i++)
        lvl2[i] = data_pa[i];
}

// ---- Shared memory and queues ----------------------------------------------------

void nv_gsp_shm_plan(nv_gsp_shm_layout *l)
{
    memset(l, 0, sizeof(*l));
    l->cmdq_size = 0x40000;
    l->msgq_size = 0x40000;

    // message_queue_cpu.c: one PTE per page of the queues, plus the pages the
    // PTE array itself needs (they are mapped too).
    uint64_t n = (l->cmdq_size + l->msgq_size) / PAGE;
    n += align_up(n * 8, PAGE) / PAGE;
    l->pte_count = (uint32_t)n;
    l->pte_size  = align_up(n * 8, PAGE);
    l->cmdq_off  = l->pte_size;
    l->msgq_off  = l->cmdq_off + l->cmdq_size;
    l->total     = l->msgq_off + l->msgq_size;

    l->rx_hdr_off = (uint32_t)align_up(sizeof(msgqTxHeader), 1u << GSP_MSG_QUEUE_HEADER_ALIGN);
    l->entry_off  = (uint32_t)align_up(l->rx_hdr_off + sizeof(msgqRxHeader), 1u << GSP_MSG_QUEUE_ELEMENT_ALIGN);
    l->msg_count  = (uint32_t)((l->cmdq_size - l->entry_off) / GSP_MSG_QUEUE_ELEMENT_SIZE_MIN);
}

void nv_gsp_cmdq_init(const nv_gsp_shm_layout *l, msgqTxHeader *tx)
{
    memset(tx, 0, sizeof(*tx));
    tx->version  = MSGQ_VERSION;
    tx->size     = (uint32_t)l->cmdq_size;
    tx->msgSize  = GSP_MSG_QUEUE_ELEMENT_SIZE_MIN;
    tx->msgCount = l->msg_count;
    tx->writePtr = 0;
    tx->flags    = MSGQ_FLAGS_SWAP_RX;
    tx->rxHdrOff = l->rx_hdr_off;
    tx->entryOff = l->entry_off;
}

// ---- Messages ---------------------------------------------------------------------

uint32_t nv_gsp_checksum(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint64_t sum = 0;
    for (uint32_t i = 0; i + 8 <= len; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        sum ^= w;
    }
    return (uint32_t)(sum >> 32) ^ (uint32_t)sum;
}

uint32_t nv_gsp_msg_build(void *elem, uint32_t cap, uint32_t seq, uint32_t function,
                          const void *payload, uint32_t payload_len)
{
    uint32_t msg_len = (uint32_t)(GSP_MSG_QUEUE_ELEMENT_HDR_SIZE + sizeof(rpc_message_header_v));
    if (payload_len > GSP_MSG_QUEUE_ELEMENT_SIZE_MAX - msg_len)
        return 0;
    msg_len += payload_len;
    uint32_t count = (msg_len + GSP_MSG_QUEUE_ELEMENT_SIZE_MIN - 1) / GSP_MSG_QUEUE_ELEMENT_SIZE_MIN;
    if ((uint64_t)count * GSP_MSG_QUEUE_ELEMENT_SIZE_MIN > cap)
        return 0;

    memset(elem, 0, (size_t)count * GSP_MSG_QUEUE_ELEMENT_SIZE_MIN);
    GSP_MSG_QUEUE_ELEMENT *e = (GSP_MSG_QUEUE_ELEMENT *)elem;
    e->seqNum    = seq;
    e->elemCount = count;
    e->rpc.header_version     = NV_VGPU_MSG_HEADER_VERSION;
    e->rpc.signature          = NV_VGPU_MSG_SIGNATURE_VALID;
    e->rpc.length             = (uint32_t)sizeof(rpc_message_header_v) + payload_len;
    e->rpc.function           = function;
    e->rpc.rpc_result         = NV_VGPU_MSG_RESULT_RPC_PENDING;
    e->rpc.rpc_result_private = NV_VGPU_MSG_RESULT_RPC_PENDING;
    e->rpc.sequence           = 0;
    if (payload_len)
        memcpy((uint8_t *)elem + GSP_MSG_QUEUE_ELEMENT_HDR_SIZE + sizeof(rpc_message_header_v),
               payload, payload_len);

    // The rest of the element is zero, so summing up to the 8-byte-padded length
    // equals NVIDIA's sum over msgLen with zero padding.
    e->checkSum = 0;
    e->checkSum = nv_gsp_checksum(elem, (uint32_t)align_up(msg_len, 8));
    return count;
}

bool nv_gsp_cmdq_push(uint8_t *cmdq, const void *msg, uint32_t count, uint32_t their_rptr)
{
    msgqTxHeader tx;
    memcpy(&tx, cmdq, sizeof(tx));
    if (!tx.msgCount || tx.writePtr >= tx.msgCount || their_rptr >= tx.msgCount || !count)
        return false;
    uint32_t used = (tx.writePtr + tx.msgCount - their_rptr) % tx.msgCount;
    if (count > tx.msgCount - 1 - used)
        return false;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t slot = (tx.writePtr + i) % tx.msgCount;
        memcpy(cmdq + tx.entryOff + (uint64_t)slot * tx.msgSize,
               (const uint8_t *)msg + (uint64_t)i * GSP_MSG_QUEUE_ELEMENT_SIZE_MIN, GSP_MSG_QUEUE_ELEMENT_SIZE_MIN);
    }
    // Entries first, then the pointer the GSP polls (the caller adds a barrier).
    uint32_t wptr = (tx.writePtr + count) % tx.msgCount;
    memcpy(cmdq + offsetof(msgqTxHeader, writePtr), &wptr, 4);
    return true;
}

bool nv_gsp_msgq_linked(const msgqTxHeader *h, uint32_t size)
{
    // msgqRxLink's checks on the other side's header.
    return h->size == size && h->msgSize == GSP_MSG_QUEUE_ELEMENT_SIZE_MIN &&
           h->rxHdrOff >= sizeof(msgqTxHeader) &&
           h->entryOff >= h->rxHdrOff + sizeof(msgqRxHeader) && h->entryOff < size &&
           h->msgCount == (size - h->entryOff) / h->msgSize && h->msgCount > 1;
}

uint32_t nv_gsp_msgq_read(const uint8_t *msgq, uint32_t rptr, void *out, uint32_t cap)
{
    msgqTxHeader h;
    memcpy(&h, msgq, sizeof(h));
    if (rptr >= h.msgCount)
        return 0;
    const uint8_t *first = msgq + h.entryOff + (uint64_t)rptr * h.msgSize;
    uint32_t count;
    memcpy(&count, first + offsetof(GSP_MSG_QUEUE_ELEMENT, elemCount), 4);
    if (count == 0 || count > GSP_MSG_QUEUE_ELEMENT_SIZE_MAX / GSP_MSG_QUEUE_ELEMENT_SIZE_MIN ||
        count >= h.msgCount || (uint64_t)count * GSP_MSG_QUEUE_ELEMENT_SIZE_MIN > cap)
        return 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t slot = (rptr + i) % h.msgCount;
        memcpy((uint8_t *)out + (uint64_t)i * GSP_MSG_QUEUE_ELEMENT_SIZE_MIN,
               msgq + h.entryOff + (uint64_t)slot * h.msgSize, GSP_MSG_QUEUE_ELEMENT_SIZE_MIN);
    }
    return count;
}

const char *nv_gsp_msg_check(const void *elem, uint32_t len)
{
    if (len < sizeof(GSP_MSG_QUEUE_ELEMENT) || (len & 7))
        return "message shorter than its headers";
    const GSP_MSG_QUEUE_ELEMENT *e = (const GSP_MSG_QUEUE_ELEMENT *)elem;
    if (e->elemCount == 0 || (uint64_t)e->elemCount * GSP_MSG_QUEUE_ELEMENT_SIZE_MIN > len)
        return "bad element count";
    // The receiver sums the whole elemCount * 4 KiB span.
    if (nv_gsp_checksum(elem, e->elemCount * GSP_MSG_QUEUE_ELEMENT_SIZE_MIN) != 0)
        return "checksum mismatch";
    if (e->rpc.signature != NV_VGPU_MSG_SIGNATURE_VALID)
        return "bad RPC signature";
    if (e->rpc.length < sizeof(rpc_message_header_v) ||
        GSP_MSG_QUEUE_ELEMENT_HDR_SIZE + (uint64_t)e->rpc.length > (uint64_t)e->elemCount * GSP_MSG_QUEUE_ELEMENT_SIZE_MIN)
        return "RPC length outside the message";
    return nullptr;
}

const char *nv_nocat_type_name(uint8_t recType)
{
    static const char *const names[] = { "unknown", "bugcheck", "engine", "TDR", "RC", "assert" };
    return recType < sizeof(names) / sizeof(names[0]) ? names[recType] : "?";
}

// ---- Registry ------------------------------------------------------------------------

const nv_reg_dword nv_gsp_default_registry[] = {
    { "RMForcePcieConfigSave", 1 },
    { "RMSecBusResetEnable",   1 },
    { "RMDevidCheckIgnore",    1 },
};
const uint32_t nv_gsp_default_registry_count =
    sizeof(nv_gsp_default_registry) / sizeof(nv_gsp_default_registry[0]);

uint32_t nv_gsp_registry_pack(void *out, uint32_t cap, const nv_reg_dword *e, uint32_t n)
{
    uint64_t size = sizeof(PACKED_REGISTRY_TABLE) + (uint64_t)n * sizeof(PACKED_REGISTRY_ENTRY);
    for (uint32_t i = 0; i < n; i++)
        size += strlen(e[i].name) + 1;
    if (size > cap || size > 0xffffffffu)
        return 0;

    uint8_t *base = (uint8_t *)out;
    memset(base, 0, (size_t)size);
    PACKED_REGISTRY_TABLE hdr = { (uint32_t)size, n };
    memcpy(base, &hdr, sizeof(hdr));

    uint32_t name_off = (uint32_t)(sizeof(PACKED_REGISTRY_TABLE) + n * sizeof(PACKED_REGISTRY_ENTRY));
    for (uint32_t i = 0; i < n; i++) {
        PACKED_REGISTRY_ENTRY ent = {};
        ent.nameOffset = name_off;
        ent.type   = REGISTRY_TABLE_ENTRY_TYPE_DWORD;
        ent.data   = e[i].value;
        ent.length = 4;
        memcpy(base + sizeof(PACKED_REGISTRY_TABLE) + i * sizeof(PACKED_REGISTRY_ENTRY), &ent, sizeof(ent));
        size_t len = strlen(e[i].name) + 1;
        memcpy(base + name_off, e[i].name, len);
        name_off += (uint32_t)len;
    }
    return (uint32_t)size;
}

// ---- Sequencer --------------------------------------------------------------------------

static int seq_payload_dwords(uint32_t op)
{
    switch (op) {
    case GSP_SEQ_BUF_OPCODE_REG_WRITE:  return 2;
    case GSP_SEQ_BUF_OPCODE_REG_MODIFY: return 3;
    case GSP_SEQ_BUF_OPCODE_REG_POLL:   return 5;
    case GSP_SEQ_BUF_OPCODE_DELAY_US:   return 1;
    case GSP_SEQ_BUF_OPCODE_REG_STORE:  return 2;
    case GSP_SEQ_BUF_OPCODE_CORE_RESET:
    case GSP_SEQ_BUF_OPCODE_CORE_START:
    case GSP_SEQ_BUF_OPCODE_CORE_WAIT_FOR_HALT:
    case GSP_SEQ_BUF_OPCODE_CORE_RESUME: return 0;
    default: return -1;
    }
}

bool nv_gsp_seq_next(const uint32_t *cmds, uint32_t used, uint32_t *pos, nv_seq_op *op)
{
    memset(op, 0, sizeof(*op));
    if (*pos >= used)
        return false;
    op->opcode = cmds[*pos];
    int dwords = seq_payload_dwords(op->opcode);
    if (dwords < 0)
        return false;
    uint32_t n = (uint32_t)dwords;
    if ((uint64_t)*pos + 1 + n > used)
        return false;
    if (op->opcode == GSP_SEQ_BUF_OPCODE_REG_STORE && cmds[*pos + 2] >= GSP_SEQ_BUF_REG_SAVE_SIZE)
        return false;
    op->nargs = n;
    for (uint32_t i = 0; i < n; i++)
        op->args[i] = cmds[*pos + 1 + i];
    *pos += 1 + n;
    return true;
}

const char *nv_gsp_seq_name(uint32_t opcode)
{
    static const char *const names[] = {
        "REG_WRITE", "REG_MODIFY", "REG_POLL", "DELAY_US", "REG_STORE",
        "CORE_RESET", "CORE_START", "CORE_WAIT_FOR_HALT", "CORE_RESUME",
    };
    return opcode < sizeof(names) / sizeof(names[0]) ? names[opcode] : "?";
}

// ---- Boot structures ------------------------------------------------------------------

void nv_gsp_wpr_meta_fill(GspFwWprMeta *m, const nv_wpr2_layout *l, const nv_gsp_sysmem *s)
{
    // kgspPopulateWprMeta_TU102 (r570). Offsets are VRAM addresses.
    memset(m, 0, sizeof(*m));
    m->magic    = GSP_FW_WPR_META_MAGIC;
    m->revision = GSP_FW_WPR_META_REVISION;

    m->sysmemAddrOfRadix3Elf    = s->radix3_pa;
    m->sizeOfRadix3Elf          = s->elf_size;
    m->sysmemAddrOfBootloader   = s->bl_pa;
    m->sizeOfBootloader         = s->bl_size;
    m->bootloaderCodeOffset     = s->bl_code_off;
    m->bootloaderDataOffset     = s->bl_data_off;
    m->bootloaderManifestOffset = s->bl_manifest_off;
    m->sysmemAddrOfSignature    = s->sig_pa;
    m->sizeOfSignature          = s->sig_size;

    m->gspFwRsvdStart     = l->nonwpr_addr;
    m->nonWprHeapOffset   = l->nonwpr_addr;
    m->nonWprHeapSize     = l->nonwpr_size;
    m->gspFwWprStart      = l->wpr2_addr;
    m->gspFwHeapOffset    = l->heap_addr;
    m->gspFwHeapSize      = l->heap_size;
    m->gspFwOffset        = l->elf_addr;
    m->bootBinOffset      = l->boot_addr;
    m->frtsOffset         = l->frts_addr;
    m->frtsSize           = l->frts_size;
    m->gspFwWprEnd        = l->wpr_end;
    m->fbSize             = l->fb_size;
    m->vgaWorkspaceOffset = l->vga_addr;
    m->vgaWorkspaceSize   = l->vga_size;
    // bootCount, flags (no clock boost, no recovery margin), verified: 0
}

const char *const nv_gsp_log_names[NV_GSP_LOG_COUNT] = { "LOGINIT", "LOGINTR", "LOGRM", "LOGMNOC" };

uint32_t nv_gsp_libos_args_fill(LibosMemoryRegionInitArgument *a, const uint64_t *log_pa,
                                uint64_t rmargs_pa, uint64_t rmargs_size)
{
    // kgspSetupLibosInitArgs: the logs first (LOGINIT must be first), then RMARGS.
    memset(a, 0, LIBOS_INIT_ARGUMENTS_SIZE);
    uint32_t i = 0;
    for (; i < NV_GSP_LOG_COUNT; i++) {
        a[i].id8  = nv_libos_id8(nv_gsp_log_names[i]);
        a[i].pa   = log_pa[i];
        a[i].size = NV_GSP_LOG_SIZE;
        a[i].kind = LIBOS_MEMORY_REGION_CONTIGUOUS;
        a[i].loc  = LIBOS_MEMORY_REGION_LOC_SYSMEM;
    }
    a[i].id8  = nv_libos_id8("RMARGS");
    a[i].pa   = rmargs_pa;
    a[i].size = rmargs_size;
    a[i].kind = LIBOS_MEMORY_REGION_CONTIGUOUS;
    a[i].loc  = LIBOS_MEMORY_REGION_LOC_SYSMEM;
    return i + 1;
}

void nv_gsp_rmargs_fill(GSP_ARGUMENTS_CACHED *g, const nv_gsp_shm_layout *l, uint64_t shm_pa)
{
    // kgspPopulateGspRmInitArgs, normal (non-resume) boot.
    memset(g, 0, sizeof(*g));
    g->messageQueueInitArguments.sharedMemPhysAddr   = shm_pa;
    g->messageQueueInitArguments.pageTableEntryCount = l->pte_count;
    g->messageQueueInitArguments.cmdQueueOffset      = l->cmdq_off;
    g->messageQueueInitArguments.statQueueOffset     = l->msgq_off;
    g->bDmemStack  = 1;     // NV_REG_STR_RM_GSP_STACK_PLACEMENT default
    g->gpuInstance = 0;
}

void nv_gsp_sysinfo_fill(GspSystemInfo *si, const nv_gsp_pci_info *p)
{
    // rpcGspSetSystemInfo_v17_00 (r570), reduced to what a bare Turing laptop GPU
    // has: no SR-IOV, no hypervisor, ACPI method data left invalid.
    memset(si, 0, sizeof(*si));
    si->gpuPhysAddr           = p->bar0_pa;
    si->gpuPhysFbAddr         = p->bar1_pa;
    si->gpuPhysInstAddr       = p->bar3_pa;
    si->nvDomainBusDeviceFunc = (uint64_t)p->bus << 8 | (uint64_t)p->dev << 3 | p->fn;
    si->maxUserVa             = 0x00007ffffffff000ull;     // x86-64 user VA limit, as Linux
    si->pciConfigMirrorBase   = 0x088000;                  // kbifGetPciConfigSpacePriMirror (TU102)
    si->pciConfigMirrorSize   = 0x001000;
    si->PCIDeviceID           = (uint32_t)p->device << 16 | p->vendor;
    si->PCISubDeviceID        = (uint32_t)p->subdevice << 16 | p->subvendor;
    si->PCIRevisionID         = p->revision;
    si->hostPageSize          = 0x1000;
}
