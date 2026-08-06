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
#include "ehci_vhub.h"   /* Path A: DoDriverIO kOpen drives the vhub bring-up (xfer_init + start_service) */

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

/* ★★ n5 INTERRUPT-SAFE LOG RING ★★
 * ehci_os_log/logx above are synchronous File Manager I/O and are FATAL below task level (the r18 hang, and
 * again in n4 — see the project's standing rule). n5 moves enumeration and the BOT probe onto our own
 * heartbeat, i.e. interrupt level, so every log call on that path must go through here instead: the ISR only
 * records a POINTER, and a task-level drain does the actual writing.
 *
 * Storing the pointer rather than a copy is what makes this nearly free, and it is safe ONLY because every
 * caller passes a STRING LITERAL — those live in the PEF's constant data for the life of the fragment.
 * ⚠ NEVER pass a stack buffer or anything else with a shorter lifetime to ehci_os_ilog/ilogx.
 *
 * Draining is opportunistic by design: once no application is running there is no task-level context of our
 * own, so the ring is flushed from wherever a task-level call happens to reach us — uim23 if something is
 * pumping, and the block driver's DoDriverIO (File Manager calls, definitely task level) once a volume is
 * mounted. Enumeration lines may therefore appear LATE, in a burst. Overflow is counted, never silent. */
#define ILOG_N 384
typedef struct { const char *msg; UInt32 val; UInt8 kind; } ILogRec;   /* kind 1 = msg, 2 = msg + value */
static volatile ILogRec gILog[ILOG_N];
static volatile UInt32  gILogHead = 0, gILogTail = 0, gILogDropped = 0;
/* ★★★★ h16: line-rate cap as a TOKEN BUCKET rather than a fixed per-second window.
 * h15's flat 30 lines/second protected the machine but was too tight for a LEGITIMATE burst: on the validated
 * four-drive run it fired twice and dropped 43 lines while the four drives were enumerating, losing healthy
 * diagnostic detail exactly where it is most interesting. A bucket separates the two things that matter — a
 * burst allowance for real events, and a sustained ceiling that no loop can exceed.
 * ILOG_BURST is the depth (a whole enumeration burst fits), ILOG_RATE_PER_SEC the refill. A livelock still
 * drains the bucket in well under a second and is then held to the sustained rate, which is what keeps the
 * File Manager responsive; a four-drive enumeration now fits entirely inside the burst. */
/* ★ The BURST is what fixes h15's lost enumeration detail; the SUSTAINED rate is what protects the machine.
 * They are independent, so keep the sustained rate at h15's already-proven-safe 30/s rather than raising it:
 * at roughly 1-5 ms per FSWrite + FlushVol that is ~3-15% of task time, against the ~100% saturation that made
 * the h14 livelock look like a hang. A legitimate four-drive enumeration (~200 lines) fits inside the burst
 * whole, and a livelock spends the burst in its first second and is then held to 30/s for as long as it runs. */
#define ILOG_BURST        256u
#define ILOG_RATE_PER_SEC  30u
static UInt32 gILogTokens = ILOG_BURST, gILogRefillT = 0;
static volatile UInt32 gILogThrottled = 0;

