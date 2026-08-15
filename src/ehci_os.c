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
#include <MixedMode.h>   /* h87: NewRoutineDescriptor for the DCE dispatch UPP */
#include "ehci.h"
#include "ehci_vhub.h"   /* Path A: DoDriverIO kOpen drives the vhub bring-up (xfer_init + start_service) */

/* Init-phase tracing to a FLUSHED disk file. uimInitialize runs synchronously in the app's
 * context via LoadUIMForEntry, so File Manager is safe here, and a per-line flush means the last
 * surviving line pinpoints the phase that crashed (r4/r5 = error type 12 inside init). The file
 * lands next to the app; open EHCIUIM_init.log in SimpleText after a reboot. */
static short gDbgRef = 0;
static short gDbgVol = 0;   /* r49: the log's OWN volume (boot), captured at create */

/* ★★★★★★★ h85 — THE DISCRIMINATOR. COMPILE THE FILE MANAGER LOGGING OUT.
 *
 * MEASURED, twice, on 2026-08-13 (validated/logs/MINI-m33-h84-*): through pump 9 every completed pump
 * body was <= 274 ms and gBodySlowN was 0. Then slot 1's AddDrive landed, the Finder began mounting, and
 * the VERY NEXT log drain took **30715 ms** — 30.7 seconds holding the Finder's own thread. The 09:12 run
 * measured the same section at 31064 ms. 160 lines x ~190 ms per FSWrite + PBFlushFileSync + FlushVol on
 * a boot volume that the mount is already hammering. The arithmetic closes to within 2%.
 *
 * ⇒ THE INSTRUMENT IS THE DOMINANT LOAD, and it is dominant exactly when a mount is in flight.
 *
 * ⚠ WHY A SWITCH AND NOT A TUNED FLUSH POLICY. h78 cut the DoDriverIO drain from 384 lines to 6 AND stood
 * it down for 30 s, then passed three runs and failed on the fourth. NARROWING A WINDOW LOOKS EXACTLY LIKE
 * A FIX UNTIL IT DOESN'T, and "flush every N lines" is that same move a third time. A switch is a
 * DISCRIMINATOR: at level 0 the code is not there, so the answer cannot be a coincidence of timing.
 *   level 0 solid over several boots -> the instrument was the disease, AND THIS IS THE SHIPPING BUILD
 *   level 0 still wedges            -> logging was never the cause, the biggest confound is eliminated,
 *                                      and the screen paint still says where
 *
 * ★ WHAT SURVIVES AT LEVEL 0: every counter, and the whole screen watchdog — it reads counters and writes
 * the framebuffer and touches no File Manager at all. A failing run still yields rows 1-5. Also the
 * BANNER, forced through, so the run-validation rule ("expected banner, or the run is void") still holds.
 * ⚠ The INIT's own three logs (Boot / NM / INIT, ~56 lines total, separate files, separate code) are
 * DELIBERATELY UNTOUCHED: they are the only proof the resident vehicle worked, they run once, and they run
 * before any mount is in flight. If extension loading is still slow at level 0, they are the next suspect —
 * changing them here would put two variables in one run. */
#ifndef EHCI_LOG_LEVEL
#define EHCI_LOG_LEVEL 0
#endif
int ehci_os_log_level(void) { return EHCI_LOG_LEVEL; }
/* Level 1 keeps the "!!" lines — every error, guard trip and MUST-BE-ZERO line in this driver is written
 * with that prefix, and logx preserves it into the formatted buffer, so one test covers both writers. */
static int log_wanted(const char *s)
{
#if EHCI_LOG_LEVEL >= 2
    (void)s; return 1;
#elif EHCI_LOG_LEVEL == 1
    return (s && s[0] == '!' && s[1] == '!');
#else
    (void)s; return 0;
#endif
}
static void log_write_raw(const char *s);

void ehci_os_log(const char *s)          { if (log_wanted(s)) log_write_raw(s); }
/* The banner, and only the banner: run validation must survive every level. */
void ehci_os_log_always(const char *s)   { log_write_raw(s); }

