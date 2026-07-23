/*
 * ehci_os.c — Mac OS 9 glue for the EHCI UIM (Milestone 2).
 *
 * Implements the init sequence decoded from Apple's OHCI UIMInitialize
 * (see ../M2-GLUE-SPEC.md): enable the PCI controller, obtain the operational
 * register base from the Name Registry node, allocate the periodic frame list
 * as wired, physically-contiguous DMA memory, then run the EHCI bring-up.
 *
 * This is the only file that pulls in the OS 9 driver headers; the pure EHCI
 * logic (ehci_hw.c) and the dispatch scaffold (ehci_uim.c) stay OS-independent.
 */
#include <MacTypes.h>
#include <NameRegistry.h>
#include <PCI.h>
#include <DriverServices.h>
#include <MacMemory.h>
#include <Devices.h>
#include <Files.h>
#include "ehci.h"

/* Init-phase tracing to a FLUSHED disk file. uimInitialize runs synchronously in the app's
 * context via LoadUIMForEntry, so File Manager is safe here, and a per-line flush means the last
 * surviving line pinpoints the phase that crashed (r4/r5 = error type 12 inside init). The file
 * lands next to the app; open EHCIUIM_init.log in SimpleText after a reboot. */
static short gDbgRef = 0;
static short gDbgVol = 0;   /* r49: the log's OWN volume (boot), captured at create */
void ehci_os_log(const char *s)
{
    FSSpec sp; long n = 0, z = 1; static int tried = 0;
    if (!tried) { tried = 1;
        (void)FSMakeFSSpec(0, 0, "\pEHCIUIM_init.log", &sp);
        /* v37: APPEND across driver loads (do NOT FSpDelete). A later mount-only run must not wipe an
         * earlier copy session's data (that footgun cost the v36 run-1 log). Each run is delimited by the
         * "=== v37 RUN ===" banner, and the per-run in-memory counters (gWrTotal/gSuspN/gSubmitReentry/...)
         * reset on load, so each run's section stands alone. Open the existing file (or create it), then
         * seek to EOF so new lines append. */
        if (FSpOpenDF(&sp, fsRdWrPerm, &gDbgRef) != noErr) {
            if (FSpCreate(&sp, 'ttxt', 'TEXT', 0) == noErr) (void)FSpOpenDF(&sp, fsRdWrPerm, &gDbgRef);
        }
        if (gDbgRef) { gDbgVol = sp.vRefNum; (void)SetFPos(gDbgRef, fsFromLEOF, 0); }
    }
    if (gDbgRef) { ParamBlockRec pbf; while (s[n]) n++; (void)FSWrite(gDbgRef, &n, (Ptr)s);
        (void)FSWrite(gDbgRef, &z, (Ptr)"\r");
        /* r49 COPY-SAFETY: flush the LOG's OWN volume, not FlushVol(0,0)=default — the default volume
         * SWITCHES to the USB stick once it mounts, so post-mount writes never committed the boot-volume
         * log's catalog EOF and every network/FAT copy read a stale, truncated snapshot. Flush the file
         * first (buffer -> disk) then its volume (commit the catalog EOF) so the open log copies at full size. */
        pbf.ioParam.ioCompletion = 0; pbf.ioParam.ioRefNum = gDbgRef; (void)PBFlushFileSync(&pbf);
        (void)FlushVol(0, gDbgVol); }
}
/* Log "label 0xVALUE" without stdio (a driver has no sprintf). */
void ehci_os_logx(const char *label, unsigned long v)
{
    char buf[80]; int i = 0, j; static const char hx[] = "0123456789abcdef";
    while (label[i] && i < 60) { buf[i] = label[i]; i++; }
    buf[i++] = ' '; buf[i++] = '0'; buf[i++] = 'x';
    for (j = 28; j >= 0; j -= 4) buf[i++] = hx[(v >> j) & 0xF];
    buf[i] = 0;
    ehci_os_log(buf);
}
#define dbg(s) ehci_os_log(s)