void ehci_os_ilog(const char *s)
{
    UInt32 i = gILogHead;
    if (i - gILogTail >= ILOG_N) { gILogDropped++; return; }   /* ring full: count, never block, never wrap */
    gILog[i % ILOG_N].msg = s; gILog[i % ILOG_N].val = 0; gILog[i % ILOG_N].kind = 1;
    __asm__ __volatile__("eieio");                             /* publish payload before the index */
    gILogHead = i + 1;
}
void ehci_os_ilogx(const char *label, unsigned long v)
{
    UInt32 i = gILogHead;
    if (i - gILogTail >= ILOG_N) { gILogDropped++; return; }
    gILog[i % ILOG_N].msg = label; gILog[i % ILOG_N].val = (UInt32)v; gILog[i % ILOG_N].kind = 2;
    __asm__ __volatile__("eieio");
    gILogHead = i + 1;
}
/* TASK LEVEL ONLY — this is the one place the ring touches the File Manager. */
void ehci_os_ilog_drain(void)
{
    UInt32 d, budget = ILOG_N;
    /* ⚠ BOUNDED, and it must stay bounded. The producer runs at INTERRUPT level and can refill the ring
     * while we are draining — each line here is a File Manager write, so the drain is orders of magnitude
     * slower than the producer. An unbounded `while (tail != head)` can therefore livelock at TASK level,
     * which wedges whatever called us: for n5 that is the application's pump loop, so the machine keeps
     * running while our driver silently stops being serviced. Draining at most one ring's worth per call
     * guarantees we return; anything still queued goes out on the next tick.
     * The `!=` is also deliberately paired with the budget: if tail ever overshot head, `!=` alone would
     * spin ~4 billion times. */
    /* ★★★★ h15: A HARD LINE-RATE CAP, because the log is a File-Manager AMPLIFIER.
     * Every line written here is a synchronous FSWrite + FlushVol. The producer runs from the 8 ms heartbeat,
     * so a driver bug that loops at heartbeat rate emits a few hundred lines a second, and the drain then
     * spends most of task time inside the File Manager. On the h14 run that is exactly what the user saw: a
     * stale-state livelock in the hub path (fixed separately) produced ~3 lines per pass, and the machine
     * showed a wristwatch cursor the moment the Finder was asked to do anything. The DRIVER bug was the cause,
     * but the LOG is what escalated it from "a drive did not mount" to "the machine is hung".
     * Consecutive-line dedup does not help — what repeats is a CYCLE of several different lines, not one line.
     * So cap the rate instead: at most ILOG_MAX_PER_SEC lines per 60-tick second, the excess counted and
     * reported exactly like a ring overflow. Diagnostics survive (the cap is an order of magnitude above what
     * a healthy run produces — h13 logged 1618 lines in a whole session), and no future bug of ANY shape can
     * take the machine down through this path again. */
    {   UInt32 now = *(volatile UInt32 *)0x016AUL;              /* lowmem Ticks, 60 Hz */
        /* h16: refill by elapsed ticks, capped at the burst depth. Integer ticks -> lines at 60 Hz means one
         * token per tick when ILOG_RATE_PER_SEC is 60; written as a ratio so the rate can change freely. */
        if (now != gILogRefillT) {
            UInt32 add = ((now - gILogRefillT) * ILOG_RATE_PER_SEC) / 60UL;
            if (add) {
                gILogTokens = (gILogTokens + add > ILOG_BURST) ? ILOG_BURST : gILogTokens + add;
                gILogRefillT = now;
            }
        }
        while (gILogTail != gILogHead && budget--) {
            volatile ILogRec *r = &gILog[gILogTail % ILOG_N];
            const char *m = r->msg; UInt32 v = r->val; UInt8 k = r->kind;
            gILogTail++;                                       /* consume BEFORE writing: a File-Mgr stall must
                                                                * not make us re-emit the same line */
            if (!gILogTokens) { gILogThrottled++; continue; }   /* h15: drop, never stall */
            gILogTokens--;
            if (k == 2) ehci_os_logx(m, v); else if (k == 1) ehci_os_log(m);
        }
    }
    d = gILogDropped;
    if (d) { gILogDropped = 0; ehci_os_logx("!! n5 log ring OVERFLOWED — lines dropped", d); }
    /* h15: report the throttle only when the budget has room again, so saying so cannot itself be throttled. */
    if (gILogThrottled && gILogTokens) {
        UInt32 t = gILogThrottled; gILogThrottled = 0; gILogTokens--;
        ehci_os_logx("!! h15 log RATE-CAPPED — lines dropped to keep the File Manager responsive (something "
                     "is looping; the cap is not the bug)", t);
    }
}

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
 * controller's Name Registry node in the command contents; bring-up is deferred
 * to kOpen (Path A boot-safety — see the switch below).
 */
/* Minimal, boot-safe quiesce (called from kInitialize, every boot). If a PREVIOUS session left the EHCI
 * controller HOT (interrupts enabled / schedules running) and the machine was WARM-restarted (so the PCI
 * card kept power), the controller keeps asserting interrupts + DMAing into this boot, where our driver
 * is dormant (kOpen not yet called) -> unserviced-interrupt storm + stale DMA = the "unhealthy boot".
 * Tame it with a HANDFUL of MMIO ops and NOTHING heavy: no HCReset, no DMA alloc, no IRQ install, no spin
 * loop -- so it stays clear of the early-boot hazards that make full bring-up freeze. Guarded: bails if
 * the BAR isn't responding, and on a cold boot (controller already quiet) it just reads two registers and
 * returns. NO logging (File Manager may be down this early). Best-effort + silent. */