static void log_write_raw(const char *s)
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
    /* h85: test the LABEL, not the formatted buffer — same verdict (logx copies the prefix through) and it
     * skips the formatting too. At level 0 this whole function is a compare and a return. */
    if (!log_wanted(label)) return;
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
/* kind 1 = msg, 2 = msg + value, 3 = CRITICAL msg, 4 = CRITICAL msg + value.
 * ★★★★★★ THE CRITICAL CLASS (2026-08-11) — SEE docs/ENGINE-STATE-MACHINE.md §0.
 *
 * ⚠ THE m24 RUN IS WHY THIS EXISTS, and the cost was a wrong conclusion I very nearly built a ROM on.
 * The rate cap dropped **458 ring lines** in one window, and the ring is where the ENTIRE probe narrative
 * lives — SELFENUM COMPLETE, READ CAPACITY, the geometry record, SELFPROBE COMPLETE. The exposure narrative
 * uses ehci_os_log (direct, task level, UNCAPPED), so what reached the file was an exposure with no probe in
 * front of it, and I read that as a device exposed without being probed. It was not: the probe completed and
 * its lines were thrown away. Three independent facts said so and none of them was a log line.
 * ⇒ A cap that can drop the lines a diagnosis RESTS ON is not protecting the machine, it is manufacturing
 * false findings — I10, for the fifth time in this project.
 *
 * ★ THE CRITICAL SET IS DELIBERATELY TINY: probe complete, geometry, the exposure latch, and the state
 * transitions that prove a device really was verified. A handful of lines per device per attach — bounded by
 * the number of devices, not by any loop. That is what makes exempting them safe: the h14 livelock this cap
 * was built for emits ORDINARY lines at heartbeat rate, and those are still capped exactly as before.
 * ⚠ The ring's own 384-entry bound and its overflow counter still apply to critical lines. They are exempt
 * from the RATE cap, not from the ring. Nothing here is unbounded.
 * ⚠ DO NOT promote a line into this class to make a hunt easier. The set is small on purpose; a critical
 * class that grows to cover the log is just the cap removed, with extra steps. */
typedef struct { const char *msg; UInt32 val; UInt8 kind; } ILogRec;
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
/* CRITICAL variants — exempt from the RATE cap (never from the ring bound). See the note at ILogRec. */
void ehci_os_ilogc(const char *s)
{
    UInt32 i = gILogHead;
    if (i - gILogTail >= ILOG_N) { gILogDropped++; return; }
    gILog[i % ILOG_N].msg = s; gILog[i % ILOG_N].val = 0; gILog[i % ILOG_N].kind = 3;
    __asm__ __volatile__("eieio");
    gILogHead = i + 1;
}
void ehci_os_ilogcx(const char *label, unsigned long v)
{
    UInt32 i = gILogHead;
    if (i - gILogTail >= ILOG_N) { gILogDropped++; return; }
    gILog[i % ILOG_N].msg = label; gILog[i % ILOG_N].val = (UInt32)v; gILog[i % ILOG_N].kind = 4;
    __asm__ __volatile__("eieio");
    gILogHead = i + 1;
}
/* ★★★★★★★ h78 — TWO DRAIN BUDGETS, BECAUSE THIS FUNCTION IS CALLED FROM TWO VERY DIFFERENT PLACES.
 *
 * ⚠ THE m27 HUB HANG. The screen watchdog measured it: driver alive (gVhubTick climbing), task level
 * DEAD (gTaskPumpN frozen at 18), and OUR DRIVER COMPLETELY IDLE — gBioPhase 0, bio ring empty, both
 * in-flight slots free, every error counter 0. We had satisfied every request and gone quiet, and the
 * machine was still wedged. So the fault is not a stuck transfer; it is something we are DOING TO the
 * File Manager.
 * ★ AND THIS IS IT. usb_disk.c's DoDriverIO calls this drain — and DoDriverIO is called BY THE FILE
 * MANAGER. kRead/kWrite are excluded deliberately, but kOpen / kClose / kStatus / kControl are NOT, and
 * those are exactly what the File Manager issues while MOUNTING a volume. With a budget of ILOG_N that
 * is up to 384 synchronous FSWrite + FlushVol ON THE BOOT VOLUME, issued from inside a File Manager
 * call chain operating on a DIFFERENT volume.
 * ★★ AND THE HUB IS WHAT LOADS THE GUN: the second drive enumerates concurrently and stuffs the ring at
 * interrupt rate, so by the time the mount calls kStatus the ring is FULL. One drive leaves it nearly
 * empty, the drain is cheap, and the mount succeeds — which is precisely the topology dependence and
 * the intermittency we have been chasing since m22, in one mechanism.
 *
 * ⇒ THE SPLIT IS BY CALLER, NOT BY A NEW ARGUMENT: `drainFn` lives in gSvc and the n4g activator holds
 * the same pointer, so the SIGNATURE CANNOT CHANGE without breaking that ABI.
 *   · ehci_os_ilog_drain()      — the exported one, what DoDriverIO calls. TINY budget. Enough to keep
 *                                 lines moving after the activator quits (n9's whole purpose) without
 *                                 ever becoming a burst inside someone else's File Manager call.
 *   · ehci_os_ilog_drain_bulk() — internal, generous budget, called ONLY from selfprobe_tick, which runs
 *                                 in the NM response and is NOT inside a File Manager call chain.
 * The bulk of the draining therefore happens in the safe context and the dangerous one is capped. */