/* PCI config space + Command register bits */
#define kPCICommandReg    0x04
#define kPCICmdMemSpace   0x0002
#define kPCICmdBusMaster  0x0004

/* OF "assigned-addresses" entry = 5 x UInt32 (phys.hi, mid, lo, size.hi, lo) */
#define kAddrEntryWords   5
#define AA_SPACE(physhi)  (((physhi) >> 24) & 0x3)   /* 2=mem32, 3=mem64 */

/* Read a Name Registry property into a pool buffer. Caller frees via PoolDeallocate. */
static OSStatus get_prop(RegEntryIDPtr node, const char *name,
                         void **outBuf, ByteCount *outSize)
{
    OSStatus err;
    ByteCount size = 0;
    void *buf;

    err = RegistryPropertyGetSize(node, name, &size);
    if (err != noErr) return err;
    buf = PoolAllocateResident(size, false);
    if (buf == NULL) return memFullErr;
    err = RegistryPropertyGet(node, name, buf, &size);
    if (err != noErr) { PoolDeallocate(buf); return err; }
    *outBuf = buf;
    *outSize = size;
    return noErr;
}

/* Claim the EHCI controller from any pre-OS owner via the USB Legacy Support
 * extended capability (USBLEGSUP). On a Mac there's usually no BIOS to hand off
 * from, but a correct EHCI driver must still request OS ownership and disable
 * legacy SMIs before touching the operational registers. HCCPARAMS.EECP points
 * to the capability list in PCI config space; walk it for cap-id 1. */
static void ehci_eecp_handoff(ehci_softc *sc, RegEntryIDPtr node)
{
    UInt32 hcc = ehci_read32(sc->capBase, EHCI_HCCPARAMS);
    UInt32 eecp = EHCI_HCC_EECP(hcc);      /* PCI config offset, 0 => none */
    int guard;

    while (eecp >= 0x40) {
        UInt32 cap = 0;
        if (ExpMgrConfigReadLong(node, (LogicalAddress)eecp, &cap) != noErr)
            return;
        if ((cap & 0xFF) == 1) {           /* USBLEGSUP capability */
            cap |= (1UL << 24);            /* set HC OS-Owned Semaphore */
            ExpMgrConfigWriteLong(node, (LogicalAddress)eecp, cap);
            for (guard = 0; guard < 1000000; guard++) {  /* wait for BIOS-owned clear */
                if (ExpMgrConfigReadLong(node, (LogicalAddress)eecp, &cap) != noErr)
                    break;
                if (!(cap & (1UL << 16)))
                    break;
            }
            /* disable all legacy SMI sources (USBLEGCTLSTS at EECP+4) */
            ExpMgrConfigWriteLong(node, (LogicalAddress)(eecp + 4), 0);
            return;
        }
        eecp = (cap >> 8) & 0xFF;          /* next capability pointer */
    }
}

#define kEHCIErrNoMemBAR      (-6641L)   /* no memory-space BAR in assigned-addresses */
#define kEHCIErrNoLogicalBase (-6642L)   /* AAPL,address missing (unexpected) */

/* Map the operational register base: find the memory BAR in assigned-addresses,
 * then use the parallel AAPL,address entry (the Mac-OS-created CPU-logical
 * mapping) as capBase. assigned-addresses/AAPL,address entries correspond by
 * index. Confirmed on the IOGEAR/NEC card: BAR0 is mem32 @ phys 0x80080000. */