static void ehci_os_boot_quiesce(void)
{
    RegEntryIter it; RegEntryID node; Boolean done = false; UInt32 want = 0x000c0320UL;
    UInt32 *aa = NULL, *la = NULL; ByteCount aaSz = 0, laSz = 0;
    UInt32 nEnt, i, memIdx = 0xFFFFFFFFUL, cmd, intr;
    volatile UInt8 *capBase, *opBase;

    if (RegistryEntryIterateCreate(&it) != noErr) return;
    if (RegistryEntrySearch(&it, kRegIterDescendants, &node, &done, "class-code", &want, sizeof(want)) != noErr) {
        RegistryEntryIterateDispose(&it); return;
    }
    RegistryEntryIterateDispose(&it);

    /* enable Memory Space so the BAR is readable (harmless if the PCI enumerator already did it) */
    (void)ExpMgrConfigWriteWord(&node, (LogicalAddress)kPCICommandReg, (UInt16)(kPCICmdMemSpace | kPCICmdBusMaster));

    if (get_prop(&node, "assigned-addresses", (void **)&aa, &aaSz) != noErr) return;
    if (get_prop(&node, "AAPL,address", (void **)&la, &laSz) != noErr) { PoolDeallocate(aa); return; }
    nEnt = aaSz / (kAddrEntryWords * sizeof(UInt32));
    for (i = 0; i < nEnt; i++) if (AA_SPACE(aa[i * kAddrEntryWords]) >= 2) { memIdx = i; break; }
    if (memIdx == 0xFFFFFFFFUL || (memIdx * sizeof(UInt32)) >= laSz) { PoolDeallocate(la); PoolDeallocate(aa); return; }
    capBase = (volatile UInt8 *)la[memIdx];
    PoolDeallocate(la); PoolDeallocate(aa);
    if (capBase == 0) return;
    opBase = capBase + ehci_read8(capBase, EHCI_CAPLENGTH);

    intr = ehci_read32(opBase, EHCI_USBINTR);
    cmd  = ehci_read32(opBase, EHCI_USBCMD);
    if (intr == 0xFFFFFFFFUL || cmd == 0xFFFFFFFFUL) return;                          /* BAR not responding -> don't poke it */
    if (intr == 0 && !(cmd & (EHCI_CMD_RUN | EHCI_CMD_ASE | EHCI_CMD_PSE))) return;   /* already quiet (normal cold boot) */

    ehci_write32(opBase, EHCI_USBINTR, 0);                                                    /* stop interrupts (de-asserts the line) */
    ehci_write32(opBase, EHCI_USBCMD, cmd & ~(EHCI_CMD_RUN | EHCI_CMD_ASE | EHCI_CMD_PSE));   /* stop schedules + halt (no wait) */
    ehci_write32(opBase, EHCI_CONFIGFLAG, 0);                                                 /* route ports to the companion */
}

int gBroughtUp = 0;   /* Path A: guards the one-time EHCI bring-up. SHARED (non-static): uimInitialize
                       * (ehci_uim.c, the LoadUIMForEntry path) checks it too, so a SECOND app launch does
                       * NOT re-run the bring-up (which armed a 2nd orphaned heartbeat timer, corrupted the
                       * saved companion-IRQ handler, re-HCReset mid-session, and leaked the DMA pool -- the
                       * cause of the warm-reboot interrupt-level crash after running the launcher twice). */
/* ==================== n0: DoDriverIO command trace ====================
 * QUESTION IT ANSWERS: does the Device Manager send us kOpen at BOOT, with no application running? That
 * decides whether the native (app-less) design needs a boot-time INIT to poke us, or whether we are opened
 * for free. See docs/NATIVE_INTEGRATION_DESIGN.md phase n0.
 * WHY IT IS BUFFERED, NOT LOGGED: every command is recorded with PURE MEMORY WRITES only. We deliberately
 * add NO logging at kInitialize/kOpen — ehci_os_log() uses the File Manager, and if the Device Manager opens
 * us during early boot a log call there could hang the boot (the r18 lesson: File-Mgr I/O at the wrong level
 * hard-hung the MDD). The trace is flushed later from uimInitialize (i.e. only once an app has deliberately
 * been run, when logging is known safe). The Ticks stamp makes boot-time entries obvious, so the trace still
 * reveals everything that arrived BEFORE the app existed. ZERO added boot risk. */