#define ILOG_DRAIN_DODRIVERIO   6u     /* inside a File Manager call — must never be a burst */
#define ILOG_DRAIN_BULK       160u     /* the NM-response path, where a burst is safe */
/* h78 fix 2: a mount is in flight — do not drain from DoDriverIO AT ALL until this deadline passes.
 * Wall-clock and self-clearing, checked against lowmem Ticks, so it needs no task-level help to expire:
 * if task level is dead the inhibit must lift on its own or we would lose the drain for ever. */
static volatile UInt32 gDrainInhibitUntil = 0;
void ehci_os_ilog_inhibit_drain(unsigned long ticks)
{
    gDrainInhibitUntil = *(volatile UInt32 *)0x016AUL + (UInt32)ticks;
}
static void ilog_drain_budgeted(UInt32 budget);

/* TASK LEVEL ONLY — this is the one place the ring touches the File Manager.
 * ⚠ This is the EXPORTED entry, i.e. the one usb_disk.c's DoDriverIO calls from inside a File Manager
 * call chain. It is deliberately tiny, and it stands down entirely while a mount is in flight. */
void ehci_os_ilog_drain(void)
{
    if ((long)(*(volatile UInt32 *)0x016AUL - gDrainInhibitUntil) < 0) return;   /* mount in flight */
    ilog_drain_budgeted(ILOG_DRAIN_DODRIVERIO);
}
/* The safe-context drain: selfprobe_tick only, from the NM response. */
void ehci_os_ilog_drain_bulk(void)
{
    ilog_drain_budgeted(ILOG_DRAIN_BULK);
}
static void ilog_drain_budgeted(UInt32 budget)
{
    UInt32 d;
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
            /* CRITICAL (kind 3/4) bypasses the token check entirely: these are the lines a diagnosis rests
             * on, and dropping them is what made the m24 log say the opposite of what happened. They still
             * SPEND a token when one is available, so a burst of them shortens the ordinary budget rather
             * than being free. */
            if (k < 3) {
                if (!gILogTokens) { gILogThrottled++; continue; }   /* h15: drop, never stall */
                gILogTokens--;
            } else if (gILogTokens) {
                gILogTokens--;
            }
            if (k == 2 || k == 4) ehci_os_logx(m, v); else ehci_os_log(m);
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
volatile UInt32 gN0Kind[3] = {0, 0, 0};   /* h92: DoDriverIO calls by kind — imm / sync / async+other */
/* ★ h93: a rolling history of Device-Manager calls into this driver, and the completion verdicts.
 * One entry per call: code, kind, cmdID, and IOCommandIsComplete's return when the queued path ran.
 * Dumped by ehci_os_h93_dump() from the periodic dump (task level). Ring semantics: first H93_N calls
 * are kept verbatim — the burst we are hunting was 72 calls, so 32 covers its head, and the counters
 * carry the totals. */
#define H93_N 32
volatile UInt32 gH93Code[H93_N], gH93Kind[H93_N], gH93Cmd[H93_N], gH93CC[H93_N];
volatile UInt32 gH93Count = 0;            /* total calls recorded (index clamps at H93_N) */
volatile UInt32 gH93CcBad = 0;            /* nonzero IOCommandIsComplete returns — MUST be 0 */
void h93_record(UInt32 code, UInt32 kind, UInt32 cmd, int completed, UInt32 cc)
{
    UInt32 i = gH93Count++;
    if (completed && cc != 0) gH93CcBad++;
    if (i < H93_N) { gH93Code[i] = code; gH93Kind[i] = kind; gH93Cmd[i] = cmd;
                     gH93CC[i] = completed ? (cc | 0x80000000UL) : 0x7FFFFFFFUL; }
}
void ehci_os_h93_dump(void)               /* task level; prints NEW entries since the last call */
{
    static UInt32 shown = 0;
    UInt32 n = (gH93Count < H93_N) ? gH93Count : H93_N;
    for (; shown < n; shown++) {
        ehci_os_logx("!! h93 DM call: code<<24|kind<<16|loword(cmdID)",
                     ((gH93Code[shown] & 0xFFUL) << 24) | ((gH93Kind[shown] & 0xFFUL) << 16)
                     | (gH93Cmd[shown] & 0xFFFFUL));
        ehci_os_logx("!!   h93 cmdID full / completion verdict next", gH93Cmd[shown]);
        ehci_os_logx("!!   h93 IOCommandIsComplete returned (0x7FFFFFFF = immediate, not called)",
                     gH93CC[shown]);
    }
}
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
    /* ★★★★ h91 — THE NATIVE COMPLETION CONTRACT, the m40 infinite wristwatch.
     * h90's descriptor works: ASP's open now dispatches HERE and nothing crashes. But a native driver's
     * return value completes only IMMEDIATE commands; synchronous/asynchronous ones are QUEUED by the
     * Device Manager and finish ONLY when the driver calls IOCommandIsComplete(cmdID, result). This
     * function ignored `kind` and returned — so ASP's synchronous Status PB kept ioResult = 1 forever and
     * ASP spun on a wait that can never end. The idiom below is usb_disk.c:700's, byte for byte — the
     * hardware-proven completion tail every mount already runs through daily. Restructured to a single
     * exit so no path can complete twice or not at all. n0 proves the Device Manager never delivered a
     * command here before h90, so this restructuring cannot change any previously-working path. */
    OSErr h91err;
    /* h92: count calls by KIND. h91's completion tail is the one genuinely NEW Device-Manager code path
     * of the h9x era (IOCommandIsComplete on queued commands), and if some dispatcher ever hands us a
     * kind that mismatches how the command was really issued, IOCommandIsComplete on a non-command is a
     * plausible system-heap corrupter. These three counters + the code trace make that theory checkable
     * from a log instead of arguable. Memory-only here (never log in DoDriverIO). */
    extern volatile UInt32 gN0Kind[3];
    if (kind & 0x00000004UL) gN0Kind[0]++;         /* immediate  */
    else if (kind & 0x00000001UL) gN0Kind[1]++;    /* synchronous */
    else gN0Kind[2]++;                             /* asynchronous / other */
    (void)spaceID; (void)contents;
    n0_record((UInt32)code);   /* n0: memory-only record; see the comment above (never log here) */
    switch (code) {
    case kInitializeCommand:
    case kReplaceCommand:
        /* Path A BOOT-SAFETY (a v63/v64 hardware lesson from an earlier PCI disk ndrv): when the ROM parcel makes the
         * Device Manager load us at boot, kInitialize runs during the early PCI-claim
         * phase, where full EHCI bring-up (HCReset/DMA/IRQ) FREEZES. So do NOTHING
         * here except acknowledge the claim — the real bring-up is deferred to kOpen,
         * which the headless mount app triggers post-boot (task/driver context up).
         * (v65 proof: stash-only kInit + open-driven bring-up = stable boot-to-desktop
         * with the card claimed.) NB the USL path (LoadUIMForEntry -> uimInitialize)
         * still brings up inline as before; only the Device-Manager path defers. */
        /* NB: NO logging here. ehci_os_log() uses the File Manager, which may not be
         * up during the early PCI-claim phase — a log call here could itself hang the
         * boot. Pure return, exactly like the stash-only kInit an earlier PCI disk ndrv proved on hardware.
         * kOpen (post-boot, File Manager up) logs the bring-up. */
        /* Lifecycle fix: DO tame a controller left HOT by a previous session (warm reboot), so its
         * unserviced interrupts + stale DMA can't storm this boot (the "unhealthy boot"). This is a
         * handful of guarded MMIO ops only — NOT the bring-up that must stay out of early boot. */
        ehci_os_boot_quiesce();
        h91err = noErr; break;
    case kFinalizeCommand:
    case kSupersededCommand:
        dbg("EHCIUIM: DoDriverIO Finalize — stop service + release ports");
        ehci_vhub_stop_service();         /* stop the heartbeat timer/ISR/interrupts/schedules BEFORE we go */
        ehci_hc_release_ports(&gSoftc);   /* hand the ports back so 1.1 works after we're gone */
        gBroughtUp = 0;
        h91err = noErr; break;
    case kOpenCommand:
        /* Full EHCI bring-up now, post-boot. Idempotent (guarded), so repeated opens
         * are safe. Self-finds the node (proven app path == uimInitialize's ...,0). */
        h91err = noErr;
        if (!gBroughtUp) {
            long e;
            dbg("EHCIUIM: DoDriverIO Open — bringing up EHCI");
            e = ehci_os_init(&gSoftc, (EHCIRegEntryIDPtr)0);
            if (e != 0) { ehci_os_logx("EHCIUIM: DoDriverIO Open bring-up FAILED e=", (unsigned long)e); h91err = (OSErr)e; break; }
            (void)ehci_vhub_xfer_init();          /* DMA page + downstream QHs */
            ehci_vhub_start_service(&gSoftc.node); /* install EHCI ISR + periodic timer */
            gBroughtUp = 1;
            dbg("EHCIUIM: DoDriverIO Open — bring-up complete");
        }
        break;
    case kCloseCommand:
        h91err = noErr; break;
    /* ★★★★ h87 — UNHANDLED COMMANDS MUST REFUSE, NOT "SUCCEED". The old `default: return noErr` was the
     * h30 disease at the Device Manager layer: report success and leave the caller's buffer untouched.
     * Unreachable while the DCE dispatch vector was garbage; h87 fixes that vector, so ASP's very next
     * call after kOpen is a DriverGestalt('vers') kStatus — and a noErr here would hand it stack garbage
     * to parse as a version. Honest errors instead, exactly what Apple's drivers return for csCodes they
     * do not implement; ASP renders that as "Not available" and moves on. */
    case kReadCommand:    h91err = -19; break;   /* readErr   */
    case kWriteCommand:   h91err = -20; break;   /* writErr   */
    case kControlCommand: h91err = -17; break;   /* controlErr */
    case kStatusCommand:  h91err = -18; break;   /* statusErr  */
    case kKillIOCommand:  h91err = noErr; break; /* nothing queued — killing nothing succeeds */
    default:
        h91err = -50; break;            /* paramErr */
    }
    /* h91: the single completion tail — usb_disk.c:700's proven idiom, byte for byte. */
    if (kind & 0x00000004UL) {
        h93_record((UInt32)code, (UInt32)kind, (UInt32)cmdID, 0, 0);
        return h91err;                                      /* kImmediateIOCommandKind: return completes */
    }
    {   /* ★★★★ h93 — CAPTURE WHAT THE COMPLETION PATH ACTUALLY DOES. The 2-for-2 correlation (sessions
         * with Device-Manager traffic through this USL-made, h90-repaired entry corrupt the system heap;
         * sessions without it stay clean) makes THIS tail the prime suspect, and the return value of
         * IOCommandIsComplete — which the h91 code THREW AWAY — is the Device Manager's own verdict on
         * whether the cmdID it was handed names a real queued command. Record every call's code, kind,
         * cmdID and that verdict; count nonzero verdicts as a MUST-BE-0. Memory-only here. */
        OSErr cc = (OSErr)IOCommandIsComplete(cmdID, (short)h91err);
        h93_record((UInt32)code, (UInt32)kind, (UInt32)cmdID, 1, (UInt32)(SInt32)cc);
        return cc;
    }
}