static OSStatus map_registers(ehci_softc *sc, RegEntryIDPtr node)
{
    OSStatus err;
    UInt32 *aa = NULL, *la = NULL;
    ByteCount aaSize = 0, laSize = 0;
    UInt32 i, nEntries, memIdx = 0xFFFFFFFFUL;

    err = get_prop(node, "assigned-addresses", (void **)&aa, &aaSize);
    if (err != noErr) return err;

    /* locate the first memory-space BAR; record its index + physical address */
    nEntries = aaSize / (kAddrEntryWords * sizeof(UInt32));
    for (i = 0; i < nEntries; i++) {
        UInt32 physHi = aa[i * kAddrEntryWords];       /* host-endian */
        if (AA_SPACE(physHi) >= 2) {                   /* mem32 / mem64 */
            memIdx = i;
            sc->regPhys = aa[i * kAddrEntryWords + 2]; /* phys.lo = assigned addr */
            break;
        }
    }
    if (memIdx == 0xFFFFFFFFUL) { PoolDeallocate(aa); return kEHCIErrNoMemBAR; }

    /* logical base = AAPL,address[memIdx] (present at OS 9 runtime; see notes) */
    err = get_prop(node, "AAPL,address", (void **)&la, &laSize);
    if (err == noErr && (memIdx * sizeof(UInt32)) < laSize) {
        sc->capBase = (volatile void *)la[memIdx];
        PoolDeallocate(la);
        PoolDeallocate(aa);
        return noErr;
    }
    if (la) PoolDeallocate(la);
    PoolDeallocate(aa);
    return kEHCIErrNoLogicalBase;   /* distinct: AAPL,address unexpectedly absent */
}

/* Wire a single physically-contiguous page and expose it as a bump allocator.
 * One page (<=4 KB) is guaranteed contiguous, so basePhys+offset is a valid
 * physical address for every sub-allocation. */
long ehci_dma_pool_init(ehci_dma_pool *p, UInt32 size)
{
    Ptr raw;
    LogicalAddress buf;
    LogicalToPhysicalTable tbl;
    unsigned long count = 1;
    OSStatus err;

    if (size > 0x1000) size = 0x1000;          /* keep to one contiguous page */
    raw = NewPtrSysClear(size + 0x1000);
    if (raw == NULL) return memFullErr;
    buf = (LogicalAddress)(((UInt32)raw + 0xFFF) & ~0xFFFUL);   /* page align */

    err = LockMemory(buf, size);
    if (err != noErr) return err;
    tbl.logical.address = buf;
    tbl.logical.count = size;
    err = GetPhysical(&tbl, &count);
    if (err != noErr) return err;

    p->base = (UInt8 *)buf;
    p->basePhys = (UInt32)tbl.physical[0].address;
    p->size = size;
    p->used = 0;
    return noErr;
}

void *ehci_dma_alloc(ehci_dma_pool *p, UInt32 bytes, UInt32 align, UInt32 *physOut)
{
    UInt32 off = (p->used + (align - 1)) & ~(align - 1);
    if (off + bytes > p->size) return 0;       /* pool exhausted */
    p->used = off + bytes;
    if (physOut) *physOut = p->basePhys + off;
    return p->base + off;                       /* already zeroed by NewPtrSysClear */
}

/* Allocate the periodic frame list: 4 KB, 4 KB-aligned, wired, with phys addr. */
static OSStatus alloc_framelist(ehci_softc *sc)
{
    OSStatus err;
    Ptr raw;
    LogicalAddress buf;
    LogicalToPhysicalTable tbl;
    unsigned long count = 1;

    raw = NewPtrSysClear(EHCI_FRAMELIST_BYTES + 0x1000);
    if (raw == NULL) return memFullErr;
    buf = (LogicalAddress)(((UInt32)raw + 0xFFF) & ~0xFFFUL);   /* 4 KB align */

    err = LockMemory(buf, EHCI_FRAMELIST_BYTES);
    if (err != noErr) return err;

    tbl.logical.address = buf;
    tbl.logical.count = EHCI_FRAMELIST_BYTES;
    err = GetPhysical(&tbl, &count);
    if (err != noErr) return err;

    sc->frameList = (UInt32 *)buf;
    sc->frameListPhys = (UInt32)tbl.physical[0].address;
    return noErr;
}