#define N0_N 32
static volatile UInt32 gN0Code[N0_N], gN0Tick[N0_N];
static volatile UInt32 gN0Count = 0;
static void n0_record(UInt32 code)
{
    UInt32 i = gN0Count++;
    if (i < N0_N) { gN0Code[i] = code; gN0Tick[i] = *(volatile UInt32 *)0x016AUL; }   /* lowmem Ticks (60Hz) */
}
void ehci_os_n0_dump(void)   /* called from uimInitialize — task level, logging safe */
{
    UInt32 i, n = (gN0Count < N0_N) ? gN0Count : N0_N;
    static int done = 0;
    if (done) return;
    done = 1;
    ehci_os_log("=== n0 DoDriverIO TRACE — every command received, incl. BEFORE any app ran ===");
    ehci_os_log("  codes: 1=Initialize 2=Finalize 3=Replace 4=Superseded 5=Open 6=Close 7=Read 8=Write 9=Control 10=Status 11=KillIO");
    ehci_os_logx("  total commands received", gN0Count);
    ehci_os_logx("  gBroughtUp at dump time (1 = we were ALREADY brought up before the app)", (UInt32)gBroughtUp);
    for (i = 0; i < n; i++) {
        ehci_os_logx("  cmd code", gN0Code[i]);
        ehci_os_logx("    at Ticks", gN0Tick[i]);
    }
    ehci_os_log("=== n0 READ THIS: a code 5 (Open) with an EARLY tick => the Device Manager opened us at BOOT with no app (native path is free). Only code 1 (Initialize) => kOpen does NOT fire at boot; the native design needs its own trigger. ===");
}
OSErr DoDriverIO(AddressSpaceID spaceID, IOCommandID cmdID,
                 IOCommandContents contents, IOCommandCode code, IOCommandKind kind)
{
    (void)spaceID; (void)cmdID; (void)kind; (void)contents;
    n0_record((UInt32)code);   /* n0: memory-only record; see the comment above (never log here) */
    switch (code) {
    case kInitializeCommand:
    case kReplaceCommand:
        /* Path A BOOT-SAFETY (eSATA v63/v64 lesson): when the ROM parcel makes the
         * Device Manager load us at boot, kInitialize runs during the early PCI-claim
         * phase, where full EHCI bring-up (HCReset/DMA/IRQ) FREEZES. So do NOTHING
         * here except acknowledge the claim — the real bring-up is deferred to kOpen,
         * which the headless mount app triggers post-boot (task/driver context up).
         * (v65 proof: stash-only kInit + open-driven bring-up = stable boot-to-desktop
         * with the card claimed.) NB the USL path (LoadUIMForEntry -> uimInitialize)
         * still brings up inline as before; only the Device-Manager path defers. */
        /* NB: NO logging here. ehci_os_log() uses the File Manager, which may not be
         * up during the early PCI-claim phase — a log call here could itself hang the
         * boot. Pure return, exactly like eSATA's stash-only kInit (sil_os_init_stash).
         * kOpen (post-boot, File Manager up) logs the bring-up. */
        /* Lifecycle fix: DO tame a controller left HOT by a previous session (warm reboot), so its
         * unserviced interrupts + stale DMA can't storm this boot (the "unhealthy boot"). This is a
         * handful of guarded MMIO ops only — NOT the bring-up that must stay out of early boot. */
        ehci_os_boot_quiesce();
        return noErr;
    case kFinalizeCommand:
    case kSupersededCommand:
        dbg("EHCIUIM: DoDriverIO Finalize — stop service + release ports");
        ehci_vhub_stop_service();         /* stop the heartbeat timer/ISR/interrupts/schedules BEFORE we go */
        ehci_hc_release_ports(&gSoftc);   /* hand the ports back so 1.1 works after we're gone */
        gBroughtUp = 0;
        return noErr;
    case kOpenCommand:
        /* Full EHCI bring-up now, post-boot. Idempotent (guarded), so repeated opens
         * are safe. Self-finds the node (proven app path == uimInitialize's ...,0). */
        if (!gBroughtUp) {
            long e;
            dbg("EHCIUIM: DoDriverIO Open — bringing up EHCI");
            e = ehci_os_init(&gSoftc, (EHCIRegEntryIDPtr)0);
            if (e != 0) { ehci_os_logx("EHCIUIM: DoDriverIO Open bring-up FAILED e=", (unsigned long)e); return (OSErr)e; }
            (void)ehci_vhub_xfer_init();          /* DMA page + downstream QHs */
            ehci_vhub_start_service(&gSoftc.node); /* install EHCI ISR + periodic timer */
            gBroughtUp = 1;
            dbg("EHCIUIM: DoDriverIO Open — bring-up complete");
        }
        return noErr;
    case kCloseCommand:
        return noErr;
    default:
        return noErr;
    }
}