/* ★★★★★★★ h87 — MAKE THE ROM-CLAIMED UNIT-TABLE ENTRY ACTUALLY DISPATCHABLE.
 *
 * THE ASP CRASH, decoded end to end (2026-08-13, validated/logs/ + the user's StdLog):
 * Apple System Profiler's Devices-and-Volumes scan walks the unit table and _Opens every driver. Our
 * parcel-claimed UIM has an entry ('EHCIUIM', created by the USL's family machinery when the ROM parcel
 * matched) that NO CODE PATH HAD EVER DISPATCHED THROUGH — n0's trace reads `total commands received 0`
 * on every boot. The Toolbox ROM's open path (base+0x18B598, inside DriverLoaderLib) does:
 *
 *     lwz  r3, 0x1E(DCE)          ; the DoDriverIO UPP DriverLoaderLib stashes in dCtlWindow
 *     ...  CallUniversalProc(r3, 0xFFF1, ...)
 *
 * and the ROM's OWN installer (base+0x18CECC) fills that field with
 * NewRoutineDescriptor(DoDriverIO, procInfo 0xFFF1, kPowerPCISA). The USL's creation path never fills
 * it, so ASP's open jumps through whatever garbage is there — PC=00000008 in the user's MacsBug, and
 * byte-for-byte an earlier ndrv project's "un-openable ROM-claimed ndrv" crash (its decoded LR sits in the
 * SAME ROM routine). Apple's own parcel ndrvs (.i2c-uni-n / .i2c-mac-io) don't crash only because their
 * clients open them at boot through the working path.
 *
 * ⇒ THE FIX IS WHAT THE ROM'S INSTALLER WOULD HAVE DONE: find our own DCE and plant a real DoDriverIO
 * UPP at +0x1E, procInfo 0xFFF1, PowerPC ISA. Then open/status/close dispatch into DoDriverIO like any
 * healthy native driver: kOpen is a gBroughtUp-guarded no-op on the USL path, and unhandled status
 * returns statusErr (above). The RoutineDescriptor is allocated in the SYSTEM zone — this runs in some
 * application's context at boot, and an RD in an app heap would dangle when that app quits.
 *
 * ⚠ Task level ONLY (allocates; walks handles). Called from uimInitialize, which logs — proven task.
 * ⚠ Field offsets are BYTE MATH, deliberately: a hand-laid struct with PPC alignment is exactly the
 *   bug that made h82's StandardAlert silent (Toolbox structs are mac68k-aligned). Never again.
 * ⚠ Idempotent + conservative: if +0x1E already holds a plausible RoutineDescriptor (points at the
 *   MixedMode magic 0xAAFE), it is LEFT ALONE — a future ROM that fills the field correctly wins. */