/* Milestone-2 init entry, called from uimInitialize with the controller node. */
OSStatus ehci_os_init(ehci_softc *sc, EHCIRegEntryIDPtr nodeArg)
{
    RegEntryIDPtr node;
    RegEntryIter it; Boolean done = false; UInt32 want = 0x000c0320UL;
    OSStatus err;
    (void)nodeArg;   /* the dispatch slot-0 argument ABI is unverified; find the node ourselves,
                      * exactly as the PROVEN app leg's main() did (RegistryEntrySearch class-code). */

    dbg("ehci_os_init: entered");
    if (RegistryEntryIterateCreate(&it) != noErr) { dbg("ehci_os_init: iter-create FAILED"); return -1; }
    err = RegistryEntrySearch(&it, kRegIterDescendants, (RegEntryID *)&sc->node, &done,
                              "class-code", &want, sizeof(want));
    RegistryEntryIterateDispose(&it);
    if (err != noErr) { dbg("ehci_os_init: EHCI node NOT FOUND"); return err; }
    node = (RegEntryIDPtr)&sc->node;
    dbg("ehci_os_init: EHCI node found");

    /* enable Memory Space + Bus Master (OHCI UIM writes Command = 0x0006) */
    err = ExpMgrConfigWriteWord(node, (LogicalAddress)kPCICommandReg,
                                (UInt16)(kPCICmdMemSpace | kPCICmdBusMaster));
    if (err != noErr) { dbg("ehci_os_init: PCI enable FAILED"); return err; }
    dbg("ehci_os_init: PCI enabled");

    err = map_registers(sc, node);
    if (err == kEHCIErrNoMemBAR)      { dbg("EHCIUIM: no memory BAR"); return err; }
    if (err == kEHCIErrNoLogicalBase) { dbg("EHCIUIM: AAPL,address missing"); return err; }
    if (err != noErr)                 { dbg("EHCIUIM: map_registers FAILED"); return err; }
    dbg("EHCIUIM: registers mapped");

    /* operational registers start at capBase + CAPLENGTH; read port count */
    sc->opBase = (volatile UInt8 *)sc->capBase + ehci_read8(sc->capBase, EHCI_CAPLENGTH);
    sc->nPorts = (UInt8)EHCI_HCS_N_PORTS(ehci_read32(sc->capBase, EHCI_HCSPARAMS));

    /* claim the controller from any pre-OS owner before touching it */
    ehci_eecp_handoff(sc, node);
    dbg("EHCIUIM: EECP handoff done");

    err = alloc_framelist(sc);
    if (err != noErr) { dbg("EHCIUIM: framelist alloc FAILED"); return err; }

    /* DMA pool for QHs/qTDs, then the async-schedule anchor QH (sets asyncQHPhys
     * so ehci_hc_start can safely enable the async schedule). */
    err = ehci_dma_pool_init(&sc->pool, 0x1000);
    if (err != noErr) { dbg("EHCIUIM: DMA pool FAILED"); return err; }
    if (ehci_build_async_anchor(sc) != 0) { dbg("EHCIUIM: async anchor FAILED"); return -1; }

    if (ehci_hc_reset(sc) != 0) { dbg("EHCIUIM: HCReset TIMEOUT"); return -1; }
    dbg("EHCIUIM: HCReset ok");

    err = ehci_hc_start(sc);
    dbg("EHCIUIM: started (init complete)");
    return err;
}

/*
 * DoDriverIO — standard native-driver entry point. The generic driver loader
 * calls this (rather than the USB Expert calling ThePluginDispatchTable) once
 * our runtime flags request self-loading. On Initialize/Replace it hands us the
 * controller's Name Registry node in the command contents; we bring up EHCI.
 */
OSErr DoDriverIO(AddressSpaceID spaceID, IOCommandID cmdID,
                 IOCommandContents contents, IOCommandCode code, IOCommandKind kind)
{
    (void)spaceID; (void)cmdID; (void)kind;
    switch (code) {
    case kInitializeCommand:
    case kReplaceCommand:
        dbg("EHCIUIM: DoDriverIO Initialize");
        return (OSErr)ehci_os_init(&gSoftc,
                    (EHCIRegEntryIDPtr)&contents.initialInfo->deviceEntry);
    case kFinalizeCommand:
    case kSupersededCommand:
        dbg("EHCIUIM: DoDriverIO Finalize");
        return noErr;
    case kOpenCommand:
    case kCloseCommand:
        return noErr;
    default:
        return noErr;
    }
}