static int h87_ptr_ok(UInt32 a) { return a >= 0x1000UL && a < 0x60000000UL; }
/* ★★★ h88 — THE h87 PATCH MISSED, and the m36 StdLog says how to aim. Same crash, same single EHCIUIM
 * entry (the full DCE table shows exactly one), the crawl naming OpenInstalledDriver+B4 outright. So the
 * walk ran and found nothing — and the leading explanation is TIMING: this was called from INSIDE
 * uimInitialize, but the USL plausibly registers the unit entry AFTER init returns (load -> init -> then
 * bookkeeping). h88 therefore (1) is called AGAIN from the first task-level pump tick, seconds later and
 * unambiguously after all registration; (2) is idempotent and re-callable (gH88Done latches only on a
 * successful plant or a found-valid vector); (3) patches EVERY match, not the first (`break` removed);
 * (4) reports on the '!!' channel so a LEVEL 1 build documents the outcome at near-zero logging cost.
 * All four survive whichever explanation is true. */
static int gH88Done = 0;
void ehci_os_fix_native_dce(void)
{
    void **utab; int n, i, found = 0, planted = 0;
    if (gH88Done) return;
    utab = *(void ** volatile *)0x011CUL;             /* UTableBase   (h52's proven accessors) */
    n    = (int)*(volatile short *)0x01D2UL;          /* UnitNtryCnt */
    if (!h87_ptr_ok((UInt32)utab) || n <= 0 || n > 1024) {
        ehci_os_logx("!! h88 unit table unavailable — ASP would still crash on our entry; UTableBase",
                     (UInt32)utab);
        return;
    }
    for (i = 0; i < n; i++) {
        void *dceh, *dce; unsigned char *drvr, *p; UInt32 v;
        dceh = utab[i];
        if (!h87_ptr_ok((UInt32)dceh)) continue;
        dce = *(void * volatile *)dceh;               /* unit table holds DCtlHandles */
        if (!h87_ptr_ok((UInt32)dce)) continue;
        drvr = *(unsigned char * volatile *)dce;      /* dCtlDriver +0x00 -> fabricated DRVR header */
        if (!h87_ptr_ok((UInt32)drvr)) continue;
        /* name at header+0x12, Pascal — the field MacsBug displays, and the m35 StdLog CONFIRMS the
         * address arithmetic on hardware: its open PB's ioNamePtr (001B9DAA) == DCE(001B9D60)+0x38+0x12. */
        if (drvr[0x12] != 7 || drvr[0x13] != 'E' || drvr[0x14] != 'H' || drvr[0x15] != 'C' ||
            drvr[0x16] != 'I' || drvr[0x17] != 'U' || drvr[0x18] != 'I' || drvr[0x19] != 'M') continue;
        found++;
        p = (unsigned char *)dce + 0x1E;              /* dCtlWindow — DriverLoaderLib's UPP slot */
        v = ((UInt32)p[0] << 24) | ((UInt32)p[1] << 16) | ((UInt32)p[2] << 8) | (UInt32)p[3];
        ehci_os_logx("!! h88 found our unit-table DCE (a report, not an error); slot<<16|refNum",
                     ((UInt32)i << 16) | (UInt32)(UInt16)*(volatile short *)((char *)dce + 0x18));
        ehci_os_logx("!!   h88 dCtlWindow (+0x1E) BEFORE — the vector OpenInstalledDriver dies through", v);
        /* ★★★★ h90 — STOP VALIDATING THE CORPSE. REPLACE UNCONDITIONALLY.
         *
         * The escalation that got here, each step proven on hardware and each check one level too shallow:
         *   h87  assumed the field was EMPTY            -> it held a real RoutineDescriptor
         *   h88  checked the RD's 0xAAFE magic          -> magic fine; left it alone; still crashed
         *   h89  checked procDescriptor is a pointer    -> it IS one (0x001ADxxx) — and it points at the
         *        very TVector every crash's R12 has been holding (m35 R12=001ADCD8, m36 R12=001ADD78):
         *        a TVector whose CODE WORD is the garbage that becomes PC=8. Three links — DCE -> RD ->
         *        TVector — and the rot was always in the last one.
         *
         * The chain's owner never finished it, and n0 proves NOTHING has ever successfully dispatched
         * through this entry (total commands 0, every boot ever). There is nothing to preserve. So: log
         * the old chain for the record — including the TVector's code word, the actual PC=8 payload —
         * then plant our own RD, every time. The old RD is leaked, never freed (unknown allocator). */
        if (h87_ptr_ok(v) && *(volatile UInt16 *)v == 0xAAFEu) {
            UInt32 tgt = *(volatile UInt32 *)(v + 0x14);
            ehci_os_logx("!!   h90 old RD magic OK; procDescriptor (RD+0x14)", tgt);
            if (h87_ptr_ok(tgt))
                ehci_os_logx("!!   h90 TVector code word (*target) — the value that became PC on the crashes",
                             *(volatile UInt32 *)tgt);
        }
        ehci_os_log("!!   h90 replacing the dispatch descriptor UNCONDITIONALLY (nothing has ever "
                    "dispatched through this entry — n0: total commands 0, every boot)");
        {   THz oldZone = GetZone();
            UniversalProcPtr upp;
            SetZone(SystemZone());                    /* the RD must outlive whichever app hosts boot */
            upp = NewRoutineDescriptor((ProcPtr)DoDriverIO, 0x0000FFF1UL, kPowerPCISA);
            SetZone(oldZone);
            if (!upp) { ehci_os_log("!! h88 NewRoutineDescriptor FAILED — entry left as found"); continue; }
            v = (UInt32)upp;
            p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
            p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)v;
            planted++;
            ehci_os_logx("!!   h88 dCtlWindow AFTER — DoDriverIO UPP planted (procInfo 0xFFF1, PowerPC)", v);
        }
    }
    if (found && planted) gH88Done = 1;               /* success latches; a miss leaves us re-callable */
    if (!found) ehci_os_log("!! h88 pass found NO EHCIUIM unit entry — if this is the uimInitialize call, "
                            "the entry is registered later and the tick-time pass must catch it");
}
