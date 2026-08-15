/*
 * ehci_vhub.c — Virtual root hub + downstream-transfer engine (see ehci_vhub.h).
 *
 * A faithful re-homing of the hardware-proven probe/ehci_hub.c logic (app leg v0.6->v0.42)
 * for the resident ndrv: the USL drives these through ThePluginDispatchTable. Everything
 * operates on the single controller soft state (gSoftc); printf becomes a log hook; the
 * app's 8 ms heartbeat count becomes a service tick, and precise reset timing uses the
 * frame clock. App-only diagnostics (console dumps, portPB search, mount check, the
 * status-log reader) are dropped — those belong to the trigger app, not the driver.
 */
#include <MacTypes.h>
#include <MacMemory.h>
#include <DriverServices.h>
#include <NameRegistry.h>
#include <Gestalt.h>
#include <DriverSynchronization.h>   /* IncrementAtomic — re-entrancy-safe ring slot claim */
#include <Devices.h>                 /* n3/n4c: InstallDriverFromMemory + PBControlSync (block driver) */
#include <Files.h>                   /* n7: PBUnmountVol + GetVCBQHdr — unmount on unexpected removal */
#include <Notification.h>            /* n7 */
#include <Shutdown.h>                /* n10: quiesce the controller before a warm reboot */            /* n7: NMInstall — "please reconnect the device", as Apple's ext does */
#include <Patches.h>                 /* h48: GetToolTrapAddress — watch _SystemTask (0xA9B4) for the Apple bug */
#include <CodeFragments.h>           /* h48: GetSharedLibrary/FindSymbol — reach the Expert's status facility */
#include "ehci_vhub.h"
#include "usb_disk_blob.h"           /* n3: the block driver's PEF, embedded so we can install it ourselves */

/* ph0a/ph0b: hide our high-speed ports from Apple's USL so its Expert never adopts the device and we own
 * enumeration end to end. 0 = transparent UIM (Apple enumerates and mounts, the pre-p1a behaviour). */
#define APPLE_HIDE 1
#define PHASE0_TRANSPARENT 0

/* v44: production verbosity flag. 0 = LEAN — the per-tick diagnostic logging is compiled out, so there is NO
 * per-tick file I/O (that per-tick FSWrite+FlushVol was the Mini's shared-IRQ enumeration timing aggravator).
 * Flip to 1 for a diagnostic build that restores the full v18–v42 per-tick trace (r88/r89 dumps, SELFPROBE-wait
 * spam, STALL/DoDriverIO traces). One-shot mount-progress logs + the failure dump stay on regardless. */
#define EHCI_VERBOSE 0

static ehci_vhub_logfn gLog = 0;
void ehci_vhub_set_log(ehci_vhub_logfn fn) { gLog = fn; }
#define VLOG(m) do { if (gLog) gLog(m); } while (0)

/* ==================== dispatch slot 24: the USL's per-bus clock ====================
 * 64-bit busTime; the USL's processDelayQ compares the LOW word (r4). Gap-free monotonic
 * 1 ms count accumulated from FRINDEX (13-14 bits, wraps ~1-2 s). The fix that beat the
 * completion-firing wall (app v0.33). Read often enough (USL clock + service) that the
 * masked delta never misses a wrap. */
static UInt32 gMicroAcc = 0, gLastRaw = 0; static int gFtInit = 0;
/* SINGLE writer — called only from the ~8 ms heartbeat (far below the ~2 s FRINDEX wrap), so the
 * accumulator never misses a wrap and advances on a steady cadence regardless of how (in)frequently
 * the USL reads the clock. Robust replacement for read+accumulate inside the reader (which raced when
 * called from two contexts and starved when the USL polled sparsely). */
static void frame_time_update(void)
{
    UInt32 raw = ehci_read32(gSoftc.opBase, EHCI_FRINDEX) & 0x3FFFUL;
    if (!gFtInit) { gLastRaw = raw; gFtInit = 1; return; }
    gMicroAcc += (raw - gLastRaw) & 0x3FFFUL; gLastRaw = raw;
}
unsigned long long ehci_vhub_frame_time(void) { return (unsigned long long)(gMicroAcc >> 3); }  /* READ-ONLY */
static UInt32 frame_ms(void) { return (UInt32)ehci_vhub_frame_time(); }
unsigned long ehci_vhub_ms(void) { return (unsigned long)(gMicroAcc >> 3); }                    /* READ-ONLY */

/* ==================== hub port status + change tracking ==================== */
#define HPS_CONNECTION 0x0001
#define HPS_ENABLE     0x0002
#define HPS_RESET      0x0010
#define HPS_POWER      0x0100
#define HPS_HIGHSPEED  0x0400
#define HPC_CONNECTION 0x0001
#define HPC_ENABLE     0x0002
#define HPC_RESET      0x0010
#define FEAT_PORT_RESET        4
#define FEAT_PORT_POWER        8
#define FEAT_C_PORT_CONNECTION 16
#define FEAT_C_PORT_ENABLE     17
#define FEAT_C_PORT_RESET      20
static UInt16 gPortChange[15];   /* accumulated wPortChange per port (we track; RW1C-safe) */

/* ★★★ n11: PORT DELIBERATELY CEDED TO THE 1.1 COMPANION — the fix for the hub thrash loop.
 *
 * THE BUG (found on hardware 2026-08-03 with an Apple 20" Cinema Display's built-in hub, 05ac:911d:
 * NOTHING downstream was detected — no keyboard, no mouse, no drive). We cede a port by setting EHCI
 * Port Owner = 1, and every "have we ceded this port?" decision then re-read that bit back out of the
 * hardware via apple_hidden_port(). When the read-back did not agree with what we wrote, the driver
 * concluded the port was still ours, saw CCS = 0 (which is what a companion-owned port reports), decided
 * "the device was pulled", pushed a spurious media-gone into the block driver, reset the probe state,
 * re-applied APPLE_HIDE, re-claimed the port and re-enumerated. Measured in ONE session: 148 handoffs and
 * 134 RECONNECT resets, running to the end of a 14,883-line log. Apple's companion never got a stable
 * window in which to enumerate the hub, which is why the keyboard and mouse were dead too.
 *
 * So this flag records the DECISION in software instead of re-deriving it from a hardware bit every pass.
 * An intent we have acted on is ours to remember; re-reading it from the controller made a 148-iteration
 * livelock out of one disagreement with the hardware. Cleared only by a driver reload or an explicit
 * re-scan — NOT by anything on the port-event path, which is exactly what used to un-cede it.
 *
 * See also the enabled-vs-disabled precondition at the n9 cede site (~L2510): both cede paths that have
 * always worked surrender a port that is NOT enabled, which is the EHCI-spec handoff point. */
static volatile UInt8 gPortCeded[15];

/* ★ n12: two DIFFERENT end states, and conflating them breaks the keyboard.
 *   gPortCeded  — we handed the port to the 1.1 companion and Owner = 1 really took. This is the
 *                 full/low-speed path (a keyboard or mouse after a reset that did not enable the port),
 *                 which is the EHCI-spec handoff case and IS hardware-proven here — it is how input stays
 *                 alive on the Mac Mini. Such a port is legitimately Apple's and MUST stay VISIBLE to it.
 *   gPortParked — we gave up on a device we cannot drive, and we still OWN the port (the high-speed
 *                 non-storage case; this controller refuses that handoff, see park_port). Apple must NOT
 *                 see a device on a port only we can talk to, so a parked port stays HIDDEN.
 * Both suppress our own re-enumeration. */
static volatile UInt8 gPortParked[15];

/* Have we deliberately stopped driving port p — by either route? Software intent first, hardware bit
 * second, so a disagreement with the controller can never resurrect a port we have already given up on. */
static int port_ceded(int p)
{
    if (p < 0 || p >= 15) return 0;
    if (gPortCeded[p] || gPortParked[p]) return 1;
    if (p >= (int)gSoftc.nPorts || gSoftc.opBase == 0) return 0;
    return (ehci_read32(gSoftc.opBase, EHCI_PORTSC(p)) & EHCI_PORT_OWNER) ? 1 : 0;
}

static void park_port(int p, const char *why);   /* n12: defined below, with the self-enum state it resets */
static int  dev_alloc(void);                     /* h7: the hub sweep allocates a slot only once it knows a
                                                  * high-speed device needs enumerating, which is before the
                                                  * definition below. */

/* Phase 0a: is port p hidden from Apple's USL?
 * ★★ n3b: hide a port only if WE OWN IT (EHCI Port Owner == 0). Per-port, read from the hardware, and
 * self-correcting as ownership changes. REPLACES a global `!gSoftc.sharedCompanion` gate that was a
 * booby-trap: ehci_hc_start sets sharedCompanion = 1 if ANY port is merely OCCUPIED at bring-up, so
 *   - on the MDD card, a drive left plugged in across a reboot silently DISABLED the entire hide, and
 *   - on the Mac mini, where kbd/mouse are ALWAYS plugged in, it would have disabled the hide
 *     PERMANENTLY — letting Apple's Expert back onto our high-speed device and resurrecting the very
 *     ExpertIdleTask monopoly this whole architecture exists to escape.
 * Owner == 1 means we deliberately handed the port to the 1.1 companion (kbd/mouse, or a full/low-speed
 * device), so it is legitimately Apple's and MUST stay visible to it. Owner == 0 means the port is ours
 * to enumerate, so Apple must not see anything on it. Safe on both machines by construction. */
static int apple_hidden_port(int p)
{
#if APPLE_HIDE
    if (p < 0 || p >= (int)gSoftc.nPorts || gSoftc.opBase == 0) return 0;
    /* n12: PARKED stays HIDDEN (we still own it — n11 un-hid it on the false assumption that the Owner
     * write had handed it over); genuinely CEDED stays VISIBLE (that handoff really happened, and it is
     * what keeps a keyboard alive on the companion). Both checked before the register read so this stays
     * one MMIO access — service_ports calls it per port per heartbeat. */
    if (gPortParked[p]) return 1;
    if (gPortCeded[p])  return 0;
    return (ehci_read32(gSoftc.opBase, EHCI_PORTSC(p)) & EHCI_PORT_OWNER) ? 0 : 1;
#else
    (void)p;
    return 0;
#endif
}

void ehci_vhub_port_status(int p, volatile UInt8 *out)
{
    UInt32 pv = ehci_read32(gSoftc.opBase, EHCI_PORTSC(p));
    UInt16 st = 0, ch = gPortChange[p];
    if (apple_hidden_port(p)) {
        /* ph0b: THE hide point. Tell Apple this port is POWERED but has NOTHING CONNECTED (CCS=0, PED=0),
         * while PASSING THE REAL CHANGE BITS THROUGH. So the flow is: a real connect sets gPortChange =>
         * deliver_completions notifies Apple normally => Apple issues GET_PORT_STATUS => it reads
         * "a change happened, but nothing is connected" (i.e. plugged-then-unplugged) => it does NOT reset
         * or enumerate => it issues ClearPortFeature(C_PORT_CONNECTION), which clears gPortChange via the
         * existing root-hub handler => it re-arms its status-change transfer and idles. A few cheap control
         * transfers, no work, Apple's stack stays HEALTHY (the ph0a starvation is gone) and our device is
         * still invisible to it. Our self-probe reads the RAW EHCI PORTSC, so it sees the real device. */
        out[0] = (UInt8)HPS_POWER; out[1] = (UInt8)(HPS_POWER >> 8);
        out[2] = (UInt8)ch;        out[3] = (UInt8)(ch >> 8);
        return;
    }
    if (pv & EHCI_PORT_CONNECT) st |= HPS_CONNECTION;
    /* HS bit is LOAD-BEARING for enumeration (r20 proved clearing it dead-ends the device on the
     * TT/split path → uim20 stub → no enumeration). OS 9's hub driver v1.5.9 DOES read the HS bit.
     * Keep reporting high-speed (matches the EHCI-enabled port); the mounter's HS-mount limitation
     * is addressed a different way (bypass / patch), not by faking the port speed. */
    if (pv & EHCI_PORT_ENABLE)  st |= HPS_ENABLE | HPS_HIGHSPEED;
    if (pv & EHCI_PORT_RESET)   st |= HPS_RESET;
    if (pv & EHCI_PORT_POWER)   st |= HPS_POWER;
    out[0] = (UInt8)st; out[1] = (UInt8)(st >> 8);
    out[2] = (UInt8)ch; out[3] = (UInt8)(ch >> 8);
}

/* ==================== fabricated root-hub descriptors (self-powered, HS) ==================== */
static const UInt8 gDevDesc[18] = {
    0x12,0x01, 0x00,0x02, 0x09,0x00,0x01, 0x40,
    0x00,0x00, 0x00,0x00, 0x00,0x01, 0x00,0x00,0x00, 0x01 };
static const UInt8 gCfgDesc[25] = {
    0x09,0x02,0x19,0x00,0x01,0x01,0x00,0xE0,0x00,
    0x09,0x04,0x00,0x00,0x01,0x09,0x00,0x00,0x00,
    0x07,0x05,0x81,0x03,0x01,0x00,0xFF };
static const UInt8 gHubDesc[9] = { 0x09,0x29,0x05,0x00,0x00,0x32,0x64,0x00,0x00 };
static void fill(volatile UInt8 *dst, const UInt8 *src, int n, UInt32 max)
{ int i; if ((UInt32)n > max) n = (int)max; for (i = 0; i < n; i++) dst[i] = src[i]; }

/* ==================== root-hub + port-service state ==================== */
static UInt32 gRootHubAddr = 0;
static volatile UInt8 gSetup[8]; static int gHaveSetup = 0;
static int    gResetSeen = 0;            /* a downstream port has been reset => real devices exist */
static UInt32 gVhubTick = 0;             /* service-call counter (coarse timeouts)                */
static volatile UInt32 gIsrHits = 0;     /* r35: real-EHCI-ISR invocations where USBSTS was ours (IRQ fires) */
/* v46: shared-IRQ storm counters. gIsrConsec = consecutive ISR entries that were ours without the SIH
 * getting a chance to run in between (it clears the counter); gIsrConsecMax = the peak. Declared here,
 * ahead of selfprobe_tick's v47 stall dump which reads them. Diagnostic only — nothing branches on them. */
static volatile UInt32 gIsrConsec = 0, gIsrConsecMax = 0;
/* h54: both declared HERE rather than beside the ISR because ctl_step's failure probe reads them and sits
 * ~3000 lines earlier. gSihRuns counts SIH entries — flat across the control failures means the SIH is
 * starved, which is the shared-interrupt-line suspicion this ROM exists to settle. */
static volatile UInt32 gSihQueued = 0;
static volatile UInt32 gSihRuns = 0;
static volatile int    gStormPainted = 0;   /* v46: storm_paint is a ONE-SHOT */
/* ★★★ H76_DIAGNOSTICS — THE BISECT SWITCH. m27 (h76, everything on) does NOT BOOT: blinking question mark,
 * while m26 (h74) boots fine. Every static check on m27 passes — PEF header valid, all three exports
 * present, section table sane (code 241664 / data 49752 / loader 9099), ROM 9/9, size within 5 KB of m26 —
 * so whatever it is, it is not visible from the desk. This flag splits the difference in ONE reboot:
 *   1 = h75 + h76a (AddDrive dump) + h76b (screen watchdog)   -> that is m27
 *   0 = h75 ONLY (the BOT-recovery routing fix)               -> that is m28
 * m28 boots  => the fault is in the h76 diagnostics, and the paint code is the prime suspect.
 * m28 fails  => the fault is in h75, which is three lines of routing and a struct field.
 * ⚠ Kept as a compile-time switch rather than two source trees: a second copy that drifts is this
 * project's oldest trap, and the whole point is that the two builds differ ONLY here. */
/* ★★ h86: H76_DIAGNOSTICS is now DRIVEN BY EHCI_PAINT, set beside EHCI_LOG_LEVEL in CMakeLists.txt
 * (user directive 2026-08-13: the release build must not paint the screen). PAINT=1 is byte-for-byte the
 * m34 behaviour — the watchdog, the exposure proof-of-life paint and rows 1-5. PAINT=0 removes every
 * paint site and the arming; the counters underneath cost nothing and stay. The bisect note above still
 * applies: PAINT=0 is the m28 shape. */
#ifndef EHCI_PAINT
#define EHCI_PAINT 1
#endif
#define H76_DIAGNOSTICS EHCI_PAINT
/* h76 screen watchdog state. Declared UP HERE because the exposure path arms it (selfprobe_tick, far above
 * the painter itself) — one definition, not a forward-declared pair that must be kept in step. */
static volatile UInt32 gPwArmed = 0;        /* set at the exposure — only watch once a mount is in progress */
static volatile UInt32 gPwPaints = 0;       /* bounded so this can never run for ever */
static volatile UInt32 gPwPumpLast = 0, gPwPumpAtTick = 0;
/* The painter itself lives beside storm_paint (it needs the framebuffer knowledge); the exposure path calls
 * it once as a proof of life, and that call site is ~1000 lines above the definition. */
static void paint_watchdog_state(void);
/* lc1: set by ehci_vhub_stop_service — the heartbeat stops re-arming and the SIH stops re-enabling
 * USBINTR, so the driver goes quiet. Declared up here because vhub_heartbeat checks it. */
static volatile int    gServiceStop = 0;
static volatile UInt8  gPortConn[15], gResetPending[15], gPortEvent[15], gPortResetDone[15];
static volatile UInt32 gResetAtFrame[15];/* frame-ms at which to deassert a port reset            */
static volatile UInt8  gResetEnabling[15];   /* r38: reset deasserted, now waiting for the port to ENABLE before reporting reset-complete */
static volatile UInt32 gEnableDeadline[15];  /* r38: frame-ms safety cap on that enable-wait          */

/* ==================== r36 RELIABILITY DIAGNOSTIC — port-event ring ====================
 * Chasing the intermittent no-mount: on a losing boot the SanDisk enumerates (class-8 claim) but
 * USBCompositeDriver's interface setup dies with -6999 BEFORE slot 6 (CreateBulkEndpoint), so our
 * self-probe never gets bulk eps (gDev[0].pOut/gDev[0].pIn=ffffffff). This ring timestamps every port CONNECT/
 * DISCONNECT/RESET transition (interrupt-safe — NO File Mgr); uim23 drains it at task level. Prime
 * suspect: a spurious DISCONNECT straddling the reset window (conn 1->0->1) that tears the device
 * down mid-claim -> the composite bounce. Pure logging — behaviour is UNCHANGED. */
#define PEV_CONNECT     1
#define PEV_DISCONN     2
#define PEV_RST_ASSERT  3
#define PEV_RST_DEASS   4
#define PEV_ENABLED     5   /* r38: port ENABLED (HS handshake done) — reset-complete reported to the hub driver */
typedef struct { UInt32 ms; UInt8 port, ev; UInt16 portsc; } PortEvt;
#define PEVT_N 48
static volatile PortEvt gPEvt[PEVT_N]; static volatile UInt32 gPEvtHead = 0, gPEvtTail = 0;
static void pevt(UInt8 port, UInt8 ev, UInt32 portsc)
{
    UInt32 i = gPEvtHead;
    if (i - gPEvtTail >= PEVT_N) return;                       /* ring full — drop (drain fell behind) */
    gPEvt[i % PEVT_N].ms = frame_ms(); gPEvt[i % PEVT_N].port = port;
    gPEvt[i % PEVT_N].ev = ev; gPEvt[i % PEVT_N].portsc = (UInt16)portsc;
    __asm__ __volatile__("eieio");                             /* publish payload before the index */
    gPEvtHead = i + 1;
}
int ehci_vhub_portevt_pop(UInt32 *ms, UInt8 *port, UInt8 *ev, UInt32 *portsc)   /* uim23 task-level drain */
{
    volatile PortEvt *e;
    if (gPEvtTail >= gPEvtHead) return 0;
    e = &gPEvt[gPEvtTail % PEVT_N];
    if (ms) *ms = e->ms; if (port) *port = e->port; if (ev) *ev = e->ev; if (portsc) *portsc = e->portsc;
    gPEvtTail++;
    return 1;
}
/* r83 OBSERVE: frame_ms/ring-independent liveness + raw-PORTSC probe (see ehci_vhub.h). Reads live PORTSC
 * so it shows the TRUE hardware state even if service_ports isn't scanning; gVhubTick tells us whether it is. */
UInt32 ehci_vhub_obs(UInt32 *svcCalls, UInt32 *portscArr, UInt32 *connArr, UInt32 maxPorts)
{
    UInt32 n = gSoftc.nPorts, i;
    if (n > maxPorts) n = maxPorts;
    if (svcCalls) *svcCalls = gVhubTick;
    for (i = 0; i < n; i++) {
        if (portscArr) portscArr[i] = ehci_read32(gSoftc.opBase, EHCI_PORTSC(i));   /* live hardware state */
        if (connArr)   connArr[i]   = gPortConn[i];                                 /* our tracked state   */
    }
    return n;
}
/* r85: gate for the [obs] probe. Armed by the app (via 'Eusb' obsArmFn) when it enters the post-desktop
 * observe loop, so uim23's probe does NOT run during boot — boot enumeration stays clean + unconflated. */
static volatile int gObsActive = 0;
void ehci_vhub_obs_arm(void)   { gObsActive = 1; }
int  ehci_vhub_obs_armed(void) { return gObsActive; }
/* r36: snapshot of the last downstream transfer the engine reaped (addr/pid set at issue, status at
 * reap) so uim23 can name WHICH transfer failed if gDownErr/gDownTimeouts climb during the setup. */
static volatile UInt32 gDpLastAddr = 0, gDpLastPid = 0; static volatile long gDpLastStat = 0;
/* ★★★★ h13: how many bytes the last reaped transfer ACTUALLY delivered. down_reap has always computed this
 * from the qTD residue (it feeds Apple's completion `actual`), but nothing on the CONTROL path could see it,
 * so a control IN that delivered 0 or a partial payload was indistinguishable from a full one. That is the
 * h12 hub bug: GET_PORT_STATUS "succeeded" without writing gAsPortSt, the caller read the PREVIOUS port's
 * status, and a high-speed drive was latched as a low-speed tier-2 device. Same defect class r78 already
 * fixed on the BOT path ("a SHORT transfer ... slipped through as SUCCESS"); the control path still had it. */
static volatile UInt32 gDpActual = 0;
/* ★★★★ h13: a CONTROL-transfer-specific completion record, so a phase's verdict belongs to that phase.
 * The old ctl_done() test was `(gDownDone + gDownErr + gDownTimeouts) != mark` — it fires when ANY transfer
 * retires, bulk included. With a drive mounted and a second device enumerating, a bulk read completing between
 * our SETUP and our check already advanced a control phase early; reading a status that way would have
 * attributed a bulk transfer's error to our control transfer. gCtlSeq is bumped only by control completions
 * (gDpBulkEp < 0), so ctl_done() now waits for OUR class of transfer and gCtlStat/gCtlActual describe it.
 * ⚠ Residual, documented rather than papered over: BOT recovery (bio_recover_*) and Apple's uim7 path also
 * submit ep0 control transfers, and one of those retiring in the same window would still be misattributed.
 * Both are rare-path, and this is strictly narrower than counting every bulk completion as ours. */
static volatile UInt32 gCtlSeq = 0, gCtlSeqMark = 0, gCtlActual = 0;
static volatile long   gCtlStat = 0;
UInt32 ehci_vhub_roothub_addr(void) { return gRootHubAddr; }   /* so ctrl_trace can budget root vs downstream */

/* ==================== status-change interrupt hold (slot 11) ==================== */
static volatile void  *gIntBuf = 0, *gIntCB = 0, *gIntRefcon = 0;
static volatile UInt32 gIntLen = 0, gIntPending = 0, gIntTrigger = 0;

/* ==================== control-completion queue (root-hub phases complete here) ==================== */
typedef struct { void *upp; void *pipe; } Compl;
#define C_N 64
static volatile Compl gC[C_N]; static volatile UInt32 gCHead = 0, gCTail = 0;
static void enq_compl(void *upp, void *pipe)
{ UInt32 i = gCHead; gC[i % C_N].upp = upp; gC[i % C_N].pipe = pipe; gCHead = i + 1; }

/* ==================== downstream control transfers (per-phase, real devices) ====================
 * DMA-from-driver: one locked page carved into a self-linked control QH + a scratch qTD + a bounce
 * buffer (EHCIProbe v0.5 recipe). The USL pipelines phases, so a request FIFO serializes them onto
 * the single in-flight qTD (app v0.36 fix); down_reap fires the USL completion UPP when each finishes. */
#define DOWN_QH_OFF  0x000
#define DOWN_QTD_OFF 0x040
/* r71 (THROUGHPUT Phase 1b): bounce enlarged 5→32 pages = 128KB, so ONE pre-queued READ command carries a
 * multi-qTD DATA CHAIN (7 qTDs × 20KB) → CSW, amortizing the ~6.7ms FIXED per-command overhead r70 exposed
 * (device flash-access + the 1 interrupt + CBW + statemachine). Model (fixed ~6.7ms + data @ ~13 MB/s):
 * 20KB→2.5, 128KB→~7.8 MB/s; bigger chunks asymptote to the ~13 MB/s data-phase ceiling. NOTE the split:
 * READS (pre-queue, multi-qTD chain in bio_issue_read) use DOWN_MAX_RBLOCKS/the whole 128KB bounce; WRITES +
 * the sync ehci_usb_read/write are still SINGLE-qTD (5 page ptrs max) so they stay capped at DOWN_MAX_BLOCKS. */
#define DOWN_DATA_PAGES 32                       /* r71: 128KB DMA bounce (gDownBuf); one data qTD still spans <=5 pages */
#define DOWN_BUF_MAX (DOWN_DATA_PAGES * 0x1000)  /* 131072 */
#define DOWN_MAX_BLOCKS 40    /* single-qTD cap (writes + sync read/write): 40 blocks = 20480 = 5 page pointers. r66/r63:
                               * the r63 per-endpoint engine + correct 5-page epq_issue made 40-block single-qTD reliable
                               * (r46's old 40-block backfire was the single-shared-QH engine). Unchanged in r71 — only the
                               * multi-qTD READ chain (below) goes past 20KB. */
#define DOWN_MAX_RBLOCKS 256  /* r71: pre-queued READ chunk = 256 blocks = 131072 = 128KB via a 7-qTD data chain.
                               * Chain uses nData(<=7)+CSW+dummy = <=9 slots of the QTD_POOL(10) — fits with 1 spare;
                               * larger chunks (256KB+) would need a bigger pool. */
static volatile UInt8 *gDownPg = 0; static UInt32 gDownPgP = 0;
/* ---- r63 (A1): PER-ENDPOINT persistent QHs with HARDWARE data toggle ----
 * Replaces the single shared gDownQH that we reprogrammed per transfer (control<->bulk) + a SOFTWARE
 * data toggle. That design desynced the BOT toggle under the interleaved Finder workload and wedged the
 * device (r60-r62; the stick is flawless on OHCI/1.1, so the fault was ours). Now: one resident QH per
 * endpoint — control ep0 (DTC=1, deterministic control toggle) + bulk-OUT + bulk-IN (DTC=0, so the
 * controller maintains the data toggle in HARDWARE across every transfer, exactly like OHCI's per-endpoint
 * EDs). Each QH carries 2 alternating qTDs and is fed by the "dummy-qTD append" (EHCI spec 4.10.2 + Linux
 * qh_append_tds): a permanently-inactive tail qTD is filled in place with the Active bit written LAST after
 * a barrier, so the controller only ever sees the old inactive dummy or a fully-formed active qTD — never a
 * half-built one. All QHs are spliced into the async ring once at init and left resident (no epChar
 * reprogramming, no overlay-poking in steady state — both were race/toggle hazards). Still one transfer in
 * flight (gDpBusy); true concurrency/pipelining is a later throughput step. */
/* r69 (THROUGHPUT Phase 2b foundation): a POOL of qTDs per endpoint (was 2) so a single BOT command can be
 * pre-queued as a qTD CHAIN — e.g. [data -> CSW] on the IN QH — with one terminal interrupt instead of the
 * 3-phase/3-interrupt serialization (r67 proved ~85-90% of each command is per-command overhead). 10 covers
 * a future 128KB data chain (7 qTDs) + CSW + a tail dummy. The dummy-append cycles the pool modulo QTD_POOL. */
#define QTD_POOL 10
typedef struct {
    ehci_qh  *qh;   UInt32 qhP;
    ehci_qtd *td[QTD_POOL]; UInt32 tdP[QTD_POOL];   /* pool: real qTDs of a chain + the tail dummy, cycled mod QTD_POOL */
    UInt8 dummy;                      /* index of the current tail dummy = where the QH advances next */
    UInt8 dtc;                        /* 1 = toggle from qTD (control ep0); 0 = HW-maintained toggle (bulk) */
    UInt32 addr, endpt;               /* endpoint this QH is programmed for (0/0 until programmed) */
} EpQ;
static EpQ gCtrlQ;        /* device control endpoint 0 — SHARED: enumeration is serialized (see below) */

/* ★★★★ n14 STEP 1+2: PER-DEVICE STATE, so more than one drive can be mounted at once.
 *
 * WHY THIS SHAPE. The engine used to hold exactly one device's worth of state in loose globals, and that is
 * what made a second drive destructive rather than merely unsupported: its arrival overwrote the mounted
 * device's port and probe flags, so the mounted device's own disconnect could no longer be recognised and
 * gDev[0].pstate latched at 10 with enumeration gated off for the rest of the session (the n12 hardware run; fixed
 * defensively in n13). Owning the state per device is the real fix.
 *
 * WHAT IS PER-DEVICE, AND WHAT DELIBERATELY IS NOT:
 *   per-device  pstate / pOut / pIn / probedPort / curAddr, and its OWN pair of bulk queue heads. The QH
 *               pair is the correctness-critical part: each endpoint's data toggle lives in its QH overlay
 *               (DTC=0, hardware-maintained since r63), so two devices sharing one QH pair would corrupt
 *               each other's toggles — exactly the class of bug r63 was built to kill.
 *   SHARED      gCtrlQ and the gAs enumeration state machine. Enumeration is inherently serial: you reset a
 *               port, assign an address and read descriptors for ONE device at a time, then hand the result
 *               to a device slot. Making that concurrent would add risk for no benefit, so it stays single
 *               and the 142 gAs call sites stay untouched.
 *   SHARED      gBulkEP[] (6 endpoint slots with a `used` flag) — enough for 3 devices at 2 endpoints each.
 *   STILL SHARED, FOR NOW: the gDp* in-flight-transfer tracker. Transfers therefore still serialize across
 *               devices, one in flight at a time. That is correct, just not yet concurrent; making
 *               down_pump round-robin over devices is step 3.
 *
 * The existing globals become macro aliases onto gDev[gCur], so every call site compiles unchanged and the
 * compiler checks all ~470 of them for us. With gCur pinned at 0 the behaviour is byte-identical to n13,
 * which is what makes this safe to land without hardware. gCur is set per operation once step 3 lands. */
/* ★★★★ h14: FOUR concurrent devices (was 2). This is the maximum the single wired DMA page holds, and the
 * layout was designed for it: devices 1..3 occupy 0x500 / 0x800 / 0xB00, each 0x300 wide, ending at 0xDFF,
 * and h10 deliberately placed the hub's status-change QH at 0xE00 "ABOVE the device area even at its
 * maximum" so raising this could not scribble it. Full arithmetic in the layout note in vhub_xfer_init.
 * ⚠ GOING BEYOND 4 IS NOT A CONSTANT CHANGE — it needs a second DMA page (a second NewPtrSysClear +
 * LockMemory + GetPhysical, with the per-device base computed across both pages), because 4 devices plus the
 * hub QH and its buffer fill this page to 0xF80 of 0x1000.
 * ⚠ ANYTHING SIZED PER-DEVICE MUST BE DERIVED FROM THIS, NOT WRITTEN AS A LITERAL. NBULK was a bare 6 and
 * 4 devices need 8 endpoint entries; that is the same defect shape as every other bug in this driver's
 * history — a fact recorded in one place and not consulted when a related one changed. */
#define USB_MAX_DEV 4
typedef struct {
    EpQ  bulkQ[2];          /* [0] = bulk-OUT (dirIn 0), [1] = bulk-IN (dirIn 1) — per device, see above */
    volatile int pstate;    /* self-probe / mount state machine (10 = probed + published + mounted) */
    volatile int pOut, pIn; /* this device's gBulkEP[] slot indices, -1 = not registered */
    volatile int probedPort;/* the root port this device is on, -1 = none */
    volatile UInt32 curAddr;/* USB address we assigned it */
    volatile UInt8 inUse;   /* 1 = this slot owns a device */
    /* ★ n27: 1 = this device is running at HIGH speed (USB 2.0), 0 = full/low (USB 1.1). Read from the port's
     * ENABLE bit after reset, which IS the EHCI speed determination: a root-hub port that enables after reset
     * carries a high-speed device, and one that stays disabled-but-connected is the FS/LS handoff signal.
     * Today this is always 1, because a port that does not enable is surrendered to Apple's 1.1 companion and
     * their stack mounts it (and posts THEIR alert) — we never own a 1.1 device. It is recorded rather than
     * assumed so the user-facing message states a measured fact, and so it stays correct when real hub
     * support lands: driving FS/LS devices behind a high-speed hub ourselves is an explicit roadmap item, and
     * at that point the 1.1 branch starts being reachable. */
    volatile UInt8 hiSpeed;
    /* ★ h3 tier 1: nonzero = this device sits behind the claimed external hub, and `hubPort` is which of its
     * downstream ports. Recorded so that when the HUB's own root port is pulled we can free every slot that
     * lived behind it — the "who resets it when the slot changes hands" question, asked up front this time
     * rather than after a hardware run. */
    volatile UInt8 viaHub;
    volatile UInt8 hubPort;
} UsbDev;
/* ★★★★ h3 TIER 1: the one external hub we have claimed, if any.
 * Deliberately NOT a gDev[] slot: a hub has no bulk endpoints, so it needs an ADDRESS and control transfers
 * and nothing else. Keeping it out of gDev[] means USB_MAX_DEV stays at 2 and the DMA page layout is not
 * touched — two drives behind the hub still fit the existing slots. */
#define HUB_ADDR 15u                     /* well clear of DEV_ADDR(slot) = 1 + slot */
static struct {
    volatile int    rootPort;            /* our root port the hub hangs off, -1 = none claimed */
    volatile UInt32 addr;                /* the hub's USB address (HUB_ADDR) */
    volatile int    nPorts;              /* bNbrPorts from its class descriptor */
    volatile int    scanPort;            /* next downstream port to examine, 1-based; wraps for a re-sweep */
    volatile UInt8  claimed;             /* 1 = enumerated + configured + ports powered */
    /* ★ h4: the walk REPEATS. h3 swept once, 120 ms after powering the ports, and found nothing — because a
     * flash drive inserted into an UNPOWERED port only starts booting when PORT_POWER arrives and commonly
     * needs several hundred ms to assert its pull-up. The h3 log proves it: ports 1 and 2 read CONNECTION=0
     * with wPortChange=0, i.e. the hub had recorded no attach at all yet. A hub driver polls; so do we now.
     * This also removes h3's hot-plug limitation — a drive plugged in later is simply seen on a later sweep. */
    volatile UInt32 nextSweepMs;         /* frame_ms() before which we do not start another sweep */
    /* ★ h5: consecutive sweep failures. h4 retried port 1 ONE HUNDRED AND SEVENTY-EIGHT times because a
     * failure never advanced scanPort, saturating the engine — which is what the user saw as the stack
     * "stopping looking" for devices on the root ports. A sweep now gets a bounded budget and then gives the
     * hub up entirely, returning to n27 behaviour. */
    volatile UInt32 failStreak;
    /* ★ h6: downstream ports we have determined are NOT high-speed. Without this an FS device would be reset
     * on every 500 ms sweep forever, because a reset is the only way to learn FS from HS and the answer does
     * not change. Cleared when the hub goes, since a different device may be plugged in next. */
    volatile UInt32 skipMask;
    /* ★ h10: ports the hub has TOLD us changed and we have not looked at yet. The sweep no longer runs on a
     * timer at all — it runs only when there is a bit set here. Seeded with every port at claim time so
     * devices already attached are found. */
    volatile UInt32 changeMask;
} gHub = { -1, 0, 0, 0, 0, 0, 0, 0, 0 };
/* ★★★★ h10: THE HUB'S STATUS-CHANGE ENDPOINT — no more polling the bus.
 * h7-h9 asked every port for its status every 500 ms, forever. Measured against h6 that is 205 control
 * transfers and climbing after everything had mounted, versus ZERO — and those transfers are issued from the
 * SIH, concurrent with File Manager I/O on the mounted volumes. That is the documented r48 hard-crash shape
 * and it took the machine down twice, both times during a write (Trash, and an eject flush).
 * A hub has exactly one interrupt IN endpoint whose whole purpose is to say WHICH port changed. We park a
 * single IN qTD on it, on its own resident QH in the async ring, and then do nothing at all: the hub NAKs it
 * in HARDWARE until something actually changes. Checking it is a read of the qTD token out of the DMA page —
 * a memory load, no bus traffic, no engine involvement, nothing issued from the SIH.
 * ⇒ steady state costs exactly nothing, which is the property h6 had by accident and h7 destroyed. */
static EpQ            gHubIntQ;               /* resident QH for the hub's status-change endpoint */
static volatile UInt8 *gHubStBuf = 0;         /* DMA buffer the change bitmap lands in */
static UInt32          gHubStPhys = 0;
static volatile int    gHubIntArmed = 0;      /* 1 = a qTD is parked and waiting on the hub */
static volatile int    gHubIntSlot = 0;       /* which qTD of the pool is parked */
static UInt32          gHubIntLen = 1;        /* ceil((nPorts+1)/8) bytes of bitmap */
static UInt32          gHubIntEp = 0;         /* the hub's interrupt IN endpoint number */
static volatile UInt32 gHubIntErrs = 0;
/* ★ h10: set at SIH level when the hub is claimed; consumed at TASK level, because programming a QH that is
 * linked into the live async ring is the r84/r85 freeze if done from the SIH. The interrupt-path audit caught
 * exactly that in the first cut of this build. Same shape as gAsNeedBulk. */
static volatile int gHubIntNeedArm = 0;
/* ★★★★ h13: two counters/masks the h12 run needed and did not have.
 *  gHubShortSt — GET_PORT_STATUS replies that completed WITHOUT delivering their 4 bytes. Before h13 these
 *    were invisible and silently produced the previous port's verdict. If this is non-zero the short-reply
 *    path is real on this hardware, which is worth knowing whatever else the run shows.
 *  gHubLsSeen  — ports that have read LOW-SPEED ONCE. The low-speed verdict latches into gHub.skipMask and is
 *    cleared only when the device physically leaves, so ONE reading decides a port's fate for as long as the
 *    device stays plugged in. That is far too much weight for a pre-reset speed bit: §11.24.2.7.1 derives it
 *    from the D± idle state, which is not settled during a device's power-up (and h5→h6 already learned that
 *    the pre-reset HIGH-speed bit is untrustworthy — this is the same bit's mirror image, left in place).
 *    A port must therefore read low-speed on TWO CONSECUTIVE sweeps before it is skipped. The display's real
 *    HID reads low-speed every sweep, so it still latches immediately after its second look; a drive that
 *    merely glitches once during power-up no longer loses its only chance. */
static volatile UInt32 gHubShortSt = 0;
static volatile UInt32 gHubLsSeen  = 0;
/* ★★★★ h14: how many times an enumeration was DEFERRED because mounted-volume I/O was in flight.
 * n18 fix 4 refuses to start enumerating while the bio ring is non-empty, because the engine runs one
 * transfer at a time and a newcomer's probe would otherwise sit in front of a mounted drive's queued reads
 * until the File Manager gave up (the n17 ~30s waits). That trade is right, but it is a GLOBAL condition —
 * "any device has I/O pending" — and its cost scales with how many drives are mounted. At USB_MAX_DEV = 4
 * the ring is non-empty far more of the time, so the newcomer's wait grows exactly when the user is most
 * likely to be plugging a drive into a busy machine.
 * Whether that is a real wall or merely a latency cost is a question about timing on real hardware, not
 * something to redesign on a guess: count it. A large value beside a drive that took a long time to appear
 * (or never did) names this guard as the cause; a small one rules it out. */
static volatile UInt32 gEnumDeferBusy = 0;
/* ★★★★ h15: consecutive enumeration arms that ended having done NOTHING.
 * The h14 hang was a run armed for a root port, sent down the hub path by a stale gAs.hubAddr, exiting via
 * h3_next_port — which clears gAs.running but touches neither gSelfEnumPort, gSelfEnumDone nor
 * gSelfEnumTries — so as_tick armed the identical run again, forever. The stale field is fixed at its source,
 * but the SHAPE is what to defend against: an arm that achieves nothing and changes no state will repeat as
 * fast as the heartbeat allows, and nothing in the retry policy notices because no failure occurred.
 * So bound it. A healthy run passes here a handful of times (hub claim walks nPorts, then the change-driven
 * design means visits are rare — h13 logged about ten in a whole session), so 64 consecutive is far above
 * normal and far below harm. On hitting it, stop re-arming and say so; the next real port event clears
 * gSelfEnumDone and everything resumes. Same lesson as h5's "a hub-sweep failure must ADVANCE the sweep",
 * generalised from failures to no-ops. */
#define ARM_NOPROGRESS_MAX 64u
static volatile UInt32 gNoProgressArms = 0;
static volatile UInt32 gArmStuckMask = 0;    /* interrupt-set, task-logged, like every other notice here */
static UsbDev gDev[USB_MAX_DEV];
/* ★★★ n18 FIX 3: DEVICE GEOMETRY IS PER-DEVICE. These were global, and the probe writes them, so a second
 * device's READ CAPACITY overwrote the MOUNTED device's block count. gSvc.blkCnt is what the block driver
 * hands the File Manager, so drive A's volume would have been described by drive B's geometry: reads and
 * writes past the end of the smaller one, or a silently truncated volume. A second corruption path,
 * independent of the address collision, and it would have survived fixing that alone.
 * gPBlkSize/gPBlkCnt remain as slot 0's view, because publish_service and the block driver are still
 * single-device (step 4); every probe now writes its OWN slot and only slot 0 is published. */
static UInt32 gDevBlkSize[USB_MAX_DEV], gDevBlkCnt[USB_MAX_DEV];
#define gPBlkSize gDevBlkSize[0]
#define gPBlkCnt  gDevBlkCnt[0]
/* ★ n14 step 3: the gDev[] slot the CURRENTLY RUNNING ENUMERATION is filling. Set by as_tick when it
 * allocates a slot, read by ehci_vhub_create_bulk, which the USL calls during that same enumeration.
 * Declared here because gAs is defined much further down and create_bulk cannot see it.
 *
 * This is NOT a return of the gCur mistake. gCur was read by the I/O path at interrupt level while
 * other contexts moved it, which is what made two devices unsafe. Nothing in the I/O path reads
 * gEnumDev: block I/O carries its device on the request, and the pb_ primitives plus bot_step take it as
 * an explicit parameter.
 * Enumeration is serial (one gAs, one shared ctrl ep0), so exactly one writer and one reader. */
static volatile int gEnumDev = 0;
/* ★★★★★★ h59 — LATCH THE SLOT AT THE MOMENT THE PROBE COMPLETES, because gEnumDev is a MOVING INDEX and the
 * deferred exposure reads it up to 45 SECONDS LATER.
 *
 * ⚠ THE DEFECT: the task-level handoff did `int hd = gEnumDev;` at RELEASE time, and gEnumDev is rewritten by
 * every slot allocation (`gAs.dev = slot; gEnumDev = slot;`). While an exposure sits parked in the h30/h44
 * defer waiting for a modal dialog, any new enumeration overwrites it — so the exposure would hand the OS the
 * WRONG SLOT. That is why the scan had to be blocked for the whole defer window (h58), and blocking it for the
 * whole window is what left the keyboard hostage for 45 s on the m10 run.
 * ★ Latching turns a moving global into a stable record, which is what lets the guard be narrowed safely.
 * Same family as the n19->n23 lesson: state that existed and was correct, read at a transition where it no
 * longer described what the reader assumed. -1 = nothing latched. */
static volatile int gExposeDev = -1;
/* h61: ms when the port last reported ENABLED. The cold-boot-vs-hot-plug question is entirely about how
 * long the device has been alive when we first address it, so the trace prints the gap explicitly. */
static volatile UInt32 gPortEnabledMs = 0;
static volatile int    gPortEnabledPort = -1;  /* h61: which port that was (gAs is declared later) */
/* h30: the boot-time announce is held until the Finder settles. h59 moved this declaration UP from beside its
 * writers so as_tick's scan guard can read it — the guard needs to tell "task level must finish this now" from
 * "task level has it and is parked on a modal dialog", which is the whole of the h59 fix. */
static volatile int gExposureDeferred = 0;
static volatile int gDpDev = 0;        /* n14 step 3: device slot the in-flight transfer belongs to. One
                                        * transfer is in flight at a time (the CBW/CSW/bounce DMA buffers are
                                        * still shared), so a single owner field is enough; true concurrency
                                        * needs per-device buffers, including a 128KB bounce each. */

/* ★★★★ n14 STEP 1 COMPLETE: THE PER-DEVICE ALIASES ARE GONE.
 *
 * Steps 1 and 2 landed the per-device storage by turning the old globals into macro aliases onto
 * gDev[gCur]. That got a ~470-site refactor in with the compiler checking every site, but it could not
 * express two devices doing I/O, and the reason is the whole point of this pass:
 *
 *   gPOut / gPIn were read by the bio engine AT INTERRUPT LEVEL, through gCur, a MUTABLE GLOBAL.
 *   as_tick runs both from the heartbeat (interrupt) and from ehci_vhub_selfprobe_tick (task, slot 23).
 *   gAsBusy makes as_tick non-reentrant but does nothing about the ISR, so any task-level path that moved
 *   gCur left a window where an interrupt would issue device A's transfer against device B's endpoints.
 *   n12's unregistered-endpoint guard downgrades that to a DROPPED request rather than corruption, which
 *   is still a failed copy. That is why the aliases had to go rather than be worked around.
 *
 * Every site now names its device explicitly. gDev[0] is a literal no interrupt can change underneath us.
 * Behaviour is unchanged, because gCur was pinned at 0 throughout. STEP 2 replaces those literal 0s with a
 * threaded `int d` argument through the pb_* / bio_* primitives, at which point a second device can be
 * enumerated into its own slot safely. See docs/MULTI-DEVICE-DESIGN.md.
 *
 * NOTE the rename deliberately did NOT touch string literals: 17 log messages name these identifiers, and
 * changing them would alter the driver's log output and break the documented watch-for markers. All 374
 * string literals in this file were verified byte-identical across the rename. */
static EpQ *gDpQ = 0;             /* the endpoint queue the in-flight transfer was issued on */
static ehci_qtd *gDpTd = 0;       /* the in-flight (activated) qTD to reap */
static volatile UInt8 *gDownBuf = 0;                 /* r46: 20KB (5-page) DMA bounce for big data chunks */
static UInt32 gDownBufPhys[DOWN_DATA_PAGES];         /* physical addr of each bounce page (may be non-contiguous) */

/* ================================================================================================
 * ★★★★★★★ THE SPLIT — TWO IN-FLIGHT SLOTS. `gDp*` IS NOW BLOCK I/O ONLY; `gEg*` IS THE ENGINE.
 *
 * ⚠ THE DEFECT THIS REMOVES, measured on hardware m25/h73 2026-08-12 (f4 direction B):
 *     f4 B CLOBBERS = 1, of a CONTROL transfer, while an enumeration was running, at 11900 ms.
 * `gDp*` was ONE in-flight slot with THREE producers — down_issue (control AND probe/enumeration bulk),
 * bio_issue_read and bio_issue_write — over ONE shared DMA bounce (gDownBuf). down_pump CHECKS the slot
 * before using it; bio_issue_* DO NOT, and that asymmetry is deliberate (h18 added the check and FROZE the
 * machine: block I/O then waits for ever behind a stuck enumeration, and since our logging is File Manager
 * I/O the log dies with it). So the fix could never be "take turns more politely".
 *
 * ★ THERE IS NO HARDWARE REASON FOR THE SHARING. Control runs on gCtrlQ, probe bulk on gDev[d].bulkQ[],
 * block I/O on the same per-device pair — all separate queue heads, all already spliced into the async
 * ring, and the controller executes qTDs on all of them concurrently. The single software slot was the
 * entire constraint. Diagnosed in docs/CTL-BULK-SPLIT.md after h16-h18, designed there, never built.
 *
 * ★★ THE CUT IS ENGINE vs BLOCK I/O, not control vs bulk. The doc proposed control-vs-bulk because h16/h17
 * showed control transfers starving during a copy; but the m25 measurement shows the victim class is
 * "whatever the ENGINE had in flight", and the probe's BULK transfers are just as exposed as its control
 * ones. Engine transfers are inherently serial anyway (one gAs, one enumeration at a time, down_pump issues
 * one at a time), so they share a slot with each other safely. Block I/O gets its own. Two slots is
 * sufficient and is the smallest change that makes the clobber structurally impossible.
 *
 * ★★★ THREE FURTHER DEFECTS FALL OUT WITH IT, all named in the doc:
 *   1. THE SHARED BOUNCE. Both paths pointed the controller at gDownBuf, so a clobber left two transfers
 *      live in hardware DMAing the same memory — latent corruption of the MOUNT'S data. The engine now has
 *      its own bounce (gEgBuf) and never touches gDownBuf.
 *   2. down_reap COULD NOT TELL WHOSE COMPLETION IT WAS. `if (gBioPhase && !gDpUPP) bio_advance(status)`
 *      fired for CONTROL completions too (ours pass upp = 0), so a control completion could advance the BOT
 *      state machine with an unrelated status. bio_advance is now driven ONLY from the block-I/O slot.
 *   3. ONE WATCHDOG FOR TWO CLASSES. gDpArmTick belonged to whichever transfer armed last, so an orphaned
 *      transfer was never timed out at all — which is exactly why gDownTimeouts stayed 0 through h16-h18.
 *      Each slot now carries its own arm tick and is polled independently.
 *
 * ⚠ AND A LATENT BUG THE SPLIT EXPOSED: epq_issue maps FIVE page pointers (20 KB) but down_submit capped
 * len at DOWN_BUF_MAX (128 KB). An engine transfer above 20 KB would have run off the end of its mapping.
 * Unreachable in practice (control <= 64 B, probe bulk <= 512 B, gPB is 20 KB) but it was one caller away.
 * ENG_BUF_MAX now bounds both the buffer and the request, with a loud refusal — the doc asked for exactly
 * this and called it "true by assertion rather than by luck". */
#define ENG_BUF_PAGES 5                             /* epq_issue maps 5 qTD buffer pointers — match it exactly */
#define ENG_BUF_MAX   (ENG_BUF_PAGES * 0x1000)      /* 20480; also == DOWN_MAX_BLOCKS*512, the gPB scratch size */
static volatile UInt8 *gEgBuf = 0;                  /* the ENGINE's own DMA bounce — never gDownBuf */
static UInt32 gEgBufPhys[ENG_BUF_PAGES];
static EpQ      *gEgQ = 0;                          /* engine slot: queue head the in-flight transfer is on */
static ehci_qtd *gEgTd = 0;                         /* engine slot: the activated qTD to reap */
static volatile int    gEgBusy = 0, gEgIsIn = 0;
static volatile void  *gEgUPP = 0, *gEgPipe = 0, *gEgDest = 0;
static volatile UInt32 gEgLen = 0, gEgArmTick = 0, gEgNpkt = 1;
static volatile int    gEgBulkEp = -1;
static volatile UInt32 gEgDev = 0, gEgLastAddr = 0, gEgLastPid = 0;
static volatile UInt32 gEgOversize = 0;             /* requests refused for exceeding ENG_BUF_MAX (must stay 0) */
static volatile UInt8  gEgForBio = 0;               /* h75: the in-flight engine transfer belongs to BOT recovery */
static volatile UInt32 gEgRecovCompl = 0;           /* h75: recovery completions routed to bio_advance (diagnostic) */
/* h75: the BOT recovery's gBioPhase values start here. Defined UP HERE rather than beside the rest of the
 * REC_* phases (far below) because down_reap must test it to route a tagged recovery completion, and
 * down_reap is defined long before that block. One definition, not two that must be kept in step. */
#define REC_BASE 20
/* ★ THE INSTRUMENT THAT PROVES THE SPLIT WORKS. Counts every time block I/O armed while the ENGINE slot was
 * busy — i.e. each occasion that WOULD have been a clobber before this change and is now harmless. A run
 * with this > 0 and f4 clobbers still 0 is the split doing its job, visibly. Keeping the old f4 counters
 * alongside it means a regression cannot hide: they must stay 0 for the engine case for ever now. */
static volatile UInt32 gSplitSaved = 0;
/* r69: SEPARATE small wired DMA buffers for the CBW and CSW, carved from the schedule page's spare space, so
 * a pre-queued [data->CSW] chain doesn't collide with the data bounce (Phase 2b needs the CBW, data, and CSW
 * to have distinct DMA targets). Unused until the r70 pre-queue wiring; allocated here as the foundation. */
static volatile UInt8 *gCbwBuf = 0, *gCswBuf = 0; static UInt32 gCbwPhys = 0, gCswPhys = 0;
static int    gDownReady = 0, gDownInitErr = 0, gDownAseOn = 0;
static volatile int    gDpBusy = 0, gDpIsIn = 0;
static volatile void  *gDpUPP = 0, *gDpPipe = 0, *gDpDest = 0;
static volatile UInt32 gDpLen = 0, gDpArmTick = 0;
static volatile UInt32 gDownDone = 0, gDownErr = 0, gDownTimeouts = 0;
static volatile UInt32 gDownRecov = 0;   /* r57: BOT reset-recoveries attempted */
/* h57: QH-overlay halts seen, and overlays re-initialised to clear them. gQhHalted > 0 with gQhUnhalted equal
 * is the fix working; gQhHalted climbing with the same endpoint failing every time would mean the halt is
 * being re-created faster than we clear it, which is a different (device-side) problem. */
static volatile UInt32 gQhHalted = 0, gQhUnhalted = 0;
static volatile UInt32 gDownRelink = 0;      /* r60: async-ring repairs performed (the QH-unlink freeze fix) */
static volatile UInt32 gLastAnchorLink = 0;  /* r60 diag: anchor->hlink (masked) at the last ring check; compare to gDownQHP */
/* r67 THROUGHPUT DIAGNOSTIC: measure the PURE data-phase rate. Two sticks both cap ~1.5-1.9 MB/s read =>
 * engine-limited, not device. Time each big (>=2KB) bulk DATA transfer with FRINDEX (125µs microframes) to
 * separate the data-phase rate from per-command overhead → decides Phase 2 (pre-queue) vs park-mode/RL. */
static volatile UInt32 gDataBytes = 0, gDataFrames = 0, gDataFr0 = 0; static volatile int gDpMeasured = 0;
/* Bulk endpoints (mass storage). uim6 records each endpoint (addr, endpt, dir 0=OUT/1=IN, maxpkt); uim7
 * routes a transfer by (addr, endpt). r63: each registered bulk endpoint is bound to its own resident QH
 * (gDev[0].bulkQ[dirIn]) with HARDWARE toggle, so the `toggle` field below is now LEGACY/unused (the controller
 * owns the toggle) — kept only so the currently-disabled recovery code still compiles; removed in A2. */
/* ★★★★ h14: DERIVED FROM USB_MAX_DEV, no longer a bare literal. Each mass-storage device registers exactly
 * two bulk endpoints (one IN, one OUT), so the table needs 2 per device. It was 6 — fine for 2 devices with
 * two spare, and SILENTLY ONE SHORT of the 8 that 4 devices need. The failure would not have looked like a
 * table overflow: create_bulk returns -1, both live call sites discarded that with (void), pb_find_eps would
 * then have found no endpoints for the fourth slot, and the drive would simply never have appeared. The
 * n24 sweep's rule is exactly this — "a per-slot array and resetting it are two separate jobs, and the
 * compiler checks neither" — so the array now cannot be left behind when USB_MAX_DEV moves again. */
#define NBULK (USB_MAX_DEV * 2)
/* n14 step 3: `dev` = which gDev[] slot owns this endpoint. A transfer is routed to its OWNING device's
 * bulk QH pair rather than to whichever device happens to be current, so two mounted devices can never be
 * issued onto each other's queue heads (and therefore never corrupt each other's hardware data toggle). */
static struct { UInt32 addr, endpt; UInt8 dirIn, toggle; UInt16 maxpkt; UInt8 used; UInt8 dev; } gBulkEP[NBULK];
static volatile int gDpBulkEp = -1; static volatile UInt32 gDpNpkt = 1;
/* MINI FIX: Apple's BULK completion UPP deadlocks at interrupt level on the Mini's shared-interrupt
 * controller -> defer it to TASK level (compl_drain from selfprobe_tick). Dedicated line (MDD) = inline. */
typedef struct { void *upp, *pipe; long status; UInt32 actual;
                 UInt8 twoArg;    /* h94: 1 = 2-arg control UPP (pipe,status); 0 = 3-arg bulk (pipe,status,actual) */
               } ComplDef;
#define NCOMPL 16u
static volatile ComplDef gComplQ[NCOMPL];
static volatile UInt32 gComplHead = 0, gComplTail = 0, gComplDrop = 0;
/* r10 diagnostic: last bulk completion snapshot (interrupt-safe stores in down_reap; task-ctx reads
 * via ehci_vhub_bulk_stats from uim7, since down_reap runs at interrupt level where File Mgr is unsafe). */
static volatile long gBulkLastStat = 0; static volatile UInt32 gBulkDoneN = 0, gBulkErrN = 0;
/* ★★★★★★ THE SPLIT MADE THESE NECESSARY, AND MISSING THEM WOULD HAVE BEEN A REGRESSION WORSE THAN THE BUG.
 *
 * pb_ready()/pb_failed() ask "has MY transfer finished?" and answered it by watching gBulkDoneN+gBulkErrN
 * move. Those aggregates are bumped by EVERY bulk completion — the probe's AND block I/O's.
 * ⚠ Before the split that was survivable only by accident: the single slot meant the probe's transfer and a
 * block transfer could not both be outstanding (bio simply overwrote the probe's — the clobber). THE SPLIT
 * MAKES THEM GENUINELY CONCURRENT, which is the entire point, and it therefore turns a latent false-ready
 * into a LIKELY one: a mounted drive's block completion would tell the probe its own INQUIRY had finished,
 * and the probe would parse whatever was in gPB. That is the p1b failure shape verbatim ("a halted endpoint
 * looked like progress ... state 5 parsed the leftover CBW bytes") arriving by a new route.
 * ⇒ The probe now watches ENGINE completions only. The aggregates stay exactly as they were, because the
 * liveness diagnostics that read them (uim23's bulkCnt, the r95 SIH watcher) genuinely do want "any bulk
 * activity at all" and would be wrong if narrowed. Two questions, two counters. */
static volatile UInt32 gEgBulkDoneN = 0, gEgBulkErrN = 0;
static volatile UInt8 gLastData[16];
typedef struct { void *upp; void *pipe; void *dest; UInt32 addr, len; UInt8 pid; UInt8 obuf[64]; UInt32 olen;
                 void *obig; UInt32 obiglen; int bulkEp;
                 UInt32 qms;          /* f4: frame_ms at submit — down_pump measures the wait */
                 UInt8  forBio;       /* h75: 1 = this ENGINE transfer belongs to the BLOCK-I/O recovery
                                       * sequence, so its completion must drive bio_advance. See down_reap. */
                 UInt8  dead;         /* h94: 1 = its device was rude-removed; the funnel already delivered a
                                       * substitute completion, so down_pump must CONSUME it, never issue it. */
                 } DownReq;   /* obig = large OUT source (write data > 64B) */
#define DOWNQ_N 48
static volatile DownReq gDownQ[DOWNQ_N];
static volatile UInt32 gDownQHead = 0, gDownQTail = 0, gDownQDrop = 0;
/* ==================== h94: USL transfer retirement on rude removal ====================
 * THE h93 RUN'S VERDICT (2026-08-13, MDD): 216 DoDriverIO calls across three sessions, every
 * IOCommandIsComplete verdict noErr — the repaired unit entry's dispatch path is CLEAN. The heap corruption
 * instead tracked the one thing both corrupt sessions shared and every clean session lacked: a RUDE REMOVAL
 * with USL transfers still in flight (a freshly-inserted SanDisk yanked mid-settle; the boot-era leg pull).
 * MECHANISM: the USL on this hardware NEVER calls AbortPipe/DeletePipe (slots 18/19 — zero calls in every
 * banked log, all the way back). On removal it simply frees its pipe/transfer structures. Our ENGINE slot
 * and FIFO still hold that client's completion UPP, pipe pointer and destination buffer; when the orphaned
 * transfer later times out (-6640), down_reap faithfully copied IN-data into the FREED client buffer and
 * fired the completion UPP with the FREED pipe — a use-after-free write into the USL's heap. MacsBug showed
 * it as "Block length is bad" at 01C20350, a 16-byte pointer scribble in a zone ~1MB below our own blocks
 * (which is why the h92 canaries, correctly, never tripped).
 * THE FIX, three layers, all flag work (NO QH surgery — a yanked device's qTD errors out on its own):
 *   1. usl_retire_device(): at SIH, from the disconnect handler, BEFORE the port change is reported to the
 *      USL: null the client pointers out of the in-flight ENGINE slot and every queued FIFO entry for the
 *      dead device, and queue SUBSTITUTE completions (status -6640, the long-proven timeout code) through
 *      gComplQ. gH94Hold then parks the root-hub status-change report until compl_drain has delivered them
 *      at task level — a real happens-before: clients complete while their memory is still live, THEN the
 *      USL learns of the removal and frees.
 *   2. down_pump refuses to issue for a dead device (the h81 lesson, USL-side: only the issue site knows
 *      the truth at the moment it matters) — bulk keyed off gDev[].inUse, control off the dead-addr ring.
 *   3. down_reap's orphan gate: the backstop. If the ENGINE transfer's device is gone and the funnel was
 *      somehow not run, suppress the copy-out and the UPP; count it in gH94Orphaned (MUST stay 0 — nonzero
 *      means layer 1 has a hole and this gate is what saved the heap).
 * The dead-addr ring FAILS OPEN (reference_os9_recovery_paths_fail_open): any new CONNECT clears it, and
 * create_bulk clears it, so a stale entry can never refuse a live device's traffic. Address 0 (enumeration
 * default) and the root hub are never considered dead. */
static volatile UInt32 gH94Retired = 0;    /* client transfers retired by the disconnect funnel */
static volatile UInt32 gH94Orphaned = 0;   /* reap-gate saves; MUST be 0 (nonzero = funnel hole, gate saved us) */
static volatile UInt32 gH94Refused = 0;    /* dead-device issues refused at the pump (h81 mirror) */
static volatile UInt32 gH94Hold = 0;       /* 1 = status-change report parked until compl_drain delivers */
#define H94_DEAD_N 4
static volatile UInt32 gDeadAddr[H94_DEAD_N];   /* recently rude-removed USB addresses (0 = empty slot) */
static void dead_ring_clear(void) { int i; for (i = 0; i < H94_DEAD_N; i++) gDeadAddr[i] = 0; }
static void dead_ring_add(UInt32 a)
{
    int i;
    if (!a) return;
    for (i = 0; i < H94_DEAD_N; i++) if (gDeadAddr[i] == a) return;
    for (i = 0; i < H94_DEAD_N; i++) if (!gDeadAddr[i]) { gDeadAddr[i] = a; return; }
    gDeadAddr[0] = a;                          /* full: overwrite the oldest slot; ring is best-effort */
}
static int addr_in_dead(UInt32 a)
{
    int i;
    if (!a) return 0;
    for (i = 0; i < H94_DEAD_N; i++) if (gDeadAddr[i] == a) return 1;
    return 0;
}
/* 1 = some live owner answers to this USB address today (a mounted/self-enum device, a registered bulk
 * endpoint, the claimed hub, the root hub, or the enumeration default address 0). Used ONLY to decide
 * whether an UNNAMED in-flight control transfer may be conservatively retired at disconnect time — never
 * as a refusal predicate on its own (Apple-era enumeration assigns addresses we cannot see in advance). */
static int addr_is_live(UInt32 a)
{
    int i;
    if (a == 0 || a == gRootHubAddr) return 1;
    if (gHub.claimed && a == HUB_ADDR) return 1;
    for (i = 0; i < USB_MAX_DEV; i++) if (gDev[i].inUse && gDev[i].curAddr == a) return 1;
    for (i = 0; i < NBULK; i++) if (gBulkEP[i].used && gBulkEP[i].addr == a) return 1;
    return 0;
}
/* Queue a substitute completion for a retired client transfer. SIH-safe (ring stores only); delivered by
 * compl_drain at task level, exactly like the v20 deferred bulk completions. twoArg picks the UPP shape:
 * control completions are 2-arg (pipe, status), bulk are 3-arg (pipe, status, actual). */
static void retire_enqueue(void *upp, void *pipe, int twoArg)
{
    UInt32 hh = gComplHead;
    if (!upp) return;                          /* our own transfers (upp 0) need no substitute */
    if ((hh - gComplTail) < NCOMPL) {
        gComplQ[hh & (NCOMPL - 1u)].upp = upp;
        gComplQ[hh & (NCOMPL - 1u)].pipe = pipe;
        gComplQ[hh & (NCOMPL - 1u)].status = -6640L;   /* the proven timeout code — clients have handled it for months */
        gComplQ[hh & (NCOMPL - 1u)].actual = 0;
        gComplQ[hh & (NCOMPL - 1u)].twoArg = (UInt8)twoArg;
        gComplHead = hh + 1u;
        gH94Hold = 1;                          /* park the status-change report until the drain delivers */
    } else gComplDrop++;
}
/* The funnel. Called at SIH from the disconnect handler for device slot d (dAddr = its USB address if
 * known, else 0), BEFORE the port change is reported. Flag stores + ring stores only — no File Manager,
 * no allocation, no QH/overlay writes (the epq task-level-only rule holds: a yanked device's in-flight
 * qTD retires ITSELF via XactErr/watchdog; we only make sure the retirement cannot touch client memory). */
static void usl_retire_device(int d, UInt32 dAddr)
{
    UInt32 t;
    if (dAddr) dead_ring_add(dAddr);
    /* Queued FIFO entries whose device is the one that left. Bulk entries name their owner exactly
     * (gBulkEP[].dev); control entries match by address when we know it. */
    for (t = gDownQTail; t != gDownQHead; t++) {
        volatile DownReq *r = &gDownQ[t % DOWNQ_N];
        int match = 0;
        if (r->dead) continue;
        if (r->bulkEp >= 0 && r->bulkEp < NBULK && gBulkEP[r->bulkEp].dev == (UInt8)d) match = 1;
        else if (r->bulkEp < 0 && dAddr && r->addr == dAddr) match = 1;
        if (!match) continue;
        retire_enqueue((void *)r->upp, (void *)r->pipe, r->bulkEp < 0);
        r->upp = 0; r->pipe = 0; r->dest = 0; r->obig = 0; r->dead = 1;
        gH94Retired++;
    }
    /* The in-flight ENGINE transfer. The slot stays busy — the qTD errors out on its own and down_reap
     * does the bookkeeping — but the client pointers are nulled NOW, so that retirement cannot write into
     * memory the USL is about to free. down_reap's existing `if (dd)` / `if (gEgUPP)` guards become the
     * enforcement. */
    if (gEgBusy) {
        int match = 0;
        if (gEgBulkEp >= 0) match = (gEgDev == (UInt32)d);
        else if (dAddr && gEgLastAddr == dAddr) match = 1;
        else if (gEgBulkEp < 0 && !addr_is_live(gEgLastAddr)) match = 1;   /* unnamed Apple-era device: the
                                                                            * conservative sweep — -6640 is
                                                                            * what a timeout would say anyway */
        if (match) {
            retire_enqueue((void *)gEgUPP, (void *)gEgPipe, gEgBulkEp < 0);
            gEgUPP = 0; gEgPipe = 0; gEgDest = 0;
            gH94Retired++;
        }
    }
    if (gH94Retired)
        ehci_os_ilogx("!! h94: rude removal — client transfers retired before the USL hears (total)",
                      gH94Retired);
}
/* h17's gCtlPromoted and h18's gBioDeferBusy are gone with the changes they measured. Both counters did their
 * job: gCtlPromoted read 0 and refuted h17's theory outright, and gBioDeferBusy's absence from the h18 freeze
 * log is what showed that logging dies with the File Manager, which is why the next attempt needs evidence
 * that survives a stall rather than more counters written through FSWrite. */

/* ================================================================================================
 * ★★★★★★★ f4 — THE SINGLE IN-FLIGHT SLOT, INSTRUMENTED. PURE OBSERVATION: NOTHING HERE CHANGES BEHAVIOUR.
 *
 * ⚠⚠ THE STANDING STRUCTURAL DEFECT, AND IT IS NOT A NEW THEORY. docs/CTL-BULK-SPLIT.md diagnosed it after
 * h16-h18, designed the fix, and THE FIX WAS NEVER BUILT — there is no control slot in this file; grep for
 * gCtlBusy and you get nothing. So `gDp*` is still ONE in-flight slot with THREE producers:
 *     · down_issue()      — every CONTROL transfer and every PROBE/enumeration BULK transfer (queued)
 *     · bio_issue_read()  — block I/O, arms the hardware DIRECTLY
 *     · bio_issue_write() — likewise
 * and `gDownBuf`/`gDownBufPhys[]` is ONE shared DMA bounce that both paths point the controller at.
 * ⚠ The comment at down_pump saying "the CTL/BULK split makes it moot — control gets its own in-flight slot"
 * describes a DESIGN, not this code. A comment is a claim, and it expires silently (CLAUDE.md).
 *
 * ★ TWO DIRECTIONS OF HARM, AND THEY HAVE OPPOSITE SIGNATURES. That is what makes this measurable:
 *
 *   A  STARVATION — the probe is starved by block I/O.
 *      down_pump()'s first statement is `if (gDpBusy) return;`, and during a multi-chunk mount read every
 *      completion re-arms the next chunk (down_reap -> bio_advance -> bio_start_chunk -> bio_issue_read,
 *      gDpBusy = 1) BEFORE down_pump gets its turn. A queued probe transfer is then never issued at all, and
 *      the probe dies on bot_step's 800 ms per-phase cap. This is EXACTLY the mechanism CTL-BULK-SPLIT.md
 *      verified for control transfers during a copy; nothing about it is specific to control.
 *      ⇒ signature: gF4PumpBlocked climbs, gF4PumpMaxWaitMs is large, the probe fails, THE MOUNT IS FINE.
 *
 *   B  CLOBBER — block I/O arms over an in-flight probe transfer.
 *      bio_issue_read/bio_issue_write overwrite gDpBusy/gDpQ/gDpTd/gDpBulkEp/gDpDev/gDpArmTick with NO test
 *      of gDpBusy (h18 added that test and it FROZE THE MACHINE — see the note in bio_kick — so its absence
 *      is deliberate). The probe's qTD is orphaned (nothing polls it), the single watchdog now belongs to
 *      the newcomer, and BOTH transfers are live in hardware on different QHs POINTING AT THE SAME BOUNCE.
 *      ⇒ signature: gF4Clobber > 0. This one can corrupt the MOUNT'S OWN DATA, which is the only hypothesis
 *      on the table that explains an exposed volume the Finder then calls unreadable, plus a crash.
 *
 * ★ AND THE THIRD POSSIBLE ANSWER IS "NEITHER", which is why this is a discriminator and not a fix: if both
 * counters read 0 across the m24 topology then the two never contend, F4 is refuted, and the split stays off
 * the critical path. One boot decides it. Two speculative ROMs on this project were both wrong; the probe
 * answered it in one run (CLAUDE.md, hardware-test discipline).
 *
 * ⚠ DESIGN RULES OBSERVED HERE, each one paid for already:
 *   · counters at interrupt level, ONE summary at task level. h47 logged 705 dumps in a single 45 s window
 *     and was "both diagnostic noise and a plausible confound" — this adds ZERO lines to the hot path.
 *   · gF4Owner is written only where gDpBusy is SET, and read only where gDpBusy is 1, so it can never be
 *     stale when it is consulted and needs no clearing on any of the several paths that clear gDpBusy.
 *   · the summary goes out on the CRITICAL (rate-cap-exempt) channel — see docs/ENGINE-STATE-MACHINE.md §0
 *     and the m24 log, where 458 dropped ring lines destroyed the previous diagnosis.
 * ================================================================================================ */
#define F4_OWNER_NONE 0
#define F4_OWNER_CTL  1        /* down_issue, control (ep0)               */
#define F4_OWNER_PROBE 2       /* down_issue, bulk = probe/enumeration    */
#define F4_OWNER_BIO  3        /* bio_issue_read / bio_issue_write        */
static volatile UInt8  gF4Owner = F4_OWNER_NONE;   /* who armed the slot that is busy NOW */
/* --- direction B: block I/O armed over a slot that was already busy --- */
static volatile UInt32 gF4Clobber = 0;             /* total */
static volatile UInt32 gF4ClobberProbe = 0;        /* ...of a PROBE/enumeration bulk transfer — the dangerous one */
static volatile UInt32 gF4ClobberCtl = 0;          /* ...of a control transfer */
static volatile UInt32 gF4ClobberBio = 0;          /* ...of another block transfer (would be a bio-engine bug) */
static volatile UInt32 gF4ClobberEnum = 0;         /* ...that happened while an enumeration sequence was running */
static volatile UInt32 gF4ClobberFirstMs = 0;      /* frame_ms of the first one, 0 = never */
/* --- direction A: a queued transfer could not be issued because the slot was busy --- */
static volatile UInt32 gF4PumpBlocked = 0;         /* down_pump early-returned with work queued */
static volatile UInt32 gF4PumpBlockedByBio = 0;    /* ...specifically because block I/O held the slot */
static volatile UInt32 gF4PumpMaxWaitMs = 0;       /* longest a queued request sat unissued */
static volatile UInt32 gF4PumpMaxWaitOwner = 0;    /* who held the slot at that worst wait */
/* --- the overlap window itself --- */
static volatile UInt32 gF4EnumArmedWhileBio = 0;   /* an enumeration ran while block I/O was in flight */
static volatile UInt32 gF4Snap = 0;                /* AddDrive snapshot, packed (see the exposure site) */
static volatile UInt32 gF4SnapDownQ = 0;

/* Program an endpoint QH's characteristics. isCtrl => DTC=1 (control toggle carried per-qTD: SETUP=DATA0,
 * data/status=DATA1); bulk => DTC=0 (the controller maintains this endpoint's data toggle in the QH across
 * every transfer). No HEAD bit — only the anchor heads the async ring; RL=4 = NAK-reload. Call only while
 * the QH is idle (enumeration / (re)registration), never mid-transfer. */
static void epq_program(EpQ *q, UInt32 addr, UInt32 endpt, UInt32 maxpkt, int isCtrl)
{
    UInt32 ch = EHCI_QH_DEVADDR(addr) | EHCI_QH_ENDPT(endpt) | EHCI_QH_EPS_HIGH |
                EHCI_QH_MAXLEN(maxpkt ? maxpkt : 512) | EHCI_QH_RL(4);
    if (isCtrl) ch |= EHCI_QH_DTC;
    q->qh->epChar = ehci_cpu_to_le32(ch);
    q->dtc = (UInt8)(isCtrl ? 1 : 0);
    q->addr = addr; q->endpt = endpt;
}
/* One-time static init of an endpoint QH: bind its QH + 2 qTDs to the wired page, program it, set MULT=1,
 * install an inactive tail dummy qTD with the overlay pointed at it (empty overlay => dt=0 = DATA0 initial),
 * then splice the QH into the async reclamation ring. */
/* Reset an endpoint QH to idle: empty transfer overlay with the data toggle cleared to DATA0 (dt=0) and the
 * tail dummy re-armed at td[0]. Used at init, and by BOT recovery to force the HARDWARE toggle back to DATA0
 * after a device reset. MUST be called only while the QH is quiescent (at init, or with the async schedule
 * stopped). Does NOT touch epChar or the ring link. */
static void epq_arm_idle(EpQ *q)
{
    int i;
    q->qh->curQtd = 0;
    q->td[0]->next = ehci_cpu_to_le32(EHCI_LINK_TERMINATE);
    q->td[0]->altNext = ehci_cpu_to_le32(EHCI_LINK_TERMINATE);
    q->td[0]->token = 0;                                 /* inactive tail dummy */
    for (i = 0; i < 5; i++) q->td[0]->buffer[i] = 0;
    q->qh->ovlNext = ehci_cpu_to_le32(q->tdP[0]);        /* overlay's Next-qTD -> the dummy */
    q->qh->ovlAltNext = ehci_cpu_to_le32(EHCI_LINK_TERMINATE);
    q->qh->ovlToken = 0;                                 /* inactive, dt=0 (DATA0) — EHCI 4.10 "first use" init */
    for (i = 0; i < 5; i++) q->qh->ovlBuffer[i] = 0;
    q->dummy = 0;
}
/* ★★★★ h10: park ONE IN qTD on the hub's status-change endpoint and walk away.
 * The hub NAKs it in hardware until a port changes, so this costs nothing until it matters. No IOC: we do not
 * want it entering the completion path (which is the down engine's bookkeeping, and this QH is not the down
 * engine's). We notice it the cheap way instead — by reading the token back out of the DMA page. */
static void hub_int_arm(void)
{
    int cur = gHubIntQ.dummy, nxt = (cur + 1) % QTD_POOL, j;
    ehci_qtd *td = gHubIntQ.td[cur], *dum = gHubIntQ.td[nxt];
    UInt32 tok = EHCI_QTD_STATUS_ACTIVE | EHCI_QTD_CERR(3) |
                 EHCI_QTD_BYTES(gHubIntLen) | EHCI_QTD_PID_IN;
    if (gHubStBuf) gHubStBuf[0] = 0;
    dum->next = ehci_cpu_to_le32(EHCI_LINK_TERMINATE);
    dum->altNext = ehci_cpu_to_le32(EHCI_LINK_TERMINATE); dum->token = 0;
    td->next = ehci_cpu_to_le32(gHubIntQ.tdP[nxt]);
    td->altNext = ehci_cpu_to_le32(EHCI_LINK_TERMINATE);
    td->buffer[0] = ehci_cpu_to_le32(gHubStPhys);
    for (j = 1; j < 5; j++) td->buffer[j] = 0;
    __asm__ __volatile__("eieio");
    td->token = ehci_cpu_to_le32(tok);
    __asm__ __volatile__("eieio");
    gHubIntQ.dummy = (UInt8)nxt;
    gHubIntSlot = cur;
    gHubIntArmed = 1;
}
/* h10: retire the parked qTD. Called wherever the hub is dropped — a qTD left armed on a departed hub's
 * address just errors forever, and a stale changeMask would drive visits to a hub that is gone. */
static void hub_int_stop(void)
{
    /* ⚠ SOFTWARE STATE ONLY. This is reached from service_ports, i.e. interrupt level, and epq_arm_idle on a
     * QH in the live async ring from there is the r84/r85 freeze — the audit flagged it. Clearing the flags is
     * enough: nothing re-arms with gHubIntArmed/gHubIntEp zeroed, and a qTD left pointing at a departed hub
     * simply errors and goes inactive on its own QH, disturbing nothing. Re-claiming a hub reprograms it at
     * task level anyway. */
    gHubIntArmed = 0; gHubIntEp = 0; gHubIntNeedArm = 0; gHub.changeMask = 0;
    gHubLsSeen = 0;   /* h13: the hub is gone; no port carries a low-speed streak into the next hub */
}
/* Returns the change bitmap (bit N = port N changed, bit 0 = hub itself), or 0 if nothing yet.
 * PURE MEMORY READ when idle — this is the whole point of the design. */
static UInt32 hub_int_poll(void)
{
    UInt32 tok, bits;
    if (!gHubIntArmed || !gHubStBuf) return 0;
    tok = ehci_le32_to_cpu(gHubIntQ.td[gHubIntSlot]->token);
    if (tok & EHCI_QTD_STATUS_ACTIVE) return 0;                    /* hub is NAKing — nothing to do, free */
    gHubIntArmed = 0;
    if (tok & (EHCI_QTD_STATUS_HALTED | EHCI_QTD_STATUS_XACTERR |
               EHCI_QTD_STATUS_BABBLE | EHCI_QTD_STATUS_DBERR)) {
        gHubIntErrs++;
        return 0;                                                  /* caller re-arms; a stuck endpoint just
                                                                    * means we stop hearing about changes,
                                                                    * which degrades to h6 behaviour */
    }
    bits = (UInt32)gHubStBuf[0];
    return bits ? bits : 0xFFFFFFFFUL;   /* a zero-length/empty report still means "look again" */
}
static void epq_init(EpQ *q, UInt8 *pg, UInt32 pgP, UInt32 qhOff, UInt32 tdBaseOff,
                     UInt32 addr, UInt32 endpt, UInt32 maxpkt, int isCtrl)
{
    int j;
    q->qh = (ehci_qh *)(pg + qhOff);  q->qhP = pgP + qhOff;
    for (j = 0; j < QTD_POOL; j++) {                     /* pool of qTDs, 32-byte-aligned, 0x20 apart */
        q->td[j]  = (ehci_qtd *)(pg + tdBaseOff + (UInt32)j * 0x20);
        q->tdP[j] = pgP + tdBaseOff + (UInt32)j * 0x20;
    }
    epq_program(q, addr, endpt, maxpkt, isCtrl);
    q->qh->epCaps = ehci_cpu_to_le32(EHCI_QH_MULT(1));   /* Mult=1 required — some HCs won't run a Mult=0 async QH */
    epq_arm_idle(q);                                     /* empty overlay, dt=0, dummy at td[0] */
    ehci_qh_link_async(&gSoftc, q->qh, q->qhP);          /* splice into the async ring (anchor stays ASYNCLISTADDR) */
}
/* ★★★★★★★ h92 — CANARIES AROUND EVERY TRANSFER-ENGINE DMA ALLOCATION, because the MDD's system heap
 * was found CORRUPT (free list bad, block header trashed at 01973390) with attribution UNKNOWN. The
 * timeline: h53 ran for weeks beside the CPU-temp CSM with no heap events; the corruption arrived with
 * the h91-era builds. So every h53->h91 system-heap writer is a suspect, and OUR DMA buffers are the
 * loudest class: the hardware writes into them at bus-master speed, and they sit in the system heap
 * surrounded by other people's blocks.
 *
 * Each of the three wired allocations below already over-allocates by 0x1000 for page alignment. That
 * slack is now PATTERN-FILLED and CHECKED: leading slack (raw..aligned) and trailing slack
 * (aligned+used..raw+size). A DMA overrun in either direction must cross a canary before it can reach a
 * neighbouring heap block, so:
 *   canary DEAD    -> the corruption is OURS, and the zone name says which buffer overran;
 *   canaries ALIVE across a corruption event -> our DMA engines did not do it, and the boot-time
 *                     address map (the '!! h92 MAP' lines) says whether the corpse is even near us.
 * The check runs from the TASK-LEVEL tick only (a full sweep is ~24 KB of reads every couple of
 * seconds); a violation latches ONCE per zone onto the critical ring, and the mask paints (PAINT=1). */
#define H92_PAT(i)  ((UInt8)(0xC5u ^ ((i) * 0x3Bu)))
#define H92_MAX 8
static struct { const char *nm; volatile UInt8 *a; UInt32 n; } gH92[H92_MAX];   /* guard spans */
static int gH92N = 0;
static volatile UInt32 gH92Mask = 0;         /* bit per span, latched on violation — painted + logged */
static void h92_guard(const char *nm, Ptr raw, UInt32 rawLen, volatile UInt8 *used, UInt32 usedLen)
{
    UInt32 i;
    volatile UInt8 *lead = (volatile UInt8 *)raw;
    UInt32 leadN = (UInt32)((UInt8 *)used - (UInt8 *)raw);
    volatile UInt8 *trail = used + usedLen;
    UInt32 trailN = ((UInt32)raw + rawLen) - (UInt32)trail;
    if (leadN && gH92N < H92_MAX) {
        for (i = 0; i < leadN; i++) lead[i] = H92_PAT(i);
        gH92[gH92N].nm = nm; gH92[gH92N].a = lead; gH92[gH92N].n = leadN; gH92N++;
    }
    if (trailN && gH92N < H92_MAX) {
        for (i = 0; i < trailN; i++) trail[i] = H92_PAT(i);
        gH92[gH92N].nm = nm; gH92[gH92N].a = trail; gH92[gH92N].n = trailN; gH92N++;
    }
    /* The boot-time MAP: where our block sits, so a MacsBug bad-block address can be compared. '!!' so a
     * LEVEL 1 build carries it. Task level here (xfer_init logs already). */
    ehci_os_logx("!! h92 MAP: our system-heap block — see next line for end; START", (UInt32)raw);
    ehci_os_logx("!!   h92 MAP: block END (name in the order: dmapage, downbuf, egbuf)", (UInt32)raw + rawLen);
}
static void h92_check(void)                   /* TASK TICK ONLY — reads + a latched flag + one ring line */
{
    int s; UInt32 i;
    for (s = 0; s < gH92N; s++) {
        if (gH92Mask & (1UL << s)) continue;                  /* already latched — report once */
        for (i = 0; i < gH92[s].n; i++)
            if (gH92[s].a[i] != H92_PAT(i)) {
                gH92Mask |= (1UL << s);
                ehci_os_ilogcx("!! h92 CANARY DEAD — a DMA guard zone was OVERWRITTEN; "
                               "span<<24|firstBadOff", ((UInt32)s << 24) | (i & 0xFFFFFFu));
                ehci_os_ilogcx("!!   h92 span address (compare with the MacsBug bad-block header)",
                               (UInt32)gH92[s].a);
                break;
            }
    }
}
int ehci_vhub_xfer_init(void)
{
    Ptr raw; LogicalAddress pg; LogicalToPhysicalTable t; unsigned long cnt = 1; UInt32 pgP; int i;
    UInt8 *p;
    raw = NewPtrSysClear(0x2000);
    if (!raw) { gDownInitErr = 1; return -1; }
    pg = (LogicalAddress)(((UInt32)raw + 0xFFFUL) & ~0xFFFUL);
    t.logical.address = pg; t.logical.count = 0x1000;
    if (LockMemory(pg, 0x1000) != noErr) { gDownInitErr = 2; return -1; }
    if (GetPhysical(&t, &cnt) != noErr)  { gDownInitErr = 3; return -1; }
    pgP = (UInt32)t.physical[0].address;
    gDownPg = (volatile UInt8 *)pg; gDownPgP = pgP;
    h92_guard("dmapage", raw, 0x2000, (volatile UInt8 *)pg, 0x1000);   /* h92: the QH/qTD pool page */
    /* r46: SEPARATE 20KB (5-page) wired DMA bounce so one BOT command moves up to 40 blocks — one qTD
     * spans 5 buffer pointers = 20KB. Record each page's PHYSICAL address; the pages need not be
     * physically contiguous (the 5 qTD buffer pointers handle the scatter). ~6x fewer BOT commands +
     * completions per MB — the ~0.8 MB/s bottleneck was 7-block chunks * per-command round-trips. */
    {
        Ptr raw2 = NewPtrSysClear(DOWN_BUF_MAX + 0x1000);
        LogicalAddress b;
        if (!raw2) { gDownInitErr = 4; return -1; }
        b = (LogicalAddress)(((UInt32)raw2 + 0xFFFUL) & ~0xFFFUL);
        if (LockMemory(b, DOWN_BUF_MAX) != noErr) { gDownInitErr = 5; return -1; }
        gDownBuf = (volatile UInt8 *)b;
        h92_guard("downbuf", raw2, DOWN_BUF_MAX + 0x1000, gDownBuf, DOWN_BUF_MAX);   /* h92 */
        for (i = 0; i < DOWN_DATA_PAGES; i++) {
            LogicalToPhysicalTable t2; unsigned long c2 = 1;
            t2.logical.address = (LogicalAddress)((UInt8 *)b + i * 0x1000);
            t2.logical.count   = 0x1000;
            if (GetPhysical(&t2, &c2) != noErr) { gDownInitErr = 6; return -1; }
            gDownBufPhys[i] = (UInt32)t2.physical[0].address;
        }
    }
    /* ★★★ THE ENGINE'S OWN BOUNCE. Same wire-and-translate pattern as gDownBuf above, 5 pages to match
     * epq_issue's five qTD buffer pointers exactly. Small and separate is the whole point: while block I/O
     * DMAs up to 128 KB through gDownBuf, an enumeration's descriptors and the probe's INQUIRY / READ
     * CAPACITY / block-0 read land HERE, so the two can never be pointed at the same memory again. */
    {
        Ptr raw3 = NewPtrSysClear(ENG_BUF_MAX + 0x1000);
        LogicalAddress e;
        if (!raw3) { gDownInitErr = 7; return -1; }
        e = (LogicalAddress)(((UInt32)raw3 + 0xFFFUL) & ~0xFFFUL);
        if (LockMemory(e, ENG_BUF_MAX) != noErr) { gDownInitErr = 8; return -1; }
        gEgBuf = (volatile UInt8 *)e;
        h92_guard("egbuf", raw3, ENG_BUF_MAX + 0x1000, gEgBuf, ENG_BUF_MAX);   /* h92 */
        for (i = 0; i < ENG_BUF_PAGES; i++) {
            LogicalToPhysicalTable t3; unsigned long c3 = 1;
            t3.logical.address = (LogicalAddress)((UInt8 *)e + i * 0x1000);
            t3.logical.count   = 0x1000;
            if (GetPhysical(&t3, &c3) != noErr) { gDownInitErr = 9; return -1; }
            gEgBufPhys[i] = (UInt32)t3.physical[0].address;
        }
    }
    /* r63: three resident per-endpoint QHs, each with 2 alternating qTDs, all spliced into the async ring.
     * 32-byte-aligned page layout: QHs at 0x000/0x040/0x080; qTD pairs at 0x0C0.../0x100.../0x140...
     * (r45 lesson retained: we SPLICE into the anchor ring and never touch ASYNCLISTADDR, which stays = the
     * anchor set once by ehci_hc_start; overwriting it live was the old intermittent-enum wall.) */
    p = (UInt8 *)pg;
    /* ★★ n14 DMA PAGE LAYOUT, now per-device. Device 0 keeps the exact r63 offsets so its hardware-proven
     * addresses are unchanged; each further device takes a 0x300 block after the shared buffers:
     *
     *   0x000        ctrl ep0 QH                 (SHARED — enumeration is serial)
     *   0x0C0-0x1FF  ctrl qTD pool               (SHARED)
     *   0x040/0x080  dev0 bulk-OUT / bulk-IN QH
     *   0x200-0x33F  dev0 bulk-OUT qTD pool
     *   0x340-0x47F  dev0 bulk-IN  qTD pool
     *   0x480/0x4C0  CBW / CSW DMA buffers       (SHARED — one transfer in flight; step 3)
     *   0x500 +      dev N>0: 2 QHs (0x40 each) + 2 qTD pools (0x140 each) = 0x300 per device
     *
     * A 4KB page therefore holds device 0 plus (0x1000-0x500)/0x300 = 3 more, i.e. USB_MAX_DEV up to 4.
     * The assert below is a compile-time guard so raising USB_MAX_DEV past what the page holds cannot
     * silently scribble past it — this DMA page is bus-mastered by the controller. */
    epq_init(&gCtrlQ, p, pgP, 0x000, 0x0C0, 0, 0,  64, 1);   /* control ep0 (addr set at SET_ADDRESS), DTC=1 */
    epq_init(&gDev[0].bulkQ[0], p, pgP, 0x040, 0x200, 0, 0, 512, 0);   /* dev0 bulk-OUT, DTC=0 (HW toggle) */
    epq_init(&gDev[0].bulkQ[1], p, pgP, 0x080, 0x340, 0, 0, 512, 0);   /* dev0 bulk-IN,  DTC=0 (HW toggle) */
    /* ★ h10: the hub status-change endpoint's own resident QH, qTD pool and bitmap buffer. Placed at 0xE00,
     * ABOVE the device area even at its maximum (0x500 + 3*0x300 = 0xE00 for USB_MAX_DEV = 4), so raising
     * USB_MAX_DEV later cannot scribble over it. 0xE00 QH, 0xE40..0xF7F qTD pool (10 x 0x20), 0xF80 buffer. */
    epq_init(&gHubIntQ, p, pgP, 0xE00, 0xE40, 0, 0, 64, 0);
    gHubStBuf = (volatile UInt8 *)(p + 0xF80); gHubStPhys = pgP + 0xF80;
    gCbwBuf = (volatile UInt8 *)(p + 0x480); gCbwPhys = pgP + 0x480;   /* r69: CBW DMA buffer (spare page space) */
    gCswBuf = (volatile UInt8 *)(p + 0x4C0); gCswPhys = pgP + 0x4C0;   /* r69: CSW DMA buffer (13 bytes) */
    {   /* devices 1..N-1 */
        int d;
        for (d = 0; d < USB_MAX_DEV; d++) { gDev[d].pOut = -1; gDev[d].pIn = -1; gDev[d].probedPort = -1;
                                            gDevBlkSize[d] = 512; gDevBlkCnt[d] = 0; }
        for (d = 1; d < USB_MAX_DEV; d++) {
            UInt32 base = 0x500UL + (UInt32)(d - 1) * 0x300UL;
            /* ★★★★ h14: the ceiling is the HUB QH at 0xE00, not the end of the page. This compared against
             * 0x1000, which at USB_MAX_DEV = 4 happens to be safe only by arithmetic accident (device 4 would
             * start at 0xE00 and 0xE00 + 0x300 > 0x1000 catches it) — but the thing actually at risk is
             * gHubIntQ's QH, qTD pool and status buffer occupying 0xE00..0xF80. Compare against what we are
             * protecting, so the guard states the real constraint instead of coinciding with it. */
            if (base + 0x300UL > 0xE00UL) {            /* runtime backstop for the layout note above */
                ehci_os_logx("!! h14: USB_MAX_DEV exceeds the DMA page (would overrun the hub QH at 0xE00) — "
                             "device not initialised", (UInt32)d);
                break;
            }
            epq_init(&gDev[d].bulkQ[0], p, pgP, base,          base + 0x080, 0, 0, 512, 0);
            epq_init(&gDev[d].bulkQ[1], p, pgP, base + 0x040,  base + 0x1C0, 0, 0, 512, 0);
        }
    }
    ehci_os_log("=== n14: per-DEVICE bulk QH pairs (shared ctrl ep0); bulk toggle in HARDWARE (DTC=0) ===");
    ehci_os_logx("  USB_MAX_DEV", (UInt32)USB_MAX_DEV);
    ehci_os_logx("  ctrlQH phys",  gCtrlQ.qhP);
    ehci_os_logx("  dev0 bulkOUT phys", gDev[0].bulkQ[0].qhP);
    ehci_os_logx("  dev0 bulkIN phys",  gDev[0].bulkQ[1].qhP);
    gDownReady = 1;
    return 0;
}
static void down_arm_ase(void)
{
    if (gDownAseOn) return;
    /* r45: do NOT write ASYNCLISTADDR here — it stays = the anchor (set once by ehci_hc_start), and our
     * QHs are spliced into the anchor ring in xfer_init. Overwriting it while the schedule ran was the
     * intermittent enum wall. Just ensure the async schedule is enabled (it already is, from init). */
    ehci_write32(gSoftc.opBase, EHCI_USBCMD, ehci_read32(gSoftc.opBase, EHCI_USBCMD) | EHCI_CMD_ASE);
    gDownAseOn = 1;
}
/* ==================== async-ring integrity backstop (r60, retained) ====================
 * The QH-unlink FREEZE theory was DISPROVEN (r60-r62: downRelink stayed 0; the ring is written only by us
 * via ehci_qh_link_async and the controller never touches a QH hlink, so it never actually breaks). This is
 * kept purely as a cheap, happy-path-free backstop + the gLastAnchorLink diagnostic. Now a 3-QH ring
 * (anchor -> our three QHs -> anchor): "intact" = the anchor still heads into one of our QHs. */
#define QH_LINK_PTR(v) (ehci_le32_to_cpu(v) & ~0x1FUL)   /* strip TYP(2:1)+T(0) -> the 32-byte-aligned phys ptr */
/* n14: EVERY device's QH pair is in the ring, not just the current one — these two must iterate gDev[] or a
 * healthy second device's QH would look foreign and trigger a spurious re-splice of the whole ring. */
static int is_our_qh(UInt32 p)
{
    int d;
    if (p == gCtrlQ.qhP) return 1;
    /* ★★★★ h13: THE HUB'S STATUS-CHANGE QH IS IN THIS RING TOO. h10 added a fourth kind of QH — epq_init
     * splices EVERY queue head it initialises into the async ring, gHubIntQ included — and this predicate was
     * never told. The comment above still describes "a 3-QH ring".
     * Consequence if the anchor ever heads into the hub QH: this returns 0, the backstop concludes the ring is
     * broken, and it re-splices gCtrlQ and every bulk QH — but NOT gHubIntQ, because the repair loop does not
     * know about it either. The hub QH would be dropped from the ring for good and we would go permanently
     * deaf to port changes, on a "repair" of a ring that was never broken.
     * Not observed on the h12 run (link order puts the last device's bulk QH at the head), which is exactly
     * why it is worth fixing now rather than after it fires. */
    if (gHubIntQ.qhP && p == gHubIntQ.qhP) return 1;
    for (d = 0; d < USB_MAX_DEV; d++)
        if (p == gDev[d].bulkQ[0].qhP || p == gDev[d].bulkQ[1].qhP) return 1;
    return 0;
}
static void down_relink_if_needed(void)
{
    UInt32 aLink; int d;
    if (!gDownReady || gSoftc.asyncAnchor == 0) return;
    aLink = QH_LINK_PTR(gSoftc.asyncAnchor->hlink);
    gLastAnchorLink = aLink;                        /* diag */
    if (is_our_qh(aLink)) return;                   /* anchor heads into our QHs -> intact (happy path, no writes) */
    ehci_qh_link_async(&gSoftc, gCtrlQ.qh, gCtrlQ.qhP);       /* should never fire — re-splice everything */
    for (d = 0; d < USB_MAX_DEV; d++) {
        if (gDev[d].bulkQ[0].qhP) ehci_qh_link_async(&gSoftc, gDev[d].bulkQ[0].qh, gDev[d].bulkQ[0].qhP);
        if (gDev[d].bulkQ[1].qhP) ehci_qh_link_async(&gSoftc, gDev[d].bulkQ[1].qh, gDev[d].bulkQ[1].qhP);
    }
    gDownRelink++;
}
/* Append one transfer to endpoint queue q via the DUMMY-qTD method (race-free; EHCI 4.10.2 + Linux
 * qh_append_tds). Fill the current tail dummy in place as the real qTD — body first, a barrier, then the
 * Active token written LAST as a single store — and hand off a fresh inactive dummy as the new tail. The
 * controller only ever observes the old inactive dummy (fetch -> Active=0 -> abort advance, retry next
 * pass) or a fully-formed active qTD, never a half-built one. Bulk QH (dtc=0): the qTD toggle bit is
 * ignored; the controller carries the endpoint toggle in the QH overlay. Control (dtc=1): `dt` seeds it. */
static void epq_issue(EpQ *q, UInt32 len, UInt32 pid, int useBounce, UInt32 dt)
{
    int d = q->dummy, nd = (d + 1) % QTD_POOL, i;   /* r69: cycle the qTD pool (was 2-way ping-pong) */
    ehci_qtd *cur = q->td[d];        /* the current tail dummy -> becomes the live qTD */
    ehci_qtd *nxt = q->td[nd];       /* the new tail dummy */
    UInt32 tok = EHCI_QTD_STATUS_ACTIVE | EHCI_QTD_CERR(3) | EHCI_QTD_IOC | EHCI_QTD_BYTES(len) |
                 (pid == 2 ? EHCI_QTD_PID_SETUP : (pid == 1 ? EHCI_QTD_PID_IN : EHCI_QTD_PID_OUT)) |
                 ((q->dtc && dt) ? EHCI_QTD_TOGGLE : 0u);
    nxt->next = ehci_cpu_to_le32(EHCI_LINK_TERMINATE);      /* new inactive tail dummy */
    nxt->altNext = ehci_cpu_to_le32(EHCI_LINK_TERMINATE);
    nxt->token = 0;
    cur->next = ehci_cpu_to_le32(q->tdP[nd]);              /* live qTD -> the new dummy */
    cur->altNext = ehci_cpu_to_le32(EHCI_LINK_TERMINATE);
    /* THE SPLIT: epq_issue has exactly one caller (down_issue), so it is ENGINE-ONLY and points at the
     * engine's own bounce. Block I/O builds its qTD chains by hand in bio_issue_read/bio_issue_write and
     * points them at gDownBufPhys[] — the two page tables are now disjoint memory. */
    for (i = 0; i < ENG_BUF_PAGES; i++) cur->buffer[i] = useBounce ? ehci_cpu_to_le32(gEgBufPhys[i]) : 0;
    __asm__ __volatile__("eieio");                          /* body + new dummy visible BEFORE Active */
    cur->token = ehci_cpu_to_le32(tok);                     /* ACTIVATE — the single Active-setting store, LAST */
    __asm__ __volatile__("eieio");
    q->dummy = (UInt8)nd;
    gEgQ = q; gEgTd = cur;                                  /* the qTD down_reap polls for the ENGINE slot */
}
/* ★★★★★★★ h75 — THE BOT RECOVERY'S TRANSFERS ARE ENGINE TRANSFERS THAT MUST DRIVE THE **BIO** MACHINE.
 *
 * ⚠ THIS IS THE h74 REGRESSION, AND IT WAS MINE. All six recovery transfers (the Bulk-Only Reset, the two
 * CLEAR_FEATURE(HALT)s and their status stages) are CONTROL transfers issued through down_submit, so the
 * split routed them to the ENGINE slot — while bio_advance, which drives bio_recover_advance, is now called
 * ONLY from the block-I/O slot. The sequence therefore STARTED AND NEVER ADVANCED: gBioPhase stuck at
 * REC_RESET_SETUP, bio_kick's `if (gBioPhase != 0) return` never let another request start, every File
 * Manager read queued and never completed, and the Finder sat on a wristwatch cursor for ever.
 * ★ That is the m26 run-2 photograph exactly — desktop loaded, watch cursor, no recovery after minutes.
 * ★ And it is why the single-drive control run PASSED: gDownErr and gDownTimeouts were both 0, so the
 *   recovery path was never entered at all. The control run could not have caught this.
 * ⇒ SAME SHAPE AS EVERY OTHER BUG IN THIS DRIVER'S HISTORY: I moved WHERE completions are routed without
 * moving what DEPENDS on that routing. h7's DEV_ADDR(0) and n19's discarded `dev` argument, once more.
 *
 * ⚠ AND WHY THE OBVIOUS ONE-LINER IS WRONG. `if (gBioPhase >= REC_BASE) bio_advance(status);` in the engine
 * branch would let ANY control completion advance the recovery machine — including an enumeration's
 * GET_DESCRIPTOR. That is defect 2 from CTL-BULK-SPLIT.md reborn, and the split makes it MORE likely, not
 * less, because engine and block I/O now genuinely overlap. So the REQUEST carries the ownership: only a
 * transfer we ourselves submitted as part of the recovery may advance the recovery. */
static void down_submit(void *pipe, void *upp, volatile UInt8 *buf, UInt32 addr, UInt32 len, UInt32 pid, int bulkEp);
static void down_submit_recov(volatile UInt8 *buf, UInt32 addr, UInt32 len, UInt32 pid)
{
    UInt32 h0 = gDownQHead;
    down_submit(0, 0, buf, addr, len, pid, -1);          /* -1 = control (ep0), as every recovery phase is */
    /* Tag the entry that was ACTUALLY queued. down_submit can refuse (oversize) or drop (queue full)
     * without advancing the head, and tagging a stale slot would hand the recovery someone else's
     * completion — so only tag when something really was enqueued. */
    if (gDownQHead != h0) gDownQ[h0 % DOWNQ_N].forBio = 1;
}
static void down_issue(volatile DownReq *r)
{
    EpQ *q; UInt32 dt, i; int useBounce = 0;
    down_arm_ase();
    if (r->bulkEp < 0) {                                    /* CONTROL (ep0): SETUP=DATA0, data/status=DATA1 */
        q = &gCtrlQ;
        if (q->addr != r->addr) epq_program(q, r->addr, 0, 64, 1);   /* (re)point ep0 at this device addr (QH idle) */
        dt = (r->pid == 2) ? 0u : 1u;
        gEgNpkt = 1;
    } else {                                                /* BULK: the endpoint's OWN resident QH (HW toggle) */
        UInt32 m = gBulkEP[r->bulkEp].maxpkt ? gBulkEP[r->bulkEp].maxpkt : 512;
        /* n14 step 3: route to the QH pair of the device that OWNS this endpoint, not to gCur's. With one
         * device this is the same queue head as before; with two it is what keeps their hardware toggles
         * separate. gEgDev records the owner so down_reap credits the completion to the right device. */
        { UInt8 od = gBulkEP[r->bulkEp].dev;
          if (od >= USB_MAX_DEV) od = 0;
          gEgDev = od;
          q = &gDev[od].bulkQ[gBulkEP[r->bulkEp].dirIn ? 1 : 0]; }
        dt = 0;                                             /* ignored for bulk (DTC=0 — HW owns the toggle) */
        gEgNpkt = (r->len + m - 1) / m; if (gEgNpkt == 0) gEgNpkt = 1;
    }
    /* THE SPLIT: stage into the ENGINE's bounce. down_submit has already bounded r->len and r->obiglen to
     * ENG_BUF_MAX, so these copies cannot overrun; the min() is kept as belt-and-braces. */
    if (r->obig) { UInt32 nn = (r->obiglen > ENG_BUF_MAX) ? (UInt32)ENG_BUF_MAX : r->obiglen;
                   for (i = 0; i < nn; i++) gEgBuf[i] = ((volatile UInt8 *)r->obig)[i]; useBounce = 1; }
    else if (r->pid == 2 || (r->pid == 0 && r->olen)) { for (i = 0; i < r->olen; i++) gEgBuf[i] = r->obuf[i]; useBounce = 1; }
    else if (r->pid == 1 && r->len) { useBounce = 1; }        /* IN: HC DMAs into the engine bounce */
    gEgUPP = r->upp; gEgPipe = r->pipe; gEgDest = r->dest; gEgLen = r->len; gEgIsIn = (r->pid == 1);
    gEgBulkEp = r->bulkEp;
    gEgLastAddr = r->addr; gEgLastPid = r->pid;             /* r36 diag: name the in-flight downstream xfer */
    gEgForBio = r->forBio;                                  /* h75: whose state machine this completion drives */
    /* THE SPLIT: arm the ENGINE's own watchdog. Defect 3 in the doc was that gDpArmTick belonged to
     * whichever transfer armed last, so an orphaned one was never timed out — which is why gDownTimeouts
     * read 0 through the whole h16-h18 hunt. Each slot now times itself. */
    gEgArmTick = *(volatile UInt32 *)0x016AUL; gEgBusy = 1;
    epq_issue(q, r->len, r->pid, useBounce, dt);
}
/* ★ n12: return the downstream engine to a clean idle. Called when we abandon a device (a failed
 * enumeration, or parking a port we cannot drive) so the next device is not blocked by a transfer that can
 * never retire. Before this, as_fail left gDpBusy latched: down_pump() bails on `if (gDpBusy) return`, so a
 * single abandoned in-flight transfer stopped EVERY later device from being talked to at all — which is what
 * "USB stops working until a reboot" was. Flag stores only, so it is safe from the SIH where as_fail runs.
 * The queued-but-unissued requests are dropped too; anything abandoned mid-sequence has no owner left. */
static void dp_engine_idle(void)
{
    gDpBusy = 0; gDpIsIn = 0; gDpBulkEp = -1;
    gDpUPP = 0; gDpPipe = 0; gDpDest = 0; gDpLen = 0; gDpMeasured = 0;
    /* THE SPLIT: this is the ABANDON path (a failed enumeration, or parking a port), so it must clear BOTH
     * slots. Leaving the engine slot latched here would reproduce the exact bug this function was written
     * to cure — down_pump bails on a busy slot, so one abandoned engine transfer would stop us talking to
     * every later device, which is what "USB stops working until a reboot" was. */
    gEgBusy = 0; gEgIsIn = 0; gEgBulkEp = -1;
    gEgUPP = 0; gEgPipe = 0; gEgDest = 0; gEgLen = 0;
    gDownQTail = gDownQHead;                  /* drop anything queued for the abandoned device */
}
static void down_pump(void)
{
    volatile DownReq *r;
    /* ★★★ f4 DIRECTION A — THE STARVATION, MEASURED AT THE ONE PLACE IT HAPPENS.
     * `if (gDpBusy) return;` with work queued IS the h16/h17 mechanism (docs/CTL-BULK-SPLIT.md): during a
     * multi-chunk mount read, down_reap -> bio_advance -> bio_start_chunk -> bio_issue_read re-arms the slot
     * on every completion BEFORE we get a turn, so the queued request is never issued and the probe dies on
     * bot_step's 800 ms cap. Count it, and measure how long the head request has actually been waiting —
     * a large gF4PumpMaxWaitMs beside a failed probe names this outright, and zero refutes it.
     * ⚠ Reads only; the early return below is unchanged. */
    /* ★★★ THE SPLIT CHANGES THIS TEST, AND THAT IS THE WHOLE POINT OF THE CHANGE.
     * This used to read `if (gDpBusy) return;` — block I/O's slot — so during a multi-chunk mount read,
     * where every completion re-arms the next chunk before we get a turn, a queued ENGINE transfer was
     * never issued at all and the probe died on bot_step's 800 ms cap (f4 direction A, and the verified
     * h16/h17 root cause). We now gate on the ENGINE's own slot: block I/O being busy is no longer any of
     * this function's business, because it can no longer be in the way.
     * ⚠ The f4 direction-A counters are KEPT, deliberately. They should now only ever record the engine
     * waiting on ITSELF (which is correct and expected — engine transfers are serial by design). If
     * gF4PumpBlockedByBio is ever non-zero again, the split has been undone somewhere. */
    if (gEgBusy) {
        if (gDownQHead != gDownQTail) {
            UInt32 w = frame_ms() - gDownQ[gDownQTail % DOWNQ_N].qms;
            gF4PumpBlocked++;
            if (gF4Owner == F4_OWNER_BIO) gF4PumpBlockedByBio++;
            if (w > gF4PumpMaxWaitMs && w < 0x40000000UL) {   /* guard a wrapped/unstamped sample */
                gF4PumpMaxWaitMs = w; gF4PumpMaxWaitOwner = gF4Owner;
            }
        }
        return;
    }
    if (gDownQHead == gDownQTail) return;
    /* ⚠ h17's CONTROL-PROMOTION SCAN LIVED HERE AND IS REVERTED — it was inert and unproven.
     * The theory was that an enumeration's control phases were queued behind the copy's bulk requests and timed
     * out on ctl_step's 800 ms cap. The build shipped with gCtlPromoted counting the promotions, and on hardware
     * it read ZERO: a control request was never once queued behind bulk, so promotion could not have been the
     * answer. Removed rather than kept, because it is a queue scan plus struct copies on the interrupt path
     * earning nothing, and the CTL/BULK split makes it moot — control gets its own in-flight slot and never
     * waits behind bulk at all.
     * ★ Recorded because the discriminator is the lesson: shipping a counter that could REFUTE my own fix is
     * what stopped a second wrong theory being built on top of the first. */
    r = &gDownQ[gDownQTail % DOWNQ_N];
    /* ★ n12: never issue a BULK transfer against an endpoint we do not have. gBulkEP[-1] is an
     * out-of-bounds read and the resulting transfer can never retire, so this used to latch gDpBusy
     * forever. A device abandoned before endpoint registration (epIn/epOut = 0xff) is exactly that case. */
    if (r->bulkEp >= 0 && (r->bulkEp >= NBULK || !gBulkEP[r->bulkEp].used)) {
        ehci_os_ilogx("!! n12: refusing a bulk transfer on an unregistered endpoint (dropped)",
                      (UInt32)r->bulkEp);
        gDownQTail++;                          /* discard it; do NOT arm an uncompletable transfer */
        return;
    }
    if (r->dead) {                             /* h94: the funnel already delivered its substitute completion */
        gDownQTail++;
        return;
    }
    /* h94, the h81 lesson applied to the USL side: only the issue site knows the truth at the moment it
     * matters. A request enqueued before the yank must not arm hardware for an absent device — and more to
     * the point, its completion pointers may already be freed. Bulk names its owner exactly; control is
     * keyed off the dead-addr ring (positive knowledge only — never off "unknown address", which is what
     * every Apple-era enumeration looks like). */
    {   int gone = 0;
        if (r->bulkEp >= 0) { UInt8 od = gBulkEP[r->bulkEp].dev;
                              if (od < USB_MAX_DEV && !gDev[od].inUse) gone = 1; }
        else if (addr_in_dead(r->addr)) gone = 1;
        if (gone) {
            retire_enqueue((void *)r->upp, (void *)r->pipe, r->bulkEp < 0);
            gH94Refused++;
            ehci_os_ilogx("!! h94: refusing an issue for a rude-removed device (addr)", r->addr);
            gDownQTail++;
            return;
        }
    }
    down_issue(r);
    gDownQTail++;
}
/* Driver-owned staging for the 8-byte control SETUP — see the copy in down_submit below. */
static UInt8 gCtlSetupStage[8];
static void down_submit(void *pipe, void *upp, volatile UInt8 *buf, UInt32 addr, UInt32 len, UInt32 pid, int bulkEp)
{
    UInt32 h, depth; volatile DownReq *r;
    if (!gDownReady) return;
    h = gDownQHead; depth = h - gDownQTail;
    if (depth >= DOWNQ_N) { gDownQDrop++; return; }
    r = &gDownQ[h % DOWNQ_N];
    /* ★★★ THE LOUD REFUSAL THE DOC ASKED FOR ("true by assertion rather than by luck").
     * ⚠ THIS WAS A LATENT BUG, not merely tidiness: epq_issue maps FIVE page pointers and this line capped
     * at DOWN_BUF_MAX = 128 KB, so an engine transfer above 20 KB would have DMA'd off the end of its own
     * mapping. Unreachable today (control <= 64 B, probe bulk <= 512 B, gPB is exactly 20 KB) but it was one
     * caller away, and ehci_vhub_bulk_xfer will hand us whatever the USL asks for. Refuse it, say so, and
     * count it — a silent truncation here would corrupt a transfer rather than fail it. */
    if (len > ENG_BUF_MAX) {
        gEgOversize++;
        ehci_os_ilogx("!! SPLIT: engine transfer larger than ENG_BUF_MAX refused (epq_issue maps only "
                      "ENG_BUF_PAGES pages; a silent truncation here would corrupt it); bytes", len);
        return;
    }
    r->upp = upp; r->pipe = pipe; r->dest = (void *)buf; r->addr = addr; r->pid = (UInt8)pid; r->len = len; r->olen = 0; r->obig = 0; r->bulkEp = bulkEp;
    r->qms = frame_ms();      /* f4: when this request joined the queue — down_pump measures the wait */
    r->forBio = 0;            /* h75: plain engine traffic; down_submit_recov overrides this */
    if (pid == 2) { int i; for (i = 0; i < 8; i++) r->obuf[i] = buf[i]; r->olen = 8; r->len = 8; }
    /* ★ Restored 2026-08-01 from the disassembly. For CONTROL transfers (bulkEp < 0) the 8-byte SETUP is
     * ALSO staged into a driver-owned static buffer. The caller's buffer may be a File-Manager or
     * Apple-USL buffer, and bus-mastering directly out of one of those is the documented way to corrupt a
     * mount — always DMA from memory we own. Cheap, and it makes the SETUP independent of the caller's
     * buffer lifetime across the queue delay. */
    if (bulkEp < 0) { int i; for (i = 0; i < 8; i++) gCtlSetupStage[i] = buf[i]; }
    else if (pid == 0 && len > 64) { r->obig = (void *)buf; r->obiglen = len; }                         /* large OUT: write data (DMA'd from src) */
    else if (pid == 0 && len) { UInt32 i; for (i = 0; i < len; i++) r->obuf[i] = buf[i]; r->olen = len; } /* small OUT: the 31-byte CBW */
    gDownQHead = h + 1;
}
/* ==================== r34: ASYNC block I/O engine (the resident driver's I/O model) ====================
 * The block driver's kRead/kWrite ENQUEUE a request here and return kIOBusyStatus — they NEVER block,
 * so the File Manager is never held and the shared engine is never re-entered (that was the Finder
 * freeze: blocking on the Finder's async reads let a 2nd I/O clobber the in-flight transfer). This
 * state machine runs each request's BOT (CBW->data->CSW, chunked <=7 blocks) driven by down_reap
 * completions (the heartbeat), then calls IOCommandIsComplete(cmdID). Requests are SERIALIZED (one BOT
 * at a time); extra async requests just queue. This is exactly the interrupt-driven model the resident
 * driver needs, tested now in the app scaffold. Transfers go via pb_* (complUPP==0) so the r32 Apple
 * fence never touches them. */
#define BIOQ_N 16
typedef struct { IOCommandID cmdID; UInt32 lba, count, reqBytes; UInt8 *buf; long *actCount; UInt8 isWrite;
                 UInt32 submitLba;                 /* v40: the ORIGINAL LBA at submit (r->lba advances per chunk) — lets the
                                                    * failure dump tell FM-gave-garbage (submitLba bad) from corrupted-after-submit. */
                 UInt8 dev;                        /* ★ n14 step 2: which gDev[] slot this request is for.
                                                    * The device travels WITH THE WORK. That is the whole
                                                    * design: bio_advance runs at interrupt level, so it must
                                                    * not consult any mutable global to learn whose transfer
                                                    * it is servicing. See docs/MULTI-DEVICE-DESIGN.md. */
                 volatile UInt8 ready; } BioReq;   /* r+: 1 once fully filled; consumer must not touch a slot until set */
static BioReq gBioQ[BIOQ_N];
static volatile int    gInSubmit = 0;                       /* r+: re-entrancy detector for the enqueue race */
static volatile UInt32 gSubmitReentry = 0, gSubmitMaxDepth = 0;
/* v36: suspicious WRITE capture — never-wraps, so the FM-race smoking gun survives the whole folder copy
 * regardless of the 512-entry 'Ucsl' ring. fl bit0 = the write was submitted from inside our completion
 * (the FM re-issued it nested = the re-entrancy the source model predicts); bit1 = its LBA is off the
 * device (a clobbered File-Manager Params offset reached us). */
static volatile UInt32 gWrTotal = 0, gSuspN = 0;
static volatile UInt32 gSuspLba[16], gSuspCnt[16], gSuspFlags[16];
static volatile UInt32 gBioHead = 0, gBioTail = 0; /* ring: [tail]=in-flight, head=enqueue. r48: volatile — the ring is now the ONE cross-context hand-off: producer = ehci_usb_submit (TASK level), consumer = bio_advance (INTERRUPT level). Must not be register-cached across that boundary. */
static int    gBioPhase = 0;                   /* 0 idle; 1 CBW issued; 2 data issued; 3 CSW issued */
#define BIO_PH_PREREAD 5                        /* r70: a whole read command is pre-queued, terminal = CSW (defined here so down_reap can time it) */
#define BIO_PH_PREWRITE 6                       /* r77: a whole write command is pre-queued (OUT:[CBW->data], IN:[CSW]), terminal = CSW */
static UInt32 gBioChunk = 0;                    /* blocks in the current READ(10)/WRITE(10) */
static long   gBioResult = 0;
static void bio_advance(long status);           /* forward: driven from down_reap on each completion (r57: takes the xfer status) */

/* r39 DIAGNOSTIC: snapshot the EHCI controller + shared-QH state at the instant a downstream transfer
 * TIMES OUT (qTD stayed Active past the watchdog). r38 proved the port is ENABLED yet transfers never
 * execute — this reveals WHY: is the controller HALTED (USBSTS bit12)? is the async schedule actually
 * RUNNING (USBSTS.ASS bit15)? is ASE (USBCMD bit5) even set? does ASYNCLISTADDR still point at our QH,
 * and is the QH overlay Active/Halted? All interrupt-safe register/memory reads; drained by uim23. */
static volatile UInt32 gToSeq = 0, gToCmd = 0, gToSts = 0, gToAsync = 0, gToQhP = 0;
static volatile UInt32 gToQhEpChar = 0, gToQhCurQtd = 0, gToQhOvlTok = 0, gToQtdTok = 0;
/* THE SPLIT: takes the timing-out slot's QH/qTD, because there are now two of them and a snapshot of the
 * wrong one is worse than none — it would describe a healthy transfer while a different one wedged. */
static void capture_timeout_state(EpQ *q, ehci_qtd *td)
{
    gToCmd      = ehci_read32(gSoftc.opBase, EHCI_USBCMD);
    gToSts      = ehci_read32(gSoftc.opBase, EHCI_USBSTS);
    gToAsync    = ehci_read32(gSoftc.opBase, EHCI_ASYNCLISTADDR);
    gToQhP      = q ? q->qhP : 0;                                        /* r63: the ACTIVE endpoint QH/qTD */
    gToQhEpChar = q ? ehci_le32_to_cpu(q->qh->epChar) : 0;
    gToQhCurQtd = q ? ehci_le32_to_cpu(q->qh->curQtd) : 0;
    gToQhOvlTok = q ? ehci_le32_to_cpu(q->qh->ovlToken) : 0;
    gToQtdTok   = td ? ehci_le32_to_cpu(td->token) : 0;
    __asm__ __volatile__("eieio");                       /* publish payload before the seq bump */
    gToSeq++;
}
UInt32 ehci_vhub_timeout_state(UInt32 *cmd, UInt32 *sts, UInt32 *async, UInt32 *qhP,
                               UInt32 *epChar, UInt32 *curQtd, UInt32 *ovlTok, UInt32 *qtdTok)
{
    if (cmd) *cmd = gToCmd; if (sts) *sts = gToSts; if (async) *async = gToAsync; if (qhP) *qhP = gToQhP;
    if (epChar) *epChar = gToQhEpChar; if (curQtd) *curQtd = gToQhCurQtd;
    if (ovlTok) *ovlTok = gToQhOvlTok; if (qtdTok) *qtdTok = gToQtdTok;
    return gToSeq;
}

/* r53 RELIABILITY FIX — the root cause of the whole "disk error"/"problem with the disk" saga. After a
 * WRITE, cheap USB flash (the SanDisk) intermittently NAKs the CSW-status IN for MULTIPLE SECONDS while it
 * does internal garbage-collection. The r39 XFER TIMEOUT snapshot proved it: qTD ACTIVE, CERR=3 (no errors),
 * NOT halted, 13 bytes (=the CSW) outstanding, schedule running on our QH — i.e. a BUSY device, not a fault.
 * The old 200-tick (~1.6s) watchdog fired anyway → we falsely completed the write with -6640/-36 → Finder
 * "cannot be written, disk error"; and since the write ACTUALLY SUCCEEDED on the device while we told the
 * File Mgr it failed, the volume's bookkeeping diverged → "problem with the disk" (metadata corruption).
 * FIX: be patient with a NAKing device (Apple's USB stack uses ~30s command timeouts). Real faults still
 * fail FAST via the HALTED/XACTERR/BABBLE/DBERR branch below — only pure device-busy waits this long. */
/* r54: r53's tick-counter watchdog (200->4096) cut timeouts 54->3 — right direction, but gVhubTick's rate
 * is variable, so switch to the OS 60.15Hz Ticks low-mem global = a RELIABLE wall clock (interrupt-safe to
 * read), wait 60s, and MEASURE the worst-case stall (gMaxStallTicks) so we stop guessing. A CSW-NAK write
 * has ALREADY landed on the device, so waiting is always correct; failing = the false-failure + corruption. */
#define TICKS_NOW (*(volatile UInt32 *)0x016AUL)   /* Ticks: 60.15Hz since boot */
#define DOWN_WATCHDOG_TICKS (10UL * 60UL)          /* r64 (A2): 10s. r63's per-endpoint HW toggle killed the wedge — a healthy run's worst stall was 50ms — so this is now a genuine-fault backstop, not a hot path. 10s is ~200x the observed max (won't false-fire on this device's flash GC, which the research warns against resetting) yet recovers a true wedge far faster than the 30s r62 test value. A timeout now triggers the CORRECT one-shot BOT reset (below), not a false-failure. Tunable if a slower device needs more patience. */
static volatile UInt32 gMaxStallTicks = 0;         /* longest observed transfer stall, in 60Hz ticks */
/* ★★★★★★ THE SPLIT: ONE poll implementation, used by BOTH slots.
 *
 * Returns 1 when the slot's transfer has finished (status/actual filled in), 0 while it is still running.
 * ⚠ WRITTEN AS A SHARED HELPER ON PURPOSE. The alternative — copying this logic per slot — would have
 * duplicated the h57 QH-overlay-halt check, the r53/r54 patience watchdog and the short-transfer accounting,
 * and every one of those took a hardware cycle to get right. Two copies drift; the second copy is where the
 * next four-run hunt would come from. The routing that genuinely DIFFERS between slots stays in down_reap.
 * `len` is the slot's requested length, used only to turn the residue into a delivered byte count. */
static int slot_poll(EpQ *q, ehci_qtd *td, UInt32 armTick, UInt32 len, long *statusOut, UInt32 *actualOut)
{
    UInt32 tok; long status; UInt32 actual = 0;
    if (!td) return 0;
    tok = ehci_le32_to_cpu(td->token);      /* r63: poll the qTD activated on the endpoint's own QH */
    if (tok & EHCI_QTD_STATUS_ACTIVE) {
        /* h57 (verbatim reasoning, see the long note formerly at this site): a transaction error is retried
         * CERR times and then the controller HALTS THE QUEUE HEAD. The halt lands in the QH OVERLAY, not in
         * the qTD we activated, and a halted QH executes nothing queued behind it — so our qTD sits ACTIVE
         * for ever with a pristine token (CERR 3, no error bits) and polling it alone can never see this.
         * Measured m8/h56: three control failures, every qTD token ACTIVE/CERR 3/clean, overlay reading
         * 0x80088148 = HALTED|XACTERR with CERR exhausted to 0.
         * ⚠ epq_arm_idle from interrupt level is legal HERE AND NOWHERE ELSE: the gate is the HALTED bit
         * read from hardware, and a halted QH is by definition one the controller has abandoned. Do not
         * relax that gate and do not lift this call elsewhere. */
        UInt32 ovl = q ? ehci_le32_to_cpu(q->qh->ovlToken) : 0;
        if (ovl & EHCI_QTD_STATUS_HALTED) {
            gQhHalted++;
            ehci_os_ilogx("!! h57 QH HALTED in the OVERLAY while our qTD still reads ACTIVE — failing "
                          "fast instead of waiting out the cap; ovlToken", ovl);
            ehci_os_ilogx("!! h57   qTD token (pristine — this is why we never saw it)", tok);
            epq_arm_idle(q);         /* clear the halt: safe ONLY because HALTED is set (see above) */
            gQhUnhalted++;
            ehci_os_ilog("!! h57   overlay re-initialised — the NEXT attempt starts on a clean endpoint "
                         "instead of being dead on arrival");
            gDownErr++; status = -6640L;
        } else {
            UInt32 el = TICKS_NOW - armTick;
            if (el > gMaxStallTicks) gMaxStallTicks = el;   /* r54: track the device's worst-case GC pause */
            if (el > DOWN_WATCHDOG_TICKS) { gDownTimeouts++; capture_timeout_state(q, td); status = -6640L; }
            else return 0;                                  /* still running, and inside its patience budget */
        }
    } else if (tok & (EHCI_QTD_STATUS_HALTED | EHCI_QTD_STATUS_XACTERR | EHCI_QTD_STATUS_BABBLE | EHCI_QTD_STATUS_DBERR)) {
        gDownErr++; status = -6640L;
    } else {
        UInt32 resid = EHCI_QTD_BYTES_GET(tok);
        status = 0; gDownDone++;
        actual = (resid <= len) ? (len - resid) : len;
    }
    *statusOut = status; *actualOut = actual;
    return 1;
}

static void down_reap(void)
{
    long status; UInt32 actual = 0;

    /* ---------------- ENGINE slot: control + probe/enumeration bulk ---------------- */
    if (gEgBusy && slot_poll(gEgQ, gEgTd, gEgArmTick, gEgLen, &status, &actual)) {
        /* h94 ORPHAN GATE (the backstop; MUST never fire — the disconnect funnel nulls these pointers
         * first). If this transfer's device has been rude-removed and the funnel somehow did not run,
         * the client buffer and pipe below may already be FREED by the USL's teardown; writing through
         * them is the h93-era heap scribble. Suppress the client-memory writes, keep every piece of our
         * own bookkeeping, and count the save so the periodic dump exposes the funnel's hole. */
        {   int orphan = 0;
            if (gEgBulkEp >= 0) { if (gEgDev < USB_MAX_DEV && !gDev[gEgDev].inUse) orphan = 1; }
            else if (addr_in_dead(gEgLastAddr)) orphan = 1;
            if (orphan && (gEgUPP || gEgDest)) {
                gH94Orphaned++;
                ehci_os_ilogx("!! h94 ORPHAN GATE fired — funnel hole, client writes suppressed; addr",
                              gEgLastAddr);
                gEgUPP = 0; gEgPipe = 0; gEgDest = 0;
            }
        }
        if (gEgIsIn && gEgLen) {                       /* copy IN data out of the ENGINE's own bounce */
            volatile UInt8 *dd = (volatile UInt8 *)gEgDest; UInt32 i;
            if (dd) for (i = 0; i < actual; i++) dd[i] = gEgBuf[i];
        }
        if (gEgBulkEp >= 0) {                          /* probe/enumeration BULK completion */
            UInt32 i; gBulkLastStat = status;
            if (status == 0) gBulkDoneN++; else gBulkErrN++;      /* aggregate: any bulk activity */
            if (status == 0) gEgBulkDoneN++; else gEgBulkErrN++;  /* ENGINE-only: what pb_ready/pb_failed watch */
            for (i = 0; i < 16; i++) gLastData[i] = gEgBuf[i];
        } else {                                       /* h13: a CONTROL completion — feed ctl_step */
            gCtlStat = status; gCtlActual = actual;
            __asm__ __volatile__("eieio");
            gCtlSeq++;
        }
        gDpLastStat = status; gDpActual = actual;      /* shared "last downstream reap" diagnostics */
        gEgBusy = 0;
        if (gEgUPP) {
            if (gEgBulkEp >= 0) {   /* v20: defer bulk completion to task level (the MDD inline call
                                     * intermittently deadlocked Apple's bulk UPP at interrupt level) */
                UInt32 hh = gComplHead;
                if ((hh - gComplTail) < NCOMPL) {
                    gComplQ[hh & (NCOMPL - 1u)].upp = (void *)gEgUPP;
                    gComplQ[hh & (NCOMPL - 1u)].pipe = (void *)gEgPipe;
                    gComplQ[hh & (NCOMPL - 1u)].status = status;
                    gComplQ[hh & (NCOMPL - 1u)].actual = actual;
                    gComplQ[hh & (NCOMPL - 1u)].twoArg = 0;        /* h94: normal deferred bulk = 3-arg */
                    gComplHead = hh + 1u;
                } else gComplDrop++;
            } else
                ((ehci_usl_complete)gEgUPP)((void *)gEgPipe, (unsigned long)status);
        }
        /* ⚠ NOTE WHAT IS **NOT** HERE: an unconditional bio_advance. That is defect 2 from
         * docs/CTL-BULK-SPLIT.md — `if (gBioPhase && !gDpUPP) bio_advance(status)` used to fire for CONTROL
         * completions too, because our enumeration control transfers pass upp = 0, so a control completion
         * could advance the BOT state machine with a status that had nothing to do with it.
         * ★★★★★★ h75 — BUT EXACTLY ONE CLASS OF ENGINE COMPLETION *MUST* DRIVE IT: the BOT recovery's own
         * control transfers, which are submitted through down_submit_recov and carry forBio. Without this
         * the recovery starts and never advances, the bio ring never drains, and the Finder hangs on a
         * wristwatch for ever — the m26 run-2 failure. The tag is what keeps this precise: an enumeration's
         * control completion still cannot touch the recovery, which is what defect 2 was about. */
        if (gEgForBio && gBioPhase >= REC_BASE) {
            gEgForBio = 0;            /* consume it: this completion belongs to exactly one phase */
            gEgRecovCompl++;
            bio_advance(status);
        } else {
            gEgForBio = 0;
        }
    }

    /* ---------------- BLOCK I/O slot: bio_issue_read / bio_issue_write ---------------- */
    if (gDpBusy && slot_poll(gDpQ, gDpTd, gDpArmTick, gDpLen, &status, &actual)) {
        if (status == 0) {
            if (gDpMeasured) {   /* r67: data-phase rate — FRINDEX delta, read BEFORE the copy */
                UInt32 fr = ehci_read32(gSoftc.opBase, EHCI_FRINDEX) & 0x3FFFUL;
                gDataFrames += (fr - gDataFr0) & 0x3FFFUL; gDataBytes += gDpLen; gDpMeasured = 0;
            } else if (gBioPhase == BIO_PH_PREREAD) {   /* r74: on-the-wire per-command READ time */
                UInt32 fr = ehci_read32(gSoftc.opBase, EHCI_FRINDEX) & 0x3FFFUL;
                gDataFrames += (fr - gDataFr0) & 0x3FFFUL; gDataBytes += gDpLen;
            }
        }
        if (gDpIsIn && gDpLen) {
            volatile UInt8 *dd = (volatile UInt8 *)gDpDest; UInt32 i;
            if (dd) for (i = 0; i < actual; i++) dd[i] = gDownBuf[i];
        }
        if (gDpBulkEp >= 0) {
            UInt32 i; gBulkLastStat = status;
            if (status == 0) gBulkDoneN++; else gBulkErrN++;
            for (i = 0; i < 16; i++) gLastData[i] = gDownBuf[i];
        }
        gDpLastStat = status; gDpActual = actual;
        gDpBusy = 0;
        if (gBioPhase) bio_advance(status);   /* r34/r57: advance the async block-I/O state machine */
    }
    down_pump();
}


/* r36 RELIABILITY: downstream-transfer engine health, read at task level from uim23. If err/timeouts
 * climb WHILE the SanDisk interface is being set up, OUR engine dropped a control transfer and the
 * composite driver bailed on that (fixable here); if they stay 0 through the failure, the transfers
 * all succeeded and the Apple handoff dies on its own -> pivot to self-claiming the bulk endpoints. */
void ehci_vhub_down_stats(UInt32 *done, UInt32 *err, UInt32 *timeouts, UInt32 *qdrop,
                          UInt32 *lastAddr, UInt32 *lastPid, long *lastStat)
{
    if (done)     *done     = gDownDone;
    if (err)      *err      = gDownErr;
    if (timeouts) *timeouts = gDownTimeouts;
    if (qdrop)    *qdrop    = gDownQDrop;
    if (lastAddr) *lastAddr = gDpLastAddr;
    if (lastPid)  *lastPid  = gDpLastPid;
    if (lastStat) *lastStat = gDpLastStat;
}

/* ==================== dispatch slots 6/7: bulk (mass storage) ====================
 * Slot 6 CreateBulkEndpoint just REGISTERS the endpoint (addr, endpt, dir, maxpkt) — the USL routes
 * slot-7 transfers purely by (addr, endpt) with no opaque handle, so a stub cannot work (RE-confirmed).
 * Slot 7 BulkTransfer enqueues onto the serialized FIFO with a valid endpoint index; down_issue routes to
 * the endpoint's OWN resident QH (HW toggle, DTC=0); down_reap fires the 3-arg completion. r63: registering
 * an endpoint also programs its dedicated QH (gDev[0].bulkQ[dirIn]); the QH's overlay was init'd to DATA0. */
/* r91 volatile: reconnect_reset writes these from the ISR-driven hub re-enum context; the task-level
 * self-probe / bulk_xfer read them. Non-volatile => readers got STALE values (the r86-r90 "reset doesn't
 * stick" bug). Boot worked only because its enumeration is task-level (same context). */
static volatile int    gMountedOnce = 0;    /* r87: set at the first self-probe completion — arms the reconnect logic */
static volatile int    gReprobe = 0;        /* r88: reconnect => state 0 takes over ASSERTIVELY (skip the passive park-wait) */
static volatile int    gBurst = 0;          /* r90: log the next N selfprobe_tick entries after a reconnect (diagnostic) */
static volatile int    gLoopDbg = 0;        /* r94: trace the app idle-loop for N passes after a reconnect — proves whether
                                             * the loop REACHES the tickFn call post-reconnect or stalls earlier (ExpertIdleTask/USL) */
/* n14: gCurAddr, gProbedPort and gDev[0].pstate are now per-device fields of gDev[] — see the UsbDev block above.
 * n13's reasoning for owning the probed port explicitly is recorded there. */
static volatile UInt32 gSecondDevMask = 0;  /* n13: ports where a 2nd device arrived while one was mounted;
                                             * set at interrupt level, logged at task level (never log here) */
static volatile int gRearmDev = 0;          /* n19 step 3: slot whose device was just pulled */
static volatile UInt32 gNewDevMask = 0;     /* n15: slots that finished enumerating, for the task-level log */
static volatile UInt32 gHubPortGoneMask = 0; /* h7: DOWNSTREAM hub ports whose device was unplugged.
                                             * Interrupt-set, task-logged, so a removal behind the hub is
                                             * visible rather than inferred from a volume disappearing. */
static volatile UInt32 gHubGoneMask = 0;    /* h3: root ports whose claimed HUB was unplugged. Interrupt-set,
                                             * task-logged, so losing a hub (and every drive behind it) is
                                             * visible rather than inferred from drives vanishing. */
static volatile UInt32 gPortUncedeMask = 0; /* h53: ports UN-CEDED because the companion released OWNER. */
static volatile UInt32 gH53Unceded = 0;     /* h53: how many times a cede was taken back (0 = never fired) */
static volatile UInt32 gPortUnparkMask = 0; /* n24: ports un-parked because their device left. Interrupt-set,
                                             * task-logged, so recovering a parked port is visible not silent. */
static volatile UInt32 gPortOwnedMask = 0;  /* n21: ports enumeration was re-armed on while a live slot already
                                             * owned them (the stale-gSelfEnumPort bug). Interrupt-set,
                                             * task-logged, so a recurrence is visible and never silent. */
/* r95: interrupt-level reconnect takeover-arm. The heartbeat SIH runs THROUGH Apple's post-reconnect
 * ExpertIdleTask monopoly (proven r94), so it can watch Apple's bulk activity from a context the task-level
 * self-probe cannot reach during that window. The moment Apple goes QUIET (parked) the SIH fences + arms
 * gReprobe; the proven task-level takeover then fires the instant ExpertIdleTask releases. The SIH does ONLY
 * counter reads + flag sets (no transfers, no File-Mgr logging), and fences ONLY when Apple is quiet — never
 * during an active probe (that was the r92 freeze). Written by SIH, read by task => volatile. */
static volatile int    gSihArmed = 0;       /* set once the SIH has fenced+armed after detecting Apple's park */
static volatile UInt32 gSihQuiet = 0;       /* consecutive heartbeats with no new bulk completion */
static volatile UInt32 gSihLastCnt = 0;     /* last (gBulkDoneN+gBulkErrN) the watcher saw */
static volatile UInt32 gSihArmTick = 0;     /* gVhubTick when we armed (diag, logged at task level on takeover) */
static void ase_quiesce(void);     /* r87 fwd decl: stop the async schedule so a QH reprogram is safe */
static void reconnect_reset(int d); /* r87 fwd decl: bounce-robust reset of stale device state */
/* ★ h45: these were defined further down, after their first use by the h45 guard below. Moved up - they
 * are the definition of "an address WE assigned", which the guard has to know. */
#define SELFENUM_ADDR 1u                 /* base address; device N gets SELFENUM_ADDR + N */
#define DEV_ADDR(slot) ((UInt32)(SELFENUM_ADDR + (UInt32)(slot)))
static volatile UInt32 gH45Refused = 0;   /* h45: foreign-address bulk refusals (MUST be 0) */
/* h46: how many times the h33 K-state cede branch ran. ONE PER LOW-SPEED DEVICE CEDED is correct; the h45
 * run scored 78 for a single mouse because the branch left gSelfEnumPort aimed at the port it had just given
 * away. This counter is the discriminator for that fix — see the note at the cede itself. */
static volatile UInt32 gH46CedeSpinGuard = 0;
long ehci_vhub_create_bulk(UInt32 addr, UInt32 endpt, UInt32 dirIn, UInt32 maxpkt)
{
    int i, freeSlot = -1;
    dead_ring_clear();   /* h94 FAIL-OPEN: a bulk-endpoint registration means this address is a LIVE device
                          * again (re-enumeration reuses addresses); no stale "dead" entry may outlive it */
    /* ★★★★★★ h45 — REFUSE A DEVICE APPLE ENUMERATED BEHIND OUR BACK. TWO STACKS, ONE MEDIUM = CORRUPTION.
     *
     * ⚠ THE T5b FREEZE (2026-08-10): keyboard ceded on a card port, drive re-inserted into another, and the log
     * ends with APPLE'S stack driving mass storage through OUR dispatch table:
     *     uim6 CreateBulkEndpoint devAddr 0x51 endpt 1 dirIn 0
     *     uim6 CreateBulkEndpoint devAddr 0x51 endpt 2 dirIn 1
     *     uim7 BulkTransfer ... prevD0_3 0x55534243   <- 'USBC', the Bulk-Only CSW signature
     * then port 3 goes 0x1005 (WE enable it at high speed), then "SELFPROBE: SIH-armed reconnect takeover",
     * then the machine freezes. Apple assigned address 0x51; OUR addresses are SELFENUM_ADDR + slot = 1..4.
     * So Apple enumerated the re-inserted drive on our bus and began SCSI transfers on it while we were
     * enumerating the same device and arming the r95 takeover. That is the n17 collision class - the one that
     * corrupted a mounted volume once already - and it is a far worse outcome than any freeze: two independent
     * stacks issuing writes to one medium.
     *
     * ★ APPLE_HIDE is supposed to make this impossible: we hide port connects so Apple's hub driver never
     * enumerates our devices. Something let a connect through after four cede cycles, and THAT is the bug to
     * find. But finding it must not risk the medium, so this is the guard: any bulk endpoint request for an
     * address that is neither our virtual root hub nor one of OUR assigned device addresses is REFUSED and
     * LOGGED. A refusal turns a freeze (and a possible two-stack write) into a clean, named failure we can
     * read in the log - Apple's class driver gets an error and gives up, our own enumeration proceeds.
     * ⚠ This is deliberately a SAFETY NET, not a fix. If it fires, the log line names the address and the next
     * step is the hide logic in service_ports, not another guess here. */
    {
        int ours = (addr == gRootHubAddr);
        for (i = 0; !ours && i < USB_MAX_DEV; i++)
            if (addr == DEV_ADDR(i)) ours = 1;                 /* an address WE assigned */
        if (!ours) {
            ehci_os_ilogx("!! h45 REFUSED CreateBulkEndpoint for a FOREIGN device address — Apple enumerated a "
                          "device on our bus behind APPLE_HIDE; two stacks on one medium is the n17 corruption "
                          "hazard. addr<<16|endpt", (addr << 16) | (endpt & 0xFFFFUL));
            gH45Refused++;
            return -1;                                          /* Apple's class driver backs off */
        }
    }
    /* ★ n15 FIX: this test is PER-DEVICE. r87 read a new address as "the mounted device reinserted or
     * bounced", which was sound while only one device could exist. With two it is false: a new address
     * usually means a SECOND DEVICE. Enumerating drive B therefore ran reconnect_reset, which wiped slot
     * 0's probe state and cleared the whole endpoint table, so drive A and B both lost their
     * registrations and pb_find_eps correctly reported none (gPOut/gPIn = 0x0000ffff, three times, then
     * the port was parked). Compare the address against the slot BEING ENUMERATED instead. */
    int reconn = (gMountedOnce && gDev[gEnumDev].inUse && addr != gDev[gEnumDev].curAddr);
    UInt32 mp = maxpkt ? maxpkt : 512;
    /* Phase 1 r87 (hot re-mount, bounce-robust). STOP the async schedule before the epq_program below
     * reprograms a QH (epq_program on a LIVE ring = the r84/r85 freeze). On a post-mount new-address enum
     * (a reinsert OR one of the SanDisk's re-enumeration bounces) reset the stale device state so the
     * self-probe re-runs on the CURRENT device — the r86 crash was the engine locked onto a bounced-past
     * stale address. Fires on EVERY address change (not once), so each bounce is handled cleanly. */
    ase_quiesce();
    if (reconn) reconnect_reset(gEnumDev);
    gDev[gEnumDev].curAddr = addr;
    for (i = 0; i < NBULK; i++) {
        if (gBulkEP[i].used && gBulkEP[i].addr == addr && gBulkEP[i].endpt == endpt) {
            gBulkEP[i].dirIn = (UInt8)(dirIn ? 1 : 0);
            gBulkEP[i].maxpkt = (UInt16)mp;
            gBulkEP[i].toggle = 0;                       /* legacy field; HW owns the toggle now */
            /* ★★★★ h8 INVARIANT: two LIVE slots must never share an address+endpoint.
             * Re-tagging on reuse is correct when the SAME device re-enumerates (the n18 fix), but if the
             * entry currently belongs to a DIFFERENT slot that is still in use, then two devices have been
             * given the same USB address and we are about to steal one device's endpoints for another. That
             * is the n17 corruption shape, and it is now the third time an address or index has been wrong
             * here (n17, h4's unused HUB_ADDR, h7's DEV_ADDR(0)). It costs one compare to make it loud. */
            if (gBulkEP[i].dev != (UInt8)gEnumDev &&
                gBulkEP[i].dev < USB_MAX_DEV && gDev[gBulkEP[i].dev].inUse) {
                ehci_os_ilogx("!! h8 ADDRESS COLLISION: this addr+endpoint already belongs to a LIVE slot; "
                              "owner<<16|newslot|addr",
                              ((UInt32)gBulkEP[i].dev << 24) | ((UInt32)gEnumDev << 16) | (addr & 0xFFFF));
            }
            gBulkEP[i].dev = (UInt8)gEnumDev;             /* ★ n18 FIX 2: RE-TAG the owner. This path
                                                          * returned without touching dev, so a reused
                                                          * entry kept its old owner and pb_find_eps then
                                                          * found nothing for the new slot. */
            /* ★★★★ n22 THE SLOT-REUSE TOGGLE BUG. Since r63 the bulk data toggle lives in HARDWARE, in the
             * QH overlay (DTC=0), and `epq_arm_idle` is the ONLY code that clears it — epq_program writes
             * epChar (address/endpoint/maxpkt) and never touches the overlay. reconnect_reset arms the QHs of
             * ONE slot, the one being reset. So when a slot is freed by a pull and later handed to a DIFFERENT
             * device, that slot's QH still carries the PREVIOUS device's toggle. The newcomer comes out of USB
             * reset at DATA0, the QH expects DATA1, and a toggle mismatch is not an error — it is silence: the
             * qTD never retires. That is the n21 hardware failure exactly, INQUIRY stalling at BOT step 9 with
             * gDownErr 0 and gDownTimeouts 0, three times, then the port parked. Slot 0 never showed it because
             * every pull ran reconnect_reset on slot 0 and re-armed its QHs for free.
             * Safe here: create_bulk has already called ase_quiesce(), so the schedule is stopped, and each
             * direction owns its own QH, so arming the one being programmed cannot disturb the other. */
            epq_arm_idle(&gDev[gEnumDev].bulkQ[dirIn ? 1 : 0]);
            epq_program(&gDev[gEnumDev].bulkQ[dirIn ? 1 : 0], addr, endpt, mp, 0);   /* bind this endpoint to its resident bulk QH */
            return 0;
        }
        if (!gBulkEP[i].used && freeSlot < 0) freeSlot = i;
    }
    /* ★★★★ h14: SAY SO WHEN THE TABLE IS FULL. This returned -1 silently, and every caller — the two
     * task-level registrations in selfprobe_tick, the reference path in selfenum_run, and the USL's own
     * calls — dropped the result. With NBULK a bare 6 against 4 devices' 8 endpoints, the fourth drive
     * would have failed here, pb_find_eps would have found nothing for its slot, and the symptom would have
     * been "the drive just never shows up" with nothing in the log pointing here. NBULK is derived now, so
     * this should be unreachable; if it ever fires, the derivation broke and this line says so. */
    if (freeSlot < 0) {
        ehci_os_ilogx("!! h14 BULK ENDPOINT TABLE FULL — no entry for this device's endpoint; slot|NBULK",
                      ((UInt32)gEnumDev << 16) | (UInt32)NBULK);
        return -1;
    }
    gBulkEP[freeSlot].addr = addr; gBulkEP[freeSlot].endpt = endpt;
    gBulkEP[freeSlot].dirIn = (UInt8)(dirIn ? 1 : 0);
    gBulkEP[freeSlot].maxpkt = (UInt16)mp;
    gBulkEP[freeSlot].toggle = 0; gBulkEP[freeSlot].used = 1;
    /* ★ n14 step 3: tag the endpoint with the slot the RUNNING ENUMERATION is filling, not with a
     * global. Using gCur here (always 0) would have tagged a second device's endpoints as slot 0's,
     * so its transfers would have been issued onto slot 0's queue heads: exactly the shared-toggle
     * corruption the per-device QH pairs exist to prevent. create_bulk is called by the USL during
     * enumeration, so gAs.dev is the correct owner. */
    gBulkEP[freeSlot].dev = (UInt8)gEnumDev;
    /* ★★★★ n22: RESET THE HARDWARE TOGGLE FOR A SLOT BEING (RE)ASSIGNED — see the note on the other
     * epq_program call above. epq_program writes epChar only; epq_arm_idle is the ONLY thing that clears
     * ovlToken, i.e. the DATA0/DATA1 toggle that DTC=0 puts in hardware. */
    epq_arm_idle(&gDev[gEnumDev].bulkQ[dirIn ? 1 : 0]);
    epq_program(&gDev[gEnumDev].bulkQ[dirIn ? 1 : 0], addr, endpt, mp, 0);   /* program the endpoint's resident bulk QH (DTC=0) */
    return 0;
}
/* r32 ROOT-CAUSE FIX: set to 1 once our self-probe takes over the endpoints. While set,
 * ehci_vhub_bulk_xfer REJECTS Apple-USL bulk transfers (they arrive with a real completion UPP;
 * our own self-probe/'Eusb' transfers pass complUPP==0), so the still-loaded Apple USB Mass Storage
 * driver can't share our single QH and intermittently corrupt our reads (the audio-CD misID) or
 * collide with a Finder copy (the freeze). Returning an error = a synchronous "submit failed" the
 * USL handles without waiting for a completion UPP. */
static volatile int gFenceApple = 0;   /* r91 volatile: written by reconnect_reset (ISR ctx), read by bulk_xfer (task) */
long ehci_vhub_bulk_xfer(void *pipe, void *complUPP, volatile UInt8 *buf,
                         UInt32 addr, UInt32 endpt, UInt32 len, UInt32 dirIn)
{
    int i;
    /* ⚠ n6d: gated OFF by default. This r95 trace fires on EVERY bulk transfer while
     * (gMountedOnce && gDev[0].pstate < 10) — which is open for the WHOLE of a hot re-insert, since
     * reconnect_reset sets gDev[0].pstate = 0. At 5 ring entries a call for the first 60 calls that is up to 300
     * of the ring's 384 slots, which would starve and DROP the enumeration's own messages exactly when we
     * need them. The r92 investigation it was written for is long closed. */
    if (EHCI_VERBOSE && gMountedOnce && gDev[0].pstate < 10) {   /* r95: reconnect-window trace. Shows whether Apple keeps issuing
                                           * (active) or stops (quiet), whether the heartbeat SIH is alive
                                           * (gVhubTick advancing), and whether/when the SIH armed the takeover.
                                           * ★★ n6b: these were ehci_os_log (FILE MANAGER). The original comment
                                           * asserted "this runs at TASK level inside Apple's ExpertIdleTask" —
                                           * true when it was written, FALSE since n5. The BOT probe's
                                           * pb_cbw/pb_in now reach here from bot_step at INTERRUPT level, and
                                           * this gate (gMountedOnce && gDev[0].pstate < 10) is OPEN during exactly one
                                           * window: a hot re-insert, because reconnect_reset sets gDev[0].pstate = 0
                                           * while gMountedOnce stays 1. So the first re-insert that got as far
                                           * as the BOT probe would have done File Manager I/O from the SIH and
                                           * hung — the r18 trap, third time. Found by a static audit of the
                                           * interrupt-level call graph, not on hardware. Ring only now. */
        static UInt32 nlg = 0;
        if (nlg++ < 60) {
            ehci_os_ilog("r95 bulk_xfer (reconnect window):");
            ehci_os_ilogx("  complUPP (0=ours, else Apple)", (UInt32)(long)complUPP);
            ehci_os_ilogx("  gVhubTick (SIH alive if climbing)", (UInt32)gVhubTick);
            ehci_os_ilogx("  bulkN (done+err)", gBulkDoneN + gBulkErrN);
            ehci_os_ilogx("  gSihQuiet", (UInt32)gSihQuiet); ehci_os_ilogx("  gSihArmed", (UInt32)gSihArmed);
        }
    }
    if (gFenceApple && complUPP != 0) return -6640L;     /* fenced: reject the Apple driver's bulk xfer */
    for (i = 0; i < NBULK; i++)
        if (gBulkEP[i].used && gBulkEP[i].addr == addr && gBulkEP[i].endpt == endpt) {
            down_submit(pipe, complUPP, buf, addr, len, (dirIn ? 1u : 0u), i);
            return 0;                                    /* DEFER: completed by the service (3-arg UPP) */
        }
    return -1;                                           /* no such endpoint (slot 6 wasn't called) */
}
/* r10 diagnostic accessor: last bulk completion (call from task context — uim7). */
UInt32 ehci_vhub_bulk_stats(long *lastStat, UInt32 *doneN, UInt32 *errN, UInt8 *d16)
{
    int i; *lastStat = gBulkLastStat; *doneN = gBulkDoneN; *errN = gBulkErrN;
    for (i = 0; i < 16; i++) d16[i] = gLastData[i];
    return gBulkDoneN + gBulkErrN;                        /* total completed (a change => new data) */
}

/* ==================== r21: SELF-DRIVEN SCSI PROBE — bypass the parked Apple mounter ====================
 * The Apple v2.0.9 mounter enumerates + does TUR/REQUEST SENSE then refuses to advance for our HIGH-SPEED
 * device (proven: the same device+driver mount at 1.1; the HS bit is load-bearing for EHCI enum so speed
 * can't be faked away — r20). So we drive the SCSI probe OURSELVES over the (now-idle) bulk endpoints the
 * mounter already registered via uim6: INQUIRY -> READ CAPACITY -> READ(10) LBA0. Proves the whole data
 * path and is the foundation for our own block driver + PBMountVol. Ticked at TASK level from uim23; each
 * BOT command = CBW(OUT) -> DATA(IN) -> CSW(IN), advanced when the prior transfer's completion counter
 * ticks. NULL completion UPP so the mounter's own callback is never invoked — it stays parked. */
static UInt8  gPB[DOWN_MAX_BLOCKS * 512];   /* CBW / data / CSW scratch for the SINGLE-qTD write + sync paths (<=20KB).
                                            * r71: stays 20KB (NOT the 128KB DOWN_BUF_MAX) — the big READ chain DMAs
                                            * into gDownBuf, and only the 13-byte CSW is mirrored back here. */
/* gDev[0].pstate moved up to the cross-context volatile group (r95) so bulk_xfer's diagnostic can see it. */
static volatile UInt32 gPIdle = 0, gPLastCnt = 0, gPMark = 0, gPTag = 0x50524231UL; /* 'PRB1' */
static volatile UInt32 gPErrMark = 0;   /* p1b: gBulkErrN snapshot at issue — lets pb_failed() see a STALL */
/* n14: gDev[0].pOut/gDev[0].pIn are now per-device fields of gDev[] (see the UsbDev block). Initialised to -1 in
 * ehci_vhub_xfer_init, since a struct array cannot carry the old inline initialiser. */

/* ★ n17: only endpoints OWNED BY DEVICE d. Without the ownership test this scanned the whole table and
 * happily selected another device's endpoints, so with two drives attached it could hand device A the
 * indices of device B's pipes. gBulkEP[].dev is set by create_bulk from the enumerating slot. */
static void pb_find_eps(int d)
{
    int i; gDev[d].pOut = gDev[d].pIn = -1;
    for (i = 0; i < NBULK; i++) if (gBulkEP[i].used && gBulkEP[i].dev == (UInt8)d) {
        if (gBulkEP[i].dirIn) { if (gDev[d].pIn  < 0) gDev[d].pIn  = i; }
        else                  { if (gDev[d].pOut < 0) gDev[d].pOut = i; }
    }
}
static void pb_cbw_dir(int d, const UInt8 *cdb, int cdbLen, UInt32 dataLen, UInt8 flags)   /* CBW; flags 0x80=data-IN, 0x00=data-OUT */
{
    int i; if (gDev[d].pOut < 0) return;
    for (i = 0; i < 31; i++) gPB[i] = 0;
    gPB[0]=0x55; gPB[1]=0x53; gPB[2]=0x42; gPB[3]=0x43;                        /* 'USBC' */
    gPB[4]=(UInt8)gPTag; gPB[5]=(UInt8)(gPTag>>8); gPB[6]=(UInt8)(gPTag>>16); gPB[7]=(UInt8)(gPTag>>24);
    gPB[8]=(UInt8)dataLen; gPB[9]=(UInt8)(dataLen>>8); gPB[10]=(UInt8)(dataLen>>16); gPB[11]=(UInt8)(dataLen>>24);
    gPB[12]=flags;                                                            /* bmCBWFlags */
    gPB[13]=0;                                                                /* LUN 0 */
    gPB[14]=(UInt8)cdbLen;                                                    /* CDB length */
    for (i = 0; i < cdbLen && i < 16; i++) gPB[15+i] = cdb[i];
    gPTag++;
    gPMark = gEgBulkDoneN + gEgBulkErrN; gPErrMark = gEgBulkErrN;   /* SPLIT: ENGINE completions only */   /* p1b: also snapshot the ERROR count (see pb_failed) */
    (void)ehci_vhub_bulk_xfer(0, 0, gPB, gBulkEP[gDev[d].pOut].addr, gBulkEP[gDev[d].pOut].endpt, 31, 0);   /* OUT (31B, via obuf) */
}
static void pb_cbw(int d, const UInt8 *cdb, int cdbLen, UInt32 dataLen) { pb_cbw_dir(d, cdb, cdbLen, dataLen, 0x80); }  /* data-IN CBW */
static void pb_in(int d, UInt32 len)   /* read len bytes on the IN endpoint into gPB */
{
    if (gDev[d].pIn < 0) return;
    gPMark = gEgBulkDoneN + gEgBulkErrN; gPErrMark = gEgBulkErrN;   /* SPLIT: ENGINE completions only */   /* p1b */
    (void)ehci_vhub_bulk_xfer(0, 0, gPB, gBulkEP[gDev[d].pIn].addr, gBulkEP[gDev[d].pIn].endpt, len, 1);     /* IN */
}
static void pb_out(int d, UInt32 len)  /* write len bytes from gPB on the OUT endpoint (large OUT via obig) */
{
    if (gDev[d].pOut < 0) return;
    gPMark = gEgBulkDoneN + gEgBulkErrN; gPErrMark = gEgBulkErrN;   /* SPLIT: ENGINE completions only */   /* p1b */
    (void)ehci_vhub_bulk_xfer(0, 0, gPB, gBulkEP[gDev[d].pOut].addr, gBulkEP[gDev[d].pOut].endpt, len, 0);   /* OUT */
}
static int pb_ready(void) { return (gEgBulkDoneN + gEgBulkErrN) != gPMark; }  /* SPLIT: MY transfer finished */
/* ★ p1b: pb_ready() means "the transfer FINISHED" — success OR error. That blindness is why the p1a probe
 * marched straight through a STALLed READ CAPACITY and still published 'Eusb': every state advanced on
 * pb_ready() alone, so a halted endpoint looked like progress and state 5 parsed the leftover CBW bytes
 * ('USBC' = 0x55534243, +1 = the bogus 0x55534244 "block count" in the log). pb_failed() distinguishes them. */
static int pb_failed(void) { return gEgBulkErrN != gPErrMark; }               /* SPLIT: MY transfer ERRORED */
#define PB_BE32(o) (((UInt32)gPB[o]<<24)|((UInt32)gPB[(o)+1]<<16)|((UInt32)gPB[(o)+2]<<8)|gPB[(o)+3])
#define BUF_BE32(b,o) (((UInt32)(b)[o]<<24)|((UInt32)(b)[(o)+1]<<16)|((UInt32)(b)[(o)+2]<<8)|(b)[(o)+3])

/* ==================== r22 (BYPASS m2): synchronous block-read SERVICE for our own disk driver ==========
 * Our block driver is a separate native PEF (installed post-selfprobe, like the author's earlier disk ndrv) and can't
 * reach this engine by CFM link, so the UIM publishes a service struct via Gestalt('Eusb'): a synchronous
 * BOT-read TVector + the geometry it found. The block driver's kRead calls readFn(lba,count,buf) through
 * the TVector (self-contained across fragments). Completion is SIH/timer-driven, so a task-level spin-wait
 * here will NOT deadlock while PBMountVol blocks the task — the 8ms heartbeat keeps down_pump/down_reap
 * running. One BOT command per call (<=7 blocks, bounce-buffer capped); the block driver loops for more. */
static int pb_wait(void)   /* spin until the last-issued transfer completes (SIH does the work); 800ms cap */
{
    UInt32 t0 = frame_ms();
    while (!pb_ready()) { if (frame_ms() - t0 > 800UL) return -1; }
    return 0;
}
static long ehci_usb_read(int dev, UInt32 lba, UInt32 count, void *buf)   /* synchronous BOT READ(10); >=0 = blocks read */
{
    UInt8 cdb[10]; UInt32 nbytes; int i;
    /* ★ n20: honour `dev`. It was declared in n19 and then ignored — every access below was hardcoded to
     * device 0 — so the block driver's volume scan for slot 1 read SLOT 0's blocks. That is why the n19 run
     * logged a valid HFS header and MDB signature for the second drive: the scan was reading the first drive.
     * (partCount looked right only because it comes from gSvc->blkCnt[dev], which the per-device probe fills,
     * not from a read.) Refuse an out-of-range slot rather than clamping to 0 — see ehci_usb_submit. */
    if (dev < 0 || dev >= USB_MAX_DEV) return -1;
    if (gDev[dev].pOut < 0 || gDev[dev].pIn < 0 || count == 0) return -1;
    if (count > DOWN_MAX_BLOCKS) count = DOWN_MAX_BLOCKS;          /* r46: up to 40*512=20480 = DOWN_BUF_MAX */
    nbytes = count * 512;
    for (i = 0; i < 10; i++) cdb[i] = 0;
    cdb[0] = 0x28;                                                /* READ(10) */
    cdb[2]=(UInt8)(lba>>24); cdb[3]=(UInt8)(lba>>16); cdb[4]=(UInt8)(lba>>8); cdb[5]=(UInt8)lba;
    cdb[7]=(UInt8)(count>>8); cdb[8]=(UInt8)count;
    pb_cbw(dev, cdb, 10, nbytes);  if (pb_wait()) return -2;         /* CBW out */
    pb_in(dev, nbytes);            if (pb_wait()) return -3;         /* data -> gPB */
    { UInt8 *d = (UInt8 *)buf; for (i = 0; i < (int)nbytes; i++) d[i] = gPB[i]; }
    pb_in(dev, 13);                if (pb_wait()) return -4;         /* CSW */
    return (gPB[12] == 0) ? (long)count : -5;                     /* CSW status 0 = passed */
}
/* synchronous BOT WRITE(10): CBW(OUT) -> DATA(OUT, large via obig) -> CSW(IN). >=0 = blocks written. */
static long ehci_usb_write(int dev, UInt32 lba, UInt32 count, void *buf)
{
    UInt8 cdb[10]; UInt32 nbytes; int i; UInt8 *src = (UInt8 *)buf;
    /* ★ n20: honour `dev` — see ehci_usb_read. This is the WRITE side of the same defect, and the more
     * dangerous half: a hardcoded device 0 here sends slot 1's data to slot 0's medium at slot 1's LBA,
     * which is a silent cross-device write. Nothing in the n19 run exercised it (the second volume was only
     * read), but it was one file copy away. Refuse out-of-range rather than clamping to 0. */
    if (dev < 0 || dev >= USB_MAX_DEV) return -1;
    if (gDev[dev].pOut < 0 || gDev[dev].pIn < 0 || count == 0) return -1;
    if (count > DOWN_MAX_BLOCKS) count = DOWN_MAX_BLOCKS;          /* r46: up to 40 blocks */
    nbytes = count * 512;
    for (i = 0; i < 10; i++) cdb[i] = 0;
    cdb[0] = 0x2A;                                                /* WRITE(10) */
    cdb[2]=(UInt8)(lba>>24); cdb[3]=(UInt8)(lba>>16); cdb[4]=(UInt8)(lba>>8); cdb[5]=(UInt8)lba;
    cdb[7]=(UInt8)(count>>8); cdb[8]=(UInt8)count;
    pb_cbw_dir(dev, cdb, 10, nbytes, 0x00);  if (pb_wait()) return -2; /* CBW out (bmCBWFlags data-OUT) */
    for (i = 0; i < (int)nbytes; i++) gPB[i] = src[i];           /* stage write data in gPB (obig source) */
    pb_out(dev, nbytes);                     if (pb_wait()) return -3; /* data out */
    pb_in(dev, 13);                          if (pb_wait()) return -4; /* CSW (overwrites gPB — data already sent) */
    return (gPB[12] == 0) ? (long)count : -5;                    /* CSW status 0 = passed */
}

/* r41 WRITE-FAILURE DIAGNOSTIC: the r40 SanDisk mounted + read/wrote, but a real ~1GB Finder copy hit a
 * "disk error" early with the transport CLEAN (DOWN ENGINE err=0/timeouts=0) — so a BOT WRITE(10)'s CSW
 * came back nonzero (silently → ioErr) OR the bio queue rejected a submit. Capture the failing CSW
 * (status/sig/residue) + the LBA/chunk + context counts, interrupt-safe, drained by uim23 — so the
 * failing write finally names itself and we can tell a real device reject (sig='USBS',stat=1) from a
 * malformed CSW read. Reads gPB (the shared BOT scratch) which holds the just-read CSW at phase-3 time. */
static volatile UInt32 gFailSeq = 0, gFailLba = 0, gFailCswSig = 0, gFailCswResid = 0;
static volatile UInt32 gFailSubmitLba = 0, gFailRetry = 0;   /* v40: garbage-LBA origin discriminator (orig LBA + timeout-retries before the fail) */
static volatile UInt16 gFailChunk = 0; static volatile UInt8 gFailIsWrite = 0, gFailCswStat = 0;
static volatile UInt32 gBioWrOk = 0, gBioRdOk = 0, gBioReject = 0;
static volatile UInt32 gBioHiWater = 0;   /* r49: peak ring occupancy seen at submit — decides ring-full(a) vs slow-engine-timeout(b) for the Finder large-copy "disk error" */
static void biofail(UInt8 isWrite, UInt32 lba, UInt32 submitLba, UInt32 retry, UInt16 chunk)
{
    gFailIsWrite = isWrite; gFailLba = lba; gFailChunk = chunk;
    gFailSubmitLba = submitLba; gFailRetry = retry;   /* v40 */
    gFailCswStat  = gPB[12];
    gFailCswSig   = ((UInt32)gPB[0]<<24)|((UInt32)gPB[1]<<16)|((UInt32)gPB[2]<<8)|gPB[3];
    gFailCswResid = (UInt32)gPB[8]|((UInt32)gPB[9]<<8)|((UInt32)gPB[10]<<16)|((UInt32)gPB[11]<<24);
    __asm__ __volatile__("eieio");
    gFailSeq++;
}
UInt32 ehci_vhub_biofail(UInt8 *isWrite, UInt32 *lba, UInt16 *chunk, UInt8 *cswStat,
                         UInt32 *cswSig, UInt32 *cswResid, UInt32 *wrOk, UInt32 *rdOk, UInt32 *reject)
{
    if (isWrite) *isWrite = gFailIsWrite; if (lba) *lba = gFailLba; if (chunk) *chunk = gFailChunk;
    if (cswStat) *cswStat = gFailCswStat; if (cswSig) *cswSig = gFailCswSig; if (cswResid) *cswResid = gFailCswResid;
    if (wrOk) *wrOk = gBioWrOk; if (rdOk) *rdOk = gBioRdOk; if (reject) *reject = gBioReject;
    return gFailSeq;
}
/* v40: separate accessor so the failure dump can also show the UNTOUCHED submit LBA + the timeout-retry count —
 * reveals whether the garbage LBA came from the FM (submitLba == the failing lba) or was corrupted after submit
 * (submitLba valid, failing lba garbage), and whether a BOT timeout/recovery preceded the fail. */
UInt32 ehci_vhub_failsubmit(UInt32 *retry) { if (retry) *retry = gFailRetry; return gFailSubmitLba; }
/* r49: block-I/O engine health for the app's idle-loop THREAD-B diagnostic (exposed via 'Eusb' healthFn).
 * reject>0                 => the 16-deep async ring overflowed (Finder out-ran us) = mechanism (a); cheap
 *                             fix = deeper ring + soft back-pressure instead of hard ioErr.
 * downTimeouts>0(reject==0) => the slow single-in-flight engine watchdog-timed-out = mechanism (b); R4.
 * hiwater                  => peak ring occupancy; near BIOQ_N(16) = ring pressure even if a later deeper
 *                             ring hides the actual reject. downDone confirms the copy volume went through. */
void ehci_vhub_health(UInt32 *reject, UInt32 *hiwater, UInt32 *downTimeouts, UInt32 *downErr, UInt32 *downDone,
                      UInt32 *failSeq, UInt32 *failStat, UInt32 *failSig, UInt32 *failLba, UInt32 *isrHits, UInt32 *maxStall,
                      UInt32 *downRecov, UInt32 *downRelink, UInt32 *lastAnchorLink,
                      UInt32 *dataBytes, UInt32 *dataFrames)
{
    if (maxStall)     *maxStall     = gMaxStallTicks;  /* r54: worst-case transfer stall in 60Hz ticks (device GC pause) */
    if (isrHits)      *isrHits      = gIsrHits;   /* R4-P1: real EHCI IRQs. vs downDone: ~= => interrupt-driven; << => 8ms-heartbeat-polled (the stall) */
    if (reject)       *reject       = gBioReject;
    if (hiwater)      *hiwater      = gBioHiWater;
    if (downTimeouts) *downTimeouts = gDownTimeouts;
    if (downErr)      *downErr      = gDownErr;
    if (downDone)     *downDone     = gDownDone;
    /* r50: the CSW-level failure that becomes the Finder's "cannot be written, disk error" (gPB[12]!=0
     * -> gBioResult=-36 in bio_advance case 3). NOT counted by downErr (that's transport-level only).
     * failSeq>0 => a CSW failure DID hit our engine; failSig=='USBS'(55534253) + failStat==1 => the DEVICE
     * rejected the write (real CHECK CONDITION — we issue NO REQUEST SENSE); failSig!='USBS' => our CSW read
     * is garbage (transport/framing bug); failSeq==0 (with the dialog seen) => the error is ABOVE us (File
     * Mgr/HFS), our engine never saw it. failLba = the block. */
    if (failSeq)      *failSeq      = gFailSeq;
    if (failStat)     *failStat     = (UInt32)gFailCswStat;
    if (failSig)      *failSig      = gFailCswSig;
    if (failLba)      *failLba      = gFailLba;
    if (downRecov)    *downRecov    = gDownRecov;   /* r57: BOT reset-recoveries attempted (a stall we RECOVERED, not false-failed) */
    /* r60: the QH-unlink freeze fix. downRelink>0 => our downstream QH fell out of the async ring under
     * heavy wedging and we RE-SPLICED it (freeze converted to slow-but-alive). lastAnchorLink = the phys
     * the async anchor currently points at; == gDownQHP means the ring is intact right now. */
    if (downRelink)     *downRelink     = gDownRelink;
    if (lastAnchorLink) *lastAnchorLink = gLastAnchorLink;
    if (dataBytes)      *dataBytes      = gDataBytes;    /* r67: bytes moved in timed big-data transfers */
    if (dataFrames)     *dataFrames     = gDataFrames;   /* r67: microframes (125µs) they took -> MB/s = bytes/(frames*125) */
}

/* r57 BOT ERROR RECOVERY — the robustness layer Apple's 1.1 mass-storage driver has and we lacked. On a
 * bulk transfer timeout/error (device wedged/NAKing the CBW), instead of false-failing the write (which
 * corrupted the volume), issue the standard USB Bulk-Only recovery: Mass-Storage Reset + CLEAR_FEATURE
 * (ENDPOINT_HALT) on both bulk endpoints + reset the data toggles to DATA0, then RETRY the command. All via
 * ep0 control transfers (down_submit(...,-1)). Bounded by BIO_MAX_RETRY. Recovery phases live above the
 * normal BOT phases (1/2/3) in gBioPhase so the happy path is untouched. */
/* h75: REC_BASE itself is defined far above, beside the engine slot — down_reap needs it to route a tagged
 * recovery completion, and down_reap comes long before this block. Kept as ONE definition rather than a
 * matching pair here: two #defines that must agree is precisely the "second copy" trap. */
#define REC_RESET_SETUP   20
#define REC_RESET_STATUS  21
#define REC_CLRIN_SETUP   22   /* r64: bulk-IN cleared FIRST, per BOT r1.0 §5.3.4 */
#define REC_CLRIN_STATUS  23
#define REC_CLROUT_SETUP  24
#define REC_CLROUT_STATUS 25
#define BIO_MAX_RETRY     3
/* r64 (A2): the CORRECT one-shot Bulk-Only Transport reset recovery, RE-ENABLED now that r63's per-endpoint
 * hardware toggle killed the wedge — so recovery is a RARE genuine-fault backstop, not the hot path. The
 * r57-r60 version froze the machine because it (a) fired constantly on a persistently-wedging device and
 * (b) reset only a SOFTWARE toggle, never re-matching the device<->host toggle, so the device NAKed forever
 * = a reset loop. The correct sequence (BOT r1.0 §5.3.4 + EHCI 4.8/4.10): Bulk-Only Mass Storage Reset ->
 * CLEAR_FEATURE(HALT) bulk-IN -> bulk-OUT (device toggles reset to DATA0) -> reset the HOST bulk QH HARDWARE
 * toggles to DATA0 (bot_reset_host_toggles, via an async-schedule quiesce) -> retry. Bounded BIO_MAX_RETRY. */
/* ⚠ r65: DISABLED. The r64 forced-recovery hook did its job — it PROVED the recovery path wedges the engine
 * (r64 HW run: the forced recovery at ~chunk 50 broke I/O → cascade of real recoveries → downDone frozen at
 * 267, everything timing out at the 10s watchdog, a recovery STATUS-IN stuck Active). Suspect the
 * bot_reset_host_toggles schedule-quiesce (fragile at interrupt level) and/or the host<->device toggle not
 * actually re-matching after reset → NAK-forever cascade. Reliability does NOT need recovery (r63 fail-clean
 * was proven: 800 files + a real 800MB Finder copy, zero errors), so we ship fail-clean + the tuned watchdog
 * and DEFER recovery to a focused, instrumented debug session (log each REC_ phase + the toggle values;
 * cap to ONE forced recovery then fail-clean so it can't cascade). Recovery code kept below, disabled. */
#define BIO_BOT_RECOVERY 0
#define BIO_FORCE_ONE_RECOVERY 0
static UInt32 gBioRetry = 0;              /* recovery+retry attempts for the current chunk */
static UInt8  gRecovSetup[8];             /* scratch SETUP packet for the recovery control requests */
static int    gForcedRecovDone = 0;       /* r64: the one-shot forced-recovery test has fired */

/* ==================== r70 (THROUGHPUT Phase 2b): READ pre-queue ====================
 * r67/r68 proved ~85-90% of each BOT command's wall-clock is PER-COMMAND overhead — three phases
 * (CBW->data->CSW), each a separate transfer with its own completion round-trip — while the data move
 * itself is ~13 MB/s. Here we PRE-QUEUE the whole READ command across the two per-endpoint QHs (r63) so the
 * controller streams it with ONE terminal interrupt instead of three: CBW on the bulk-OUT QH (no IOC), and
 * the data + CSW chained on the bulk-IN QH with IOC on the CSW ONLY. The device NAK-gates ordering (it will
 * not return the CSW until the command has run, and NAKs an early IN token until the CBW lands), so issuing
 * the IN tokens ahead of the CBW completing is safe. We reap the terminal CSW qTD — by then the data has
 * DMA'd into gDownBuf and the CSW into gCswBuf, and bio_advance(BIO_PH_PREREAD) copies the data out + checks
 * gCswBuf[12]. WRITE stays 3-phase for now (the next step); chunk is still one 5-pointer data qTD (<=20KB) —
 * the multi-qTD data CHAIN for bigger chunks is Phase 1b. */
static void bio_build_cbw(volatile UInt8 *dst, const UInt8 *cdb, UInt32 dataLen, UInt8 flags)
{
    int i;
    for (i = 0; i < 31; i++) dst[i] = 0;
    dst[0]=0x55; dst[1]=0x53; dst[2]=0x42; dst[3]=0x43;                        /* 'USBC' */
    dst[4]=(UInt8)gPTag; dst[5]=(UInt8)(gPTag>>8); dst[6]=(UInt8)(gPTag>>16); dst[7]=(UInt8)(gPTag>>24);
    dst[8]=(UInt8)dataLen; dst[9]=(UInt8)(dataLen>>8); dst[10]=(UInt8)(dataLen>>16); dst[11]=(UInt8)(dataLen>>24);
    dst[12]=flags; dst[13]=0; dst[14]=10;                                     /* bmCBWFlags, LUN 0, CDB len (READ(10)) */
    for (i = 0; i < 10; i++) dst[15+i] = cdb[i];
    gPTag++;
}
/* Pre-queue the CBW (already in gCbwBuf) + a DATA CHAIN (nData qTDs, <=20KB each) -> CSW for an nbytes READ.
 * r71: the data phase is now a CHAIN (was one 20KB qTD) so a command can move up to 128KB, amortizing the
 * fixed per-command overhead. Layout on the IN QH: [data0 -> data1 -> ... -> data(N-1) -> CSW -> dummy], IOC
 * on the CSW ONLY; every data qTD's Alt-Next -> CSW so a short read anywhere jumps straight to status. The
 * hazard is activation order: the IN QH is parked on its tail dummy (= the FIRST data slot), so we build the
 * dummy + CSW + data(N-1..1) fully-active FIRST (all unreachable until the chain head runs), set the gDp
 * state, then activate data0 LAST — that single store unparks the whole chain. The gDp state and gDpBusy are
 * set before data0 goes active so an early reap sees the terminal CSW qTD ACTIVE and just waits. Slots:
 * nData + CSW + dummy = at most 9, within the QTD_POOL of 10. */
/* ★★★ f4 DIRECTION B — BLOCK I/O IS ABOUT TO ARM THE SHARED SLOT. WAS SOMEONE ELSE USING IT?
 * bio_issue_read/bio_issue_write overwrite the whole gDp* slot with no test of gDpBusy, and that is
 * DELIBERATE (h18 added the test and froze the machine — see the long note in bio_kick). So the clobber is
 * a live possibility on every block command, and if it lands on a probe transfer the victim's qTD is
 * orphaned while BOTH remain live in hardware pointing at the SAME bounce (gDownBuf). That is the only
 * mechanism on the table that can corrupt the MOUNT'S OWN data, so it is the one worth naming precisely.
 * ⚠ Observation only — it does not refuse, delay or reorder anything.
 * ⚠ Defined AFTER gAs (it reads gAs.running), which is declared ~1800 lines below this point. Forward
 * declaration rather than a duplicate "is an enumeration running" flag: a second copy of state that must be
 * kept in step with the first is the exact defect shape this driver's whole history is made of. */
static void f4_note_bio_arm(void);
static void bio_issue_read(int dv, UInt32 nbytes)
{
    EpQ *qo = &gDev[dv].bulkQ[0];                                         /* bulk-OUT : CBW */
    EpQ *qi = &gDev[dv].bulkQ[1];                                         /* bulk-IN  : data chain -> CSW */
    int co = qo->dummy, no = (co + 1) % QTD_POOL;                 /* OUT: CBW slot, new tail dummy */
    int nData = (int)((nbytes + 0x4FFF) / 0x5000);               /* # of <=20KB(0x5000) data qTDs (ceil) */
    int base, cswSlot, dumSlot, i, j;
    ehci_qtd *cbw = qo->td[co], *cbwDum = qo->td[no], *csw, *inDum;
    UInt32 cbwTok = EHCI_QTD_STATUS_ACTIVE | EHCI_QTD_CERR(3) | EHCI_QTD_BYTES(31) | EHCI_QTD_PID_OUT;                 /* no IOC */
    UInt32 cswTok = EHCI_QTD_STATUS_ACTIVE | EHCI_QTD_CERR(3) | EHCI_QTD_BYTES(13) | EHCI_QTD_PID_IN | EHCI_QTD_IOC;
    if (nData < 1) nData = 1;
    f4_note_bio_arm();                                           /* f4: before we overwrite the shared slot */
    base = qi->dummy;                                            /* first data qTD = the parked slot */
    cswSlot = (base + nData) % QTD_POOL;
    dumSlot = (base + nData + 1) % QTD_POOL;
    csw = qi->td[cswSlot]; inDum = qi->td[dumSlot];
    down_arm_ase();
    /* --- OUT QH: CBW qTD (single, no IOC; activate now) --- */
    cbwDum->next = ehci_cpu_to_le32(EHCI_LINK_TERMINATE); cbwDum->altNext = ehci_cpu_to_le32(EHCI_LINK_TERMINATE); cbwDum->token = 0;
    cbw->next = ehci_cpu_to_le32(qo->tdP[no]); cbw->altNext = ehci_cpu_to_le32(EHCI_LINK_TERMINATE);
    cbw->buffer[0] = ehci_cpu_to_le32(gCbwPhys); for (j = 1; j < 5; j++) cbw->buffer[j] = 0;
    __asm__ __volatile__("eieio");
    cbw->token = ehci_cpu_to_le32(cbwTok);
    __asm__ __volatile__("eieio");
    qo->dummy = (UInt8)no;
    /* --- IN QH: new dummy + CSW(active) + the data chain (built LAST->FIRST; data0 activated last) --- */
    inDum->next = ehci_cpu_to_le32(EHCI_LINK_TERMINATE); inDum->altNext = ehci_cpu_to_le32(EHCI_LINK_TERMINATE); inDum->token = 0;
    csw->next = ehci_cpu_to_le32(qi->tdP[dumSlot]); csw->altNext = ehci_cpu_to_le32(EHCI_LINK_TERMINATE);
    csw->buffer[0] = ehci_cpu_to_le32(gCswPhys); for (j = 1; j < 5; j++) csw->buffer[j] = 0;
    csw->token = ehci_cpu_to_le32(cswTok);                       /* CSW active — unreachable until the chain runs */
    for (i = nData - 1; i >= 0; i--) {
        int slot = (base + i) % QTD_POOL;
        ehci_qtd *d = qi->td[slot];
        UInt32 off = (UInt32)i * 0x5000;
        UInt32 len = (nbytes - off > 0x5000) ? 0x5000u : (nbytes - off);          /* this qTD's byte count (<=20KB) */
        int nextSlot = (i == nData - 1) ? cswSlot : ((base + i + 1) % QTD_POOL);
        UInt32 dTok = EHCI_QTD_STATUS_ACTIVE | EHCI_QTD_CERR(3) | EHCI_QTD_BYTES(len) | EHCI_QTD_PID_IN;   /* no IOC */
        d->next = ehci_cpu_to_le32(qi->tdP[nextSlot]);
        d->altNext = ehci_cpu_to_le32(qi->tdP[cswSlot]);         /* short read anywhere -> jump to the CSW */
        for (j = 0; j < 5; j++)                                  /* up to 5 page pointers; guard caps at len (no OOB) */
            d->buffer[j] = ((UInt32)j * 0x1000 < len) ? ehci_cpu_to_le32(gDownBufPhys[i * 5 + j]) : 0;
        if (i == 0) {                                            /* the parked head qTD: set state, activate LAST */
            gDpUPP = 0; gDpPipe = 0; gDpDest = 0; gDpIsIn = 0; gDpLen = nbytes; gDpMeasured = 0;   /* bio path; skip generic copy; gDpLen = command bytes for the r74 on-the-wire timing */
            gDpBulkEp = gDev[dv].pIn; gDpDev = dv; gDpLastAddr = (gDev[dv].pIn >= 0) ? gBulkEP[gDev[dv].pIn].addr : 0; gDpLastPid = 1;
            gDpQ = qi; gDpTd = csw;                              /* reap the terminal CSW qTD */
            gDpArmTick = TICKS_NOW; gDpBusy = 1;                 /* arm watchdog; busy while the CSW qTD is ACTIVE */
            gF4Owner = F4_OWNER_BIO;                             /* f4: tag the slot */
            gDataFr0 = ehci_read32(gSoftc.opBase, EHCI_FRINDEX) & 0x3FFFUL;   /* r74: stamp the on-the-wire command START */
            __asm__ __volatile__("eieio");                       /* whole chain + gDp* visible BEFORE data0 Active */
            d->token = ehci_cpu_to_le32(dTok);                   /* ACTIVATE the chain head LAST -> unpark */
            __asm__ __volatile__("eieio");
        } else {
            __asm__ __volatile__("eieio");                       /* body before token; unreachable until data0 runs */
            d->token = ehci_cpu_to_le32(dTok);
        }
    }
    qi->dummy = (UInt8)dumSlot;
}

/* r77: WRITE pre-queue — the OUT-direction mirror of bio_issue_read. One BOT WRITE command as ONE terminal
 * interrupt (was 3-phase/3-interrupt + single 20KB qTD): the bulk-OUT QH gets [CBW -> data-chain] (the CBW is
 * the parked head, activated LAST), and the bulk-IN QH gets the [CSW] (IOC — the only interrupt AND the
 * terminal reap). The device NAK-gates the CSW-IN until it has taken the CBW+data, so issuing the IN token
 * early is safe. The write data is staged into gDownBuf FIRST (the device DMAs it out of the driver-owned
 * bounce, never the FM buffer). Slots: OUT = CBW + nData + dummy (<=9 <= POOL 10); IN = CSW + dummy (2). */
static void bio_issue_write(int dv, const void *src, UInt32 nbytes)
{
    EpQ *qo = &gDev[dv].bulkQ[0];                                         /* bulk-OUT : CBW -> data chain */
    EpQ *qi = &gDev[dv].bulkQ[1];                                         /* bulk-IN  : CSW (terminal) */
    int nData = (int)((nbytes + 0x4FFF) / 0x5000);               /* # of <=20KB(0x5000) data qTDs (ceil) */
    int co, dn, ci, nci, i, j;
    ehci_qtd *cbw, *outDum, *csw, *inDum;
    UInt32 cbwTok = EHCI_QTD_STATUS_ACTIVE | EHCI_QTD_CERR(3) | EHCI_QTD_BYTES(31) | EHCI_QTD_PID_OUT;                 /* no IOC */
    UInt32 cswTok = EHCI_QTD_STATUS_ACTIVE | EHCI_QTD_CERR(3) | EHCI_QTD_BYTES(13) | EHCI_QTD_PID_IN | EHCI_QTD_IOC;
    if (nData < 1) nData = 1;
    /* f4: BEFORE the BlockMoveData — that write lands in gDownBuf, the bounce a victim transfer may still be
     * DMAing to or from, so the bounce is clobbered here, ahead of the gDp* slot itself at the bottom. */
    f4_note_bio_arm();
    BlockMoveData(src, (void *)gDownBuf, (Size)nbytes);          /* stage the write data into the DMA bounce */
    down_arm_ase();
    /* --- IN QH: CSW qTD (single, IOC) = the terminal reap. Active now; device NAKs the IN token until it has
     *     consumed the CBW+data on the OUT QH. --- */
    ci = qi->dummy; nci = (ci + 1) % QTD_POOL;
    csw = qi->td[ci]; inDum = qi->td[nci];
    inDum->next = ehci_cpu_to_le32(EHCI_LINK_TERMINATE); inDum->altNext = ehci_cpu_to_le32(EHCI_LINK_TERMINATE); inDum->token = 0;
    csw->next = ehci_cpu_to_le32(qi->tdP[nci]); csw->altNext = ehci_cpu_to_le32(EHCI_LINK_TERMINATE);
    csw->buffer[0] = ehci_cpu_to_le32(gCswPhys); for (j = 1; j < 5; j++) csw->buffer[j] = 0;
    gDpQ = qi; gDpTd = csw;                                       /* reap the terminal CSW qTD (on the IN QH) */
    __asm__ __volatile__("eieio");
    csw->token = ehci_cpu_to_le32(cswTok);                       /* activate CSW (NAK-gated until the command runs) */
    __asm__ __volatile__("eieio");
    qi->dummy = (UInt8)nci;
    /* --- OUT QH: [CBW -> data0 -> ... -> data(N-1) -> dummy]; CBW is the parked head, activated LAST. Build
     *     the dummy + data qTDs (active but unreachable until CBW runs), then the CBW body, then set gDp* +
     *     gDpBusy, then activate the CBW LAST (that single store unparks the whole OUT chain). --- */
    co = qo->dummy;
    dn = (co + 1 + nData) % QTD_POOL;                            /* new tail dummy = after CBW + nData data qTDs */
    outDum = qo->td[dn];
    outDum->next = ehci_cpu_to_le32(EHCI_LINK_TERMINATE); outDum->altNext = ehci_cpu_to_le32(EHCI_LINK_TERMINATE); outDum->token = 0;
    for (i = nData - 1; i >= 0; i--) {                           /* data qTDs LAST->first (downstream of CBW) */
        int slot = (co + 1 + i) % QTD_POOL;
        ehci_qtd *d = qo->td[slot];
        UInt32 off = (UInt32)i * 0x5000;
        UInt32 len = (nbytes - off > 0x5000) ? 0x5000u : (nbytes - off);
        int nextSlot = (i == nData - 1) ? dn : ((co + 1 + i + 1) % QTD_POOL);
        UInt32 dTok = EHCI_QTD_STATUS_ACTIVE | EHCI_QTD_CERR(3) | EHCI_QTD_BYTES(len) | EHCI_QTD_PID_OUT;  /* no IOC */
        d->next = ehci_cpu_to_le32(qo->tdP[nextSlot]); d->altNext = ehci_cpu_to_le32(EHCI_LINK_TERMINATE);
        for (j = 0; j < 5; j++)
            d->buffer[j] = ((UInt32)j * 0x1000 < len) ? ehci_cpu_to_le32(gDownBufPhys[i * 5 + j]) : 0;
        __asm__ __volatile__("eieio"); d->token = ehci_cpu_to_le32(dTok);
    }
    cbw = qo->td[co];
    cbw->next = ehci_cpu_to_le32(qo->tdP[(co + 1) % QTD_POOL]); cbw->altNext = ehci_cpu_to_le32(EHCI_LINK_TERMINATE);   /* CBW -> data0 */
    cbw->buffer[0] = ehci_cpu_to_le32(gCbwPhys); for (j = 1; j < 5; j++) cbw->buffer[j] = 0;
    gDpUPP = 0; gDpPipe = 0; gDpDest = 0; gDpIsIn = 0; gDpLen = nbytes; gDpMeasured = 0;
    gDpBulkEp = gDev[dv].pIn; gDpDev = dv; gDpLastAddr = (gDev[dv].pOut >= 0) ? gBulkEP[gDev[dv].pOut].addr : 0; gDpLastPid = 0;
    gDpArmTick = TICKS_NOW; gDpBusy = 1;                          /* busy while the terminal CSW qTD is ACTIVE */
    gF4Owner = F4_OWNER_BIO;                                      /* f4: tag the slot */
    __asm__ __volatile__("eieio");                               /* CBW body + data chain + gDp* visible BEFORE CBW Active */
    cbw->token = ehci_cpu_to_le32(cbwTok);                       /* ACTIVATE the CBW LAST -> unpark the OUT chain */
    __asm__ __volatile__("eieio");
    qo->dummy = (UInt8)dn;
}

/* ---- r34 async block-I/O state machine (see the block comment at gBioQ). Uses the same pb_* BOT
 * primitives as the self-probe but advances on completions instead of spinning in pb_wait. ---- */
/* v23: read-after-write verify. gDownBuf holds the just-transferred chunk's data at completion; we
 * fingerprint its first 512B (= block r->lba). On a WRITE we cache lba->fp; on a READ of a cached lba we
 * compare — a mismatch means HFS's read-back of a block it just wrote got data != what we wrote, the
 * suspected trigger for HFS aborting the copy above us. Diagnostic only (side cache; changes no behavior). */
#define RAW_N 32
static UInt32 gRawLba[RAW_N]; static UInt32 gRawFp[RAW_N]; static UInt32 gRawHead = 0;
static volatile UInt32 gRawChecks = 0, gRawMismatch = 0, gRawMLba = 0, gRawMExp = 0, gRawMAct = 0;
static volatile int gRawMPending = 0;
/* v24: write-then-read staleness (DIFFERENT block). Track the LAST write; if a later read's data equals it
 * (but a different block), the read returned stale gDownBuf = the just-written block's bytes. */
static volatile UInt32 gLastWrLba = 0, gLastWrFp = 0, gLastWrW0 = 0;
static volatile UInt32 gStaleN = 0, gStaleRLba = 0, gStaleWLba = 0, gStaleRW0 = 0, gStaleWW0 = 0;
static volatile int gStalePending = 0;
static UInt32 raw_fp(void)
{
    UInt32 h = 0, i; volatile UInt32 *w;
    if (!gDownBuf) return 0;
    w = (volatile UInt32 *)gDownBuf;
    for (i = 0; i < 128; i++) { h = (h << 1) | (h >> 31); h ^= w[i]; }   /* rotate-XOR over first 512B */
    return h ? h : 1;                                                     /* never 0 (0 = empty slot) */
}
static void raw_note_write(UInt32 lba)
{
    UInt32 i, fp = raw_fp();
    gLastWrLba = lba; gLastWrFp = fp; gLastWrW0 = gDownBuf ? *(volatile UInt32 *)gDownBuf : 0;    /* v24: remember last write */
    for (i = 0; i < RAW_N; i++) if (gRawLba[i] == lba && gRawFp[i]) { gRawFp[i] = fp; return; }  /* update existing */
    gRawLba[gRawHead % RAW_N] = lba; gRawFp[gRawHead % RAW_N] = fp; gRawHead++;                   /* else add */
}
static void raw_check_read(UInt32 lba)
{
    UInt32 i, fp = raw_fp();
    /* v24: did this read return the LAST write's data but for a DIFFERENT block? = stale gDownBuf (the read
     * got the just-written block's bytes instead of the block it asked for). The suspected abort trigger:
     * HFS reads block 0 right after writing 0x245a and gets 0x245a's data. */
    if (gLastWrFp > 1 && fp == gLastWrFp && lba != gLastWrLba) {
        gStaleN++;
        if (!gStalePending) { gStaleRLba = lba; gStaleWLba = gLastWrLba;
            gStaleRW0 = gDownBuf ? *(volatile UInt32 *)gDownBuf : 0; gStaleWW0 = gLastWrW0; gStalePending = 1; }
    }
    for (i = 0; i < RAW_N; i++) if (gRawLba[i] == lba && gRawFp[i]) {   /* v23: same-block read-after-write */
        gRawChecks++;
        if (fp != gRawFp[i]) { gRawMismatch++; if (!gRawMPending) { gRawMLba = lba; gRawMExp = gRawFp[i]; gRawMAct = fp; gRawMPending = 1; } }
        return;
    }
}
static void bio_start_chunk(void)                 /* issue the CBW for the current request's next chunk */
{
    BioReq *r = &gBioQ[gBioTail % BIOQ_N];
    UInt32 cap = DOWN_MAX_RBLOCKS;   /* r77: BOTH reads and writes now pre-queue a multi-qTD chain to 128KB (was: writes capped at 40) */
    UInt8 cdb[10]; UInt32 n = (r->count > cap) ? cap : r->count, nbytes = n * 512; int i;
    gBioChunk = n;
    for (i = 0; i < 10; i++) cdb[i] = 0;
    cdb[0] = r->isWrite ? 0x2A : 0x28;            /* WRITE(10) / READ(10) */
    cdb[2]=(UInt8)(r->lba>>24); cdb[3]=(UInt8)(r->lba>>16); cdb[4]=(UInt8)(r->lba>>8); cdb[5]=(UInt8)r->lba;
    cdb[7]=(UInt8)(n>>8); cdb[8]=(UInt8)n;
    if (!r->isWrite) {                                       /* r70: READ -> pre-queue the whole command (1 interrupt) */
        bio_build_cbw(gCbwBuf, cdb, nbytes, 0x80);           /* CBW into the dedicated gCbwBuf (data-IN) */
        bio_issue_read((int)r->dev, nbytes);                              /* CBW-OUT + [data -> CSW]-IN, IOC on CSW only */
        gBioPhase = BIO_PH_PREREAD;
    } else {                                                 /* r77: WRITE -> pre-queue the whole command (1 interrupt) */
        bio_build_cbw(gCbwBuf, cdb, nbytes, 0x00);           /* CBW into gCbwBuf (data-OUT flag) */
        bio_issue_write((int)r->dev, r->buf, nbytes);                     /* OUT:[CBW -> data-chain] + IN:[CSW], IOC on CSW only */
        gBioPhase = BIO_PH_PREWRITE;
    }
}
static void bio_finish(BioReq *r, long res);      /* h81: bio_kick fails a departed device's request here */
static void bio_kick(void)                        /* if idle and the queue is non-empty, start the next request */
{
    if (gBioPhase != 0 || gBioHead == gBioTail) return;
    /* ⚠⚠ h18's GUARD LIVED HERE AND IS REVERTED — IT FROZE THE MACHINE.
     * The observation behind it was correct: bio_issue_read/bio_issue_write arm the hardware DIRECTLY (gDpBusy,
     * gDpQ, gDpTd, gDpBulkEp) rather than going through down_pump, so they overwrite a control transfer already
     * in flight — orphaning it, since nothing then polls its qTD. But `if (gDpBusy) return;` here is the WRONG
     * remedy: it makes block I/O wait on the enumeration engine indefinitely, and when a control transfer does
     * not complete (the still-unexplained h16/h17 failure) the File Manager blocks — which stops LOGGING too,
     * because ehci_os_log is File Manager I/O. On the h18 run the machine froze while enumerating the second
     * hub drive and the log simply ended mid-sequence.
     * ⇒ Before h18, bio clobbering the control transfer was an ACCIDENTAL SAFETY VALVE: the enumeration failed
     * silently and the machine kept running. h18 removed the valve and turned a silent failure into a deadlock.
     * ★★ THE REAL DEFECT IS STRUCTURAL, and arbitrating here cannot fix it: gDpBusy/gDpTd/gDpQ/gDpBulkEp are
     * ONE in-flight slot shared by TWO independent producers (the enumeration engine and the bio engine), and
     * both arm the hardware themselves. Every symptom across h16-h18 is that one fact — enumeration transfers
     * orphaned by bio arming over them, then bio blocked indefinitely by enumeration holding the slot.
     * There is no hardware reason for it: control runs on gCtrlQ and bulk on gDev[d].bulkQ[], different queue
     * heads both already in the async ring, and the controller can execute qTDs on both at once. The
     * serialisation is purely this single software slot. Splitting it is the fix; see the CTL/BULK split. */
    if (!gBioQ[gBioTail % BIOQ_N].ready) return;   /* r+: slot atomically claimed but not yet filled — wait */
    /* ★★★★★★★ h81 — GUARD AT **ISSUE** TIME, NOT ONLY AT SUBMIT TIME. h80 GUARDED THE WRONG POINT.
     *
     * ⚠ h80 put the departed-device check in ehci_usb_submit, and on the m30 run IT FIRED ZERO TIMES while
     * the machine still died on a yanked drive. The reason is the two-stage path: submit only ENQUEUES into
     * gBioQ, and the hardware is armed LATER by bio_kick -> bio_start_chunk -> bio_issue_read, which arms
     * gDp* DIRECTLY and never passes through submit again. So every request the Finder had already queued
     * BEFORE the yank was issued to absent hardware with no re-check — and the m30 tail shows exactly that:
     * "h57 QH HALTED in the OVERLAY", ovlToken 0x02000148 = HALTED|XACTERR with CERR EXHAUSTED, on a
     * 512-byte read. The device was gone; we asked anyway.
     * ★ THE WINDOW IS REAL AND UNAVOIDABLE AT SUBMIT: a request can sit in the ring for as long as the bio
     * engine is busy, and the device can leave at any point in that interval. Only the issue site knows the
     * truth at the moment it matters.
     * ⇒ gDev[dev].inUse is cleared at INTERRUPT level by the disconnect handler, so this check is correct
     * here and cannot be starved. offLinErr is n6e/n25's deliberate code: "off-line drive" tells the File
     * Manager the medium is ABSENT, not DAMAGED, so the volume goes away instead of being called corrupt.
     * ⚠ bio_finish ends by calling bio_kick again, so a ring full of requests for a departed device drains
     * one per re-entry — bounded by BIOQ_N (16) and with small frames, which is the same shape the existing
     * completion path already relies on. */
    {   BioReq *rq = &gBioQ[gBioTail % BIOQ_N];
        if (rq->dev < USB_MAX_DEV && !gDev[rq->dev].inUse) {
            static UInt32 sIssGone = 0;
            if (sIssGone++ < 32)
                ehci_os_ilogx("!! h81: block request ISSUE for a device that has been REMOVED — completing "
                              "offLinErr instead of arming the hardware; slot", (UInt32)rq->dev);
            bio_finish(rq, (long)offLinErr);
            return;
        }
    }
    gBioResult = 0; gBioRetry = 0;                 /* r57: fresh request -> full retry budget */
    bio_start_chunk();
}
/* r57: build a zero-data SETUP packet for a recovery control request (wLength=0). */
static void recov_setup(UInt8 bmRT, UInt8 bReq, UInt16 wValue, UInt16 wIndex)
{
    gRecovSetup[0] = bmRT; gRecovSetup[1] = bReq;
    gRecovSetup[2] = (UInt8)wValue; gRecovSetup[3] = (UInt8)(wValue >> 8);
    gRecovSetup[4] = (UInt8)wIndex; gRecovSetup[5] = (UInt8)(wIndex >> 8);
    gRecovSetup[6] = 0; gRecovSetup[7] = 0;
}
/* v43: the v28 min-gap pacing (gGapUS / throttle_complete / throttle_drain / the DLYQ) is RETIRED. It only
 * ever MASKED the 2GB signed-divide LBA bug (fixed in v41) by throttling the completion rate; with v41+v42 the
 * driver is clean at FULL SPEED, so the throttle machinery is gone and completions go straight out (bio_finish). */
/* v36: nonzero while we are inside IOCommandIsComplete. The File Manager can re-issue the next I/O
 * synchronously from within a completion (see bio_finish's "completion may enqueue more" note); ehci_usb_submit
 * samples this to flag a WRITE born from a completion — the FM-race re-entrancy the source model predicts. */
static volatile UInt32 gInCompletion = 0;
static void complete_now(IOCommandID cmd, long res)
{
    gInCompletion++;
    (void)IOCommandIsComplete(cmd, (OSErr)res);
    gInCompletion--;
}
/* r57: complete the current block request back to the File Manager (res 0=ok, else the failure code). */
static void bio_finish(BioReq *r, long res)
{
    IOCommandID done = r->cmdID;
    /* ★★★★ n25: a request that was IN FLIGHT when its device was yanked must complete with offLinErr, not
     * the engine's generic failure. n6e made a NEWLY SUBMITTED read/write on absent media return offLinErr
     * (-65) — "R/W requested for an off-line drive" — precisely so the File Manager treats the medium as
     * ABSENT rather than BAD. But a transfer already accepted (we returned kIOBusyStatus) completes through
     * here instead, and a failed one carries gBioResult = -36 = ioErr. ioErr on a mounted volume is the File
     * Manager's cue that the DISK IS DAMAGED, which is exactly the alert n6e existed to prevent.
     * Seen on hardware in the n24 run: hard-removing an IDLE drive was silent and clean, while hard-removing
     * one that had just been copied to/from produced the OS's "there may be a problem with the disk" —
     * because that one had I/O in flight. Same defect n6e fixed, on the path n6e did not cover.
     * gDev[dev].inUse is cleared by the disconnect handler, so it is the signal available at interrupt
     * level; it says "this device is no longer attached", which is exactly what offLinErr means. */
    if (res != 0 && r->dev < USB_MAX_DEV && !gDev[r->dev].inUse) res = (long)offLinErr;
    if (r->actCount) *r->actCount = (res == 0) ? (long)r->reqBytes : (long)(r->reqBytes - r->count * 512);
    gBioQ[gBioTail % BIOQ_N].ready = 0;            /* r+: slot empty before dequeue (so it is 0 when reclaimed) */
    gBioTail++; gBioPhase = 0; gBioRetry = 0;      /* dequeue BEFORE completing (completion may enqueue more) */
    complete_now(done, res);                       /* v43: pacing retired — complete immediately (full speed) */
    bio_kick();                                    /* start the next queued request */
}
/* r57: begin BOT reset recovery for the current (timed-out) chunk — issue the Bulk-Only Mass Storage Reset. */
static void bio_recover_start(int d)
{
    UInt32 addr;
    if (gDev[d].pOut < 0 || gDev[d].pIn < 0) return;
    addr = gBulkEP[gDev[d].pOut].addr;
    gDownRecov++;
    /* b12: a recovery START is exactly the event the B&W's full-speed stalls hide at LEVEL1 —
     * the user watches a wristwatch while this machinery cycles. Name it on the '!!' channel. */
    ehci_os_ilogx("!! b12 BOT RECOVERY START — slot<<16|totalRecoveries (a slow/refusing device or a "
                  "timeout budget too tight for it)", ((UInt32)d << 16) | (gDownRecov & 0xFFFFu));
    recov_setup(0x21, 0xFF, 0x0000, 0x0000);       /* class, iface recipient; bRequest 0xFF = Bulk-Only Reset; iface 0 */
    down_submit_recov(gRecovSetup, addr, 8, 2);   /* h75: SETUP on ep0, TAGGED as the recovery's own */
    gBioPhase = REC_RESET_SETUP;
}
/* r64: reset the HOST bulk QH hardware toggles to DATA0, to match the device after a Bulk-Only Reset +
 * CLEAR_FEATURE(HALT). The toggle lives in each bulk QH's overlay and may only be rewritten while the QH is
 * quiescent, so briefly stop the async schedule (EHCI 4.8: clear ASE, wait ASS=0), re-arm both bulk QHs to
 * idle/DATA0, then re-enable (wait ASS=1). The control QH pauses too (~a few ms) — fine, this is the rare
 * recovery path. NOT the IAA doorbell (that's for REMOVING a QH and must not be rung while ASE is disabled).
 * Host and device toggles now agree; skipping this was the r57-r60 reset-loop bug. */
static void bot_reset_host_toggles(int d)
{
    volatile void *op = gSoftc.opBase;
    long spin;
    ehci_write32(op, EHCI_USBCMD, ehci_read32(op, EHCI_USBCMD) & ~EHCI_CMD_ASE);
    for (spin = 0; spin < 200000; spin++)
        if (!(ehci_read32(op, EHCI_USBSTS) & EHCI_STS_ASS)) break;   /* async schedule stopped */
    epq_arm_idle(&gDev[d].bulkQ[0]);           /* bulk-OUT: empty overlay, dt=0 */
    epq_arm_idle(&gDev[d].bulkQ[1]);           /* bulk-IN:  empty overlay, dt=0 */
    __asm__ __volatile__("eieio");
    ehci_write32(op, EHCI_USBCMD, ehci_read32(op, EHCI_USBCMD) | EHCI_CMD_ASE);
    for (spin = 0; spin < 200000; spin++)
        if (ehci_read32(op, EHCI_USBSTS) & EHCI_STS_ASS) break;      /* async schedule running again */
}
/* r57/r64: drive the recovery sequence on each control completion, then retry the command. Order per BOT
 * r1.0 §5.3.4: Bulk-Only Reset, then CLEAR_FEATURE(HALT) bulk-IN, then bulk-OUT, then re-match host toggle. */
static void bio_recover_advance(long status)
{
    BioReq *r = &gBioQ[gBioTail % BIOQ_N];
    int d = (int)r->dev;                    /* n14 step 2: device travels with the request */
    UInt32 addr = (gDev[d].pOut >= 0) ? gBulkEP[gDev[d].pOut].addr : 0;
    if (status != 0) { bio_finish(r, status); return; }   /* ep0 itself unresponsive -> give up on this request */
    switch (gBioPhase) {
    /* h75: every phase below goes out through down_submit_recov so its completion is TAGGED as belonging to
     * this state machine. A plain down_submit here re-creates the m26 hang exactly. */
    case REC_RESET_SETUP:   down_submit_recov(gPB, addr, 0, 1); gBioPhase = REC_RESET_STATUS; break;  /* reset STATUS-IN (0 len) */
    case REC_RESET_STATUS:  recov_setup(0x02, 0x01, 0x0000, (UInt16)(gBulkEP[gDev[d].pIn].endpt | 0x80u));   /* CLEAR_FEATURE(HALT) bulk-IN (IN first) */
                            down_submit_recov(gRecovSetup, addr, 8, 2); gBioPhase = REC_CLRIN_SETUP; break;
    case REC_CLRIN_SETUP:   down_submit_recov(gPB, addr, 0, 1); gBioPhase = REC_CLRIN_STATUS; break;
    case REC_CLRIN_STATUS:  recov_setup(0x02, 0x01, 0x0000, gBulkEP[gDev[d].pOut].endpt);            /* CLEAR_FEATURE(HALT) bulk-OUT */
                            down_submit_recov(gRecovSetup, addr, 8, 2); gBioPhase = REC_CLROUT_SETUP; break;
    case REC_CLROUT_SETUP:  down_submit_recov(gPB, addr, 0, 1); gBioPhase = REC_CLROUT_STATUS; break;
    case REC_CLROUT_STATUS: bot_reset_host_toggles(d);  /* r64: re-match host QH toggles to the device's post-reset DATA0 */
                            bio_start_chunk();         /* RETRY the timed-out chunk from the CBW (gBioPhase -> 1) */
                            break;
    }
}
static void bio_advance(long status)              /* r57: status = down-engine result (0=ok, else timeout/error) */
{
    BioReq *r = &gBioQ[gBioTail % BIOQ_N];
    UInt32 nbytes = gBioChunk * 512, i;
    if (gBioPhase >= REC_BASE) { bio_recover_advance(status); return; }   /* r57: in the recovery sequence */
#if BIO_FORCE_ONE_RECOVERY
    /* r64: once, mid-run, on a healthy CSW, pretend it timed out so recovery fires — validates the reset
     * path on real hardware. The retry re-does one chunk (idempotent re-write of identical data). */
    if (!gForcedRecovDone && gBioPhase == 3 && status == 0 && (gBioWrOk + gBioRdOk) >= 50) {
        gForcedRecovDone = 1; status = -6640L;
    }
#endif
    if (status != 0) {   /* r64: a BOT phase TIMED OUT/errored. Do the correct one-shot Bulk-Only Reset recovery
                          * (bounded), else fail cleanly with the real code. Do NOT read stale gPB (old fake-CSW). */
        if (BIO_BOT_RECOVERY && gBioRetry < BIO_MAX_RETRY && gDev[r->dev].pOut >= 0 && gDev[r->dev].pIn >= 0) { gBioRetry++; bio_recover_start((int)r->dev); }
        else bio_finish(r, status);
        return;
    }
    switch (gBioPhase) {
    case BIO_PH_PREREAD:                          /* r70/r71: whole READ command completed on ONE (CSW) interrupt */
        BlockMoveData((const void *)gDownBuf, r->buf, (Size)nbytes);   /* deliver the read data (up to 128KB) at interrupt level */
        /* fall through to the shared CSW check */
    case BIO_PH_PREWRITE: {                        /* r77: whole WRITE command completed on ONE (CSW) interrupt (data already sent) */
        /* r78 HARDENING: a valid+complete BOT command needs CSW signature 'USBS' + bCSWStatus 0 + ZERO
         * dCSWDataResidue. We used to check only the status byte [12]; a SHORT transfer (device processed
         * fewer bytes than declared, status still 0) or a malformed CSW slipped through as SUCCESS -> silent
         * HFS inconsistency = the prime suspect for the intermittent Finder "disk error" on used/fragmented
         * volumes. Now any of the three fails the chunk, and biofail records failSeq/residue/LBA (persistent
         * globals) so a recurrence is CAPTURED (survives the app-starved-during-a-real-copy problem) not silent.
         * Safe: every currently-passing transfer has residue 0 + a valid signature (the byte-compare verify
         * proves the data was complete), so this adds no false failures. */
        UInt32 resid = (UInt32)gCswBuf[8] | ((UInt32)gCswBuf[9] << 8) | ((UInt32)gCswBuf[10] << 16) | ((UInt32)gCswBuf[11] << 24);
        int sigOk = (gCswBuf[0] == 0x55 && gCswBuf[1] == 0x53 && gCswBuf[2] == 0x42 && gCswBuf[3] == 0x53);   /* 'USBS' */
        if (gCswBuf[12] != 0 || resid != 0 || !sigOk) {         /* status!=passed OR short transfer OR bad CSW sig */
            for (i = 0; i < 13; i++) gPB[i] = gCswBuf[i];       /* mirror the failing CSW to gPB for biofail's snapshot */
            gBioResult = -36L; biofail(r->isWrite, r->lba, r->submitLba, gBioRetry, (UInt16)gBioChunk);
        } else { if (r->isWrite) gBioWrOk++; else gBioRdOk++; gBioRetry = 0;
                 if (r->isWrite) raw_note_write(r->lba); else raw_check_read(r->lba); }   /* v23: read-after-write verify (gDownBuf = this chunk's data) */
        r->lba += gBioChunk; r->buf += gBioChunk * 512; r->count -= gBioChunk;
        if (gBioResult == 0 && r->count > 0) bio_start_chunk();  /* more chunks in this request */
        else bio_finish(r, gBioResult);                          /* done (or CSW-failed) -> complete */
        break;
    }
    /* ★ n20: these three legacy multi-phase cases serve request r, so they must use r->dev. They passed a
     * literal 0, which would have sent a second device's data phase to device 0 even once r->dev was set
     * correctly. The primary path (BIO_PH_PREREAD/PREWRITE via bio_start_chunk) always routed on r->dev;
     * this fallback did not, so the fix is incomplete without it. */
    case 1:                                       /* CBW done -> data phase */
        if (r->isWrite) { for (i = 0; i < nbytes; i++) gPB[i] = r->buf[i]; pb_out((int)r->dev, nbytes); }  /* stage+send write data */
        else pb_in((int)r->dev, nbytes);                                                                    /* read data into gPB */
        gBioPhase = 2; break;
    case 2:                                       /* data done -> (read: copy in) then CSW */
        if (!r->isWrite) { for (i = 0; i < nbytes; i++) r->buf[i] = gPB[i]; }
        pb_in((int)r->dev, 13); gBioPhase = 3; break;
    case 3:                                       /* CSW done -> check, advance chunk, or complete */
        if (gPB[12] != 0) { gBioResult = -36L; biofail(r->isWrite, r->lba, r->submitLba, gBioRetry, (UInt16)gBioChunk); }  /* real CSW status (the IN xfer succeeded) */
        else { if (r->isWrite) gBioWrOk++; else gBioRdOk++; gBioRetry = 0; }                       /* r57: chunk OK -> refresh retry budget */
        r->lba += gBioChunk; r->buf += gBioChunk * 512; r->count -= gBioChunk;
        if (gBioResult == 0 && r->count > 0) bio_start_chunk();        /* more chunks in this request */
        else bio_finish(r, gBioResult);                               /* done (or CSW-failed) -> complete */
        break;
    }
}
/* Async submit — the block driver's kRead/kWrite call this and return kIOBusyStatus. 0=accepted,
 * 1=nothing-to-do (complete noErr), -1=queue full (complete with error). */
static long ehci_usb_submit(int dev, IOCommandID cmdID, UInt32 lba, UInt32 count, void *buf, int isWrite, long *actCount)
{
    UInt32 depth = gBioHead - gBioTail; BioReq *r;
    /* ★ n20: `dev` crosses the 'Eusb' ABI from the block driver's own fragment, and gDev[] holds only
     * USB_MAX_DEV entries while the ABI's arrays are sized EUSB_MAX_DEV (4) so raising one does not move the
     * other. Validate before the value can index anything. REFUSE an out-of-range slot — never clamp it to 0.
     * Clamping to 0 is exactly the defect this build fixes, and its consequence is a write to the wrong disk.
     * ehci_os_ilog (not ehci_os_log) because the File Manager re-issues I/O from inside our completion, so
     * this function is also entered at interrupt level. */
    if (dev < 0 || dev >= USB_MAX_DEV) {
        ehci_os_ilog("!! n20 submit for an out-of-range device slot - refusing");
        ehci_os_ilogx("  dev", (UInt32)dev);
        return -1;
    }
    /* ★★★★★★★ h80 — REFUSE I/O FOR A DEVICE THAT IS PHYSICALLY GONE, IMMEDIATELY AND WITHOUT TASK LEVEL.
     *
     * ⚠ THE m29 RUN-4 FAULT, and it is the "crash" the user hit by opening a volume whose stick had been
     * pulled without ejecting. n6e already makes reads fail with offLinErr "once the media is gone" — but
     * the media-gone flag lives in the BLOCK DRIVER and is set by blk_notify_media(), which issues a
     * synchronous Device Manager call and therefore runs at TASK LEVEL, off gSelfEnumRearm. On that run
     * the task-level pump was taking 106.9 SECONDS per turn, so the flag was never set: the medium still
     * read PRESENT, the Finder's reads were ACCEPTED, issued to a device that was not there, and each one
     * then sat on the 10 s transfer watchdog. gDownErr reached 2 and the machine looked hung. It was the
     * File Manager blocking on reads that could never complete.
     *
     * ★ THE UIM ALREADY KNOWS BETTER, AND KNOWS IT AT INTERRUPT LEVEL. gDev[dev].inUse is cleared by the
     * disconnect handler in service_ports the moment the port reports the device gone — no pump, no File
     * Manager, no waiting. Consulting it here gives n6e's protection on a path that CANNOT be starved.
     * ⇒ offLinErr (-65) is exactly right and is the code n6e/n25 chose deliberately: "R/W requested for an
     * off-line drive" tells the File Manager the medium is ABSENT, not DAMAGED, so the OS reports a removed
     * disk instead of the "there may be a problem with the disk" alert that a generic ioErr produces.
     * ⚠ ehci_os_ilog, not ehci_os_log: the File Manager re-issues I/O from inside our completion, so this
     * function is also entered at INTERRUPT level (see the n20 note above). */
    if (!gDev[dev].inUse) {
        static UInt32 sGoneN = 0;
        if (sGoneN++ < 32)          /* bounded: a Finder that keeps asking must not flood the ring */
            ehci_os_ilogx("!! h80: block I/O for a device that has been REMOVED — refusing with offLinErr "
                          "instead of issuing it and waiting out the 10 s watchdog; slot", (UInt32)dev);
        return (long)offLinErr;
    }
    if (depth > gBioHiWater) gBioHiWater = depth;       /* r49: track peak ring occupancy for the thread-B diagnostic */
    { int save = gInSubmit; gInSubmit = save + 1;       /* r+ re-entrancy detector (nested via IOCommandIsComplete) */
      if (save) { gSubmitReentry++; if ((UInt32)gInSubmit > gSubmitMaxDepth) gSubmitMaxDepth = (UInt32)gInSubmit; } }
    if (count == 0)      { gInSubmit--; return 1; }
    if (depth >= BIOQ_N) {                                            /* r41: count queue-full submit rejections */
        gBioReject++;
        { static UInt32 rjN = 0; if (rjN++ < 64) {                   /* v19: bounded per-event detail (task level -> ehci_os_log safe); gBioReject = true total */
            ehci_os_log("!! v19 SUBMIT REJECT (ring full)");
            ehci_os_logx("  depth", depth);
            ehci_os_logx("  lba", lba);
            ehci_os_logx("  count", count);
            ehci_os_logx("  isWrite", (UInt32)(isWrite ? 1 : 0)); } }
        gInSubmit--; return -1;
    }
    if (isWrite) {                                   /* v36: FM-race write instrumentation (never-wrap capture) */
        UInt32 fl = 0; gWrTotal++;
        if (gInCompletion) fl |= 1;                  /* born inside our completion => the FM re-issued it (re-entrancy) */
        /* ★ n24 SWEEP FINDING 3: bound-check against THIS device's capacity. gPBlkCnt is a macro for
         * gDevBlkCnt[0], so every device's writes were checked against SLOT 0's block count: false "LBA off
         * the device" flags for a larger drive, and missed ones for a smaller. Diagnostic-only — it sets a
         * flag rather than rejecting the write — but a diagnostic that lies has cost this project cycles
         * before, and the per-device geometry it should have used has existed since n18. */
        UInt32 cap = gDevBlkCnt[dev];
        if (cap && (lba >= cap || (lba + count) > cap || (lba + count) < lba)) fl |= 2;  /* LBA off the device */
        if (fl) { UInt32 k = gSuspN; if (k < 16) { gSuspLba[k] = lba; gSuspCnt[k] = count; gSuspFlags[k] = fl; } gSuspN++; }
    }
    {   /* r+ RACE FIX: called RE-ENTRANTLY (File Mgr re-issues the next I/O from inside IOCommandIsComplete
         * at interrupt level, nested in a task-level submit). Claim the slot ATOMICALLY so nested producers
         * get DISTINCT slots — the old fill-then-gBioHead++ let two grab the SAME slot -> one BioReq left
         * GARBAGE -> a write with a garbage LBA/count -> silent corruption, only under a many-small-file
         * copy's rapid completions. Consumer gates on the per-slot 'ready' flag (set after the fill). */
        UInt32 myHead = (UInt32)IncrementAtomic((SInt32 *)&gBioHead);   /* atomic fetch-and-increment */
        UInt32 slot = myHead % BIOQ_N;
        r = &gBioQ[slot];
        r->cmdID = cmdID; r->lba = lba; r->submitLba = lba; r->count = count; r->reqBytes = count * 512;
        r->buf = (UInt8 *)buf; r->actCount = actCount; r->isWrite = (UInt8)(isWrite ? 1 : 0);
        /* ★★★★ n20 THE FIX. n14 stamped this 0 with a note that "step 4 gives it a per-device drive number
         * and passes the real slot in". Step 4 (n19) added the `dev` parameter — the very change that forced
         * the 'EUS2' ABI bump — and then never came back here, so EVERY mounted-volume read and write went to
         * device 0 no matter which drive the File Manager named. Two drives therefore showed the SAME volume
         * (n19 hardware run: the SanDisk mounted as a second copy of the generic stick, and only mounted
         * correctly once the generic was pulled and it re-enumerated into slot 0).
         * Everything downstream was already correct — bio_start_chunk routes on r->dev — so this one
         * assignment is what connects the block driver's per-slot routing to the per-device engine. */
        r->dev = (UInt8)dev;
        __asm__ __volatile__("eieio");                     /* publish BioReq fields BEFORE ready */
        r->ready = 1;
    }
    gInSubmit--;
    /* r48 RE-ENTRANCY FIX (thread B — Finder large-copy hard-crash): do NOT bio_kick() here.
     * This runs at TASK level (File Mgr kRead/kWrite). bio_kick -> bio_start_chunk mutates the shared
     * bio+down engine (gPB, gDownQH, gBioPhase) — the SAME state the INTERRUPT-level completion path
     * (down_reap -> bio_advance, and the request the File Mgr re-issues from inside IOCommandIsComplete)
     * drives. A Finder copy interleaves rapid reads+writes; starting a transfer from task level while
     * the controller is DMA-executing the async QH corrupts it -> hard crash. So the task side now only
     * APPENDS (single producer: only this fn writes gBioHead). The engine is kicked SOLELY from interrupt
     * level — bio_advance on each completion, and ehci_vhub_service() to start an idle engine. */
    return 0;
}

/* ★★★★ n19 STEP 4 ABI. Every data-path call now names its DEVICE. The struct below is mirrored in
 * usb_disk.c and THE TWO MUST STAY IN SYNC; magic2 catches a layout drift but CANNOT catch a changed
 * function signature, which is a silent break. So `magic` is bumped to 'EUS2' as well: a block driver
 * built against the old layout refuses to bind rather than calling through with the wrong arguments.
 * ⇒ the ROM and the activator are a PAIR and must be installed together. */
typedef long (*ehci_usb_rw_fn)(int dev, UInt32 lba, UInt32 count, void *buf);
typedef long (*ehci_usb_submit_fn)(int dev, IOCommandID cmdID, UInt32 lba, UInt32 count, void *buf, int isWrite, long *actCount);
typedef void (*ehci_usb_health_fn)(UInt32 *reject, UInt32 *hiwater, UInt32 *downTimeouts, UInt32 *downErr, UInt32 *downDone,
                                   UInt32 *failSeq, UInt32 *failStat, UInt32 *failSig, UInt32 *failLba, UInt32 *isrHits, UInt32 *maxStall,
                                   UInt32 *downRecov, UInt32 *downRelink, UInt32 *lastAnchorLink,
                                   UInt32 *dataBytes, UInt32 *dataFrames);
typedef UInt32 (*ehci_usb_tostate_fn)(UInt32 *cmd, UInt32 *sts, UInt32 *async, UInt32 *qhP, UInt32 *epChar, UInt32 *curQtd, UInt32 *ovlTok, UInt32 *qtdTok);   /* r56: controller state captured at the last watchdog timeout */
typedef UInt32 (*ehci_usb_sim_fn)(UInt32 n);   /* r81: hot-replug async-schedule teardown/rebuild isolation test */
typedef void   (*ehci_usb_arm_fn)(void);       /* r85: arm the [obs] probe (post-desktop) */
typedef void   (*ehci_usb_crumb_fn)(UInt32 tag); /* r94: app idle-loop breadcrumb (diagnostic) */
typedef long (*ehci_usb_quit_fn)(void);
typedef void (*ehci_usb_drain_fn)(void);
typedef void (*ehci_usb_eject_fn)(int dev);   /* ★ n24: the SLOT the Finder ejected, so the alert names it */
void ehci_vhub_notify_ejected(int dev);   /* n24: takes the ejected SLOT. n8: clean-quit hook, appended LAST so the block
                                           * driver's shorter prefix view of this struct is unaffected */
long ehci_vhub_prepare_quit(void);
/* ★ n19: EUSB_MAX_DEV is pinned at 4 (the DMA page ceiling) and is deliberately INDEPENDENT of
 * USB_MAX_DEV. Raising USB_MAX_DEV later then costs no further ABI break, because the array sizes here
 * do not move. present[] says which slots hold a probed device; blkCnt[] is 0 for the rest. */
#define EUSB_MAX_DEV 4
static struct { UInt32 magic; ehci_usb_rw_fn readFn; ehci_usb_rw_fn writeFn;
                UInt32 blkSize[EUSB_MAX_DEV], blkCnt[EUSB_MAX_DEV]; UInt8 present[EUSB_MAX_DEV];
                UInt32 devCount; ehci_usb_submit_fn submitFn; ehci_usb_health_fn healthFn;
                ehci_usb_tostate_fn toStateFn; ehci_usb_sim_fn simReplugFn; ehci_usb_arm_fn obsArmFn;
                ehci_usb_arm_fn tickFn; ehci_usb_crumb_fn loopFn; ehci_usb_quit_fn quitFn;
                ehci_usb_drain_fn drainFn; ehci_usb_eject_fn ejectFn; UInt32 magic2; } gSvc;
static void ehci_vhub_publish_service(void)
{
    gSvc.magic = 0x45555332UL;  /* ★ n19: 'EUS2'. Bumped WITH the layout+signature change so a block
                                 * driver built against the old ABI fails the magic check and refuses to
                                 * bind, instead of calling readFn with the wrong argument list. */
    gSvc.readFn = ehci_usb_read;
    gSvc.writeFn = ehci_usb_write;
    {   /* ★ n19: per-device geometry. A device that has not probed reports blkCnt 0 and present 0. */
        int _d; gSvc.devCount = 0;
        for (_d = 0; _d < EUSB_MAX_DEV; _d++) {
            int live = (_d < USB_MAX_DEV) && gDev[_d].inUse && gDevBlkCnt[_d];
            gSvc.blkSize[_d] = live ? gDevBlkSize[_d] : 512;
            gSvc.blkCnt[_d]  = live ? gDevBlkCnt[_d]  : 0;
            gSvc.present[_d] = (UInt8)(live ? 1 : 0);
            if (live) gSvc.devCount++;
        }
    }
    gSvc.submitFn = ehci_usb_submit;               /* r34: async path for kRead/kWrite */
    gSvc.healthFn = ehci_vhub_health;              /* r49: engine health for the app idle-loop diagnostic */
    gSvc.toStateFn = ehci_vhub_timeout_state;      /* r56: controller state at the last 60s stall */
    gSvc.simReplugFn = ehci_vhub_simulate_replug;  /* r81: hot-replug isolation test (async-schedule surgery) */
    gSvc.obsArmFn = ehci_vhub_obs_arm;             /* r85: app arms the [obs] probe on reaching the observe loop */
    gSvc.tickFn = ehci_vhub_selfprobe_tick;        /* r92: app idle-loop drives the self-probe directly. The USL
                                                    * pump (USLPolledProcessDoneQueue -> slot 23 -> selfprobe_tick)
                                                    * DROPS our bus after a reinsert (r91 logs: uim6/uim7 keep
                                                    * firing but uim23 goes silent at the RECONNECT line), so the
                                                    * reconnect self-probe never re-ran. This pointer lets the
                                                    * (proven-alive) app loop tick it regardless of the USL. */
    gSvc.drainFn = ehci_os_ilog_drain;             /* n9: the BLOCK DRIVER drains our log ring — see below */
    gSvc.ejectFn = ehci_vhub_notify_ejected;       /* n10: block driver calls this on Finder eject */
    gSvc.magic2  = 0x45555332UL;   /* n19: matches magic */                   /* n9: END-marker. The block driver verifies THIS before
                                                    * touching drainFn, so if the two fragments' views of
                                                    * this struct ever drift it fails safe instead of
                                                    * calling a garbage pointer. */
    gSvc.quitFn = ehci_vhub_prepare_quit;          /* n8: activator calls this before exiting */
    gSvc.loopFn = ehci_vhub_loopcrumb;             /* r94: app idle-loop breadcrumb (armed only around a reconnect) */
    (void)NewGestaltValue('Eusb', (long)&gSvc);
    ehci_os_log("=== r34: USB block service published via Gestalt 'Eusb' (sync rw + async submit) ===");
}

/* ★★★★ h24: publish 'Eusb' AS SOON AS THE CONTROLLER IS UP, from uimInitialize, before any device has
 * enumerated. Until now the ONLY callers were the post-probe handoff and the legacy sync probe, so the
 * selector did not exist until a drive had already mounted.
 *
 * ★★ WHY, and it is a real chicken-and-egg that voided the n4h USL-pump run (2026-08-08): an external
 * vehicle can only reach this driver through Gestalt('Eusb') — there is no CFM link into a ROM parcel, and
 * DoDriverIO is never called (n0). The n4h experiment's idle loop therefore did
 *     if (Gestalt('Eusb', &v) == noErr && v) { sv->tickFn(); }
 * which can NEVER fire before the first mount, because the publication it needs is done BY the first mount,
 * and the first mount needs the tick. The hardware log is unambiguous: init complete, then not one
 * portmap_tick baseline line and not one tick marker for the rest of the session. Nothing ran at all, which
 * is also why nothing froze.
 *
 * ⚠ SAFE BEFORE A MOUNT, checked rather than assumed:
 *  - the geometry table already handles it — a slot that has not probed reports blkCnt 0 and present 0, and
 *    devCount stays 0, so nothing reads as mountable;
 *  - the block driver is not installed yet, so it cannot bind to a half-ready service (install_block_driver
 *    still runs only on the probe-success path, which is where the n5 "endpoints missing" guard lives);
 *  - the one shipping behaviour that changes is the activator's Cmd-Q: ask_driver_to_prepare_quit() treats a
 *    missing selector as "nothing mounted by us -> safe", and will now call quitFn for real. That is
 *    harmless — ehci_vhub_prepare_quit -> blk_unmount_our_volumes2 returns 0 on `if (!gBlkDref) return 0;`
 *    with no block driver installed, so it walks no VCBs and unmounts nothing. It costs one log line.
 * ★ NewGestaltValue on an already-registered selector simply fails, and the pointer never changes (&gSvc),
 * so the per-device republication below keeps working exactly as before. */
void ehci_vhub_publish_service_early(void)
{
    ehci_vhub_publish_service();
    ehci_os_log("=== h24: 'Eusb' published AT INIT (before any device) — an external pump can now find "
                "tickFn. The n4h run failed here: no selector meant no tick, and no tick meant no mount. ===");
}

/* ==================== v1 hot re-mount: engine teardown / rebuild (r81) ====================
 * The reliability-critical core of hot re-mount is stopping and restarting the async schedule at
 * runtime. We do NOT unlink our QHs from the ring (the r45/r60 lesson: the ring is ours, resident,
 * and the controller never breaks it) — we quiesce ASE exactly as the proven BOT-recovery path
 * (bot_reset_host_toggles) does, reprogram the resident QHs while stopped, then re-run. This is the
 * same schedule surgery the device-toggle recovery has performed reliably on hardware since r64. */
void ehci_vhub_engine_teardown(int resetToggles)
{
    volatile void *op = gSoftc.opBase;
    long spin;
    /* EHCI 4.8: stop the async schedule and wait for the controller to actually park it. */
    ehci_write32(op, EHCI_USBCMD, ehci_read32(op, EHCI_USBCMD) & ~EHCI_CMD_ASE);
    for (spin = 0; spin < 200000; spin++)
        if (!(ehci_read32(op, EHCI_USBSTS) & EHCI_STS_ASS)) break;   /* async schedule stopped */
    if (resetToggles) {                        /* real re-plug: the device reset its toggles -> DATA0 */
        { int _d; epq_arm_idle(&gCtrlQ);      /* n18: idle EVERY device's bulk pair, not just slot 0's.
                                              * A teardown that left slot 1's queue heads armed with
                                              * stale state would resume them on the next transfer. */
          for (_d = 0; _d < USB_MAX_DEV; _d++) {
              epq_arm_idle(&gDev[_d].bulkQ[0]); epq_arm_idle(&gDev[_d].bulkQ[1]);
          } }
    }
    __asm__ __volatile__("eieio");
    /* Engine + BOT state back to idle. Endpoint identity (gDev[0].pOut/gDev[0].pIn/gBulkEP) and the QH DMA pages
     * are preserved — enumeration owns those, not us. NOTE (C-phase TODO): the real disconnect path
     * must FAIL-COMPLETE any in-flight gBioQ request (IOCommandIsComplete with an error) rather than
     * just dropping it; here the caller guarantees the engine is idle first, so draining is safe. */
    gDpBusy = 0; gEgBusy = 0;                  /* THE SPLIT: a teardown must idle BOTH slots, or the engine
                                                * slot stays latched and down_pump never issues again */
    gBioPhase = 0; gBioRetry = 0; gBioTail = gBioHead; { int _i; for (_i=0;_i<BIOQ_N;_i++) gBioQ[_i].ready = 0; }
    gDownReady = 0; gDownAseOn = 0;            /* rebuild re-enables ASE + readiness */
}
void ehci_vhub_engine_rebuild(UInt32 ctrlAddr)
{
    volatile void *op = gSoftc.opBase;
    long spin; int i;
    /* Re-point the resident QHs (epChar only; the overlay/toggle is left as-is here — teardown owns
     * the toggle decision). Control ep0 shares the device address; each bulk QH from gBulkEP. */
    epq_program(&gCtrlQ, ctrlAddr, 0, 64, 1);
    for (i = 0; i < NBULK; i++)
        if (gBulkEP[i].used)
            epq_program(&gDev[0].bulkQ[gBulkEP[i].dirIn ? 1 : 0], gBulkEP[i].addr, gBulkEP[i].endpt,
                        gBulkEP[i].maxpkt ? gBulkEP[i].maxpkt : 512, 0);
    __asm__ __volatile__("eieio");
    gDownReady = 1;
    gDownAseOn = 0; down_arm_ase();            /* re-enable ASE (down_arm_ase sets the bit + gDownAseOn) */
    for (spin = 0; spin < 200000; spin++)
        if (ehci_read32(op, EHCI_USBSTS) & EHCI_STS_ASS) break;   /* async schedule running again */
}
/* Loop teardown(0)->rebuild->one BOT READ(10) of block 0, n times, WITHOUT a physical pull: the device
 * stays live at its current address, so we keep the toggle (resetToggles=0). Each cycle verifies the CSW
 * is good AND the block-0 boot signature is unchanged from a pre-teardown baseline. A clean pass proves
 * the async-schedule stop/start surgery is safe in isolation, before it is wired to a real port event.
 * Returns the pass count and logs one <1KB summary. Reached from the app via the 'Eusb' simReplugFn. */
UInt32 ehci_vhub_simulate_replug(UInt32 n)
{
    static UInt8 blk[512];
    UInt32 i, passes = 0, sig0 = 0xFFFFFFFFUL, addr;
    long r;
    if (gDev[0].pOut < 0 || gDev[0].pIn < 0) { ehci_os_log("[replug] no bulk endpoints — skipped"); return 0; }
    addr = gBulkEP[gDev[0].pIn].addr;                 /* device address (ep0 + bulk share it) */
    r = ehci_usb_read(0, 0, 1, blk);           /* n19: (dev, lba, count, buf). Slot 0: this is a diagnostic. */
    if (r >= 0) sig0 = ((UInt32)blk[510] << 8) | blk[511];
    for (i = 0; i < n; i++) {
        ehci_vhub_engine_teardown(0);          /* quiesce; keep toggle (device still live) */
        ehci_vhub_engine_rebuild(addr);        /* re-enable the async schedule */
        r = ehci_usb_read(0, 0, 1, blk);       /* n19: (dev, lba, count, buf) */
        if (r >= 0 && (((UInt32)blk[510] << 8) | blk[511]) == sig0) passes++;
    }
    ehci_os_log("=== r81 [replug] async-schedule teardown/rebuild isolation test ===");
    ehci_os_logx("  cycles requested", n);
    ehci_os_logx("  passed (read OK + sig stable)", passes);
    ehci_os_logx("  blk0 sig (55AA = FAT boot)", sig0);
    ehci_os_logx("  downErr (cumulative)", gDownErr);
    ehci_os_logx("  downTimeouts (cumulative)", gDownTimeouts);
    return passes;
}

/* Phase 1 r87 (hot re-mount): stop the async schedule so a QH reprogram (create_bulk's epq_program) is safe
 * on a live ring — the r84/r85 freeze. Guarded on gDownAseOn so it is a no-op when the schedule is already
 * stopped (e.g. early boot). The next transfer re-arms ASE via down_arm_ase. */
/* ★★★★★ INSTRUMENTED: how long does ase_quiesce ACTUALLY spin?
 *
 * WHY THIS MATTERS, and it is the one number the app-less design hangs on. The RE of Apple's
 * USBMassStorageSupport (docs/APPLE-UMSS-RE.md) established that Apple gets to task level with
 * NMInstall + nmStr=0, whose response procedure runs inside SOME APPLICATION'S EVENT LOOP. If we adopt
 * that, the bulk-endpoint registration moves there, and create_bulk calls THIS function, whose bound is
 * 200,000 MMIO reads of USBSTS. Apple never does anything remotely like that from an nmResp — their class
 * drivers never reprogram a live async schedule — so this is precisely where our needs exceed theirs, and
 * it has to be measured rather than assumed.
 *
 * WHAT IS MEASURED, and why in two units:
 *   - ITERATIONS is the exact figure, directly comparable to the 200,000 bound, and needs no clock.
 *   - TIMEBASE TICKS give real time. Deliberately via UpTime(), which this driver ALREADY calls on the
 *     heartbeat path, rather than AbsoluteDeltaToNanoseconds (InterfaceLib, which we should not enter from
 *     a driver path) or a raw mftb (this project has a scar from privileged-SPR reads faulting).
 *     Only the low word is taken: a quiesce is microseconds and cannot wrap it.
 *   - The tick rate is machine-specific, so it self-calibrates on the heartbeat, which already reads
 *     UpTime() and already advances the microframe accumulator. gAseTbSpan/gAseUfSpan are that calibration
 *     pair; 1 microframe = 125 us, so us = ticks * 125 * gAseUfSpan / gAseTbSpan. No division here.
 *
 * ★ AND IT CAPTURES A SILENT FAILURE THAT HAS BEEN THERE ALL ALONG: if the loop exhausts its 200,000
 * iterations WITHOUT the schedule stopping, it falls through and reprograms a QH on a still-running ring
 * anyway, with nothing logged. That is the documented r84/r85 freeze shape. gAseTimeouts makes it loud.
 * The early return when the schedule is already stopped is counted separately, because most calls take it
 * and averaging them in would flatter the result. */
static volatile UInt32 gAseCalls = 0, gAseSkip = 0, gAseSpun = 0, gAseTimeouts = 0;
static volatile UInt32 gAseIterLast = 0, gAseIterMax = 0, gAseIterSum = 0;
static volatile UInt32 gAseTbLast = 0, gAseTbMax = 0;
static volatile UInt32 gAseTbSpan = 0, gAseUfSpan = 0;   /* calibration, filled on the heartbeat */
static volatile UInt32 gAseTimeoutKind = 0;              /* 1 = the 10 ms time bound, 2 = the iteration cap */

/* ★★★★★ BOUNDED BY TIME, NOT BY ITERATION COUNT. Measured on hardware 2026-08-07 (MDD, h22):
 * worst case 1718 iterations = 1.55 ms, mean 950 = 0.85 ms, 0 timeouts, at 0.88 us per MMIO read.
 *
 * WHY THIS CHANGED. The old bound was 200,000 iterations, which at that measured cost is ~181 ms. Nothing has
 * ever come near it (the worst case observed is 0.86% of it), but app-less moves this call inside a
 * Notification Manager response procedure, which runs in SOME APPLICATION'S EVENT LOOP -- and an nmResp has to
 * be bounded by what the code ALLOWS, not by what it has happened to do. A sixth of a second of frozen event
 * loop is not acceptable there; 10 ms is, and is still ~6x the worst case ever seen.
 *
 * WHY FRINDEX AND NOT A SOFTWARE CLOCK. FRINDEX is the controller's own microframe counter, 125 us per tick.
 * It needs no calibration (unlike UpTime, whose rate is machine-specific), it is available immediately at boot
 * (unlike our heartbeat-fed frame_ms, which would deadlock this loop if the heartbeat could not run), and it is
 * one more read of a register we are already polling. 14 bits, wrapping every 2.048 s, so the masked
 * subtraction is wrap-safe for a 10 ms window.
 *
 * ★ AND THE FROZEN-COUNTER CASE IS NOT A HANG: FRINDEX only advances while the controller is running, and a
 * controller that is not running has ASS clear, so the very first status read below exits the loop. The
 * iteration cap is therefore pure paranoia -- kept, but lowered from 200,000 to 20,000 (~11x the worst case
 * ever observed) so that even the impossible case is bounded work.
 * The clock is read once per 256 status reads (~225 us at the measured cost), which bounds the overshoot to
 * well under the 10 ms budget while adding ~0.4% to the loop. */
#define ASE_QUIESCE_LIMIT_US   10000UL
#define ASE_QUIESCE_LIMIT_UF   (ASE_QUIESCE_LIMIT_US / 125UL)   /* microframes = 80 */
#define ASE_QUIESCE_ITER_CAP   20000L                            /* backstop only; see above */

static void ase_quiesce(void)
{
    volatile void *op = gSoftc.opBase;
    long spin;
    UInt32 tb0, uf0;
    int timedOut = 0;
    gAseCalls++;
    if (!gDownAseOn) { gAseSkip++; return; }
    tb0 = UpTime().lo;
    uf0 = ehci_read32(op, EHCI_FRINDEX) & 0x3FFFUL;
    ehci_write32(op, EHCI_USBCMD, ehci_read32(op, EHCI_USBCMD) & ~EHCI_CMD_ASE);
    for (spin = 0; spin < ASE_QUIESCE_ITER_CAP; spin++) {
        if (!(ehci_read32(op, EHCI_USBSTS) & EHCI_STS_ASS)) break;   /* the schedule actually stopped */
        if ((spin & 0xFF) == 0xFF) {                                 /* check the clock 1 read in 256 */
            UInt32 d = (ehci_read32(op, EHCI_FRINDEX) - uf0) & 0x3FFFUL;
            if (d >= ASE_QUIESCE_LIMIT_UF) { timedOut = 1; break; }
        }
    }
    if (spin >= ASE_QUIESCE_ITER_CAP) timedOut = 2;                  /* the paranoia backstop fired */
    {   UInt32 dt = UpTime().lo - tb0;                               /* unsigned: wrap-safe for a short span */
        gAseTbLast = dt; if (dt > gAseTbMax) gAseTbMax = dt; }
    gAseSpun++;
    gAseIterLast = (UInt32)spin; gAseIterSum += (UInt32)spin;
    if ((UInt32)spin > gAseIterMax) gAseIterMax = (UInt32)spin;
    /* ★ Still the same silent failure it always was: we are about to reprogram a QH on a ring that did not
     * stop, which is the documented r84/r85 freeze shape. Now it is bounded AND counted, and the two causes
     * are distinguished so a future log says which limit was hit. */
    if (timedOut) { gAseTimeouts++; gAseTimeoutKind = (UInt32)timedOut; }
    gDownAseOn = 0;
}
/* Phase 1 r87 (hot re-mount, bounce-robust): reset the stale device state on a POST-MOUNT re-enumeration at a
 * new address — a reinsert, OR one of the SanDisk's re-enumeration bounces (r86 showed the reinsert bounces
 * through several addresses, and the engine locking onto a bounced-past stale address is what crashed). Fires
 * on EVERY address change (create_bulk gate: gMountedOnce && addr != gCurAddr), so each bounce is handled and
 * only the CURRENT device's endpoints survive in gBulkEP. MUST run with the schedule already stopped
 * (create_bulk calls ase_quiesce first) so the epq_arm_idle below is safe. Leaves ctrl ep0 alone (per-qTD
 * toggle + per-transfer re-address) so the ongoing enumeration's control transfers are not disturbed. */
static void reconnect_reset(int d)
{
    int i;
    ehci_os_log("=== RECONNECT: post-mount re-enumeration at a new address — reset stale state (r88) ===");
    epq_arm_idle(&gDev[d].bulkQ[0]); epq_arm_idle(&gDev[d].bulkQ[1]);   /* fresh device => bulk toggles back to DATA0 */
    gDpBusy = 0; gEgBusy = 0;   /* THE SPLIT: both slots — see the teardown note in the rebuild path */
    gBioPhase = 0; gBioRetry = 0; gBioTail = gBioHead; { int _i; for (_i=0;_i<BIOQ_N;_i++) gBioQ[_i].ready = 0; }   /* idle the BOT/down engine */
    gFenceApple = 0;                                     /* r93: do NOT fence during Apple's reconnect probing. r90 set this to
                                                          * 1 immediately, to "free the pump" for a self-probe we now know was
                                                          * never being CALLED post-reconnect (not starved) — r92's tickFn is
                                                          * what fixes the driving. Fencing here rejected Apple's reconnect
                                                          * TUR/REQUEST SENSE with -6640, and Apple's error handling on that
                                                          * intermittently HUNG the machine (r92 froze there; r91 bounced past
                                                          * it). Boot never fences during probing — it lets Apple stall+park,
                                                          * THEN the self-probe takes over and fences. Mirror boot: stay
                                                          * unfenced until the passive park-wait sets the fence itself. */
    gDev[d].pstate = 0; gPIdle = 0; gPLastCnt = 0;              /* self-probe re-runs from the top (passive park-wait, as at boot) */
    gDev[d].probedPort = -1;                             /* n13: no device is probed once the state is reset */
    gDev[d].pOut = gDev[d].pIn = -1;                                   /* pb_find_eps re-selects the CURRENT device's endpoints */
    /* ★ I3, and it is what makes the I15 guard at the exposure honest. This function resets a slot
     * completely — pstate, probedPort, endpoints, its gBulkEP[] entries — and left the GEOMETRY behind, so a
     * slot that had lost its device kept reporting the departed drive's block count until some later probe
     * overwrote it. Harmless while nothing read it before a rewrite; not harmless once "gDevBlkCnt != 0" is
     * being used to mean "THIS device was probed". Reset per-operation state at the transition that changes
     * its meaning. Found by the §7 write-site sweep, not on hardware. */
    gDevBlkCnt[d] = 0; gDevBlkSize[d] = 512;
    for (i = 0; i < NBULK; i++)                          /* n15: drop only THIS device's stale regs. Clearing
        if (gBulkEP[i].dev == (UInt8)d) gBulkEP[i].used = 0;   /* the whole table destroyed the other devicers) */
    gDownReady = 1;                                      /* engine ready; ASE re-arms on the next transfer */
    gReprobe = 0;                                        /* r93: assertive re-take-over DISABLED (see selfprobe_tick case 0) —
                                                          * it issued a CBW while Apple was still probing the shared eps = collision */
    gBurst = 40;                                         /* r90: capture the next 40 selfprobe_tick entries (post-reconnect path) */
    gLoopDbg = 150;                                      /* r94: trace the app idle-loop for the next 150 passes after this reconnect */
    gSihArmed = 0; gSihQuiet = 0; gSihLastCnt = gBulkDoneN + gBulkErrN;   /* r95: re-arm the SIH takeover-watcher for this reconnect */
    ehci_os_logx("  r88 reset done — gPState now", gDev[d].pstate);   /* diag: confirm the reset took */
    ehci_os_logx("  r94 gLoopDbg armed", (unsigned long)gLoopDbg);   /* proves the arm ran (absence of APPLOOP crumbs => loop stalled) */
    ehci_os_logx("  r89 &gPState", (unsigned long)(void *)&gDev[d].pstate);   /* compare w/ the self-probe entry's &gDev[0].pstate */
}

/* r94 DIAGNOSTIC: app idle-loop breadcrumb. The app calls this at 3 points per pass (tag 1 = top, before
 * ExpertIdleTask; tag 2 = after USLPolledProcessDoneQueue, just before the tickFn/selfprobe call; tag 3 =
 * after tickFn). Armed (gLoopDbg=N) by reconnect_reset, so it traces ONLY the passes right after a reinsert
 * and can't flood. Reading EHCIUIM_init.log post-RECONNECT: tag 1 but never 2 => stuck in ExpertIdleTask;
 * 2 but never 3 => stuck in USLPolledProcessDoneQueue; 3 present but NO "r90 burst:" => the loop reaches the
 * tickFn call yet selfprobe_tick isn't running (svc/tickFn wiring); 1,2,3 repeating + burst => it works. */
void ehci_vhub_loopcrumb(UInt32 tag)
{
    if (gLoopDbg > 0) {
        ehci_os_logx("APPLOOP crumb", tag);
        if (tag == 3) gLoopDbg--;      /* count PASSES: decrement on the end-of-pass crumb */
    }
}
/* ================================================================================================
 * PHASE 1b: control transfers, the error-checked BOT probe, self-enumeration, and the n5 async engine.
 * RECONSTRUCTED 2026-08-01 from verbatim source fragments captured before the file was lost.
 * Placed here because it needs recov_setup,
 * bot_reset_host_toggles, ase_quiesce and reconnect_reset, and must precede selfprobe_tick.
 * ================================================================================================ */

/* n5: split into ISSUE and POLL so the async engine can wait without blocking. Control phases retire on
 * the DOWN counters (gDownDone/Err/Timeouts) — NOT the bulk counters pb_ready() watches, which is a trap:
 * a control phase never moves gBulkDoneN, so polling pb_ready() for one waits forever. */
static void ctl_issue(volatile UInt8 *buf, UInt32 addr, UInt32 len, UInt32 pid)
{
    gCtlSeqMark = gCtlSeq;                           /* h13: wait for a CONTROL completion, not just any */
    down_submit(0, 0, buf, addr, len, pid, -1);      /* -1 = control (ep0) */
}
/* h13: was `(gDownDone + gDownErr + gDownTimeouts) != gCtlMark`, which a BULK completion satisfied — so with
 * a drive mounted and a device enumerating, a control phase could advance on someone else's transfer. */
static int ctl_done(void) { return gCtlSeq != gCtlSeqMark; }
/* The blocking form. TASK LEVEL ONLY — it spin-waits on a counter the SIH advances, so calling it below
 * task level deadlocks (which is exactly why the n5 engine exists). */
static int pb_ctrl_phase(volatile UInt8 *buf, UInt32 addr, UInt32 len, UInt32 pid)
{
    UInt32 t0;
    ctl_issue(buf, addr, len, pid);
    t0 = frame_ms();
    while (!ctl_done())
        if (frame_ms() - t0 > 800UL) return -1;      /* per-phase cap (down watchdog also backstops) */
    return 0;
}
static int pb_bot_reset(void)
{
    UInt32 addr;
    if (gDev[0].pOut < 0 || gDev[0].pIn < 0) return -100;
    addr = gBulkEP[gDev[0].pOut].addr;
    recov_setup(0x21, 0xFF, 0x0000, 0x0000);                                /* Bulk-Only Mass Storage Reset */
    if (pb_ctrl_phase(gRecovSetup, addr, 8, 2)) return -1;                   /*   SETUP */
    if (pb_ctrl_phase(gPB, addr, 0, 1))         return -2;                   /*   STATUS-IN (0 len) */
    recov_setup(0x02, 0x01, 0x0000, (UInt16)(gBulkEP[gDev[0].pIn].endpt | 0x80u));  /* CLEAR_FEATURE(HALT) IN first */
    if (pb_ctrl_phase(gRecovSetup, addr, 8, 2)) return -3;
    if (pb_ctrl_phase(gPB, addr, 0, 1))         return -4;
    recov_setup(0x02, 0x01, 0x0000, gBulkEP[gDev[0].pOut].endpt);                   /* CLEAR_FEATURE(HALT) OUT */
    if (pb_ctrl_phase(gRecovSetup, addr, 8, 2)) return -5;
    if (pb_ctrl_phase(gPB, addr, 0, 1))         return -6;
    bot_reset_host_toggles(0);                                              /* host bulk QHs -> DATA0 (step 4: real slot) */
    return 0;
}

#if APPLE_HIDE
/* ==================== PHASE 1b: error-checked BOT probe ====================
 * p1a advanced purely on pb_ready() (= "finished", success OR error) and never looked at bCSWStatus, so a
 * STALLed READ CAPACITY looked like progress and we published 'Eusb' over a HALTED endpoint. Root cause of
 * that stall: no TEST UNIT READY and no REQUEST SENSE anywhere. Apple's mounter used to run them before we
 * took over, clearing the UNIT ATTENTION asserted on every fresh SET_CONFIGURATION. INQUIRY is exempt from
 * Unit Attention, which is exactly why INQUIRY passed and the next data command stalled. Now we own
 * enumeration, so WE must clear it. */
static UInt8 gSense[18];

/* Validate the 13-byte CSW in gPB. 0 = passed, 1 = failed, 2 = phase error, -1 = not a CSW at all. */
static int pb_csw(void)
{
    if (!(gPB[0] == 0x55 && gPB[1] == 0x53 && gPB[2] == 0x42 && gPB[3] == 0x53)) return -1;   /* 'USBS' */
    if (gPB[12] == 0x00) return 0;
    if (gPB[12] == 0x02) return 2;
    return 1;
}
static int pb_clear_halt(int slot)
{
    UInt32 addr; UInt16 wIndex;
    if (slot < 0) return -1;
    addr   = gBulkEP[slot].addr;
    wIndex = (UInt16)(gBulkEP[slot].endpt | (gBulkEP[slot].dirIn ? 0x80u : 0u));
    recov_setup(0x02, 0x01, 0x0000, wIndex);
    if (pb_ctrl_phase(gRecovSetup, addr, 8, 2)) return -2;
    if (pb_ctrl_phase(gPB, addr, 0, 1))         return -3;
    return 0;
}
/* One data-IN (or no-data) BOT command, synchronous, with stall recovery. save/saveLen copies the data
 * phase out BEFORE the 13-byte CSW read overwrites the head of gPB. Returns the CSW status, or negative
 * on a transport failure. */
static int pb_cmd_in(const UInt8 *cdb, int cdbLen, UInt32 dataLen, UInt8 *save, UInt32 saveLen)
{
    pb_cbw(0, cdb, cdbLen, dataLen);
    if (pb_wait())   return -1;
    if (pb_failed()) return -2;                              /* the CBW itself never went out */
    if (dataLen) {
        pb_in(0, dataLen);
        if (pb_wait()) return -3;
        if (pb_failed()) {                                   /* data phase STALLED (BOT 5.3.4): clear the
                                                              * halt, then STILL read the CSW so the device
                                                              * stays in sync */
            (void)pb_clear_halt(gDev[0].pIn);
        } else if (save && saveLen) {
            UInt32 k; for (k = 0; k < saveLen; k++) save[k] = gPB[k];
        }
    }
    pb_in(0, 13);
    if (pb_wait()) return -4;
    if (pb_failed()) { (void)pb_clear_halt(gDev[0].pIn); return -5; }
    return pb_csw();
}
/* Bring the drive to READY — the step Apple's mounter used to perform for us.
 * ★ Both CDBs are 6 bytes and we DECLARE 6 — Apple hardcodes bCBWCBLength=12 for every command
 * (DDK USBSampleStorageDriver), a BOT spec violation strict devices dislike. We stay honest. */
static int pb_unit_ready(void)
{
    static const UInt8 cdbTur[6]   = {0x00,0,0,0,0,0};       /* TEST UNIT READY, no data */
    static const UInt8 cdbSense[6] = {0x03,0,0,0,18,0};      /* REQUEST SENSE, 18 bytes */
    int t, st;
    for (t = 0; t < 8; t++) {
        st = pb_cmd_in(cdbTur, 6, 0, 0, 0);
        if (st == 0) { if (t) ehci_os_logx("  unit ready after TUR attempts", (UInt32)(t + 1)); return 0; }
        if (st < 0)  { ehci_os_logx("  SELFPROBE: TUR transport failure rc", (UInt32)(long)st); return -1; }
        gSense[2] = gSense[12] = gSense[13] = 0;
        (void)pb_cmd_in(cdbSense, 6, 18, gSense, 18);        /* clears UNIT ATTENTION */
        ehci_os_logx("  REQUEST SENSE senseKey/ASC/ASCQ",
                     ((UInt32)(gSense[2] & 0x0Fu) << 16) | ((UInt32)gSense[12] << 8) | gSense[13]);
    }
    ehci_os_log("  SELFPROBE: device never reported ready");
    return -2;
}

/* ==================== n3/n4c: install our own block driver — no application needed ====================
 * The last thing a launcher was doing for us. MUST run AFTER ehci_vhub_publish_service(): the block
 * driver's kInitialize reads the disk THROUGH the 'Eusb' service and AddDrives, and it fails with ioErr if
 * there is no media. One-shot: installing twice would add a duplicate Drive-Queue entry. */
static int   gBlkInstalled = 0;
static short gBlkDref = 0;
static void h41_queue_dump(const char *when);   /* h41: defined further down, used by as_tick */
static volatile UInt32 gH41LateDue;              /* h41: one-shot post-mount dump deadline */
/* ★★★★★★ f4: AN EARLY VERDICT DEADLINE, BECAUSE THE MACHINE DIED INSIDE THE LATE ONE.
 * m24 crashed within 15 s of the exposure — the h41 +15 s dump NEVER FIRED, which is itself how we know
 * the death was inside that window. Hanging the f4 verdict solely off that deadline would have reproduced
 * the exact failure the whole run exists to avoid: the evidence dies with the machine.
 * ⇒ Two deadlines. +5 s guarantees a floor (the mount attempt has begun; any clobber during it is already
 * counted), +15 s gives the full window. Both print; the labels say which is which. Cheap insurance against
 * losing a reboot. */
static volatile UInt32 gF4EarlyDue;
/* n4c: tell the ALREADY-INSTALLED block driver that the media state changed, via its private
 * kCsUsbDiskMedia control (magic-guarded; see usb_disk.c). This is the piece hot-plug was missing: the
 * re-enumeration and re-probe were always correct, but AddDrive + PostEvent(diskEvt) live in the block
 * driver's kInitialize, which only ever runs at install time — so on a re-insert nothing told the File
 * Manager the media was back. TASK LEVEL ONLY: the block driver re-scans the volume synchronously through
 * 'Eusb' and writes its log, exactly as kInitialize does from this same context. NEVER from the ISR. */
static void blk_notify_media(int dv, int present)
{
    ParamBlockRec pb;
    OSErr e;
    if (!gBlkInstalled || gBlkDref == 0) return;
    pb.cntrlParam.ioCompletion = 0;
    pb.cntrlParam.ioNamePtr    = 0;
    pb.cntrlParam.ioVRefNum    = 0;
    pb.cntrlParam.ioCRefNum    = gBlkDref;
    pb.cntrlParam.csCode       = 20481;                  /* kCsUsbDiskMedia */
    pb.cntrlParam.csParam[0]   = (short)0x4548;          /* 'EH' \  magic guard 0x45484349 */
    pb.cntrlParam.csParam[1]   = (short)0x4349;          /* 'CI' /                         */
    pb.cntrlParam.csParam[2]   = (short)(present ? 1 : 0);
    pb.cntrlParam.csParam[3]   = (short)dv;              /* ★ n19 step 3: which device slot */
    e = PBControlSync(&pb);
    ehci_os_logx(present ? "n4c media-ARRIVED control -> block driver, err"
                         : "n4c media-GONE control -> block driver, err", (UInt32)(long)e);
    ehci_os_logx("    for device slot", (UInt32)dv);
}
/* ★★ n7: UNEXPECTED REMOVAL — unmount the volume and, if it is busy, tell the user, exactly as Apple's own
 * "USB Mass Storage Support" extension does. Reverse-engineering that extension shows precisely
 * this design: it imports UnmountVol, Eject, NMInstall and NMRemove, and carries the strings
 *   "Please reconnect the USB device …"
 *   "Before disconnecting the device, you must close all files and applications using it and choose
 *    [Eject] from the [Special] menu."
 * i.e. on removal it unmounts, and when the volume is BUSY it posts a Notification Manager alert asking the
 * user to plug the device back in. Without this the volume just stays mounted over absent media and the
 * File Manager eventually reports it as damaged — the behaviour the user reported.
 *
 * ⚠ TASK LEVEL ONLY, and deliberately NOT called from inside the block driver's own Control handler: doing
 * it there would re-enter the File Manager from inside a Device Manager call. We run here, in
 * selfprobe_tick, AFTER blk_notify_media(0)'s PBControlSync has fully returned — no nesting.
 * ⚠ Depends on the n6e offLinErr fix: UnmountVol flushes first, and that flush must FAIL FAST against the
 * absent device rather than stalling in the transfer engine. */
static NMRec  gNM;
static Str255 gNMMsg;
static int    gNMPosted = 0;
/* ★ n26: WHICH slot the posted notification is about, or -1. Apple's alert vanishes on its own once the
 * referenced drive is physically unplugged — no OK click needed — and ours lingered until dismissed. Knowing
 * the slot is what lets the removal path retract exactly the right notification. */
static int    gNMDev = -1;
/* ★ n8: WORD-FOR-WORD Apple. Recovered from their USB Mass Storage Support extension's resource fork
 * (USBMassStorageSupport's own resources), which carries this exact text with the device name substituted
 * between Mac Roman curly quotes (0xD2/0xD3) and CR (\r) as the line break:
 *
 *   Please reconnect the USB device "^".
 *
 *   It is in use and must be reconnected now to prevent damage to its contents.
 *
 *   Before disconnecting the device, you must close all files and applications using it and choose
 *   "Eject" from the "Special" menu.
 *
 * The whole point of this driver is to feel like Apple shipped it, so the wording is theirs, not mine. */
#define MACQ_OPEN  0xD2u      /* Mac Roman left  double quote */
#define MACQ_CLOSE 0xD3u      /* Mac Roman right double quote */
static void nm_cat(int *i, const char *s)
{ while (*s && *i < 250) { gNMMsg[++(*i)] = (unsigned char)*s++; } }
/* ★ n10: the USB DEVICE name, built from the INQUIRY vendor (bytes 8..15) + product (16..31), each trimmed
 * of trailing spaces. Apple's alerts name the DEVICE, not the volume — their screenshot reads
 * "the USB device ⟨SanDisk Ultra⟩", which is exactly vendor+product from INQUIRY. */
/* ★★★★ n24 SWEEP FINDING 1: PER-DEVICE. This was a single Str63, so it held whichever device enumerated
 * MOST RECENTLY. With two drives mounted, ejecting drive A produced Apple's alert naming drive B — the exact
 * "names the wrong thing" failure that message fidelity is a stated requirement against. Same family as the
 * n19-n23 five: per-device state kept in a singleton. */
static Str63 gDevName[USB_MAX_DEV];
static void capture_device_name(int dev, const UInt8 *inq)
{
    int i, n = 0, end;
    unsigned char *d;
    if (dev < 0 || dev >= USB_MAX_DEV) return;
    d = gDevName[dev];
    for (end = 15; end >= 8 && inq[end] == ' '; end--) ;
    for (i = 8; i <= end && n < 40; i++) d[++n] = inq[i];
    if (n) d[++n] = ' ';
    for (end = 31; end >= 16 && inq[end] == ' '; end--) ;
    for (i = 16; i <= end && n < 60; i++) d[++n] = inq[i];
    d[0] = (unsigned char)n;
}
/* ★ n10: Apple's EJECT alert, word for word from their resource fork:
 *   "You may now remove the cartridge from the USB device ⟨name⟩ because your Macintosh is finished with it.
 *    The cartridge will not be remounted until it is removed from the drive."
 * Posted when the Finder ejects our volume, which is the case the user showed had no message at all. */
static void notify_reconnect(const unsigned char *volName);
static void notify_rude_removal(const unsigned char *volName);   /* h82: Apple's "unexpectedly disconnected" */
/* ★ n24: takes the SLOT being ejected, so the alert names the drive the user actually ejected.
 * Signature change is safe against the n4g activator: ejectFn sits at offset 88, AFTER the activator's
 * view of gSvc (which ends at quitFn, offset 80) and the activator never calls it. The only caller is the
 * block driver, whose PEF is EMBEDDED IN THIS ROM (gUsbDiskPef + InstallDriverFromMemory), so the two are
 * always built and shipped together and cannot disagree. No field offsets move, so magic/magic2 stay put. */
void ehci_vhub_notify_ejected(int dev)
{
    int i = 0;
    const unsigned char *nmd = (dev >= 0 && dev < USB_MAX_DEV) ? gDevName[dev] : gDevName[0];
    if (gNMPosted) { (void)NMRemove(&gNM); gNMPosted = 0; gNMDev = -1; }   /* n26: drop the slot too */
    /* ★★★ n27 WORDING — a DELIBERATE departure from Apple's string, at the user's request 2026-08-04.
     * Apple's is "You may now remove the cartridge from the USB device ⟨X⟩ … The cartridge will not be
     * remounted until it is removed from the drive." "Cartridge" is Zip/SyQuest-era vocabulary for removable
     * MEDIA inside a fixed drive; a USB stick has no separable cartridge, so on this hardware the sentence
     * describes something that does not exist. We name the device and its SPEED instead.
     * ⚠ This intentionally relaxes the standing "match Apple's OHCI strings word for word" rule for THIS
     * message. Do not "correct" it back — the deviation is the requirement. Every other string still matches.
     * The speed comes from gDev[].hiSpeed, measured at enumeration, so it is a fact and not an assertion. */
    nm_cat(&i, "You may now remove the USB ");
    nm_cat(&i, (dev >= 0 && dev < USB_MAX_DEV && !gDev[dev].hiSpeed) ? "1.1" : "2.0");
    nm_cat(&i, " device ");
    gNMMsg[++i] = MACQ_OPEN;
    { int n; for (n = 1; n <= nmd[0] && i < 200; n++) gNMMsg[++i] = nmd[n]; }
    gNMMsg[++i] = MACQ_CLOSE;
    nm_cat(&i, " because your Macintosh is finished with it.");
    nm_cat(&i, "\r\rThe device will not be remounted until it is physically removed.");
    gNMMsg[0] = (unsigned char)i;
    gNM.qType = nmType; gNM.nmMark = 0; gNM.nmIcon = 0; gNM.nmSound = 0;
    gNM.nmStr = gNMMsg; gNM.nmResp = (NMUPP)-1L; gNM.nmRefCon = 0;
    if (NMInstall(&gNM) == noErr) { gNMPosted = 1; gNMDev = dev; }   /* n26: remember whose alert this is */
}
/* ★★★ n26: retract the eject alert once THAT drive is physically gone.
 * Apple's alert self-dismisses on unplug; ours had to be clicked. The message is "you may now remove the
 * cartridge" — so the moment the cartridge IS removed the message has served its purpose and should go.
 * Called from the task-level rearm (NMRemove is task-level only, and that path already does File Manager
 * work, so it is the right place). Retracts ONLY the notification belonging to the departed slot: with two
 * drives mounted, unplugging one must not clear an alert that is still about the other. */
/* ★★★★ h15: TELL THE USER WHEN THE FOUR-DRIVE LIMIT REFUSES A DRIVE.
 * Until now this was a single line in EHCIUIM_init.log and nothing else: the drive silently never appeared,
 * which is indistinguishable from the failures we spent three builds chasing. On the h13 run that exact
 * refusal was read as a fault by someone reading the driver log — an end user has no such option.
 * ⚠ THIS IS A DELIBERATE DEVIATION from "match Apple's OHCI strings word for word", and unavoidably so:
 * Apple has NO equivalent message, because their stack has no device-count limit to report (it allocates an
 * address per device from 1-127 on demand). A search of Apple's USBMassStorageSupport resources for "too many" /
 * "maximum" / "cannot use" finds nothing, and that search does find Apple's known wording in the same file,
 * so it is a real absence rather than a failed grep. Second such exception after the n27 eject wording, and
 * the user approved it explicitly (2026-08-05).
 * ⚠ THE MESSAGE IS GENERIC ON PURPOSE — it cannot name the drive. Naming needs INQUIRY, INQUIRY needs bulk
 * endpoints, endpoints need a slot, and having no free slot is the entire reason we are here. Spending a
 * mounted drive's slot to learn the newcomer's name would be a worse trade than a generic sentence. */
static void notify_slot_limit(void)
{
    int i = 0;
    if (gNMPosted) return;        /* never displace a live eject/reconnect alert with this advisory */
    nm_cat(&i, "An additional USB 2.0 drive cannot be used because ");
    gNMMsg[++i] = (unsigned char)('0' + USB_MAX_DEV);
    nm_cat(&i, " USB 2.0 drives are already connected.");
    nm_cat(&i, "\r\rDisconnect one of the other drives to use this one.");
    gNMMsg[0] = (unsigned char)i;
    gNM.qType = nmType; gNM.nmMark = 0; gNM.nmIcon = 0; gNM.nmSound = 0;
    gNM.nmStr = gNMMsg; gNM.nmResp = (NMUPP)-1L; gNM.nmRefCon = 0;
    /* gNMDev stays -1: this alert is about no device in particular, so no slot's departure should retract
     * it — the same reasoning that keeps notify_reconnect from auto-retracting. */
    if (NMInstall(&gNM) == noErr) { gNMPosted = 1; gNMDev = -1; }
}
static void nm_retract_for_dev(int dev)
{
    if (gNMPosted && gNMDev == dev) {
        (void)NMRemove(&gNM);
        gNMPosted = 0; gNMDev = -1;
        ehci_os_logx("n26: eject alert retracted — its drive was unplugged, no OK needed; slot", (UInt32)dev);
    }
}
static void notify_reconnect(const unsigned char *volName)
{
    int i = 0, n;
    if (gNMPosted) { (void)NMRemove(&gNM); gNMPosted = 0; gNMDev = -1; }   /* n26: drop the slot too */
    nm_cat(&i, "Please reconnect the USB device ");
    gNMMsg[++i] = MACQ_OPEN;
    if (volName) for (n = 1; n <= volName[0] && i < 200; n++) gNMMsg[++i] = volName[n];
    gNMMsg[++i] = MACQ_CLOSE;
    nm_cat(&i, ".\r\rIt is in use and must be reconnected now to prevent damage to its contents.");
    nm_cat(&i, "\r\rBefore disconnecting the device, you must close all files and applications using it "
               "and choose ");
    gNMMsg[++i] = MACQ_OPEN; nm_cat(&i, "Eject"); gNMMsg[++i] = MACQ_CLOSE;
    nm_cat(&i, " from the ");
    gNMMsg[++i] = MACQ_OPEN; nm_cat(&i, "Special"); gNMMsg[++i] = MACQ_CLOSE;
    nm_cat(&i, " menu.");
    gNMMsg[0] = (unsigned char)i;
    gNM.qType    = nmType;
    gNM.nmMark   = 0;
    gNM.nmIcon   = 0;
    gNM.nmSound  = 0;
    gNM.nmStr    = gNMMsg;
    gNM.nmResp   = (NMUPP)-1L;                         /* the NM dequeues it once the user dismisses */
    gNM.nmRefCon = 0;
    /* ★ n26: gNMDev stays -1 for THIS alert on purpose. "Please reconnect the USB device" asks the user to
     * plug the device BACK IN — the device being absent is the whole reason it is showing. So the removal
     * path must never retract it; the opposite event (a re-connect) is what makes it stale, and that already
     * replaces it via the NMRemove at the top of both posters. */
    if (NMInstall(&gNM) == noErr) { gNMPosted = 1; gNMDev = -1; }
}
/* Returns the number of OUR volumes that could NOT be unmounted (busy). notifyIfBusy posts Apple's
 * "please reconnect" alert, which is right for a REMOVAL but not for a clean quit — hence the flag. */
/* ★★★★ n23: does the drive behind this VCB still have its media?
 * Asked of the BLOCK DRIVER via a plain kDriveStatus(8), because the block driver is the fragment that owns
 * the slot -> drive-number map and it already answers this per drive, routed on ioVRefNum. That keeps the
 * 'Eusb' ABI unchanged (so the n4g activator still pairs) instead of exporting the map.
 * Fails SAFE: any error, or no block driver, reports "present", so an unanswered question can never cause an
 * extra unmount. Task level only — PBStatusSync is synchronous; the one caller is the task-level rearm. */
static int blk_drive_media_present(short drvNum)
{
    ParamBlockRec pb; int k;
    if (!gBlkDref || drvNum == 0) return 1;
    for (k = 0; k < 11; k++) pb.cntrlParam.csParam[k] = 0;
    pb.cntrlParam.ioCompletion = 0;
    pb.cntrlParam.ioNamePtr    = 0;
    pb.cntrlParam.ioVRefNum    = drvNum;              /* the block driver routes on this */
    pb.cntrlParam.ioCRefNum    = gBlkDref;
    pb.cntrlParam.csCode       = 8;                   /* kDriveStatus */
    if (PBStatusSync((ParmBlkPtr)&pb) != noErr) return 1;
    /* DrvSts prefix: short track; char writeProt; char diskInPlace; ... — diskInPlace is byte 3. */
    return ((const char *)&pb.cntrlParam.csParam[0])[3] != 0;
}
/* onlyAbsentMedia: 1 = unmount ONLY the volumes whose drive has lost its media (a removal), 0 = all of ours
 * (the clean-quit path, which really does want every one). */
static long blk_unmount_our_volumes2(int notifyIfBusy, int onlyAbsentMedia)
{
    QHdrPtr q = GetVCBQHdr();
    void *v, *next;
    long busy = 0;
    if (!gBlkDref) return 0;
    for (v = (void *)(q ? q->qHead : 0); v; v = next) {
        VCB *vcb = (VCB *)v;
        next = (void *)vcb->qLink;
        if (vcb->vcbDRefNum != gBlkDref) continue;     /* not one of ours */
        /* ★★★★ n23 THE BUG THIS FIXES. One block driver serves N drives (n19), so vcbDRefNum is the SAME
         * for every volume we expose and the test above matches ALL of them. On a removal this function
         * therefore unmounted the surviving drive too — the n22 hardware report, "Finder-eject one drive and
         * remove it and the OTHER drive disappears as well". It was written when there was one drive, where
         * "all our volumes" and "the removed volume" were the same set; n19 made it wrong and nothing noticed
         * until two volumes could coexist. blk_notify_media(gRearmDev, 0) has already run by the time we get
         * here, so exactly the departed drive reports diskInPlace == 0. */
        if (onlyAbsentMedia && blk_drive_media_present(vcb->vcbDrvNum)) continue;   /* still here — leave it alone */
        {
            HParamBlockRec pb; OSErr e;
            Str27 nm; int k;
            for (k = 0; k <= vcb->vcbVN[0] && k < 28; k++) nm[k] = vcb->vcbVN[k];
            pb.volumeParam.ioCompletion = 0;
            pb.volumeParam.ioNamePtr    = 0;
            pb.volumeParam.ioVRefNum    = vcb->vcbVRefNum;
            e = PBUnmountVol((ParmBlkPtr)&pb);
            ehci_os_logx("n7 UnmountVol on our volume, err", (UInt32)(long)e);
            /* ★ h82: a SUCCESSFUL unmount on the onlyAbsentMedia path IS a rude removal — the volume was
             * still mounted when the device physically left. A clean eject has already torn the VCB down,
             * so this loop never sees it and stays silent, which is correct and matches Apple. */
            if (e == noErr && onlyAbsentMedia) notify_rude_removal(nm);
            if (e != noErr) {
                busy++;
                /* Busy (fBsyErr = open files) — Apple's exact fallback: ask for the device back. */
                if (notifyIfBusy) {
                    ehci_os_log("n7 volume BUSY — asking the user to reconnect the device (Apple's behaviour)");
                    notify_reconnect(nm);
                }
            }
        }
    }
    return busy;
}
/* n23: a REMOVAL unmounts only the drive that actually went away (onlyAbsentMedia = 1). */
static void blk_unmount_removed_volumes(void) { (void)blk_unmount_our_volumes2(1, 1); }

/* ★★★★★★★ h82 — "The device for disk X was unexpectedly disconnected." APPLE'S OWN ALERT, VERBATIM.
 *
 * ⚠ AND IT CORRECTS SOMETHING I TOLD THE USER. After m31 I said Apple is silent when an IDLE drive is
 * removed, citing n25. n25 is right about a CLEAN removal — eject first, then unplug — but the user
 * photographed Apple's OHCI stack on a RUDE removal (unplug while still mounted) and it shows this alert.
 * We were silent in BOTH cases. That is the gap.
 *
 * ★ THE STRINGS ARE APPLE'S, RECOVERED BYTE-EXACT from the Mac OS 9.2.2 install image, where they sit in
 * the SYSTEM string table beside "The disk X appears to be damaged" and the UPS/thermal messages — NOT in
 * USBMassStorageSupport's STR# 128, which has no such case. The stored form is
 *     "The device for disk \xD2" <name> "\xD3 was unexpectedly disconnected." \r\r
 *     "To prevent data loss, always use the Finder to \xD2Put Away\xD3 a disk before disconnecting its disk device."
 * The \r\r is the headline/explanation break, which is exactly StandardAlert's two-argument form.
 * 0xD2/0xD3 are MacRoman curly quotes — they MUST stay as raw bytes; typing ASCII quotes would visibly
 * differ from Apple's alert, and matching Apple word for word is this project's standing rule.
 *
 * ★★ WHY StandardAlert IS SAFE FROM HERE, and why n26's note said otherwise. n26 recorded that drawing a
 * real alert "needs a task-level APPLICATION context" and declined to try. docs/APPLE-UMSS-RE.md later
 * disassembled Apple's presenter and found the opposite: StandardAlert is called with NO BeginSystemMode
 * wrapper — system mode wraps only the hand-built GetNewDialog+ModalDialog fallback. The Notification
 * Manager response context is by itself sufficient. This runs on exactly that path (selfprobe_tick).
 * ⚠ StandardAlert lives in AppearanceLib, which we do not link. Apple reaches it as a weak import and, if
 * unresolved, via GetSharedLibrary/FindSymbol — the same CFM route h48 already uses. Resolved ONCE and
 * cached; if it cannot be resolved we simply skip the alert. FAIL SILENT, NEVER FAIL LOUD: this is a
 * courtesy message, and it must never be able to take down a stack that took this long to stabilise. */
typedef pascal OSErr (*StandardAlertProc)(short, StringPtr, StringPtr, void *, short *);
static StandardAlertProc gStdAlert = 0;
static int gStdAlertTried = 0;
static StandardAlertProc resolve_standard_alert(void)
{
    CFragConnectionID cid; Ptr mainAddr; Str255 err;
    if (gStdAlertTried) return gStdAlert;
    gStdAlertTried = 1;
    if (GetSharedLibrary("\pAppearanceLib", kPowerPCCFragArch, kLoadCFrag, &cid, &mainAddr, err) == noErr) {
        CFragSymbolClass cls; Ptr sym = 0;
        if (FindSymbol(cid, "\pStandardAlert", &sym, &cls) == noErr && sym)
            gStdAlert = (StandardAlertProc)sym;
    }
    ehci_os_logx("h82: StandardAlert resolved from AppearanceLib (0 = unavailable, alert will be skipped)",
                 (UInt32)(gStdAlert ? 1 : 0));
    return gStdAlert;
}
static void notify_rude_removal(const unsigned char *volName)
{
    /* Apple's bytes. \xD2 = left curly quote, \xD3 = right curly quote (MacRoman). */
    static const char kHeadPre[]  = "The device for disk \xD2";
    static const char kHeadPost[] = "\xD3 was unexpectedly disconnected.";
    static const char kExpl[]     = "To prevent data loss, always use the Finder to \xD2Put Away\xD3 "
                                    "a disk before disconnecting its disk device.";
    StandardAlertProc sa = resolve_standard_alert();
    Str255 head, expl;
    UInt32 i, n = 0;
    if (!sa) return;                                   /* fail silent — see the note above */
    for (i = 0; kHeadPre[i] && n < 254; i++)  head[++n] = (unsigned char)kHeadPre[i];
    if (volName) for (i = 1; i <= volName[0] && n < 254; i++) head[++n] = volName[i];
    for (i = 0; kHeadPost[i] && n < 254; i++) head[++n] = (unsigned char)kHeadPost[i];
    head[0] = (unsigned char)n;
    n = 0;
    for (i = 0; kExpl[i] && n < 254; i++) expl[++n] = (unsigned char)kExpl[i];
    expl[0] = (unsigned char)n;
    {   short hit = 0;
        /* ★★★★ h86 — NIL alertParam, NOT a hand-laid struct. h82 laid AlertStdAlertParamRec out by hand
         * "so we need no Dialogs.h", and that is exactly what broke it: Dialogs.h wraps the real struct in
         * #pragma options align=mac68k (2-byte), the by-hand copy got PPC natural alignment, so filterProc
         * and every field after it sat +2 from where StandardAlert reads. PROVEN CALLED AND SILENT on m32:
         * three yanks, gate noErr x3, resolver ok, "h82: posting" logged x3, nothing drawn.
         * nil is the DOCUMENTED all-defaults form — one OK button, not movable — which is precisely
         * Apple's own alert. No struct, no alignment question, nothing left to be wrong. */
        ehci_os_log("h82: posting Apple's \"unexpectedly disconnected\" alert for a RUDE removal");
        (void)(*sa)(1 /* kAlertNoteAlert */, head, expl, 0 /* nil = defaults */, &hit);
    }
}

/* ★★ n8: CLEAN QUIT. The activator calls this through the 'Eusb' service BEFORE it exits.
 * WHY IT EXISTS (found on hardware): quitting the activator with a drive still mounted SHADOWED the
 * volume, produced "please insert the disk", and FROZE THE FINDER. The driver is not torn down at quit —
 * the log proves no Finalize and no stop_service ran — so the cause is that slot 23 stops and something
 * the mounted volume still needs stops with it. My earlier claim that a mounted volume keeps working with
 * no app was reasoning, never tested, and that freeze disproves it.
 * Until the underlying dependency is found and removed, the honest fix is to make the dangerous state
 * unreachable: unmount our volumes before the app goes away. Returns the number that were BUSY, so the
 * activator can refuse to quit and tell the user rather than leaving a live volume with no pump. */
long ehci_vhub_prepare_quit(void)
{
    /* n23: the QUIT path really does want every one of our volumes gone, so onlyAbsentMedia = 0. */
    long busy = blk_unmount_our_volumes2(0, 0);
    ehci_os_logx("n8 prepare-quit: our volumes still busy (0 = safe to quit)", (UInt32)busy);
    return busy;
}
static void install_block_driver(int dv)
{
    OSErr e; short dref = 0;
    /* n4c: already installed = this is a RE-insert. Do not install twice (and do not AddDrive twice, which
     * would leak a drive number per insertion) — just re-announce the media. */
    if (gBlkInstalled) { blk_notify_media(dv, 1); return; }
    e = InstallDriverFromMemory((Ptr)gUsbDiskPef, (ByteCount)gUsbDiskPefLen, "\pUSBDisk",
                                (RegEntryID *)&gSoftc.node, 48, 127, &dref);
    ehci_os_logx("n3 InstallDriverFromMemory(block driver) err", (UInt32)(long)e);
    ehci_os_logx("  drefNum", (UInt32)dref);
    if (e == noErr && dref != 0) {
        gBlkInstalled = 1; gBlkDref = dref;   /* n4c: keep the refNum so re-inserts can reach the driver */
        /* ★ n19 step 3: the driver's own kInitialize adds a drive for SLOT 0, because that is the
         * slot the first device gets (dev_alloc returns the lowest free one). If the first device to
         * be exposed is somehow not slot 0 - slot 0 enumerated but failed to probe, say - it would
         * otherwise never get a drive, so announce it explicitly. Harmless when dv is 0: that path
         * finds a drive number already present and just re-announces. */
        if (dv != 0) blk_notify_media(dv, 1);
        ehci_os_log("=== n3: block driver installed BY THE DRIVER (no app) — it AddDrives + posts diskEvt, so the OS mounts ===");
    } else {
        ehci_os_log("!! n3: block-driver install FAILED — no mount will happen");
    }
}

/* The whole probe, error-checked. 0 = the disk was genuinely read and geometry is sane (safe to publish).
 * ⚠ RECONSTRUCTED: only this function's first lines were captured verbatim. The body is re-derived from
 * its log strings (which survive in EHCIUIM_n5.strings.txt) and from the n5 async engine below, which is a
 * faithful transcription of this same sequence. Behaviourally equivalent; not guaranteed line-identical. */
static int selfprobe_sync(void)
{
    static const UInt8 cdbInq[6]  = {0x12,0,0,0,36,0};
    static const UInt8 cdbCap[10] = {0x25,0,0,0,0,0,0,0,0,0};
    UInt8 inq[36], cap[8], head[16], cdbRd[10];
    int st, i;

    st = pb_cmd_in(cdbInq, 6, 36, inq, 36);                  /* INQUIRY (exempt from Unit Attention) */
    if (st != 0) { ehci_os_logx("SELFPROBE FAIL: INQUIRY st", (UInt32)(long)st); return -1; }
    ehci_os_log("SELFPROBE INQUIRY:");
    ehci_os_logx("  periphType", inq[0]);
    ehci_os_logx("  vendor0_3", BUF_BE32(inq, 8));  ehci_os_logx("  vendor4_7", BUF_BE32(inq, 12));
    ehci_os_logx("  prod0_3",   BUF_BE32(inq, 16)); ehci_os_logx("  prod4_7",   BUF_BE32(inq, 20));

    if (pb_unit_ready() != 0) return -2;                     /* TUR + REQUEST SENSE until ready */

    st = pb_cmd_in(cdbCap, 10, 8, cap, 8);                   /* READ CAPACITY */
    if (st != 0) { ehci_os_logx("SELFPROBE FAIL: READ CAPACITY st", (UInt32)(long)st); return -3; }
    gPBlkCnt  = BUF_BE32(cap, 0) + 1UL;                      /* returns the LAST LBA */
    gPBlkSize = BUF_BE32(cap, 4);
    ehci_os_log("SELFPROBE READ CAPACITY:");
    ehci_os_logx("  blocks", gPBlkCnt);
    ehci_os_logx("  blockSize", gPBlkSize);
    if (gPBlkSize != 512UL || gPBlkCnt < 4UL) {
        ehci_os_log("SELFPROBE FAIL: implausible geometry (stale/garbage data phase)"); return -4; }

    for (i = 0; i < 10; i++) cdbRd[i] = 0;                   /* READ(10) block 0 = proof we can read */
    cdbRd[0] = 0x28; cdbRd[8] = 1;
    st = pb_cmd_in(cdbRd, 10, 512, head, 16);
    if (st != 0) { ehci_os_logx("SELFPROBE FAIL: READ block 0 st", (UInt32)(long)st); return -5; }
    ehci_os_log("SELFPROBE READ block 0:");
    ehci_os_logx("  b0_3", BUF_BE32(head, 0));
    return 0;
}
#endif /* APPLE_HIDE */

/* Task-level state machine; call once per uim23. */
static void compl_drain(void)   /* MINI FIX: deliver deferred Apple bulk completions at TASK level */
{
    while (gComplTail != gComplHead) {
        UInt32 t = gComplTail & (NCOMPL - 1u);
        void *upp = gComplQ[t].upp, *pipe = gComplQ[t].pipe;
        long st = gComplQ[t].status; UInt32 act = gComplQ[t].actual;
        UInt8 two = gComplQ[t].twoArg;
        gComplTail++;
        if (upp) {
            if (two) ((ehci_usl_complete)upp)(pipe, (unsigned long)st);   /* h94: retired control (2-arg) */
            else ((ehci_usl_intcomplete)upp)(pipe, st, (unsigned long)act);
        }
    }
    /* h94: everything queued (including any substitute completions for a rude removal) has now been
     * DELIVERED at task level — the clients' memory was still live when they ran. Only now may the USL be
     * told the port changed; releasing the hold here is what makes that ordering a real happens-before. */
    gH94Hold = 0;
}
/* v22: mirror of the block driver's gCsLog (usb_disk.c), published via Gestalt('Ucsl'). Lets the UIM dump
 * the FM-level Prime trace into the reliable EHCIUIM log. Layout MUST match usb_disk.c exactly. */
typedef struct { short kind; short csCode; short ioVRefNum; short pad; long p0, p1; } CsRecMirror;   /* 16B */
typedef struct { UInt32 magic; UInt32 count; UInt32 cap; UInt32 nReads; UInt32 nWrites; CsRecMirror recs[512]; } CsLogMirror;
static CsLogMirror *gCsLogPtr = 0; static int gCsDumped = 0; static UInt32 gCsLastIo = 0xFFFFFFFFUL;
/* v25: mirror of the block driver's gDioLog (Gestalt 'Ucs2') — the DoDriverIO code+err control-plane trace. */
typedef struct { short code; short err; long aux; } DioRecMirror;
typedef struct { UInt32 magic; UInt32 count; DioRecMirror recs[128]; } DioLogMirror;
static DioLogMirror *gDioLogPtr = 0;
#if APPLE_HIDE
/* ==================== PHASE 1a: WE enumerate the hidden device ====================
 * With APPLE_HIDE the device is invisible to Apple's USL, so NOTHING resets the port, assigns an address,
 * or configures it. We do the whole of USB enumeration ourselves. RECONSTRUCTED verbatim. */
/* ★★★★ n18 FIX 1, THE DATA-CORRUPTION CAUSE. This was ONE constant for every device, so with two drives
 * attached BOTH were assigned USB address 1 (the n17 log: 'SET_ADDRESS ok; device now at 0x00000001'
 * four times over). Two devices answering to the same address on the same bus is a protocol violation:
 * drive A's transfers reached drive B and vice versa, which is why the Finder reported disk errors and
 * 'there is a problem with the disk' on a MOUNTED volume, and why every operation waited ~30s (the 10s
 * transfer watchdog firing repeatedly). It also made create_bulk's reuse path match on address, so B's
 * endpoints were folded into A's table entries.
 *
 * The address is now derived from the device SLOT, so each device has its own. These live in our own
 * namespace (Apple's hub sits far higher), and USB_MAX_DEV is at most 4, so 1..4 is safe. */
static volatile int   gSelfEnumDone = 0; /* 1 once endpoints are registered (or the port went to the companion) */
static int            gSelfEnumPort = -1;
static UInt32         gSelfEnumTries = 0;
static volatile UInt8 gBounceCnt[15];      /* h33: consecutive reset-bounces per root port; 3 = cede, not strand */
static volatile int   gSelfEnumRearm = 0;  /* set at ISR when an ENUMERATED device is pulled; consumed at task
                                            * level, because reconnect_reset does File-Mgr logging and must
                                            * NOT run at interrupt level */
/* n4b: ports whose hidden connect still needs LOGGING, as a bitmask set at interrupt level and drained at
 * task level. service_ports runs from vhub_sih / the heartbeat, and ehci_os_log is synchronous File Manager
 * I/O — which is the r18 hang, and hung the machine again on the first connect in n4. Flags at the ISR,
 * File-Mgr work at task level; same shape as gSelfEnumRearm above. */
static volatile UInt32 gHideLogMask = 0;
static UInt8 gEnumSetup[8];
static UInt8 gEnumDesc[320];               /* device + full config descriptor scratch (a BOT config is ~32B) */

static void enum_setup(UInt8 bmRT, UInt8 bReq, UInt16 wValue, UInt16 wIndex, UInt16 wLength)
{
    gEnumSetup[0] = bmRT;                gEnumSetup[1] = bReq;
    gEnumSetup[2] = (UInt8)wValue;       gEnumSetup[3] = (UInt8)(wValue >> 8);
    gEnumSetup[4] = (UInt8)wIndex;       gEnumSetup[5] = (UInt8)(wIndex >> 8);
    gEnumSetup[6] = (UInt8)wLength;      gEnumSetup[7] = (UInt8)(wLength >> 8);
}
/* Control READ (device->host): SETUP -> DATA-IN -> STATUS-OUT(0). Returns bytes requested, or <0. */
static int enum_ctrl_in(UInt8 bmRT, UInt8 bReq, UInt16 wValue, UInt16 wIndex,
                        UInt32 addr, volatile UInt8 *dst, UInt32 len)
{
    enum_setup(bmRT, bReq, wValue, wIndex, (UInt16)len);
    if (pb_ctrl_phase(gEnumSetup, addr, 8, 2)) return -1;    /* SETUP  */
    if (pb_ctrl_phase(dst, addr, len, 1))      return -2;    /* DATA-IN */
    if (pb_ctrl_phase(gEnumDesc, addr, 0, 0))  return -3;    /* STATUS-OUT (zero length) */
    return (int)len;
}
/* Control WRITE with no data phase (SET_ADDRESS / SET_CONFIGURATION): SETUP -> STATUS-IN(0). */
static int enum_ctrl_nodata(UInt8 bmRT, UInt8 bReq, UInt16 wValue, UInt16 wIndex, UInt32 addr)
{
    enum_setup(bmRT, bReq, wValue, wIndex, 0);
    if (pb_ctrl_phase(gEnumSetup, addr, 8, 2)) return -1;    /* SETUP */
    if (pb_ctrl_phase(gEnumDesc, addr, 0, 1))  return -2;    /* STATUS-IN (zero length) */
    return 0;
}
static void enum_delay_ms(UInt32 ms) { UInt32 t0 = frame_ms(); while (frame_ms() - t0 < ms) ; }

/* Drive a USB reset on port p using the existing interrupt-level machinery, then wait for the verdict.
 * Returns 1 = port ENABLED (high-speed, ours), 0 = not enabled (full/low-speed or gone). */
static int selfenum_reset_port(int p)
{
    UInt32 v, t0;
    v = ehci_read32(gSoftc.opBase, EHCI_PORTSC(p)) & ~EHCI_PORTSC_RW1C;
    v &= ~EHCI_PORT_ENABLE; v |= EHCI_PORT_RESET;
    ehci_write32(gSoftc.opBase, EHCI_PORTSC(p), v);
    gPortResetDone[p] = 0;
    gResetAtFrame[p] = frame_ms() + 50; gResetPending[p] = 1; gResetSeen = 1;
    gResetEnabling[p] = 0;
    pevt((UInt8)p, PEV_RST_ASSERT, ehci_read32(gSoftc.opBase, EHCI_PORTSC(p)));
    t0 = frame_ms();                                  /* service_ports (SIH) deasserts + waits for enable */
    while (!gPortResetDone[p]) if (frame_ms() - t0 > 500UL) break;
    return (ehci_read32(gSoftc.opBase, EHCI_PORTSC(p)) & EHCI_PORT_ENABLE) ? 1 : 0;
}

/* Full self-enumeration of the device on port p. 0 = endpoints registered, ready for the BOT self-probe.
 * TASK LEVEL (it blocks in pb_ctrl_phase). Superseded on the live path by the n5 async engine below, but
 * kept as the reference the state machine was transcribed from, and for the non-APPLE_HIDE path. */
static int selfenum_run(int p)
{
    UInt32 addr0 = 0, a = SELFENUM_ADDR, tot, o;
    int epIn = -1, epOut = -1, mpIn = 512, mpOut = 512, inMSC = 0, iface = -1, cfgVal = 1;
    UInt32 portsc;

    ehci_os_log("=== SELFENUM: enumerating the hidden device OURSELVES (no Apple) ===");
    ehci_os_logx("  port", (UInt32)p);

    /* 1. Reset the port and read the speed verdict (EHCI enables the port only for HIGH SPEED). */
    if (!selfenum_reset_port(p)) {
        portsc = ehci_read32(gSoftc.opBase, EHCI_PORTSC(p));
        if (portsc & EHCI_PORT_CONNECT) {
            /* Not high speed => this device belongs to the 1.1 companion. Hand the PORT over (EHCI Port
             * Owner = 1): the companion sees a fresh connect and Apple's NATIVE OHCI enumerates it. This is
             * the OS-X routing rule ("connected but not enabled after reset" = the full-speed hand-off
             * signal, NOT an error) and it is already HW-proven on this machine via CONFIGFLAG=0. */
            UInt32 w = ehci_read32(gSoftc.opBase, EHCI_PORTSC(p)) & ~EHCI_PORTSC_RW1C;
            w |= EHCI_PORT_OWNER;
            ehci_write32(gSoftc.opBase, EHCI_PORTSC(p), w);
            gPortCeded[p] = 1;                        /* n11: record the decision, don't re-read it back */
            ehci_os_log("  SELFENUM: not high-speed (port not enabled after reset) -> handed port to the 1.1 companion");
            ehci_os_logx("  portsc", portsc);
            gSelfEnumDone = 1;                        /* the companion owns it now; don't retry */
            return -1;
        }
        ehci_os_log("  SELFENUM: port did not enable and nothing is connected -> abort");
        return -2;
    }
    ehci_os_logx("  port ENABLED at high speed; portsc", ehci_read32(gSoftc.opBase, EHCI_PORTSC(p)));

    /* 2. Device descriptor at address 0 (first 8 bytes give bMaxPacketSize0). */
    gEnumDesc[0] = gEnumDesc[1] = 0;
    if (enum_ctrl_in(0x80, 0x06, 0x0100, 0, addr0, gEnumDesc, 8) < 0) {
        ehci_os_log("  SELFENUM FAIL: GET_DESCRIPTOR(device,8) @addr0"); return -3; }
    /* VALIDATE: pb_ctrl_phase only detects a TIMEOUT (no completion), not an error status — a STALLed phase
     * still bumps the completion counters and looks like success. */
    if (gEnumDesc[1] != 0x01 || gEnumDesc[0] < 18) {
        ehci_os_logx("  SELFENUM FAIL: bad device descriptor @addr0 (type/len)",
                     ((UInt32)gEnumDesc[1] << 8) | gEnumDesc[0]); return -3; }
    ehci_os_logx("  addr0 devDesc bLength/bMaxPacketSize0", ((UInt32)gEnumDesc[0] << 8) | gEnumDesc[7]);

    /* 3. SET_ADDRESS, then the mandatory recovery time before using the new address. */
    if (enum_ctrl_nodata(0x00, 0x05, (UInt16)a, 0, addr0) < 0) {
        ehci_os_log("  SELFENUM FAIL: SET_ADDRESS"); return -4; }
    enum_delay_ms(4);
    ehci_os_logx("  SET_ADDRESS ok; device now at", a);

    /* 4. Full device descriptor at the new address (confirms the address took). */
    gEnumDesc[0] = gEnumDesc[1] = 0;
    if (enum_ctrl_in(0x80, 0x06, 0x0100, 0, a, gEnumDesc, 18) < 0) {
        ehci_os_log("  SELFENUM FAIL: GET_DESCRIPTOR(device,18) @new addr"); return -5; }
    if (gEnumDesc[1] != 0x01) {   /* the address did not take, or the read stalled */
        ehci_os_logx("  SELFENUM FAIL: bad device descriptor at new addr (type)", (UInt32)gEnumDesc[1]); return -5; }
    ehci_os_logx("  VID", ((UInt32)gEnumDesc[9] << 8) | gEnumDesc[8]);
    ehci_os_logx("  PID", ((UInt32)gEnumDesc[11] << 8) | gEnumDesc[10]);

    /* 5. Config descriptor header -> wTotalLength, then the whole thing. */
    gEnumDesc[0] = gEnumDesc[1] = 0;
    if (enum_ctrl_in(0x80, 0x06, 0x0200, 0, a, gEnumDesc, 9) < 0) {
        ehci_os_log("  SELFENUM FAIL: GET_DESCRIPTOR(config,9)"); return -6; }
    if (gEnumDesc[1] != 0x02) {   /* bDescriptorType 2 = CONFIGURATION */
        ehci_os_logx("  SELFENUM FAIL: bad config descriptor (type)", (UInt32)gEnumDesc[1]); return -6; }
    tot = ((UInt32)gEnumDesc[3] << 8) | gEnumDesc[2];
    if (tot < 9 || tot > sizeof(gEnumDesc)) { ehci_os_logx("  SELFENUM FAIL: bad wTotalLength", tot); return -7; }
    gEnumDesc[1] = 0;
    if (enum_ctrl_in(0x80, 0x06, 0x0200, 0, a, gEnumDesc, tot) < 0) {
        ehci_os_log("  SELFENUM FAIL: GET_DESCRIPTOR(config,full)"); return -8; }
    if (gEnumDesc[1] != 0x02) {
        ehci_os_logx("  SELFENUM FAIL: bad full config descriptor (type)", (UInt32)gEnumDesc[1]); return -8; }
    cfgVal = gEnumDesc[5];
    ehci_os_logx("  config wTotalLength", tot);
    ehci_os_logx("  bConfigurationValue", (UInt32)cfgVal);

    /* 6. Walk the config: find the Mass-Storage / Bulk-Only interface, then ITS bulk endpoints.
     *    (interface: class 0x08, protocol 0x50 = BOT, altSetting 0; endpoint: bmAttributes&3 == 2 = bulk) */
    o = 0;
    while (o + 2u <= tot) {
        UInt8 blen = gEnumDesc[o], btype = gEnumDesc[o + 1];
        if (blen == 0) break;
        if (btype == 0x04 && blen >= 9) {                          /* INTERFACE */
            inMSC = (gEnumDesc[o + 5] == 0x08 && gEnumDesc[o + 7] == 0x50 && gEnumDesc[o + 3] == 0x00);
            if (inMSC) iface = gEnumDesc[o + 2];
        } else if (btype == 0x05 && blen >= 7 && inMSC) {           /* ENDPOINT of that interface */
            UInt8 eaddr = gEnumDesc[o + 2], attr = gEnumDesc[o + 3];
            int mp = (int)(((UInt32)gEnumDesc[o + 5] << 8) | gEnumDesc[o + 4]);
            if ((attr & 0x03) == 0x02) {                           /* bulk */
                if (eaddr & 0x80) { if (epIn  < 0) { epIn  = eaddr & 0x0F; mpIn  = mp ? mp : 512; } }
                else              { if (epOut < 0) { epOut = eaddr & 0x0F; mpOut = mp ? mp : 512; } }
            }
        }
        o += blen;
    }
    if (iface < 0 || epIn < 0 || epOut < 0) {
        ehci_os_log("  SELFENUM FAIL: no Mass-Storage/Bulk-Only interface with both bulk endpoints");
        ehci_os_logx("  iface", (UInt32)iface); ehci_os_logx("  epIn", (UInt32)epIn); ehci_os_logx("  epOut", (UInt32)epOut);
        return -9;
    }
    ehci_os_logx("  MSC interface", (UInt32)iface);
    ehci_os_logx("  bulk-IN ep / maxpkt",  ((UInt32)epIn  << 16) | (UInt32)mpIn);
    ehci_os_logx("  bulk-OUT ep / maxpkt", ((UInt32)epOut << 16) | (UInt32)mpOut);

    /* 7. Configure the device (bulk endpoints are dead until this is done). */
    if (enum_ctrl_nodata(0x00, 0x09, (UInt16)cfgVal, 0, a) < 0) {
        ehci_os_log("  SELFENUM FAIL: SET_CONFIGURATION"); return -10; }
    ehci_os_log("  SET_CONFIGURATION ok");

    /* 8. Register the bulk endpoints — exactly what Apple's slot-6 CreateBulkEndpoint used to do. */
    (void)ehci_vhub_create_bulk(a, (UInt32)epOut, 0, (UInt32)mpOut);
    (void)ehci_vhub_create_bulk(a, (UInt32)epIn,  1, (UInt32)mpIn);
    ehci_os_log("  bulk endpoints registered (we replaced Apple's CreateBulkEndpoint)");
    return 0;
}

/* ==================== n3b: PORT MAP / insert visibility ====================
 * Every port's raw PORTSC is logged whenever it changes, so (a) any insert is visible even when we do not
 * act on it, and (b) the values are a permanent SOCKET -> PORT NUMBER map. Task level only. Active only
 * while we have NOT yet enumerated, so it can never spam a working session.
 * Log format 0xPnnnn: top nibble = port, low 16 = PORTSC.
 *   bit0 CCS  bit1 CSC  bit2 PED(=high speed)  bit8 PR  bits11:10 LineStatus  bit12 PP
 *   bit13 Port Owner (1 = the 1.1 companion owns it, 0 = we do) */
static UInt32 gPmLast[15];
static UInt32 gPmNextIdle = 0;
static int    gPmPrimed = 0;
static void portmap_tick(void)
{
    UInt32 now = *(volatile UInt32 *)0x016AUL;          /* lowmem Ticks (60 Hz) */
    int p, np = (int)gSoftc.nPorts, changed = 0;
    if (gSelfEnumDone || np <= 0 || np > 15 || gSoftc.opBase == 0) return;
    for (p = 0; p < np; p++) {
        UInt32 v = ehci_read32(gSoftc.opBase, EHCI_PORTSC(p));
        if ((v & 0x3005UL) != (gPmLast[p] & 0x3005UL)) changed = 1;   /* CCS|PED|PP|Owner — ignore RW1C churn */
        gPmLast[p] = v;
    }
    if (!gPmPrimed) { gPmPrimed = 1; changed = 1; }      /* always emit one baseline map */
    if (!changed && (long)(now - gPmNextIdle) < 0) return;
    gPmNextIdle = now + 900UL;                           /* ~15 s between idle reminders */
    ehci_os_log(changed ? "n3b PORTMAP (CHANGED) 0xPnnnn = port<<28 | PORTSC; bit0=connected bit2=enabled bit12=power bit13=companion-owns:"
                        : "n3b PORTMAP (idle — nothing connected on any port we poll):");
    for (p = 0; p < np; p++)
        ehci_os_logx("  port|PORTSC", ((UInt32)p << 28) | (gPmLast[p] & 0xFFFFUL));
}

/* ================================================================================================
 * ★★★★ n5: ASYNC ENUMERATION + PROBE ENGINE ★★★★
 *
 * WHY: everything above runs at TASK level and blocks in pb_wait()/pb_ctrl_phase, which spin-wait on
 * counters the SIH updates. That is why discovery has always needed an application — slot 23 is only
 * called from USLPolledProcessDoneQueue, i.e. an app's pump loop. Calling the synchronous probe from our
 * own heartbeat instead would DEADLOCK: the heartbeat IS the thing that would have to retire the transfer
 * it is now blocked waiting for. So the sequence has to stop blocking. Same sequence, every wait a return.
 *
 * Three layers, so the conversion stays faithful to the proven code above rather than being a rewrite:
 *   ctl_step()   one control transfer (SETUP -> [DATA] -> STATUS) = enum_ctrl_in/nodata
 *   bot_step()   one BOT command (CBW -> [DATA] -> CSW) incl. BOT 5.3.4 stall recovery = pb_cmd_in
 *   as_advance() the enumeration + probe sequence = selfenum_run then selfprobe_sync
 *
 * ⚠ RUNS AT INTERRUPT LEVEL. Every log call here MUST be ehci_os_ilog/ilogx (the ring), never
 * ehci_os_log — that one is File Manager and hangs the machine below task level (r18, and again in n4).
 * ================================================================================================ */

/* ---------- layer 1: one control transfer ---------- */
typedef struct {
    int    pc, dir;            /* dir: 1 = control-IN (has DATA-IN), 0 = no-data */
    UInt32 addr, len, t0;
    UInt32 got;                /* h13: bytes the DATA-IN phase actually delivered (0 for a no-data control) */
    volatile UInt8 *buf;
} CtlState;
static CtlState gCtl;
/* ★★★★ h13: how many bytes the last completed control-IN delivered, and whether every phase of it was
 * clean. Callers that parse a fixed-size reply MUST check ctl_got() before believing their buffer. */
static UInt32 ctl_got(void) { return gCtl.got; }
static void ctl_begin(UInt8 bmRT, UInt8 bReq, UInt16 wValue, UInt16 wIndex,
                      UInt32 addr, volatile UInt8 *buf, UInt32 len, int dir)
{
    enum_setup(bmRT, bReq, wValue, wIndex, (UInt16)(dir ? len : 0));
    gCtl.pc = 0; gCtl.addr = addr; gCtl.buf = buf; gCtl.len = len; gCtl.dir = dir;
    gCtl.got = 0;                /* h13: no bytes yet — never let a caller read last request's length */
}
/* 0 = still running, 1 = complete, -1 = failed. Waits on ctl_done() — the DOWN counters — never pb_ready().
 * ★★★★ h13: ctl_done() only reports that a COUNTER MOVED, and gDownErr / gDownTimeouts move the very same
 * counter as gDownDone — so before h13 an errored or timed-out phase read as a successful one and this
 * function returned 1 (complete). The caller then parsed whatever was already in its buffer. gDpLastStat is
 * the reap's verdict (0 = clean, -6640 = error/watchdog), so each phase now checks it. */
/* ★★★★★★ h54 — THE CONTROL-FAILURE PROBE. Diagnostic only: it changes NO behaviour and is the entire
 * point of the m6 ROM.
 *
 * ⚠ WHAT IT IS FOR. On the mini, a drive attached at COLD BOOT enables at high speed on every attempt
 * (PORTSC 0x1005) and then its first control transfer to address 0 dies three times, and the port is parked.
 * The in-flight dump from 2026-08-11 read gAs.pc 2, gDownDone 1 (only the SETUP retired), gIsrHits 3, and
 * gDownErr / gDownTimeouts BOTH 0 — issued, never reaped, invisible to the watchdog, which is the recorded
 * h18 signature. The MDD does the same thing successfully (h47+v8 phase 1, four clean runs), so this is
 * mini-specific, and the mini-specific thing on this path is sharedCompanion = 1: our ISR chains Apple's on
 * every one of our own interrupts.
 *
 * ★ AND THE EXISTING LOG CANNOT TELL US WHICH FAILURE IT IS. "control transfer FAILED at step 0x02" prints
 * the AS_CTL label, not gCtl.pc, so SETUP / DATA-IN / STATUS are indistinguishable, and a timeout is
 * indistinguishable from a transfer that reported an error. This prints both, plus the four things that
 * separate the candidates:
 *
 *   qTD token, bit 7 ACTIVE:  SET   -> the controller NEVER finished it  => a schedule / doorbell problem
 *                             CLEAR -> it finished and WE NEVER REAPED   => the SIH / reap path
 *   USBINTR:                  0     -> our interrupts are still MASKED. The ISR masks on entry and only the
 *                                      SIH restores them, so 0 here means the SIH did not get to run.
 *   FRINDEX:                  frozen across the three failures => the controller's schedule is not running
 *                                      at all, which is a different fault again.
 *   gSihRuns:                 flat across the failures => the SIH is starved (the shared-line suspicion);
 *                                      climbing => it runs and the reap still misses.
 *
 * ⚠ ilogx ONLY — ctl_step runs from the SIH, and ehci_os_log is synchronous File Manager I/O, which hangs
 * the machine below task level. This has bitten three times (r18, n4, the r95 bulk trace). */
/* ★ h55: the CONTROL per-phase cap. Was 800 ms since the engine was written; r53 raised the BULK watchdog to
 * 10 s for a NAKing device and never revisited this one. 5 s sits comfortably inside that 10 s backstop.
 * ⚠ Deliberately NOT applied to pb_ctrl_phase (the blocking task-level form) — a different, largely dead path;
 * changing something this run does not exercise would only add risk. */
#define CTL_PHASE_MS 5000UL
/* ★★★★★★ h55 — THE CAP, AND THE DUMP THAT THE CAP WAS HIDING.
 *
 * ⚠⚠ TWO READINGS SURVIVED h54, AND THIS BUILD SEPARATES THEM. h54 measured, in all three failures: qTD
 * ACTIVE, CERR = 3, no error bits, not halted, USBINTR 0x7 (not masked), gSihRuns climbing (SIH alive),
 * FRINDEX moving and both schedules enabled. So the qTD was armed and never executed. That leaves:
 *   (a) THE DEVICE IS NAKing — busy after its power-cycle. A NAK does NOT decrement CERR, which is exactly
 *       why CERR reads an untouched 3. ★ THIS PROJECT HAS SEEN THIS SIGNATURE BEFORE AND CALLED IT CORRECTLY:
 *       the r53 fix quotes the r39 snapshot as "qTD ACTIVE, CERR=3 (no errors), NOT halted ... schedule
 *       running on our QH -- i.e. a BUSY device, not a fault", and the answer there was PATIENCE.
 *   (b) THE QH IS UNREACHABLE — active, but the async ring does not reach it.
 *
 * ★ CHANGE 1, THE CAP. r53 raised the BULK watchdog to 10 s for exactly reading (a) and the CONTROL phase cap
 * was never revisited: it is still 800 ms, TWELVE TIMES shorter, and it is what gives up here. CTL_PHASE_MS
 * takes it to 5 s — still well inside the 10 s down watchdog, and Apple's own stack uses ~30 s.
 * ⚠ COST IF (b) IS RIGHT: three attempts at 5 s = ~15 s before the port parks, so on the mini the keyboard
 * stays hostage ~15 s instead of ~2.4 s. Known, accepted for one run, and flagged in the test card.
 *
 * ★ CHANGE 2, THE DUMP. capture_timeout_state() already snapshots USBCMD / USBSTS / ASYNCLISTADDR / the QH
 * address / epChar / curQtd / ovlToken / the qTD token — precisely what separates (a) from (b). It has never
 * run on this path because it hangs off the 10 s down watchdog, and the 800 ms control cap always fired
 * first. So the one dump that could settle this was itself unreachable. Now the control failure calls it.
 *
 * ★★ HOW TO READ IT: ovlToken/curQtd pointing at our ACTIVE qTD, with ASYNCLISTADDR reaching gCtrlQ.qhP,
 * means the controller IS parked on our QH and the device is simply not answering => (a). A terminated or
 * stale overlay, or an anchor link that does not lead to our QH, means => (b), and gDownRelink/gLastAnchorLink
 * say whether the r60 backstop ever noticed. */
static void h54_ctl_fail(void)
{
    /* THE SPLIT: control now lives in the ENGINE slot, so this diagnostic must read gEg*. Pointing it at
     * gDp* would describe BLOCK I/O's transfer while a CONTROL transfer failed — a diagnostic that lies,
     * which is I10 and has cost this project cycles four times. */
    UInt32 tok = gEgTd ? ehci_le32_to_cpu(gEgTd->token) : 0xFFFFFFFFUL;
    volatile void *op = gSoftc.opBase;
    capture_timeout_state(gEgQ, gEgTd);   /* h55: fill the gTo* snapshot on THIS path for the first time */
    ehci_os_ilogx("!! h54 CTL FAIL: gCtl.pc<<16|gCtlStat (pc 1=SETUP 2=DATA-IN 3=STATUS)",
                  ((UInt32)gCtl.pc << 16) | (gCtlStat & 0xFFFFUL));
    ehci_os_ilogx("!! h54   ms waited in this phase (>800 = the cap fired; <800 = a status error)",
                  frame_ms() - gCtl.t0);
    ehci_os_ilogx("!! h54   ★ qTD TOKEN — bit 7 (0x80) SET = controller NEVER finished it; CLEAR = it "
                  "finished and WE NEVER REAPED", tok);
    ehci_os_ilogx("!! h54   ★ USBINTR — 0 = OUR INTERRUPTS ARE STILL MASKED (ISR masks, only the SIH restores)",
                  op ? ehci_read32(op, EHCI_USBINTR) : 0xFFFFFFFFUL);
    ehci_os_ilogx("!! h54   USBSTS", op ? ehci_read32(op, EHCI_USBSTS) : 0xFFFFFFFFUL);
    ehci_os_ilogx("!! h54   ★ FRINDEX — frozen across the three failures = the schedule is not running",
                  op ? ehci_read32(op, EHCI_FRINDEX) : 0xFFFFFFFFUL);
    ehci_os_ilogx("!! h54   ★ gSihRuns — flat across the failures = the SIH is STARVED", gSihRuns);
    ehci_os_ilogx("!! h54   gSihQueued<<16|gIsrConsec", ((UInt32)gSihQueued << 16) | (gIsrConsec & 0xFFFFUL));
    ehci_os_ilogx("!! h54   gIsrHits", gIsrHits);
    ehci_os_ilogx("!! h54   gDownDone<<16|gDownErr", ((gDownDone & 0xFFFFUL) << 16) | (gDownErr & 0xFFFFUL));
    ehci_os_ilogx("!! h54   gDownTimeouts<<16|gEgBusy (engine slot)", ((gDownTimeouts & 0xFFFFUL) << 16) | (UInt32)(gEgBusy & 1));
    ehci_os_ilogx("!! h54   gVhubTick (8 ms heartbeat — advancing = the SIH source is alive)", gVhubTick);
    /* ---- h55: the snapshot that the 800 ms cap has been hiding. THESE FOUR SETTLE (a) vs (b). ---- */
    ehci_os_ilogx("!! h55   ★ ASYNCLISTADDR — compare with gCtrlQ.qhP below and the anchor link", gToAsync);
    ehci_os_ilogx("!! h55   ★ the ACTIVE QH's phys addr (gEgQ->qhP) — should BE our control QH", gToQhP);
    ehci_os_ilogx("!! h55   ★ our control QH phys (gCtrlQ.qhP) — if these two differ, we are on the wrong QH",
                  gCtrlQ.qhP);
    ehci_os_ilogx("!! h55   ★ QH ovlToken — ACTIVE(0x80) here = the controller is PARKED ON OUR qTD and the "
                  "DEVICE IS NAKing (reading a). Terminated/stale = unreachable (reading b)", gToQhOvlTok);
    ehci_os_ilogx("!! h55   ★ QH curQtd — should point at the qTD we armed", gToQhCurQtd);
    ehci_os_ilogx("!! h55   QH epChar (endpoint characteristics: addr, ep, speed, maxpkt)", gToQhEpChar);
    ehci_os_ilogx("!! h55   USBCMD (bit5 ASE = async schedule enable, bit0 RS = run/stop)", gToCmd);
    ehci_os_ilogx("!! h55   anchor->hlink NOW (masked) — must lead into one of our QHs",
                  gSoftc.asyncAnchor ? QH_LINK_PTR(gSoftc.asyncAnchor->hlink) : 0xFFFFFFFFUL);
    ehci_os_ilogx("!! h55   gDownRelink<<16|gLastAnchorLink&0xFFFF (r60 backstop: did it ever fire?)",
                  ((gDownRelink & 0xFFFFUL) << 16) | (gLastAnchorLink & 0xFFFFUL));
}
/* ★★★★★★★ h61 — INSTRUMENT WHAT THE DEVICE ACTUALLY RETURNS. THE PIVOT.
 *
 * ⚠ WHY THIS EXISTS. Six builds adjusted OUR side — the control cap (h55), the halt recovery (h57), the
 * engine release (h56), the exposure latch and guard (h58/h59), a bring-up settle (h60, which locked the
 * driver out and cost a CD boot). Every one fixed something real; none made a cold-boot-attached drive
 * enumerate. The one thing never measured is THE DEVICE'S ANSWER.
 *
 * ★★ AND THE COMPARISON IS THE INSTRUMENT. A hot-plugged drive ALWAYS works; the same drive attached at cold
 * boot fails. So this traces EVERY control phase of EVERY enumeration UNCONDITIONALLY — success and failure
 * alike — and the run captures both cases in ONE log: boot with the drive attached (fails), then hot-plug it
 * (works). The difference is then read off directly instead of inferred across runs, which is what every
 * previous cycle had to do.
 *
 * WHAT IT ANSWERS, per phase: did anything come back at all, was it partial, and how long did it take.
 *   · phase + status + elapsed ms          — where, and how fast
 *   · qTD token AND QH overlay             — the two words h55/h57 proved disagree
 *   · bytes requested vs actually delivered — a SHORT answer looks nothing like a silent one
 *   · the first 8 bytes of the buffer      — a real device descriptor starts 12 01; garbage or zeros do not
 * ⚠ ilogx only: ctl_step runs from the SIH. */
static void h61_trace(const char *tag)
{
    /* ★ THE SPLIT: h61 traces CONTROL phases (it reads gCtl.pc/gCtl.dir/gCtlStat), and control now lives in
     * the ENGINE slot. Left on gDp* this would print BLOCK I/O's qTD and overlay beside a control phase's
     * status — a diagnostic that lies, which is I10 and is exactly the class of error that has cost this
     * project four separate hunts. h61 is also the trace that found the TRSTRCY root cause, so it is the
     * last one that should be allowed to drift. */
    UInt32 tok = gEgTd ? ehci_le32_to_cpu(gEgTd->token) : 0xFFFFFFFFUL;
    UInt32 ovl = gEgQ ? ehci_le32_to_cpu(gEgQ->qh->ovlToken) : 0xFFFFFFFFUL;
    ehci_os_ilog(tag);
    ehci_os_ilogx("    h61 pc<<24|dir<<16|status", ((UInt32)gCtl.pc << 24) | ((UInt32)gCtl.dir << 16)
                                                   | (gCtlStat & 0xFFFFUL));
    ehci_os_ilogx("    h61 ms in phase", frame_ms() - gCtl.t0);
    ehci_os_ilogx("    h61 qTD token", tok);
    ehci_os_ilogx("    h61 QH ovlToken", ovl);
    ehci_os_ilogx("    h61 requested<<16|delivered (short answer != silent one)",
                  ((gCtl.len & 0xFFFFUL) << 16) | (gCtlActual & 0xFFFFUL));
    if (gCtl.buf) {
        ehci_os_ilogx("    h61 buf[0..3]  (a real device descriptor starts 0x12 0x01)",
                      ((UInt32)gCtl.buf[0] << 24) | ((UInt32)gCtl.buf[1] << 16)
                      | ((UInt32)gCtl.buf[2] << 8) | (UInt32)gCtl.buf[3]);
        ehci_os_ilogx("    h61 buf[4..7]",
                      ((UInt32)gCtl.buf[4] << 24) | ((UInt32)gCtl.buf[5] << 16)
                      | ((UInt32)gCtl.buf[6] << 8) | (UInt32)gCtl.buf[7]);
    }
}
static int ctl_step(void)
{
    switch (gCtl.pc) {
    case 0:
        /* h61: PORTSC and the time since the port enabled, immediately before the first packet goes out.
         * This is the number the whole cold-boot-vs-hot-plug question turns on. */
        ehci_os_ilogx("  h61 SETUP about to issue; PORTSC",
                      (gPortEnabledPort >= 0 && gSoftc.opBase)
                          ? ehci_read32(gSoftc.opBase, EHCI_PORTSC(gPortEnabledPort)) : 0xFFFFFFFFUL);
        ehci_os_ilogx("  h61   ms since this port enabled (hot-plug gives the device far more than cold boot)",
                      frame_ms() - gPortEnabledMs);
        ctl_issue(gEnumSetup, gCtl.addr, 8, 2);                         /* SETUP */
        gCtl.pc = 1; gCtl.t0 = frame_ms(); return 0;
    case 1:
        if (!ctl_done()) { if (frame_ms() - gCtl.t0 <= CTL_PHASE_MS) return 0; h54_ctl_fail(); return -1; }
        if (gCtlStat != 0) { h61_trace("  h61 SETUP phase DONE (error)"); h54_ctl_fail(); return -1; }
        h61_trace("  h61 SETUP phase DONE");                            /* h13: the SETUP itself failed */
        if (gCtl.dir) {
            ctl_issue(gCtl.buf, gCtl.addr, gCtl.len, 1);                /* DATA-IN */
            gCtl.pc = 2; gCtl.t0 = frame_ms(); return 0;
        }
        ctl_issue(gEnumDesc, gCtl.addr, 0, 1);                          /* STATUS-IN (no-data control) */
        gCtl.pc = 3; gCtl.t0 = frame_ms(); return 0;
    case 2:
        if (!ctl_done()) { if (frame_ms() - gCtl.t0 <= CTL_PHASE_MS) return 0; h54_ctl_fail(); return -1; }
        if (gCtlStat != 0) { h61_trace("  h61 DATA-IN phase DONE (error)"); h54_ctl_fail(); return -1; }
        h61_trace("  h61 ★ DATA-IN phase DONE — THIS is the device's answer");
        gCtl.got = gCtlActual;   /* h13: what actually landed, from THIS control completion */
        ctl_issue(gEnumDesc, gCtl.addr, 0, 0);                          /* STATUS-OUT */
        gCtl.pc = 3; gCtl.t0 = frame_ms(); return 0;
    case 3:
        if (!ctl_done()) { if (frame_ms() - gCtl.t0 <= CTL_PHASE_MS) return 0; h54_ctl_fail(); return -1; }
        if (gCtlStat != 0) { h61_trace("  h61 STATUS phase DONE (error)"); h54_ctl_fail(); return -1; }
        h61_trace("  h61 STATUS phase DONE — control transfer COMPLETE");
        return 1;
    }
    h54_ctl_fail();
    return -1;
}

/* ---------- layer 2: one BOT command (the async pb_cmd_in) ---------- */
typedef struct {
    int    pc, cdbLen, csw, stalled;
    UInt32 dataLen, saveLen, t0;
    const UInt8 *cdb;
    UInt8 *save;
} BotState;
static BotState gBot;
static void bot_begin(const UInt8 *cdb, int cdbLen, UInt32 dataLen, UInt8 *save, UInt32 saveLen)
{
    gBot.pc = 0; gBot.cdb = cdb; gBot.cdbLen = cdbLen; gBot.dataLen = dataLen;
    gBot.save = save; gBot.saveLen = saveLen; gBot.csw = -1; gBot.stalled = 0;
}
/* 0 = running, 1 = complete (gBot.csw = CSW status), -1 = transport failure. */
static int bot_step(int d)
{
    switch (gBot.pc) {
    case 0:
        pb_cbw(d, gBot.cdb, gBot.cdbLen, gBot.dataLen);
        gBot.pc = 1; gBot.t0 = frame_ms(); return 0;
    case 1:
        if (!pb_ready()) return (frame_ms() - gBot.t0 > 800UL) ? -1 : 0;
        if (pb_failed()) return -1;                       /* the CBW itself never went out */
        if (gBot.dataLen) { pb_in(d, gBot.dataLen); gBot.pc = 2; gBot.t0 = frame_ms(); return 0; }
        pb_in(d, 13); gBot.pc = 4; gBot.t0 = frame_ms(); return 0;
    case 2:
        if (!pb_ready()) return (frame_ms() - gBot.t0 > 800UL) ? -1 : 0;
        if (pb_failed()) {                                /* data phase STALLED (BOT 5.3.4): clear the halt,
                                                           * but STILL read the CSW so the device stays in sync */
            gBot.stalled = 1;
            ctl_begin(0x02, 0x01, 0x0000,
                      (UInt16)(gBulkEP[gDev[d].pIn].endpt | 0x80u), gBulkEP[gDev[d].pIn].addr, gPB, 0, 0);
            gBot.pc = 3; return 0;
        }
        if (gBot.save && gBot.saveLen) {
            UInt32 k; for (k = 0; k < gBot.saveLen; k++) gBot.save[k] = gPB[k];
        }
        pb_in(d, 13); gBot.pc = 4; gBot.t0 = frame_ms(); return 0;
    case 3: {
        int r = ctl_step();
        if (r == 0) return 0;
        pb_in(d, 13); gBot.pc = 4; gBot.t0 = frame_ms(); return 0;   /* recovery is advisory; read the CSW */
    }
    case 4:
        if (!pb_ready()) return (frame_ms() - gBot.t0 > 800UL) ? -1 : 0;
        if (pb_failed()) {
            ctl_begin(0x02, 0x01, 0x0000,
                      (UInt16)(gBulkEP[gDev[d].pIn].endpt | 0x80u), gBulkEP[gDev[d].pIn].addr, gPB, 0, 0);
            gBot.pc = 5; return 0;
        }
        gBot.csw = pb_csw(); return 1;
    case 5: {
        int r = ctl_step();
        if (r == 0) return 0;
        return -1;                                        /* the CSW itself stalled = transport failure */
    }
    }
    return -1;
}

/* ---------- layer 3: the enumeration + probe sequence ---------- */
/* CDBs must have STATIC lifetime: bot_begin only keeps a pointer and the command is issued across ticks,
 * long after the caller's frame is gone. */
static const UInt8 gAsCdbInq[6]   = {0x12,0,0,0,36,0};          /* INQUIRY            */
static const UInt8 gAsCdbTur[6]   = {0x00,0,0,0,0,0};           /* TEST UNIT READY    */
static const UInt8 gAsCdbSense[6] = {0x03,0,0,0,18,0};          /* REQUEST SENSE, 18B */
static const UInt8 gAsCdbCap[10]  = {0x25,0,0,0,0,0,0,0,0,0};   /* READ CAPACITY      */
static UInt8 gAsCdbRd[10];                                      /* READ(10), built per use */
static UInt8 gAsInq[36], gAsCap[8], gAsBlk0[512];
typedef struct {
    int    pc, port, running, done, failed;
    int    dev;                 /* n15: gDev[] slot this enumeration is filling; -1 = none allocated yet */
    UInt32 addr, tot, o, t0;
    int    epIn, epOut, mpIn, mpOut, inMSC, iface, cfgVal, tur;
    UInt32 blocks, blkSize;
    /* ★ h1 HUB PROBE: loop state for walking a downstream hub's ports. hubPorts = bNbrPorts from the hub
     * descriptor, hubPort = the 1-based port being queried. Kept in gAs because the walk yields across
     * heartbeats like every other step here — a local would not survive the return. */
    int    hubPorts, hubPort;
    /* ★ h3 tier 1: 0 = this enumeration is on a ROOT port (reset via PORTSC, the proven path); nonzero = the
     * device is behind the claimed hub at this address, so reset and speed come from hub class requests
     * instead. Only the RESET steps differ — every later step is a control transfer to an address, which the
     * hub forwards transparently, so there is ONE enumeration path and no divergence to keep in sync. */
    UInt32 hubAddr;
    UInt32 chBits;              /* h11: the port's wPortChange, carried across the acknowledge yields */
    UInt32 pump0;               /* m3: gTaskPumpN sampled when an AS_TASK park was entered — see AS_TASK */
} AsState;
static UInt8 gAsHubDesc[16];   /* h1: the downstream hub's own class descriptor (type 0x29) */
static UInt8 gAsDevDesc[18];   /* h1: device descriptor re-read to get bDeviceClass (gEnumDesc is reused) */
static UInt8 gAsPortSt[4];     /* h1: wPortStatus + wPortChange for one downstream port */
/* n15: `dev` must start at -1 ("no slot allocated"). A plain static would zero-initialise it to 0, which
 * reads as "slot 0 is already mine" and would let a later device silently reuse a mounted device's slot. */
static AsState gAs = { .dev = -1 };
static volatile int gAsBusy = 0;      /* re-entrancy guard: heartbeat and uim23 may both call in */
/* Set at interrupt level when the async probe has fully verified the medium; consumed at TASK level, which
 * is where 'Eusb' publication and the block-driver handoff have to happen (NewGestaltValue and
 * InstallDriverFromMemory are both task-only). This flag IS the interrupt->task boundary of the design. */
static volatile int gAsProbeOK = 0;
/* ★★★ f4 DIRECTION B, the body — declared far above (before bio_issue_read), defined here because it reads
 * gAs.running and gAs is only in scope now. See the long f4 note beside the counters. Pure observation. */
static void f4_note_bio_arm(void)
{
    /* ★★★★★★ THE INSTRUMENT THAT PROVES THE SPLIT. Before the split, arming block I/O while the ENGINE had a
     * transfer in flight WAS the clobber — it wiped the slot and orphaned the victim's qTD. Now the engine
     * has its own slot and its own bounce, so the same moment is HARMLESS. Counting it is what turns "the
     * split should work" into "the split demonstrably did work, N times, on this boot".
     * ⇒ Expect gSplitSaved > 0 and gF4Clobber* == 0. If gF4ClobberProbe or gF4ClobberCtl is EVER non-zero
     * again, something has re-merged the slots. */
    if (gEgBusy) gSplitSaved++;
    if (!gDpBusy) return;               /* the common, healthy case: block I/O's own slot was free */
    /* Reaching here now means BLOCK I/O armed over BLOCK I/O — the engine cannot be the victim any more.
     * That would be a bio-engine bug (bio_kick/bio_advance are supposed to be strictly serial), so the
     * counter changes meaning from "the known defect" to "an unexpected one". Keep it loud. */
    gF4Clobber++;
    switch (gF4Owner) {
        case F4_OWNER_PROBE: gF4ClobberProbe++; break;   /* ★ the dangerous one */
        case F4_OWNER_CTL:   gF4ClobberCtl++;   break;
        case F4_OWNER_BIO:   gF4ClobberBio++;   break;
        default: break;
    }
    if (gAs.running) gF4ClobberEnum++;
    if (!gF4ClobberFirstMs) { UInt32 t = frame_ms(); gF4ClobberFirstMs = t ? t : 1; }
}
/* Set by the engine at interrupt level when it needs TASK level to register the bulk endpoints; cleared by
 * selfprobe_tick once done. create_bulk must not run from the SIH — see the note at its call site. */
static volatile int gAsNeedBulk = 0;
/* ★★★★ m3: COUNTS THE TASK-LEVEL PUMP, so the AS_TASK park can tell "the pump is broken" (the fault the
 * park's deadline was written to catch) from "the pump has not had a turn yet" (which is not a fault at all).
 * Incremented in selfprobe_tick immediately before the gAsNeedBulk check, so an advance here means the check
 * was actually EVALUATED — not merely that the function was entered. See the note on AS_TASK. */
static volatile UInt32 gTaskPumpN = 0;

/* ★★★ n12: PARK a port we cannot drive — and DO NOT TOUCH THE HARDWARE DOING IT.
 *
 * n11 tried to hand such a port to the 1.1 companion: clear Port Enable, then set Port Owner. The n11
 * hardware run REFUTED both halves. The log is explicit —
 *     n11 PORTSC before cede 0x00001005
 *     n11 PORTSC after cede  0x00001002     (bit 13 CLEAR)
 * so Port Owner does NOT stick on this NEC controller even from the not-enabled state that EHCI §2.3.9
 * describes as the handoff point. Worse, clearing Port Enable left the port in a state the driver could not
 * recover from: it came back connected AND enabled (0x1005) with our QHs still addressed to the old device,
 * so from then on EVERY transfer went unanswered — zero interrupts (gIsrHits flat at 0x1ed), zero
 * completions (gDownDone flat at 0xdb), the 10s watchdog firing repeatedly (gDownTimeouts 0→5) and
 * enumeration re-arming six times, none of which could exchange a single packet. That is the "annoy the app
 * and mounting stops working until a reboot, but nothing freezes" report — and it was self-inflicted.
 *
 * So parking means exactly what the word says: STOP. No PORTSC write of any kind (the device is left
 * enumerated and idle, which is harmless), mark the port so we never re-arm on it, keep it HIDDEN from Apple
 * because we still own it, and return the transfer engine to a clean idle so a device on ANOTHER port still
 * works. Making a hub usable needs real hub support, not a handoff this controller will not perform. */
static void park_port(int p, const char *why)
{
    if (p < 0 || p >= 15) return;
    gPortParked[p] = 1;                      /* survives every port event EXCEPT the device leaving — n24 */
    /* ★★★★★★★ h70 — PARKING ONE PORT MUST NOT END ENUMERATION FOR THE WHOLE CONTROLLER.
     * This is h39's defect on a THIRD path. h39 fixed the CEDE path, h56 fixed the SUCCESS path, and park_port
     * was never revisited -- while THIS function's own comment states the intent it was violating: "return the
     * transfer engine to a clean idle SO A DEVICE ON ANOTHER PORT STILL WORKS".
     * ⚠ gPortParked[p] ALREADY records the decision per port, and the h39 scan consults it. Setting the GLOBAL
     * as well killed enumeration for every other port.
     * ★ AND IT IS THE DIRECT CAUSE OF THE SYMPTOM THE USER HAS REPORTED ON EVERY FAILING BOOT. When the drive
     * fails 3x and parks, the global stop meant the KEYBOARD'S port was never serviced -- so it was never
     * ceded to Apple's companion, and input stayed dead until the user PHYSICALLY MOVED the keyboard, whose
     * connect event clears gSelfEnumDone in service_ports and lets enumeration resume. "Had to switch the
     * keyboard over" was this line, every time.
     * ⇒ Found by the mechanical exposure-path flag sweep (2026-08-11), not by another hardware run: 11 writes
     * to gSelfEnumDone across 7 functions is what made it visible. Same technique, and same payoff, as the n24
     * PER-DEVICE-SWEEP that found three bugs for zero hardware cycles. */
    gAs.running = 0; gAs.pc = 0;
    ehci_os_ilog("  h70: port PARKED but enumeration NOT ended for the controller — another port may still "
                 "hold a device (gPortParked already records this port)");
    if (gSelfEnumPort == p) gSelfEnumPort = -1;
    dp_engine_idle();                        /* never leave a transfer in flight on a port we just gave up */
    ehci_os_ilog(why);
    ehci_os_ilogx("  n12 parked port (left electrically UNTOUCHED — no PORTSC write)", (UInt32)p);
}
static void as_fail(const char *why, UInt32 v)
{
    ehci_os_ilogx(why, v);
    gAs.running = 0; gAs.failed = 1; gAs.pc = 0;
    /* ★ n15: release a slot we allocated for an enumeration that never completed. gAs is a static struct so
     * `dev` zero-initialises to 0; without this reset a later device would silently reuse slot 0 and
     * overwrite a mounted device's state — the n12 failure, reintroduced by the back door. */
    if (gAs.dev > 0) {                 /* n14 step 3: unconditional. SELFENUM COMPLETE sets inUse before
                                        * the BOT probe runs, so gating this on !inUse would leak the slot
                                        * for any device that enumerated but then failed to probe. */
        int k;
        for (k = 0; k < NBULK; k++)
            if (gBulkEP[k].used && gBulkEP[k].dev == (UInt8)gAs.dev) gBulkEP[k].used = 0;
        gDev[gAs.dev].inUse = 0;
        gDev[gAs.dev].pstate = 0; gDev[gAs.dev].probedPort = -1;
        gDev[gAs.dev].pOut = -1;  gDev[gAs.dev].pIn = -1;
    }
    gAs.dev = -1;
    /* ★★★★ h5: A HUB-SWEEP FAILURE MUST ADVANCE THE SWEEP. h4 did not, so the same downstream port was
     * retried forever — 178 times in one run — and the engine never went idle long enough for a ROOT-port
     * device to be enumerated. That is the "it stopped looking for drives" report: we never stopped, we were
     * stuck on one hub port. Advance past the offending port, and give the hub a bounded budget; when it is
     * spent, unclaim the hub and park its root port so the stack falls back to proven n27 behaviour instead
     * of spinning. Hub failures deliberately do NOT touch gSelfEnumTries — the root-port retry policy is not
     * theirs to consume. */
    if (gAs.hubAddr) {
        gAs.hubAddr = 0;
        gHub.scanPort++;
        if (++gHub.failStreak >= 6u) {
            int rp = gHub.rootPort;
            gHub.claimed = 0; gHub.rootPort = -1; gHub.nPorts = 0; gHub.scanPort = 0; gHub.failStreak = 0;
            hub_int_stop();   /* h10: the parked qTD points at an address that no longer answers */
            gHub.skipMask = 0;
            gHubGoneMask |= (1UL << (rp & 0x0F));
            dp_engine_idle();
            if (rp >= 0) park_port(rp, "!! h5: hub sweep failed repeatedly -> UNCLAIMED and parked (n27 fallback)");
        }
        dp_engine_idle();
        return;
    }
    gSelfEnumTries++;                                     /* bounded retry, same policy as the sync path */
    /* ★ n12: an abandoned attempt must not leave a transfer in flight. Without this, gDpBusy stayed 1 and
     * down_pump()'s `if (gDpBusy) return` blocked every later device — the whole stack was dead until a
     * reload, which is what the n11 hardware run showed. */
    dp_engine_idle();
    /* ★ n12: and when the retries are spent, PARK the port instead of leaving it to be re-armed forever.
     * The n11 log shows the cost of not doing this: six full enumeration attempts, each one timing out
     * through the 10s watchdog, none able to exchange a single packet. Retrying a device that has already
     * failed three times just keeps the engine busy failing. */
    if (gSelfEnumTries >= 3u && gAs.port >= 0)
        park_port(gAs.port, "!! n12: enumeration failed 3x — parking this port so other ports keep working");
}
/* Park until the outstanding transfer retires; on timeout, abort the whole sequence. */
#define AS_AWAIT(L) \
    do { gAs.pc = (L); gAs.t0 = frame_ms(); return; \
         case (L): \
            if (!pb_ready()) { \
                if (frame_ms() - gAs.t0 > 800UL) { as_fail("!! n5 SELFENUM/PROBE: transfer TIMEOUT at step", (L)); return; } \
                return; \
            } \
    } while (0)
/* Park for a fixed delay (the SET_ADDRESS recovery time). */
#define AS_DELAY(L, MS) \
    do { gAs.pc = (L); gAs.t0 = frame_ms(); return; \
         case (L): if (frame_ms() - gAs.t0 < (UInt32)(MS)) return; \
    } while (0)
/* Drive the control sub-machine to completion. */
#define AS_CTL(L) \
    do { gAs.pc = (L); return; \
         case (L): { int _r = ctl_step(); \
                     if (_r == 0) return; \
                     if (_r < 0) { as_fail("!! n5 SELFENUM: control transfer FAILED at step", (L)); return; } } \
    } while (0)
/* Park until TASK level has done a piece of work the engine must not do from the SIH.
 *
 * ★★★★★ m3 — THIS DEADLINE WAS MEASURING THE WRONG THING, AND IT IS WHY THE MINI DID NOT MOUNT.
 * The park was abandoned after 5000 ms of WALL CLOCK. But the thing being waited for is not time, it is one
 * turn of the task-level pump, and the two are only interchangeable while the pump is running. On the mini's
 * m2 run task level went dead for ~10.8 s from the instant of the connect (proved by the port-event ring:
 * all ten events accumulated undrained, and the three ring-buffered SELFENUM traces were emitted AFTER them
 * even though they were written earlier — an ordering only possible with no task-level drain in between).
 * So the deadline expired three times over without the registration ever having been offered a turn, each
 * failure re-reset the port, and after 3 tries n12 parked the port — the drive was gone before the pump came
 * back. The enumeration itself was flawless: HS reset to PORTSC 0x1005, descriptors, SET_ADDRESS,
 * SET_CONFIGURATION ok, bulk ep1-IN/ep2-OUT @512 all read correctly, three times.
 *
 * ⇒ Fail only when the pump has DEMONSTRABLY had turns and still not cleared the flag (the original fault:
 * tickFn/slot-23 not wired to us at all — the message's own guess, "no pump?"). A task-level hole now costs
 * latency instead of the device. The absolute cap stays as a backstop so a permanently dead pump cannot park
 * the engine forever and block the retry, but it is generous enough that no plausible stall reaches it.
 * ★ This CANNOT regress the MDD: there the flag is cleared on the very next task-level call — h21's log has
 * `SET_CONFIGURATION ok` and `n6b bulk endpoints registered` on ADJACENT LINES, 9 times out of 9, with zero
 * occurrences of this failure — so neither condition below is ever approached. */
#define AS_TASK_PUMP_TURNS  4UL       /* pump turns with the flag still set that prove the pump is broken */
#define AS_TASK_CAP_MS      60000UL   /* backstop only: a truly dead pump must not park the engine forever */
#define AS_TASK(L) \
    do { gAs.pc = (L); gAs.t0 = frame_ms(); gAs.pump0 = gTaskPumpN; return; \
         case (L): \
            if (gAsNeedBulk) { \
                if (gTaskPumpN - gAs.pump0 >= AS_TASK_PUMP_TURNS) \
                    as_fail("!! n5: the pump RAN and did not register the endpoints — tickFn/slot-23 wiring; turns", \
                            gTaskPumpN - gAs.pump0); \
                else if (frame_ms() - gAs.t0 > AS_TASK_CAP_MS) \
                    as_fail("!! m3: task level never got a turn at all within the cap; ms", frame_ms() - gAs.t0); \
                return; \
            } \
    } while (0)
/* Drive the BOT sub-machine to completion. */
#define AS_BOT(L) \
    do { gAs.pc = (L); return; \
         case (L): { int _r = bot_step(gAs.dev >= 0 ? gAs.dev : 0); \
                     if (_r == 0) return; \
                     if (_r < 0) { as_fail("!! n5 PROBE: BOT transport failure at step", (L)); return; } } \
    } while (0)

/* ★★★★★★★ h48 — THE PROBE. Diagnostic only: it changes no USB behaviour of ours.
 *
 * WHAT IT IS TESTING. The desk RE of Apple's USB keyboard shim found that Apple's
 * `USBKeyboardSupport` shim patches **_SystemTask (trap 0xA9B4)** with a handler living in the fragment's own
 * memory, saving the original in TOC[0x58] — and that its `.term` restores that saved address onto
 * **_EventAvail (trap 0xA971)**, a trap it never patched. SetToolTrapAddress has exactly two call sites in
 * that fragment: 0x280 (install, 0xA9B4) and 0x5d8 (term, 0xA971). So `.term` NEVER un-patches _SystemTask.
 * Once the shim terminates and its fragment is released, _SystemTask still points into freed memory — and
 * _SystemTask is called constantly by the Finder and every application. Our five crashes are all consistent
 * with that: a PC inside a FREE System-heap block holding valid PowerPC, entered from
 * KeyboardSystemTaskPatch via the Mixed Mode glue.
 * The Expert-side RE shows CloseConnection — which is what runs a fragment's
 * term — IS reachable from LoadUIMForEntry, through a helper that also calls SetDriverClosureMemory(conn, 0),
 * i.e. a full release.
 *
 * ⇒ TWO MEASUREMENTS, AND THEY ARE INDEPENDENT:
 *
 * A. WATCH THE TRAP. Log _SystemTask's handler and the first two words at it, every periodic dump, so we get
 *    a timeline rather than two snapshots. A live Mixed Mode routine descriptor begins with **0xAAFE** (the
 *    _MixedModeMagic A-trap that makes JSRing a descriptor dispatch). If those bytes stop looking like a
 *    descriptor, the fragment behind the still-installed patch has been released — the crash's precondition,
 *    observed directly, before it fires.
 *
 * B. MAKE THE EXPERT TELL US. USBFamilyExpertLib exports the data symbol `gUSBStatusBuffer`, and
 *    USBServicesLib exports `void USBExpertSetStatusLevel(UInt32)`. Level 5 (kUSBStatusLevelVerbose) turns on
 *    "General status messages from the Expert and USL"; the default at boot is 3. The Expert's own strings
 *    include "unloading plugin.", "AddDriverForReference - calling RemoveDriverForReference", "Checking disk
 *    for USB shims" and "Send driver removal notifications to shims" — so if it is tearing a plugin down
 *    around our activation, it says so in its own words. We SCAN for those phrases and log a found-mask
 *    rather than dumping the buffer, which keeps this to a few lines.
 *
 * ⚠ SAFETY, deliberately conservative:
 *  - Task level only. This is called from selfprobe_tick_body, alongside the existing periodic dump, so the
 *    Toolbox and CFM calls are legal here. Nothing in it runs at interrupt level.
 *  - We do NOT add USBServicesLib to the UIM's link line. This driver is loaded from the ROM by the USL, and
 *    an unresolvable import would break activation outright. Everything foreign is reached with
 *    GetSharedLibrary + FindSymbol at runtime and degrades to a logged failure.
 *  - The handler pointer is range-checked before it is dereferenced.
 *  - We never CALL anything whose prototype we are guessing. USBExpertSetStatusLevel's signature is read
 *    from USB.h. gUSBStatusBuffer is DATA, so it is only ever read.
 * ⚠⚠ ONE ADMITTED PERTURBATION: raising the status level makes Apple's Expert do more work, which could
 * shift the very race we are chasing. If h48 behaves differently from h47, suspect this first. Both the old
 * and new level are logged so the change is visible. */
#define H48_SYSTEMTASK_TRAP  ((short)0xA9B4)   /* _SystemTask — the trap the shim patches */
#define H48_EVENTAVAIL_TRAP  ((short)0xA971)   /* _EventAvail — the trap its .term wrongly restores */
static UInt32 gH48Handler = 0;      /* last seen _SystemTask handler */
static UInt32 gH48Word0 = 0;        /* first word at it (0xAAFE.... = live descriptor) */
static UInt32 gH48Changed = 0;      /* times the handler address changed */
static UInt32 gH48WentBad = 0;      /* ★ times the first word stopped looking like a descriptor */
static UInt32 gH48Mask = 0;         /* Expert phrases seen (bit per phrase, see h48_probe) */
static UInt32 gH48LevelOld = 0xFFFFFFFF, gH48LevelSet = 0;
static Ptr    gH48StatusBuf = 0;
static int    gH48Tried = 0;
/* h49: the descriptor's TARGET — the code address the crash PCs actually land in. */
static UInt32 gH49Target = 0, gH49T0 = 0, gH49T1 = 0;
static UInt32 gH49TargetChanged = 0;   /* TVector contents changed */
static UInt32 gH49TargetMoved = 0;     /* target address changed = a re-patch, a different thing */
static int    gH49Seen = 0;
/* h51: the SHIM'S OWN STATE, read rather than inferred, and the Expert's REAL text buffer. */
static UInt32 gH51ShimToc = 0;      /* USBKeyboardSupport's TOC, from any exported TVector's word1 */
static UInt32 gH51Saved = 0;        /* TOC[0x58] = the ORIGINAL _SystemTask handler the shim saved */
static UInt32 gH51SavedChanged = 0;
static UInt32 gH51TermRan = 0;      /* ★★ _EventAvail == TOC[0x58] ⇒ Apple's .term executed */
static UInt32 gH51Text = 0, gH51Size = 0, gH51Off = 0, gH51LastOff = 0xFFFFFFFF;
static int    gH51Windows = 0;       /* budget for text-window dumps */
/* h50: one level further — the actual CODE the TVector points at. THIS is the precondition. */
static UInt32 gH50Code = 0, gH50C0 = 0, gH50C1 = 0;
static UInt32 gH50CodeChanged = 0;     /* ★★ code bytes changed = freed AND reused = the crash precondition */
static UInt32 gH50CodeMoved = 0;
static int    gH50Seen = 0;

/* ★★★★★★★ h52 — WATCH THE DRIVER SWAP. The first probe here that does not need to catch a crash.
 *
 * WHY. The crashing code is byte-matched to Apple's... no: to **ATI's** display drivers inside
 * `Extensions/ATI Driver Update` — three 20-byte runs from three crashes, landing in `ATY,Pheonix` twice and
 * `ATY,Moonraker` once (a byte-match scan across the driver set). And the MDD ROM carries **no ATY parcel at all**
 * (checked: parcel files, parcel blob, manifest), so on this machine the display driver comes from the
 * Radeon's own declaration ROM and `ATI Driver Update` then REPLACES it during extension load. That is what
 * the extension is for, so **a display-driver swap happens every boot, by design.**
 *
 * ⇒ A superseded 'ndrv' whose unit-table entry is still reachable is a use-after-free of ATI code, reached
 * from 68K Device Manager dispatch through Mixed Mode — which is exactly the signature of all five crashes:
 * 68K executing PowerPC, `Int 0` (task level), `CurApName Finder`, during desktop load, with the caller at a
 * STABLE `0x0053xxxx-0x0054xxxx` low-system-heap address while ordinary heap addresses move every boot.
 *
 * ★★ WHAT THIS MEASURES, AND IT WORKS ON A CLEAN BOOT. Walk the unit table, snapshot every `dCtlDriver`, and
 * thereafter report only the entries that CHANGE — that is the swap, caught as it happens. Then keep reading
 * the SUPERSEDED address: if its bytes stay put, a stale call would still execute valid code and the boot
 * survives; if they change, the block was freed AND reused, which is the difference between a boot that lives
 * and a boot that dies. Every previous probe needed the fault in the act. This one does not — and if the old
 * block is reused on clean boots too, then reuse is NOT the discriminator and I am wrong again, which we learn
 * from one boot instead of ten.
 *
 * ⚠ Pure memory reads: `UTableBase` (0x011C) and `UnitNtryCnt` (0x01D2) from low memory, one handle deref per
 * slot, `dCtlDriver` at +0 and `dCtlRefNum` at +0x18. No QuickDraw (so no dependence on an app's A5 world), no
 * CFM, no Toolbox call that can move memory. Every pointer is range-checked before use. Task level only. */
#define H52_MAX 64
static UInt32 gH52Drv[H52_MAX];
static short  gH52Ref[H52_MAX];
static int    gH52Init = 0;
static UInt32 gH52Changes = 0;      /* ★ driver-pointer swaps observed */
static UInt32 gH52Old[4], gH52OldW0[4], gH52OldW1[4];
static int    gH52Nold = 0;
static UInt32 gH52OldReused = 0;    /* ★★ a superseded driver's bytes CHANGED = freed and reused */

static int h52_ok(UInt32 p) { return p > 0x1000UL && !(p & 1UL); }

static void h52_probe(void)
{
    void **utab = *(void ** volatile *)0x011CUL;      /* UTableBase */
    int n = (int)*(volatile short *)0x01D2UL;         /* UnitNtryCnt */
    int i;

    if (!h52_ok((UInt32)utab) || n <= 0) {
        ehci_os_logx("!! h52 unit table unavailable; UTableBase", (UInt32)utab);
        return;
    }
    if (n > H52_MAX) n = H52_MAX;

    for (i = 0; i < n; i++) {
        void *dceh = utab[i];
        UInt32 drv = 0; short ref = 0;
        if (h52_ok((UInt32)dceh)) {
            void *dce = *(void * volatile *)dceh;      /* the unit table holds DCtlHandles */
            if (h52_ok((UInt32)dce)) {
                drv = *(volatile UInt32 *)dce;                        /* dCtlDriver  +0x00 */
                ref = *(volatile short *)((char *)dce + 0x18);        /* dCtlRefNum  +0x18 */
            }
        }
        if (!gH52Init) {
            gH52Drv[i] = drv; gH52Ref[i] = ref;
            if (drv) ehci_os_logx("  h52 snapshot slot<<24|refNum ; dCtlDriver next line",
                                  ((UInt32)i << 24) | ((UInt32)(UInt16)ref));
            if (drv) ehci_os_logx("    h52   dCtlDriver", drv);
        } else if (drv != gH52Drv[i]) {
            gH52Changes++;
            ehci_os_logx("!! h52 DRIVER SWAPPED — slot<<24|refNum", ((UInt32)i << 24) | ((UInt32)(UInt16)ref));
            ehci_os_logx("  h52   old dCtlDriver", gH52Drv[i]);
            ehci_os_logx("  h52   new dCtlDriver", drv);
            /* remember the superseded address and watch it for reuse */
            if (gH52Nold < 4 && h52_ok(gH52Drv[i]) && !(gH52Drv[i] & 3UL)) {
                gH52Old[gH52Nold] = gH52Drv[i];
                gH52OldW0[gH52Nold] = *(volatile UInt32 *)gH52Drv[i];
                gH52OldW1[gH52Nold] = *(volatile UInt32 *)(gH52Drv[i] + 4);
                ehci_os_logx("  h52   now watching the SUPERSEDED driver at", gH52Old[gH52Nold]);
                gH52Nold++;
            }
            gH52Drv[i] = drv; gH52Ref[i] = ref;
        }
    }
    gH52Init = 1;

    for (i = 0; i < gH52Nold; i++) {
        UInt32 a = gH52Old[i];
        UInt32 w0 = *(volatile UInt32 *)a, w1 = *(volatile UInt32 *)(a + 4);
        if (w0 != gH52OldW0[i] || w1 != gH52OldW1[i]) {
            gH52OldReused++;
            ehci_os_logx("!! h52 SUPERSEDED DRIVER'S BYTES CHANGED — freed AND reused; addr", a);
            ehci_os_logx("  h52   was", gH52OldW0[i]);
            ehci_os_logx("  h52   now", w0);
            gH52OldW0[i] = w0; gH52OldW1[i] = w1;
        }
    }
    ehci_os_logx("!! h52 gH52Changes (driver-pointer swaps seen this boot)", gH52Changes);
    ehci_os_logx("!! h52 gH52OldReused (a superseded driver's code was overwritten — THE crash precondition)",
                 gH52OldReused);
}

static void h48_probe(void)
{
    UInt32 h, w0 = 0, w1 = 0;

    /* ---- B, once: reach the Expert's status facility and turn it up ---- */
    if (!gH48Tried) {
        CFragConnectionID conn; Ptr mainAddr; Str255 err; CFragSymbolClass cls; Ptr p;
        gH48Tried = 1;
        if (GetSharedLibrary("\pUSBServicesLib", kPowerPCCFragArch, kLoadCFrag,
                             &conn, &mainAddr, err) == noErr) {
            if (FindSymbol(conn, "\pUSBExpertGetStatusLevel", &p, &cls) == noErr && p)
                gH48LevelOld = ((UInt32 (*)(void))p)();
            /* ⚠ h52 REMOVES THE STATUS-LEVEL RAISE. h48-h51 called
             * USBExpertSetStatusLevel(kUSBStatusLevelVerbose) so Apple's Expert would narrate a shim teardown.
             * That purpose is MOOT — crash 5 proved _SystemTask healthy and _EventAvail app-owned, so the
             * USBKeyboardSupport .term theory is not our crash path. Meanwhile the call made Apple's Expert do
             * more work every boot, which is a real perturbation of the very layout/timing this crash is
             * sensitive to. Dropping it removes a confound and shrinks the driver. The level is still READ and
             * logged, which costs nothing. */
            gH48LevelSet = 0;
        }
        if (GetSharedLibrary("\pUSBFamilyExpertLib", kPowerPCCFragArch, kLoadCFrag,
                             &conn, &mainAddr, err) == noErr) {
            if (FindSymbol(conn, "\pgUSBStatusBuffer", &p, &cls) == noErr) gH48StatusBuf = p;
        }
        ehci_os_logx("!! h48 Expert status level  old<<8|new (0xFFFFFFFF old = lookup failed)",
                     (gH48LevelOld == 0xFFFFFFFFUL) ? 0xFFFFFFFFUL
                                                    : ((gH48LevelOld << 8) | gH48LevelSet));
        ehci_os_logx("!! h48 gUSBStatusBuffer (0 = FindSymbol failed; nothing else to read)",
                     (UInt32)gH48StatusBuf);
        /* ★★ h50 — STOP GUESSING AT THIS BUFFER AND LOOK AT IT.
         * h49 proved the phrase scan does not work: the symbol resolves to a real address (0x002c8a0c) but
         * the "Expert" positive control never matched, so the region is not the plain-text log I assumed.
         * Most likely it is a header or ring structure whose entries are reached by pointer. Rather than
         * guess a second time, dump the first 64 bytes ONCE. Sixteen words is cheap, it happens on one probe
         * cycle only, and it turns the next step into reading rather than another assumption — any pointers
         * in here can be chased in the following build. */
        if (gH48StatusBuf) {
            int w;
            for (w = 0; w < 16; w++)
                ehci_os_logx("  h50 statusbuf word (16 words, once, to read its STRUCTURE)",
                             ((volatile UInt32 *)gH48StatusBuf)[w]);
        }

        /* ★★★★★★ h51 — READ THE SHIM'S OWN STATE INSTEAD OF INFERRING IT.
         *
         * h48/h49/h50 all watched the HEAD of the _SystemTask trap chain on the assumption it was Apple's
         * keyboard shim. h50 disproved that: the code behind the trap began 7c0802a6 bfa1fff4, and that
         * prologue does not occur ANYWHERE in USBKeyboardSupport's 9728-byte code section (search method
         * validated first -- it finds the same pair at 0x161c in the Expert, exactly where LoadUIMForEntry's
         * prologue is). Trap patches CHAIN; the head is only the most recently installed one, each patch
         * stores its predecessor privately, and the chain cannot be walked from outside. So three builds of
         * mine were pointed at the wrong object.
         *
         * ⇒ Go to the source. The RE says the shim keeps the ORIGINAL _SystemTask handler in **TOC[0x58]**
         * (install at code 0x274: `addi r12, r2, 0x58` / `stw r3, 0(r12)`). A PowerPC TVector is
         * {code, TOC}, so FindSymbol on ANY exported TVector of the shim hands us its TOC in word1 -- and
         * then TOC+0x58 is the saved handler, read directly rather than deduced.
         *
         * ★★ AND THIS GIVES A CLEAN, CHAIN-INDEPENDENT TEST THAT .term RAN. Apple's .term writes that saved
         * value onto _EventAvail (0xA971), a trap it never patched. So `_EventAvail == TOC[0x58]` is proof
         * .term executed. On clean boots _EventAvail has held ordinary app-heap values (0x5e853ca0 /
         * 0x5ef639f0), so this is a sharp discriminator, not a coincidence risk.
         *
         * ⚠⚠ kFindCFrag, NOT kLoadCFrag. kFindCFrag (0x0002) finds an existing copy WITHOUT incrementing
         * reference counts; kReferenceCFrag/kLoadCFrag would use-or-load it and bump the count. Loading the
         * shim ourselves would run its .init and patch a trap -- we would be CAUSING the thing we are
         * measuring. (Noted for the eventual fix: kReferenceCFrag's refcount bump is exactly how the "pin the
         * fragment" repair would be implemented, in one call.) */
        {
            CFragConnectionID sc; Ptr sm; Str255 se; CFragSymbolClass scl; Ptr sp;
            OSErr e = GetSharedLibrary("\pUSBKeyboardSupport", kPowerPCCFragArch, kFindCFrag,
                                       &sc, &sm, se);
            ehci_os_logx("!! h51 GetSharedLibrary(USBKeyboardSupport, kFindCFrag) err (0 = it IS loaded)",
                         (UInt32)(SInt32)e);
            if (e == noErr && FindSymbol(sc, "\pUSBShim", &sp, &scl) == noErr && sp) {
                gH51ShimToc = *(volatile UInt32 *)((char *)sp + 4);   /* TVector word1 = the shim's TOC */
                ehci_os_logx("!! h51 shim TVector / its TOC", gH51ShimToc);
                if (gH51ShimToc > 0x1000UL && !(gH51ShimToc & 3UL))
                    gH51Saved = *(volatile UInt32 *)(gH51ShimToc + 0x58);
                ehci_os_logx("!! h51 shim TOC[0x58] = the ORIGINAL _SystemTask it saved (0 = never patched)",
                             gH51Saved);
            }
        }
    }

    /* ---- A, every dump: the _SystemTask trap, the descriptor, AND WHAT THE DESCRIPTOR POINTS AT ----
     *
     * ★★★★★★ h49 — h48 WATCHED THE WRONG ADDRESS, and its own output is what showed it.
     * h48 logged the descriptor at 0x018E8018 with word0 0xAAFE0700 — a live Mixed Mode descriptor, so the
     * install half of the RE is hardware-confirmed. But the four crash PCs were 0x01E7A2BA / 0x01E8F2AA /
     * 0x01E8F182 / 0x01E797A2, about 0x59000 AWAY from the descriptor. The PC is therefore not the descriptor
     * — it is the CODE THE DESCRIPTOR POINTS AT, which is what gets released. On a crashing boot the
     * descriptor would very likely still read 0xAAFE0700 while its target had gone, gH48WentBad would have
     * stayed 0, and I would have read that as "the theory is refuted". Watching only the magic could produce
     * a confident false negative, which is the one outcome this project cannot afford again.
     *
     * ⇒ h49 follows the descriptor. RoutineDescriptor / RoutineRecord layout (MixedMode.h):
     *      +0x00 UInt16 goMixedModeTrap   0xAAFE = _MixedModeMagic
     *      +0x02 SInt8  version           (0x07 here)
     *      +0x03 UInt8  flags
     *      +0x04 UInt32 reserved1         (0 here — matches what h48 logged as word1)
     *      +0x0A UInt16 routineCount
     *      +0x0C UInt32 procInfo          the first RoutineRecord starts at +0x0C
     *      +0x11 UInt8  ISA               kM68kISA = 0, kPowerPCISA = 1
     *      +0x14 ProcPtr procDescriptor   ★ THE CODE ADDRESS
     * ★ ISA is logged because the crashes were PowerPC bytes executed AS 68K. If the descriptor claims 68K
     * while the target holds PowerPC, that is the mechanism stated outright.
     *
     * ★★ AND THE RIGHT INVARIANT FOR THE TARGET IS "DID ITS BYTES CHANGE", not "is it still magic". Freed
     * memory that nobody has reused still holds the original code and would still RUN — that is why some
     * boots survive. The crash needs the block freed AND overwritten. So a change in the first two words at
     * the target IS the crash precondition, and it gets its own counter. */
    h = (UInt32)GetToolTrapAddress(H48_SYSTEMTASK_TRAP);
    if (h > 0x1000UL && !(h & 1UL)) {                /* non-null, even, past low memory */
        w0 = *(volatile UInt32 *)h;
        w1 = *(volatile UInt32 *)(h + 4);
    }
    if (gH48Handler && h != gH48Handler) gH48Changed++;
    if ((gH48Word0 >> 16) == 0xAAFEUL && (w0 >> 16) != 0xAAFEUL) gH48WentBad++;
    gH48Handler = h; gH48Word0 = w0;
    ehci_os_logx("!! h48 _SystemTask(0xA9B4) handler", h);
    ehci_os_logx("  h48   word0 at handler (0xAAFExxxx = LIVE Mixed Mode descriptor)", w0);
    ehci_os_logx("  h48   word1 at handler", w1);
    ehci_os_logx("!! h48 gH48WentBad (descriptor magic LOST; MUST be 0)", gH48WentBad);
    ehci_os_logx("  h48 gH48Changed (handler address changed; 0 = the shim never re-patched)", gH48Changed);
    ehci_os_logx("  h48 _EventAvail(0xA971) handler (== old _SystemTask ⇒ .term ran)",
                 (UInt32)GetToolTrapAddress(H48_EVENTAVAIL_TRAP));

    /* ★ h49: follow it. Only if we really are looking at a descriptor. */
    if ((w0 >> 16) == 0xAAFEUL) {
        UInt32 tgt = *(volatile UInt32 *)(h + 0x14);
        UInt32 cnt = (*(volatile UInt32 *)(h + 8)) & 0xFFFFUL;      /* routineCount at +0x0A */
        UInt32 pinf = *(volatile UInt32 *)(h + 0x0C);
        UInt32 isa = (*(volatile UInt8 *)(h + 0x11));
        UInt32 t0 = 0, t1 = 0;
        if (tgt > 0x1000UL && !(tgt & 3UL)) {        /* non-null and 4-byte aligned (PPC code must be) */
            t0 = *(volatile UInt32 *)tgt;
            t1 = *(volatile UInt32 *)(tgt + 4);
        }
        if (gH49Target && tgt != gH49Target) gH49TargetMoved++;
        if (gH49Seen && tgt == gH49Target && (t0 != gH49T0 || t1 != gH49T1)) gH49TargetChanged++;
        gH49Target = tgt; gH49T0 = t0; gH49T1 = t1; gH49Seen = 1;
        ehci_os_logx("!! h49 descriptor TARGET code address (+0x14) — THIS is what the crash PC is in", tgt);
        ehci_os_logx("  h49   word0 at target", t0);
        ehci_os_logx("  h49   word1 at target", t1);
        ehci_os_logx("  h49   routineCount<<16|ISA (ISA: 0=68K 1=PowerPC)", (cnt << 16) | isa);
        ehci_os_logx("  h49   procInfo", pinf);
        ehci_os_logx("  h49 gH49TargetChanged (TVector contents changed)", gH49TargetChanged);
        ehci_os_logx("  h49 gH49TargetMoved (target address changed; a re-patch, not a free)",
                     gH49TargetMoved);

        /* ★★★★★★ h50 — ONE MORE LEVEL, AND THIS IS THE ONE THAT MATTERS.
         *
         * The h49 run showed word0 at the target reading 0x019b46e8 — opcode 0, which is not a valid
         * PowerPC instruction. That is because for a PowerPC routine the RoutineRecord's procDescriptor
         * points at a **TVector**, not at code: {code address, TOC pointer}. So h49 was watching a TVector,
         * and a TVector lives in a DIFFERENT heap block from the code it names. If the fragment's code
         * section is freed and reused, the TVector reads unchanged while the code underneath has gone —
         * h49's counter would miss exactly the event we are hunting.
         *
         * ⇒ Follow it. ISA (+0x11) says which kind of pointer procDescriptor is:
         *      kPowerPCISA (1) -> procDescriptor is a TVector; code = *(tgt), TOC = *(tgt+4)
         *      kM68kISA    (0) -> procDescriptor IS the 68K code address
         * The h49 boot read ISA = 1, so the TVector branch is the live one here, but both are handled so a
         * future re-patch by 68K code does not silently read as "no data".
         *
         * ★ The invariant is the same and it is the right one: freed-but-untouched memory still holds the
         * original code and would still RUN, which is why boots survive. The crash needs the block freed AND
         * overwritten. A CHANGE in the first two words at a code address that has NOT moved is therefore the
         * precondition itself. gH50CodeMoved is kept separate — a moved address is a re-patch, not a free. */
        {
            UInt32 code = 0, toc = 0, c0 = 0, c1 = 0;
            if (isa == 1UL) {                        /* kPowerPCISA: procDescriptor is a TVector */
                if (tgt > 0x1000UL && !(tgt & 3UL)) { code = t0; toc = t1; }
            } else {                                  /* kM68kISA: procDescriptor is the code itself */
                code = tgt;
            }
            if (code > 0x1000UL && !(code & 3UL)) {
                c0 = *(volatile UInt32 *)code;
                c1 = *(volatile UInt32 *)(code + 4);
            }
            if (gH50Code && code != gH50Code) gH50CodeMoved++;
            if (gH50Seen && code == gH50Code && (c0 != gH50C0 || c1 != gH50C1)) gH50CodeChanged++;
            gH50Code = code; gH50C0 = c0; gH50C1 = c1; gH50Seen = 1;
            ehci_os_logx("!! h50 CODE address behind the TVector (ISA 1) or the descriptor (ISA 0)", code);
            ehci_os_logx("  h50   TOC pointer (ISA 1 only)", toc);
            ehci_os_logx("  h50   word0 at code", c0);
            ehci_os_logx("  h50   word1 at code", c1);
            ehci_os_logx("!! h50 gH50CodeChanged (CODE BYTES changed = freed AND reused — THE CRASH "
                         "PRECONDITION; MUST be 0)", gH50CodeChanged);
            ehci_os_logx("  h50 gH50CodeMoved (code address changed; a re-patch, not a free)", gH50CodeMoved);
        }
    }

    /* ---- h51, every dump: the shim's saved handler, and the chain-independent .term test ---- */
    if (gH51ShimToc > 0x1000UL && !(gH51ShimToc & 3UL)) {
        UInt32 sv = *(volatile UInt32 *)(gH51ShimToc + 0x58);
        UInt32 ea = (UInt32)GetToolTrapAddress(H48_EVENTAVAIL_TRAP);
        if (gH51Saved && sv != gH51Saved) gH51SavedChanged++;
        gH51Saved = sv;
        /* ★★ THE TEST. Apple's .term writes the shim's saved _SystemTask handler onto _EventAvail, a trap it
         * never patched. Equality is therefore proof .term executed — and it does not depend on the trap
         * chain, on which fragment owns the head, or on any pointer assumption of mine. */
        if (sv && ea == sv) gH51TermRan++;
        ehci_os_logx("!! h51 shim TOC[0x58] saved _SystemTask handler", sv);
        ehci_os_logx("!! h51 gH51TermRan (_EventAvail == that saved handler ⇒ APPLE'S .term RAN; MUST be 0)",
                     gH51TermRan);
        ehci_os_logx("  h51 gH51SavedChanged (the shim re-patched)", gH51SavedChanged);
    }

    /* ---- B, every dump: has the Expert said any of the teardown words? ---- */
    if (gH48StatusBuf) {
        /* ★ h49 adds bit 6, "Expert", as a POSITIVE CONTROL. h48 returned gH48Mask = 0 and that reading was
         * uninterpretable: it could not distinguish "the Expert never said any of these things" from "my scan
         * is looking at the wrong memory, or the buffer is not plain text". Every message this library emits
         * begins "Expert - ", so bit 6 MUST be set if the scan is working at all. **If b6 is clear, bits 0-5
         * carry no information** — that is now written into the log line itself rather than left to me. */
        static const char *phrase[7] = { "unloading plugin", "RemoveDriverForReference",
                                         "Checking disk for USB shims", "will not be unloaded",
                                         "removal notifications to shims", "DeferredTermination",
                                         "Expert" };
        /* ★★★ h51 — SCAN THE REAL TEXT, NOT THE HEADER. h50's 16-word dump showed gUSBStatusBuffer is a
         * HEADER: +0x00 pointer to the text (0x002cb330), +0x04 size (0x2000 = 8192), +0x08 current offset
         * (0x1fba — a nearly-full ring), +0x0C a timestamp. My earlier scans read the header itself, which is
         * why the "Expert" positive control never matched. Follow the pointer and scan `size` bytes there. */
        const volatile char *b;
        UInt32 scanLen;
        int i, k, n;
        gH51Text = ((volatile UInt32 *)gH48StatusBuf)[0];
        gH51Size = ((volatile UInt32 *)gH48StatusBuf)[1];
        gH51Off  = ((volatile UInt32 *)gH48StatusBuf)[2];
        if (gH51Text > 0x1000UL && gH51Size >= 0x100UL && gH51Size <= 0x20000UL) {
            b = (const volatile char *)gH51Text;
            scanLen = gH51Size;
        } else {                                  /* header did not look like one — fall back, and say so */
            b = (const volatile char *)gH48StatusBuf;
            scanLen = 2048;
        }
        ehci_os_logx("  h51 statusbuf text ptr", gH51Text);
        ehci_os_logx("  h51 statusbuf size<<16|writeOffset", ((gH51Size & 0xFFFFUL) << 16) | (gH51Off & 0xFFFFUL));
        /* ★ Dump a window of the NEWEST text when the ring's write offset moves — that is the Expert saying
         * something new, in its own words. Budgeted so a chatty Expert cannot flood the log. */
        if (gH51Off != gH51LastOff && gH51Windows < 6 && b == (const volatile char *)gH51Text) {
            UInt32 start = (gH51Off > 64UL) ? (gH51Off - 64UL) : 0UL;
            int w;
            gH51LastOff = gH51Off; gH51Windows++;
            for (w = 0; w < 16; w++)
                ehci_os_logx("  h51 newest text word (window at the ring's write offset)",
                             *(volatile UInt32 *)(gH51Text + start + (UInt32)(w * 4)));
        }
        for (k = 0; k < 7; k++) {
            if (gH48Mask & (1UL << k)) continue;          /* already seen; do not rescan */
            n = 0; while (phrase[k][n]) n++;
            for (i = 0; (UInt32)(i + n) < scanLen; i++) {  /* h51: scan the whole text buffer, bounded by its
                                                           * own declared size rather than a guessed 2048 */
                int j = 0;
                while (j < n && b[i + j] == phrase[k][j]) j++;
                if (j == n) { gH48Mask |= (1UL << k); break; }
            }
        }
        ehci_os_logx("!! h49 mask b6=\"Expert\" IS THE CONTROL — if b6 is 0, bits 0-5 MEAN NOTHING. "
                     "b0 unloading-plugin b1 RemoveDriverForReference b2 checking-disk-for-shims "
                     "b3 will-not-be-unloaded b4 removal-notifications-to-shims b5 DeferredTermination",
                     gH48Mask);
    }
}

static void as_advance(void)
{
    switch (gAs.pc) {
    case 0:
        ehci_os_ilog("=== n5 SELFENUM (ASYNC, no application): enumerating the hidden device ourselves ===");
        ehci_os_ilogx("  port", (UInt32)gAs.port);
        gAs.addr = DEV_ADDR(gAs.dev >= 0 ? gAs.dev : 0);   /* n18: per-device address */
        gAs.epIn = gAs.epOut = -1; gAs.mpIn = gAs.mpOut = 512;
        gAs.inMSC = 0; gAs.iface = -1; gAs.cfgVal = 1; gAs.tur = 0;
        /* ★★ 0. CONNECT DEBOUNCE — USB 2.0 §7.1.7.3 T_ATTDB = 100 ms. A port must be debounced for 100 ms
         * after a connect is detected and BEFORE reset is issued; resetting a device that has not finished
         * attaching is exactly what makes it bounce.
         * NEITHER path ever implemented this explicitly. The synchronous selfenum satisfied it BY ACCIDENT:
         * it ran from selfprobe_tick (uim23), which the app pumps at roughly 10/sec, so the reset landed
         * ~100 ms after the connect. This engine arms from the 8 ms heartbeat and reset ~8 ms after — which
         * is why the post-reset bounce appeared with n5 and had never been seen before. The retry guard in
         * case 1 treats the symptom; this removes the cause. */
        /* ★★★★ h3 TIER 1: a device BEHIND THE HUB resets via hub class requests, not PORTSC. This is the ONLY
         * step that differs — everything from the device descriptor onwards is a control transfer to an
         * address, which the hub forwards transparently. */
        if (gAs.hubAddr) {
            ehci_os_ilogx("  h3 enumerating a device BEHIND THE HUB; downstream port", (UInt32)gAs.hubPort);
            /* Is anything actually there, and at what speed? */
            gAsPortSt[0] = gAsPortSt[1] = gAsPortSt[2] = gAsPortSt[3] = 0;   /* h13: never inherit a stale reply */
            ctl_begin(0xA3, 0x00, 0x0000, (UInt16)gAs.hubPort, gAs.hubAddr, gAsPortSt, 4, 1);
            AS_CTL(70);
            /* ★★★★ h13 THE h12 HUB BUG. gAsPortSt is one 4-byte static shared by every port, and this site
             * used to trust it the moment AS_CTL returned. A control IN that completed without delivering its
             * payload therefore handed us the PREVIOUS port's status — and on the h12 run that was port 3, the
             * display's genuine LOW-SPEED HID (0x0301). Port 1's high-speed drive was read as low-speed, so it
             * was latched into skipMask as a tier-2 device and NEVER looked at again while it stayed plugged
             * in. The tell in the log was that the bogus reads carried change bits 0x0000 on a port whose
             * connect change had never been acknowledged, which §11.24.2.7.2 makes impossible.
             * A short read is not an error to retry-and-park over: it means we simply do not know yet. Say so
             * and move on; the next sweep asks again. */
            if (ctl_got() < 4) {
                ehci_os_ilogx("    h13: GET_PORT_STATUS delivered fewer than 4 bytes — NO decision this sweep "
                              "(a stale buffer here read a high-speed drive as low-speed in h12); port",
                              (UInt32)gAs.hubPort);
                gHubShortSt++;
                goto h3_next_port;
            }
            { UInt32 st = ((UInt32)gAsPortSt[1] << 8) | gAsPortSt[0];
              ehci_os_ilogx("    h7 wPortStatus", st);
              gHub.failStreak = 0;        /* h5: the hub answered — the budget resets */
              gAs.chBits = ((UInt32)gAsPortSt[3] << 8) | gAsPortSt[2];
              if (gAs.chBits) ehci_os_ilogx("    h11 wPortChange (must be acknowledged)", gAs.chBits); }
            /* ★★★★ h11: ACKNOWLEDGE THE PORT'S CHANGE BITS BEFORE DECIDING ANYTHING.
             * USB 2.0 §11.24.2.7.2: a port's change bits LATCH. Until they are cleared with
             * CLEAR_PORT_FEATURE the hub keeps reporting that port in its status-change bitmap — forever.
             * I only cleared them on the successful high-speed reset path, never on the low-speed skip or the
             * empty path, and the display's HID on port 3 has had C_PORT_CONNECTION latched since h2.
             * With h7-h9's 500 ms timer that merely wasted a transfer per sweep. With h10's change-driven
             * design it is a LIVELOCK: hub says "port 3 changed" -> we look, do not acknowledge -> re-park the
             * qTD -> it completes instantly -> look again, as fast as the engine allows. That is what crashed
             * h10, and it is the same omission h1 shipped with; the timer was hiding it.
             * C_PORT_CONNECTION = 16, C_PORT_ENABLE = 17. Clearing a bit that is not set is harmless, so this
             * costs two transfers only on a port that actually reported something. */
            if (gAs.chBits) {
                ctl_begin(0x23, 0x01, 0x0010, (UInt16)gAs.hubPort, gAs.hubAddr, gEnumDesc, 0, 0);
                AS_CTL(83);
                ctl_begin(0x23, 0x01, 0x0011, (UInt16)gAs.hubPort, gAs.hubAddr, gEnumDesc, 0, 0);
                AS_CTL(84);
                ehci_os_ilog("    h11 port change acknowledged");
            }
            {   UInt32 st = ((UInt32)gAsPortSt[1] << 8) | gAsPortSt[0];
              /* ★★★★ h7 THE DECISION TABLE. Every sweep asks each port for its status and then decides
               * against what we already know about that port. Ordering matters: "is it already ours" must be
               * asked BEFORE "is it connected", because the interesting case is a port that WAS ours and is
               * now empty — that is the downstream disconnect h6 could never see. */
              {   int d, mine = -1;
                  for (d = 0; d < USB_MAX_DEV; d++)
                      if (gDev[d].inUse && gDev[d].viaHub && gDev[d].hubPort == (UInt8)gAs.hubPort)
                          { mine = d; break; }

                  if (mine >= 0) {
                      if (st & 0x0001) goto h3_next_port;      /* still there — nothing to do, quietly */
                      /* ★ DOWNSTREAM DISCONNECT. Free the slot and hand the removal to the task-level rearm,
                       * which is the same path a root-port pull uses: blk_notify_media(dev, 0) then the
                       * selective unmount. Without this the volume stayed mounted over a device that had
                       * physically gone, and the port stayed "taken" so it could never be reused. */
                      {   int k;
                          for (k = 0; k < NBULK; k++)
                              if (gBulkEP[k].used && gBulkEP[k].dev == (UInt8)mine) gBulkEP[k].used = 0;
                          gDev[mine].inUse = 0; gDev[mine].viaHub = 0; gDev[mine].hubPort = 0;
                          gDev[mine].probedPort = -1; gDev[mine].pOut = -1; gDev[mine].pIn = -1;
                          if (mine != 0) gDev[mine].pstate = 0;
                          gRearmDev = mine; gSelfEnumRearm = 1;   /* task level unmounts THIS drive */
                      }
                      gHub.skipMask &= ~(1UL << (gAs.hubPort & 0x1F));   /* a different device may follow */
                      gHubPortGoneMask |= (1UL << (gAs.hubPort & 0x1F));
                      goto h3_next_port;
                  }

                  if (gHub.skipMask & (1UL << (gAs.hubPort & 0x1F))) {
                      /* A port we already determined is not high-speed. Still watch for its REMOVAL, so the
                       * next device plugged in there gets a fresh look instead of inheriting the verdict. */
                      if (!(st & 0x0001)) {
                          gHub.skipMask &= ~(1UL << (gAs.hubPort & 0x1F));
                          gHubLsSeen &= ~(1UL << (gAs.hubPort & 0x1F));   /* h13: fresh device, fresh streak */
                          ehci_os_ilogx("    h7 -> the non-high-speed device left; port re-armed for a fresh "
                                        "look; port", (UInt32)gAs.hubPort);
                      }
                      goto h3_next_port;
                  }

                  if (!(st & 0x0001)) {                       /* empty and unknown — nothing to say */
                      gHubLsSeen &= ~(1UL << (gAs.hubPort & 0x1F));   /* h13: an empty port holds no streak */
                      goto h3_next_port;
                  }

                  /* ★ h6: pre-reset, LOW speed is the ONLY speed a hub can report. USB 2.0 §11.24.2.7.1 —
                   * high-speed is determined by the chirp DURING reset, so demanding HS here (as h5 did) skips
                   * every high-speed drive. Low speed IS knowable now, from the D± idle state. */
                  /* ★★★★ h13: CONFIRM LOW SPEED BEFORE LATCHING IT. skipMask is cleared only when the device
                   * leaves, so latching on a single reading spends the port's entire attached lifetime on one
                   * sample of a bit that is not settled while a device powers up. Require two consecutive
                   * low-speed readings. A genuine low-speed device reads low-speed every sweep and so latches
                   * one sweep later than before; a drive that glitches once keeps its chance. */
                  if (st & 0x0200) {
                      if (!(gHubLsSeen & (1UL << (gAs.hubPort & 0x1F)))) {
                          gHubLsSeen |= (1UL << (gAs.hubPort & 0x1F));
                          ehci_os_ilogx("    h13: reads LOW-SPEED — NOT latching on one sample; will confirm "
                                        "on the next sweep before skipping; port", (UInt32)gAs.hubPort);
                          goto h3_next_port;
                      }
                      ehci_os_ilogx("    h7 -> LOW-SPEED device: needs SPLIT transactions (tier 2). Left "
                                    "powered and untouched; port", (UInt32)gAs.hubPort);
                      gHub.skipMask |= (1UL << (gAs.hubPort & 0x1F));
                      goto h3_next_port;
                  }
                  gHubLsSeen &= ~(1UL << (gAs.hubPort & 0x1F));   /* h13: not low-speed now — the streak breaks */

                  /* A high-speed candidate. NOW we need a slot — not before, so asking the question never
                   * churns one. If the table is full, leave the port alone and try again next sweep. */
                  gAs.dev = dev_alloc();
                  if (gAs.dev < 0) {
                      gSecondDevMask |= (1UL << (gHub.rootPort & 0x0F));
                      goto h3_next_port;
                  }
                  gEnumDev = gAs.dev;
                  /* ★★★★ h8 THE h7 REGRESSION. case 0 computed gAs.addr = DEV_ADDR(dev) on entry, when the
                   * hub sweep had not chosen a slot yet (dev = -1), so it resolved to DEV_ADDR(0) = 1 for
                   * EVERY downstream device. Both drives came up at address 1, and because create_bulk
                   * matches an existing endpoint entry on address+endpoint and RE-TAGS its owner, the second
                   * device stole the first's endpoint registrations — slot 0 was left with none and neither
                   * drive ever reached AddDrive.
                   * h7 moved WHEN the slot is chosen without moving what DEPENDS on that choice. Same family
                   * as n19's discarded `dev` argument: the compiler cannot see it, because DEV_ADDR(0) is a
                   * perfectly valid expression. Recompute the address at the moment the slot is known. */
                  gAs.addr = DEV_ADDR(gAs.dev);
                  ehci_os_ilogx("    h8 slot allocated for this downstream device; USB address",
                                (UInt32)gAs.addr);
                  ehci_os_ilog("    h6 -> connected and NOT low-speed: a high-speed CANDIDATE. Resetting to "
                               "find out — the chirp during reset is the only thing that can tell HS from FS.");
              } }
            /* High-speed device present: reset its hub port. SET_PORT_FEATURE(PORT_RESET = 4). */
            gNoProgressArms = 0;   /* h15: a reset is real forward progress — the no-op streak is broken */
            ctl_begin(0x23, 0x03, 0x0004, (UInt16)gAs.hubPort, gAs.hubAddr, gEnumDesc, 0, 0);
            AS_CTL(71);
            gAs.t0 = frame_ms();
        case 72:
            /* Poll until the hub clears PORT_RESET and reports the port ENABLED. USB 2.0 §11.24.2.13: the hub
             * times the reset itself (10-20 ms), so we watch rather than time it. */
            if (frame_ms() - gAs.t0 > 800UL) {
                as_fail("!! h3: downstream port reset never completed; port", (UInt32)gAs.hubPort);
                gHub.scanPort++; return;
            }
            gAsPortSt[0] = gAsPortSt[1] = gAsPortSt[2] = gAsPortSt[3] = 0;   /* h13: never inherit a stale reply */
            ctl_begin(0xA3, 0x00, 0x0000, (UInt16)gAs.hubPort, gAs.hubAddr, gAsPortSt, 4, 1);
            AS_CTL(73);
            /* h13: a short reply here would read as "still resetting" against the previous port's bits. This is
             * already a timed poll loop, so the safe answer is simply to ask again until the 800 ms cap. */
            if (ctl_got() < 4) { gHubShortSt++; gAs.pc = 72; return; }
            { UInt32 st = ((UInt32)gAsPortSt[1] << 8) | gAsPortSt[0];
              if ((st & 0x0010) || !(st & 0x0002)) { gAs.pc = 72; return; }   /* still resetting / not enabled */
              ehci_os_ilogx("    h3 downstream port ENABLED after reset; wPortStatus", st);
              /* ★★★★ h6: NOW the speed is real — the reset handshake has happened. */
              if (!(st & 0x0400)) {
                  ehci_os_ilog("    h6 -> FULL-SPEED after reset: needs SPLIT transactions (tier 2). SKIPPING, "
                               "and remembering this port so we do not reset it on every sweep.");
                  gHub.skipMask |= (1UL << (gAs.hubPort & 0x1F));
                  goto h3_next_port;
              }
              ehci_os_ilog("    h6 -> HIGH-SPEED confirmed by the reset handshake: driving it (tier 1)");
              gDev[gAs.dev].hiSpeed = 1; }
            /* Acknowledge the reset change so the hub does not keep reporting it. CLEAR_PORT_FEATURE
             * (C_PORT_RESET = 20, C_PORT_CONNECTION = 16). */
            ctl_begin(0x23, 0x01, 0x0014, (UInt16)gAs.hubPort, gAs.hubAddr, gEnumDesc, 0, 0);
            AS_CTL(74);
            ctl_begin(0x23, 0x01, 0x0010, (UInt16)gAs.hubPort, gAs.hubAddr, gEnumDesc, 0, 0);
            AS_CTL(75);
            /* h60: TRSTRCY was 12 ms — the spec MINIMUM (10) plus two. That is enough for a device that has
             * been powered for a while and is only recovering from a reset; it is not enough for one that is
             * also finishing its power-on sequence, which is every cold-boot device here because HCReset
             * power-cycles the port. 50 ms costs nothing on the happy path and is still far inside every
             * enclosing timeout. */
            AS_DELAY(76, 50);        /* TRSTRCY — reset recovery before addressing the device */
            gDev[gAs.dev].viaHub = 1; gDev[gAs.dev].hubPort = (UInt8)gAs.hubPort;
            gHub.scanPort++;         /* consumed: the walk moves on whatever happens to this device */
            gAs.pc = 77; return;     /* join the PROVEN path AT the device-descriptor issue (not case 2) */
        h3_next_port:
            gHub.scanPort++;
            gAs.running = 0; gAs.pc = 0;
            if (gAs.dev >= 0) { gDev[gAs.dev].inUse = 0; gDev[gAs.dev].probedPort = -1; gAs.dev = -1; }
            /* h15: this exit did nothing. If it keeps happening, something is re-arming an identical no-op —
             * stop, rather than spinning the heartbeat and flooding the log (see ARM_NOPROGRESS_MAX). */
            if (++gNoProgressArms >= ARM_NOPROGRESS_MAX) {
                gNoProgressArms = 0;
                gSelfEnumDone = 1;                 /* a real port event clears this again */
                gArmStuckMask |= 1UL;              /* task level logs it */
            }
            return;
        }
        gAs.t0 = frame_ms();
        gAs.pc = 20; return;
    case 20:
        if (frame_ms() - gAs.t0 < 100UL) return;          /* T_ATTDB */
        /* ★★★★★ h33 — LOW SPEED IS KNOWABLE BEFORE THE RESET, AND AN LS DEVICE MUST NEVER BE RESET HERE.
         * EHCI PORTSC Line Status (bits 11:10): 01b = K-state = a LOW-SPEED device is attached, and the
         * spec's own instruction for that case is "release ownership to the companion" — no reset, no
         * chirp test. The T4-on-v4 run (2026-08-09) is why this matters: the keyboard's port read K
         * (0x3400) at sweep time, we reset it anyway, the LS device bounced under an HS-timed reset, and
         * the port was KEPT — a dead keyboard until it was physically moved. The port is OURS and idle at
         * this point (100 ms debounced, nobody driving it), so a single read is reliable — there is no
         * companion traffic to alias the sample, unlike at INIT/sweep time where we sample repeatedly. */
        {   UInt32 pv = ehci_read32(gSoftc.opBase, EHCI_PORTSC(gAs.port));
            if (((pv >> 10) & 3) == 1) {                  /* K-state: low-speed attached */
                UInt32 w = pv & ~EHCI_PORTSC_RW1C;
                w |= EHCI_PORT_OWNER;
                ehci_write32(gSoftc.opBase, EHCI_PORTSC(gAs.port), w);
                gPortCeded[gAs.port] = 1;                 /* n11: record the decision, don't re-read it */
                ehci_os_ilogx("  h33: LOW-SPEED by line state (K) — ceded to the companion WITHOUT reset; port",
                              (UInt32)gAs.port);
                /* ★★★★★★ h46 — CLEAR THIS PORT'S AIM, OR THIS CEDE SPINS FOREVER.
                 *
                 * ⚠ THE h45 RUN (2026-08-10): the mouse went into a card port and this exact branch ran
                 * **78 times** on a port already reading 0x3400 (Owner set, we had ceded it on the first
                 * pass). The arm guard in as_tick is
                 *     if (!gAs.running && !gSelfEnumDone && gSelfEnumPort >= 0 && gSelfEnumTries < 3u)
                 * and the old code here cleared ONLY gAs.running — leaving gSelfEnumPort still aimed at the
                 * port we had just given away and gSelfEnumTries still 0. So the guard was satisfied again on
                 * the very next heartbeat: re-arm, 100 ms T_ATTDB, read K, cede a port that was already
                 * Apple's, repeat. It stopped only because a drive insertion re-aimed gSelfEnumPort.
                 *
                 * ★ THE SIBLING PATH ALREADY HAD THE ANSWER. The 3-attempt cede below (the h39 fix) ends with
                 * exactly these three lines, which is precisely why the KEYBOARD ceded once and stayed quiet
                 * in that same run while the MOUSE spun: h39 fixed one of the two cede sites and this fast
                 * path, added in h33, never got the same treatment. The h15 livelock note eight lines under
                 * the arm guard describes this shape verbatim — "clears gAs.running but touches neither
                 * gSelfEnumPort, gSelfEnumDone nor gSelfEnumTries — and as_tick armed the identical run
                 * again". 5th instance of a decision recorded per-port while the AIM was left pointing at it.
                 *
                 * ⚠ THE HAZARD THIS REMOVES, which is worse than the wasted work: the spin holds gAs.running
                 * for 100 ms of every cycle, and a real device's arm requires !gAs.running. In the h45 run
                 * nothing was mounted so it cost only log lines; with a volume mounted it puts a repeating
                 * enumeration attempt alongside block I/O, which is the documented starvation shape.
                 *
                 * gSelfEnumDone still stays CLEAR — it means "nothing left to enumerate ANYWHERE", which is
                 * false while another port may hold a drive. That was h39's other half and it is not undone
                 * here. */
                gAs.running = 0; gAs.pc = 0; gAs.dev = -1;
                if (gSelfEnumPort == gAs.port) gSelfEnumPort = -1;   /* stop aiming at a port that is Apple's */
                gSelfEnumTries = 0;
                gH46CedeSpinGuard++;      /* h46: how many times this branch ran; 1 per LS device is correct */
                return;
            }
        }
        gNoProgressArms = 0;       /* h15: a root-port reset is real forward progress too */
        /* 1. Port reset. The existing interrupt-level machinery deasserts at +50ms and reports enable. */
        {
            UInt32 v = ehci_read32(gSoftc.opBase, EHCI_PORTSC(gAs.port)) & ~EHCI_PORTSC_RW1C;
            v &= ~EHCI_PORT_ENABLE; v |= EHCI_PORT_RESET;
            ehci_write32(gSoftc.opBase, EHCI_PORTSC(gAs.port), v);
            gPortResetDone[gAs.port] = 0;
            gResetAtFrame[gAs.port] = frame_ms() + 50; gResetPending[gAs.port] = 1; gResetSeen = 1;
            gResetEnabling[gAs.port] = 0;
            pevt((UInt8)gAs.port, PEV_RST_ASSERT, ehci_read32(gSoftc.opBase, EHCI_PORTSC(gAs.port)));
        }
        gAs.pc = 1; gAs.t0 = frame_ms(); return;
    case 1:
        if (!gPortResetDone[gAs.port] && frame_ms() - gAs.t0 <= 500UL) return;
        /* ★★ BOUNCE GUARD (the n5 bug, found on hardware 2026-08-01). The port events from a real insert
         * read: connect -> reset-assert -> reset-deassert -> ENABLED -> **DISCONNECT**. The device bounces
         * right after a perfectly good high-speed reset. The synchronous selfenum tolerated this because
         * reset-and-check happened inside one tight task-level block; this engine spreads the check across
         * heartbeats, so the transient lands exactly in the middle of the decision — we read PORTSC while
         * CCS was momentarily down, concluded "not high-speed", and handed the port to the companion.
         * That handover is a ONE-WAY DOOR (Owner=1 + gSelfEnumDone=1): Apple's OHCI then mounts the drive
         * at 1.1 and we never retry. It is what produced every "mounted as USB 1.1" run.
         * service_ports clears gSelfEnumPort to -1 the instant it sees a disconnect, so that is our
         * bounce flag: abandon this attempt WITHOUT surrendering the port, and let the re-connect re-arm.
         *
         * ★★★★★★ h37 — ASK THE PORT I AM ENUMERATING, NOT THE GLOBAL "WHICH PORT NEXT". THIS WAS THE T4 BUG.
         *
         * ⚠ The test was `gSelfEnumPort != gAs.port`, and gSelfEnumPort is the SINGLE global that
         * service_ports overwrites on the most recent connect event on ANY port. So with a keyboard/mouse also
         * on the card, the HID's connect event flips gSelfEnumPort from the drive's port to the HID's port
         * WHILE THE DRIVE IS MID-RESET, and this guard then declared the drive "bounced" and abandoned a
         * perfectly good high-speed enumeration. The h36 log is explicit: port 2 attempt -> "device bounced
         * during reset" -> engine straight to port 4, while the PORTMAP shows port 2 at 0x1005 — CONNECTED AND
         * ENABLED throughout. Nothing bounced. With the drive alone (T2) no other connect event exists, the
         * test never misfires, and the drive mounts perfectly: four T2 passes against four T4 failures.
         *
         * ★★ IT ALSO EXPLAINS THE CORRUPT MEDIUM, not merely a delay. The abandoned port is left ENABLED with
         * its device still at ADDRESS 0 (we abandon before SET_ADDRESS). The engine then moves to the HID's
         * port and issues address-0 control traffic for THAT device — which the drive, still enabled at
         * address 0, answers too. That breaks this driver's own recorded invariant (one enabled port at a
         * time, because a freshly reset device answers at address 0), scrambles the drive's state, and makes
         * the eventual mount read garbage: "this disk needs to be initialized". A physical replug is a full
         * USB reset, which is exactly why re-inserting always mounted cleanly at 2.0.
         *
         * ★ THE FIX is the n21 lesson for the third time — where the fact is per-port, consult the per-port
         * record. gPortConn[] is maintained per port by service_ports at interrupt level, so ask whether MY
         * port lost its device. A genuine bounce still abandons (the device really is gone, so it answers
         * nothing and the leftover enable is harmless); another port's event no longer touches this run. */
        if (!gPortConn[gAs.port]) {
            /* ★ h33 BACKSTOP: n5's keep-on-bounce is right for a DRIVE (its bounce is a one-off attach
             * transient and the reconnect re-arms). But T4-on-v4 stranded a keyboard here: an LS/FS device
             * that cannot take an HS-timed reset bounces EVERY attempt, and "port KEPT" then means "port
             * dead forever". Three consecutive bounces with the device still present is not a transient —
             * cede it; the companion can drive what we cannot. Cleared on any successful enumeration. */
            if (gAs.port >= 0 && gAs.port < 15 && ++gBounceCnt[gAs.port] >= 3) {
                UInt32 w = ehci_read32(gSoftc.opBase, EHCI_PORTSC(gAs.port));
                if (w & EHCI_PORT_CONNECT) {
                    w &= ~EHCI_PORTSC_RW1C; w |= EHCI_PORT_OWNER;
                    ehci_write32(gSoftc.opBase, EHCI_PORTSC(gAs.port), w);
                    gPortCeded[gAs.port] = 1;
                    ehci_os_ilogx("  h33: THREE consecutive bounces with the device still present — ceding "
                                  "to the companion rather than stranding the port; port", (UInt32)gAs.port);
                    gBounceCnt[gAs.port] = 0;
                    gAs.running = 0; gAs.pc = 0;
                    return;
                }
                gBounceCnt[gAs.port] = 0;     /* nothing connected: a real removal, not a stuck bouncer */
            }
            ehci_os_ilogx("  n5 SELFENUM: MY port's device really disconnected during reset — abandoning this "
                          "attempt, port KEPT; port", (UInt32)gAs.port);
            gAs.running = 0; gAs.pc = 0;      /* deliberately NOT gSelfEnumDone, NOT a tries++ */
            return;
        }
        /* h37: another port signalling mid-reset is NOT a bounce. Log it (this is what used to abandon the
         * drive) and carry on with MY port; gSelfEnumPort is left alone so that port gets its turn after. */
        if (gSelfEnumPort != gAs.port)
            ehci_os_ilogx("  h37: another port signalled mid-reset (a FALSE 'bounce' before h37) — CONTINUING "
                          "this port; mine<<8|next", ((UInt32)gAs.port << 8) | ((UInt32)gSelfEnumPort & 0xFF));
        if (!(ehci_read32(gSoftc.opBase, EHCI_PORTSC(gAs.port)) & EHCI_PORT_ENABLE)) {
            UInt32 portsc = ehci_read32(gSoftc.opBase, EHCI_PORTSC(gAs.port));
            if (portsc & EHCI_PORT_CONNECT) {
                /* Not enabled but still connected. This is the documented full/low-speed hand-off signal —
                 * BUT only trust it once the bounce has had chances to settle, because a mid-bounce read
                 * looks identical. Retry first; surrender the port only on the last attempt. */
                if (gSelfEnumTries + 1 < 3u) {
                    as_fail("!! n5 SELFENUM: not enabled but still connected — retrying before surrendering; portsc",
                            portsc);
                    return;
                }
                /* Genuinely not high speed => the 1.1 companion's device. Hand the PORT over (Owner = 1). */
                { UInt32 w = portsc & ~EHCI_PORTSC_RW1C; w |= EHCI_PORT_OWNER;
                  ehci_write32(gSoftc.opBase, EHCI_PORTSC(gAs.port), w); }
                gPortCeded[gAs.port] = 1;   /* n11: record the decision, don't re-read it back */
                ehci_os_ilog("  n5 SELFENUM: not high-speed after 3 attempts -> handed the port to the 1.1 companion");
                ehci_os_ilogx("  portsc", portsc);
                /* ★★★★★★ h39 — CEDING ONE PORT MUST NOT END ENUMERATION FOR THE WHOLE CONTROLLER.
                 *
                 * ⚠ This line used to be `gSelfEnumDone = 1` with the note "companion owns it; do not retry".
                 * That is right for the port just ceded and catastrophic for every OTHER port: gSelfEnumDone is
                 * the GLOBAL "stop looking" flag, so surrendering an FS mouse ended enumeration for the entire
                 * card. The h38+v7 run shows it exactly — all three attempts on port 4 (the mouse), cede,
                 * gSelfEnumDone=1, engine stops, and the DRIVE on port 2 sits at 0x1801 in the final PORTMAP:
                 * ours, powered, connected, NEVER ENUMERATED. Nothing of ours ever mounted, which is why
                 * "Eject" left no volume behind.
                 *
                 * ★ THIRD INSTANCE OF ONE PATTERN in this driver: a GLOBAL standing in for a PER-PORT fact.
                 * n21 was gSelfEnumPort stealing a live slot's port; h37 was the bounce guard reading that same
                 * global instead of gPortConn[]; this is gSelfEnumDone. The per-port record ALREADY EXISTS here
                 * — gPortCeded[gAs.port], set on the line above — so the global adds nothing but the bug.
                 *
                 * FIX: record the cede per-port (done), release the engine, and clear this port's aim so the
                 * next tick picks up any OTHER port still holding an unenumerated device. gSelfEnumDone stays
                 * CLEAR: it means "there is nothing left to enumerate anywhere", which is simply false while a
                 * connected, unenumerated device sits on another port. gSelfEnumTries resets because the next
                 * port's attempts are its own, not a continuation of this port's three. */
                gAs.running = 0; gAs.pc = 0; gAs.dev = -1;
                if (gSelfEnumPort == gAs.port) gSelfEnumPort = -1;   /* stop aiming at a port that is Apple's */
                gSelfEnumTries = 0;
                ehci_os_ilog("  h39: engine RELEASED (not gSelfEnumDone) — another port may still hold an "
                             "unenumerated device");
                return;
            }
            as_fail("!! n5 SELFENUM: port did not enable and nothing is connected; portsc", portsc);
            return;
        }
        gPortEnabledMs = frame_ms(); gPortEnabledPort = gAs.port;   /* h61: the clock the cold-boot-vs-hot-plug comparison hangs on */
        ehci_os_ilogx("  port ENABLED at high speed; portsc",
                      ehci_read32(gSoftc.opBase, EHCI_PORTSC(gAs.port)));
        /* ★★★★★★★ h62 — TRSTRCY ON THE ROOT-PORT PATH, WHICH HAS NEVER HAD ONE. THE m13 MEASUREMENT.
         *
         * ⚠ USB 2.0 §7.1.7.3: after a port reset the device gets TRSTRCY — 10 ms MINIMUM — before it may be
         * addressed. A device behind a HUB gets it (AS_DELAY(76, …) on that path). A device on a ROOT PORT
         * got NOTHING: the port enabled and the very next step was AS_CTL(2), the first control transfer.
         * h61 measured the gap on hardware: ★ 8 ms ★ — BELOW THE SPEC MINIMUM, on every attempt.
         *
         * ★★ AND h61 SHOWED EXACTLY WHAT THAT COSTS, which is what makes this the answer rather than another
         * guess. The SETUP stage SUCCEEDS — "req 8 / delivered 8", status 0 — so the device is on the bus and
         * answering. The DATA stage then never completes: overlay ACTIVE, CERR still 3, buffer all zeroes.
         * A device that ACKs SETUP and then withholds the data stage is NAKing, i.e. out of reset but not yet
         * ready — and NAKs do not decrement CERR, which is precisely the "ACTIVE, CERR=3, no error bits"
         * signature this hunt has been staring at since m6.
         * ★ It also explains the two things that never fitted: HOT-PLUG ALWAYS WORKED (the connect-debounce
         * and event path put far more than 8 ms in front of the first transfer) and the failure was
         * INTERMITTENT (8 ms against a device wanting 10-100 is exactly on the edge).
         *
         * ★★★ WHY THIS IS SAFE WHERE h60 WAS NOT — the lesson from that regression, applied.
         *  · It is a DELAY INSIDE THE SEQUENCE, not a gate in front of the arm. AS_DELAY parks as_advance and
         *    returns; as_tick then reaches `gAsBusy = 0` as it always does. It cannot latch the guard.
         *  · It sits AFTER "port ENABLED at high speed", so it is reached ONLY by devices that chirped high
         *    speed. A full-speed keyboard takes the not-enabled branch and cedes WITHOUT EVER GETTING HERE:
         *    ★ the user's route back to input is untouched by this change. */
        AS_DELAY(85, 100);

    /* ★ h3 TIER 1 JOIN POINT. The hub path jumps HERE, not to case 2 — case 2 is the COMPLETION half of
     * AS_CTL(2), so entering there would call ctl_step() with no ctl_begin issued for this device and act on
     * stale gCtl state. Everything from this line on is address-based control traffic that a hub forwards
     * transparently, so root-port and behind-hub devices share it verbatim. */
    case 77:
        /* 2. Device descriptor at address 0 (first 8 bytes give bMaxPacketSize0). */
        gEnumDesc[0] = gEnumDesc[1] = 0;
        ctl_begin(0x80, 0x06, 0x0100, 0, 0, gEnumDesc, 8, 1);
        AS_CTL(2);
        if (gEnumDesc[1] != 0x01 || gEnumDesc[0] < 18) {
            as_fail("!! n5 SELFENUM: bad device descriptor @addr0 (type/len)",
                    ((UInt32)gEnumDesc[1] << 8) | gEnumDesc[0]); return; }
        ehci_os_ilogx("  addr0 devDesc bLength/bMaxPacketSize0", ((UInt32)gEnumDesc[0] << 8) | gEnumDesc[7]);

        /* 3. SET_ADDRESS, then the mandatory recovery time before using the new address. */
        ctl_begin(0x00, 0x05, (UInt16)gAs.addr, 0, 0, gEnumDesc, 0, 0);
        AS_CTL(3);
        AS_DELAY(4, 4);
        ehci_os_ilogx("  SET_ADDRESS ok; device now at", gAs.addr);

        /* 4. Full device descriptor at the new address (confirms the address took). */
        gEnumDesc[0] = gEnumDesc[1] = 0;
        ctl_begin(0x80, 0x06, 0x0100, 0, gAs.addr, gEnumDesc, 18, 1);
        AS_CTL(5);
        if (gEnumDesc[1] != 0x01) {
            as_fail("!! n5 SELFENUM: bad device descriptor at new addr (type)", (UInt32)gEnumDesc[1]); return; }
        ehci_os_ilogx("  VID", ((UInt32)gEnumDesc[9] << 8) | gEnumDesc[8]);
        ehci_os_ilogx("  PID", ((UInt32)gEnumDesc[11] << 8) | gEnumDesc[10]);

        /* 5. Config descriptor header -> wTotalLength, then the whole thing. */
        gEnumDesc[0] = gEnumDesc[1] = 0;
        ctl_begin(0x80, 0x06, 0x0200, 0, gAs.addr, gEnumDesc, 9, 1);
        AS_CTL(6);
        if (gEnumDesc[1] != 0x02) {
            as_fail("!! n5 SELFENUM: bad config descriptor (type)", (UInt32)gEnumDesc[1]); return; }
        gAs.tot = ((UInt32)gEnumDesc[3] << 8) | gEnumDesc[2];
        if (gAs.tot < 9 || gAs.tot > sizeof(gEnumDesc)) {
            as_fail("!! n5 SELFENUM: bad wTotalLength", gAs.tot); return; }
        gEnumDesc[1] = 0;
        ctl_begin(0x80, 0x06, 0x0200, 0, gAs.addr, gEnumDesc, gAs.tot, 1);
        AS_CTL(7);
        if (gEnumDesc[1] != 0x02) {
            as_fail("!! n5 SELFENUM: bad full config descriptor (type)", (UInt32)gEnumDesc[1]); return; }
        gAs.cfgVal = gEnumDesc[5];
        ehci_os_ilogx("  config wTotalLength", gAs.tot);
        ehci_os_ilogx("  bConfigurationValue", (UInt32)gAs.cfgVal);

        /* 6. Walk the config for the Mass-Storage / Bulk-Only interface and ITS bulk endpoints.
         *    Pure computation — no waiting, so this stays exactly as the synchronous version. */
        gAs.o = 0;
        while (gAs.o + 2u <= gAs.tot) {
            UInt8 blen = gEnumDesc[gAs.o], btype = gEnumDesc[gAs.o + 1];
            if (blen == 0) break;
            if (btype == 0x04 && blen >= 9) {                          /* INTERFACE */
                gAs.inMSC = (gEnumDesc[gAs.o + 5] == 0x08 && gEnumDesc[gAs.o + 7] == 0x50 &&
                             gEnumDesc[gAs.o + 3] == 0x00);
                if (gAs.inMSC) gAs.iface = gEnumDesc[gAs.o + 2];
            } else if (btype == 0x05 && blen >= 7 && gAs.inMSC) {      /* ENDPOINT of that interface */
                UInt8 eaddr = gEnumDesc[gAs.o + 2], attr = gEnumDesc[gAs.o + 3];
                int mp = (int)(((UInt32)gEnumDesc[gAs.o + 5] << 8) | gEnumDesc[gAs.o + 4]);
                if ((attr & 0x03) == 0x02) {
                    if (eaddr & 0x80) { if (gAs.epIn  < 0) { gAs.epIn  = eaddr & 0x0F; gAs.mpIn  = mp ? mp : 512; } }
                    else              { if (gAs.epOut < 0) { gAs.epOut = eaddr & 0x0F; gAs.mpOut = mp ? mp : 512; } }
                }
            }
            gAs.o += blen;
        }
        if (gAs.iface < 0 || gAs.epIn < 0 || gAs.epOut < 0) {
            /* This device enumerated fine at high speed but is not Bulk-Only mass storage — a USB 2.0 hub,
             * webcam, printer, whatever. We cannot drive it yet.
             *
             * ★★★ n12: PARK IT. Two earlier attempts to hand such a port to the 1.1 companion both failed on
             * hardware, and the second one was actively harmful:
             *   n9  set Port Owner = 1 with the port still ENABLED. The bit did not stick, and because
             *       apple_hidden_port() re-derived "have we ceded this?" from that same bit, the driver
             *       un-ceded the port every pass — 148 handoffs / 134 probe resets in one session.
             *   n11 cleared Port Enable first (the EHCI §2.3.9 handoff precondition) and recorded the
             *       decision in software. The loop stopped, but the Owner bit STILL did not stick
             *       (PORTSC 0x1005 -> 0x1002, bit 13 clear), and disabling the port left it unrecoverable:
             *       it came back connected+enabled with our QHs addressed to the old device, after which no
             *       transfer ever completed again — gIsrHits and gDownDone both flat, the watchdog firing
             *       repeatedly, enumeration re-arming six times to no effect. USB was dead until a reboot.
             * So this controller will not perform the companion handoff, and touching PORTSC to force it
             * costs us the whole stack. Park instead: no PORTSC write at all, remember the decision, stay
             * hidden from Apple, and idle the engine so OTHER ports keep working. The device is left
             * enumerated and idle, which is harmless.
             *
             * Making a hub actually usable needs real hub support — downstream port control, and split
             * transactions for any full/low-speed device behind it — which is the next feature, not a
             * handoff this hardware refuses to do. */
            ehci_os_ilogx("  n12 SELFENUM: high-speed but NOT Bulk-Only mass storage; iface/epIn/epOut",
                          ((UInt32)(gAs.iface & 0xFF) << 16) | ((UInt32)(gAs.epIn & 0xFF) << 8) | (UInt32)(gAs.epOut & 0xFF));
            ehci_os_ilogx("  n12 PORTSC (left exactly as-is — we do NOT write it)",
                          ehci_read32(gSoftc.opBase, EHCI_PORTSC(gAs.port)));

            /* ★★★★ h1 HUB PROBE — READ-ONLY RECONNAISSANCE, then park exactly as before.
             * This is the branch a high-speed non-storage device lands in, which is where a HUB lands. The
             * question this answers, before any hub code is written: does a downstream hub enumerate cleanly
             * on our stack, how many ports does it have, and what is attached to each and at what speed?
             * Strictly GET requests — no SET_PORT_FEATURE, no PORT_POWER, no PORT_RESET, no PORTSC write. The
             * device is already enumerated and idle at this point, so asking it questions costs nothing and
             * changes nothing; the park below still happens, so post-probe behaviour is identical to n27.
             * ⚠ If the hub's ports are unpowered by default they will read !POWER with no connection. That is
             * itself the answer (the next probe would add SET_PORT_FEATURE(PORT_POWER)); do not conclude "no
             * devices attached" from a port that reads unpowered.
             * Runs at SIH level like the rest of this state machine, so every log goes through the
             * interrupt-safe ring. */
            gAsDevDesc[1] = gAsDevDesc[4] = 0;                               /* so a short read cannot read as a hub */
            ctl_begin(0x80, 0x06, 0x0100, 0, gAs.addr, gAsDevDesc, 18, 1);   /* GET_DESCRIPTOR(DEVICE) */
            AS_CTL(60);
            ehci_os_ilogx("  h1 devDesc bDeviceClass/SubClass/Protocol",
                          ((UInt32)gAsDevDesc[4] << 16) | ((UInt32)gAsDevDesc[5] << 8) | gAsDevDesc[6]);
            ehci_os_ilogx("  h1 idVendor/idProduct",
                          ((UInt32)gAsDevDesc[9] << 24) | ((UInt32)gAsDevDesc[8] << 16) |
                          ((UInt32)gAsDevDesc[11] << 8) | gAsDevDesc[10]);
            if (gAsDevDesc[4] != 0x09) {
                ehci_os_ilog("  h1: not a hub (bDeviceClass != 0x09) — nothing more to probe");
                park_port(gAs.port, "  n12: cannot drive this device -> PARKING the port, hardware untouched");
                return;
            }
            /* ★ h6: one hub at a time. A hub found BEHIND the claimed hub, or a second hub on another root
             * port, is not supported — nested hubs would need their own address/port bookkeeping. Park it and
             * say so, rather than overwriting gHub and losing track of the first. */
            if (gHub.claimed) {
                ehci_os_ilog("!! h6: a hub is already claimed — this second hub is NOT supported (nested hubs "
                             "would need their own bookkeeping). Parking it.");
                park_port(gAs.port, "!! h6: second hub -> parking");
                return;
            }
            ehci_os_ilog("=== h1 HUB DETECTED on our port — reading its descriptor and every downstream port (READ-ONLY) ===");
            /* Hub class descriptor: bmRequestType 0xA0 (class | IN | device), GET_DESCRIPTOR, type 0x29. */
            ctl_begin(0xA0, 0x06, 0x2900, 0, gAs.addr, gAsHubDesc, 9, 1);
            AS_CTL(61);
            if (gAsHubDesc[1] != 0x29) {
                ehci_os_ilogx("!! h1: hub descriptor has wrong bDescriptorType (expected 0x29)", gAsHubDesc[1]);
                park_port(gAs.port, "  n12: parking (hub descriptor unreadable)");
                return;
            }
            gAs.hubPorts = gAsHubDesc[2];                       /* bNbrPorts */
            ehci_os_ilogx("  h1 hub bNbrPorts", (UInt32)gAs.hubPorts);
            ehci_os_ilogx("  h1 hub wHubCharacteristics (bits0-1 power switching, bit2 compound, 3-4 overcurrent)",
                          ((UInt32)gAsHubDesc[4] << 8) | gAsHubDesc[3]);
            ehci_os_ilogx("  h1 hub bPwrOn2PwrGood (x2 ms)", (UInt32)gAsHubDesc[5]);
            ehci_os_ilogx("  h1 hub bHubContrCurrent (mA)", (UInt32)gAsHubDesc[6]);
            /* ★★★★ h2 THE h1 BUG. h1 asked GET_PORT_STATUS here and it FAILED (step 0x3f, gDownErr -> 1) —
             * because this branch sits BEFORE the SET_CONFIGURATION at the bottom of the enumeration, so the
             * hub was still in the ADDRESS state. USB 2.0 §9.4: a device in Address state need only answer
             * standard DEVICE requests. GET_PORT_STATUS is a class request with recipient OTHER (a port) and
             * requires the CONFIGURED state. GET_DESCRIPTOR(0x29) is recipient DEVICE, which is exactly why
             * that one worked and the port query did not. The hub was never at fault. */
            /* ★★★★ h5 ADDRESS COLLISION FIX, IN THE RIGHT STATE THIS TIME.
             * The hub must move off DEV_ADDR(0) = 1, because we release slot 0 when we claim it and the first
             * downstream device would then be handed address 1 too — the n17 two-devices-one-address defect
             * that corrupted a mounted volume.
             * ⚠ h4 did this AFTER SET_CONFIGURATION and it silently failed: USB 2.0 §9.4.6 defines SET_ADDRESS
             * only for the Default and Address states, so a CONFIGURED device is entitled to reject it, and
             * this hub did. We then talked to address 15 forever and every request failed (178 of them).
             * Worse, we never saw the rejection, because ctl_done() cannot tell a STALL from success — the
             * same blindness pb_ready() has, documented since p1a. So: re-address HERE, while the device is
             * still in the ADDRESS state, which is exactly where the spec allows it. */
            ctl_begin(0x00, 0x05, (UInt16)HUB_ADDR, 0, gAs.addr, gEnumDesc, 0, 0);   /* SET_ADDRESS(15) */
            AS_CTL(78);
            AS_DELAY(79, 4);                     /* TSETADDR recovery before using the new address */
            gAs.addr = HUB_ADDR;
            ehci_os_ilogx("  h5 hub re-addressed while still in the ADDRESS state; addr", (UInt32)HUB_ADDR);
            /* Prove the new address actually took, instead of trusting a transfer that cannot report a STALL:
             * re-read the device descriptor AT the new address. If the hub ignored SET_ADDRESS this fails and
             * we abandon the hub rather than spinning on a dead address. */
            gAsDevDesc[1] = 0;
            ctl_begin(0x80, 0x06, 0x0100, 0, gAs.addr, gAsDevDesc, 18, 1);
            AS_CTL(80);
            if (gAsDevDesc[1] != 0x01) {
                ehci_os_ilogx("!! h5: hub did not answer at its new address — abandoning the hub; bDescType",
                              gAsDevDesc[1]);
                park_port(gAs.port, "!! h5: hub unreachable after re-address -> parking (n27 behaviour)");
                return;
            }
            ehci_os_ilog("  h5 hub CONFIRMED at its new address (device descriptor re-read)");
            ctl_begin(0x00, 0x09, (UInt16)gAs.cfgVal, 0, gAs.addr, gEnumDesc, 0, 0);   /* SET_CONFIGURATION */
            AS_CTL(64);
            ehci_os_ilogx("  h2 hub CONFIGURED; bConfigurationValue", (UInt32)gAs.cfgVal);
            /* ★ h2: power the downstream ports. SET_PORT_FEATURE(PORT_POWER=8): bmRequestType 0x23
             * (host-to-device | class | other), bRequest 0x03. h1 read wHubCharacteristics = 0x0080, i.e.
             * GANGED switching, so the first one powers them all — we still issue it per port, as the spec
             * expects, because a ganged hub simply ignores the redundant ones. */
            gAs.hubPort = 1;
        case 65:
            if (gAs.hubPort <= gAs.hubPorts && gAs.hubPort <= 15) {
                ctl_begin(0x23, 0x03, 0x0008, (UInt16)gAs.hubPort, gAs.addr, gEnumDesc, 0, 0);
                AS_CTL(66);
                gAs.hubPort++; gAs.pc = 65; return;
            }
            ehci_os_ilog("  h2 downstream ports powered — waiting bPwrOn2PwrGood before reading status");
            AS_DELAY(67, 120);        /* h1 measured bPwrOn2PwrGood = 50 => 100 ms; 120 for margin */
            gAs.hubPort = 1;
        case 62:
            if (gAs.hubPort <= gAs.hubPorts && gAs.hubPort <= 15) {
                /* GET_PORT_STATUS: bmRequestType 0xA3 (class | IN | OTHER), bRequest 0, wIndex = port. */
                gAsPortSt[0] = gAsPortSt[1] = gAsPortSt[2] = gAsPortSt[3] = 0;  /* h13: no stale inheritance */
                ctl_begin(0xA3, 0x00, 0x0000, (UInt16)gAs.hubPort, gAs.addr, gAsPortSt, 4, 1);
                AS_CTL(63);
                /* h13: the claim-time survey. A short reply here would print another port's numbers under this
                 * port's heading — and this log is the record we reason from afterwards. Say "unknown". */
                if (ctl_got() < 4) {
                    ehci_os_ilogx("  h13: downstream port status reply was SHORT — status unknown, not "
                                  "reported; port", (UInt32)gAs.hubPort);
                    gHubShortSt++;
                    gAs.hubPort++; gAs.pc = 62; return;
                }
                { UInt32 st = ((UInt32)gAsPortSt[1] << 8) | gAsPortSt[0];
                  UInt32 ch = ((UInt32)gAsPortSt[3] << 8) | gAsPortSt[2];
                  ehci_os_ilogx("  h1 downstream port", (UInt32)gAs.hubPort);
                  ehci_os_ilogx("    wPortStatus (b0 conn b1 ena b2 susp b3 oc b4 rst b8 pwr b9 LS b10 HS)", st);
                  ehci_os_ilogx("    wPortChange", ch);
                  /* Decoded so the log is readable without the spec to hand. FS = connected, not LS, not HS. */
                  if (!(st & 0x0001)) ehci_os_ilog(!(st & 0x0100) ? "    -> EMPTY and UNPOWERED"
                                                                 : "    -> empty (powered)");
                  else if (st & 0x0200) ehci_os_ilog("    -> LOW-SPEED device attached (needs SPLIT transactions = tier 2)");
                  else if (st & 0x0400) ehci_os_ilog("    -> HIGH-SPEED device attached (tier 1: normal transfers, NO splits)");
                  else                  ehci_os_ilog("    -> FULL-SPEED device attached (needs SPLIT transactions = tier 2)");
                }
                gAs.hubPort++;
                gAs.pc = 62; return;                            /* loop to the next port */
            }
            /* ★★★★ h3 TIER 1: CLAIM the hub instead of parking its port.
             * The port stays ours and hidden from Apple (it always was — parking never wrote PORTSC), but now
             * we keep the hub live and walk its downstream ports for devices we can drive. Arming the scan
             * here rather than recursing: this gAs run ENDS, and as_tick starts a fresh enumeration per
             * downstream port, exactly the way gSelfEnumPort already arms one for a root port. Reusing that
             * mechanism is what keeps a single enumeration path. */
            /* ★★★★ h10: find the hub's status-change endpoint and park a qTD on it.
             * Re-read the config descriptor at the hub's address and walk it for the interrupt IN endpoint —
             * every hub has exactly one, and it is the only thing that will tell us a port changed without us
             * asking. Re-reading rather than trusting whatever is still in gEnumDesc: this runs once, at
             * claim, and a wrong endpoint here means silent deafness later. */
            ctl_begin(0x80, 0x06, 0x0200, 0, gAs.addr, gEnumDesc, 64, 1);
            AS_CTL(81);
            {   int o = 0, epFound = 0;
                while (o + 1 < 64 && gEnumDesc[o] >= 2) {
                    if (gEnumDesc[o + 1] == 0x05 && o + 6 < 64) {          /* ENDPOINT descriptor */
                        UInt8 epa = gEnumDesc[o + 2], attr = gEnumDesc[o + 3];
                        if ((attr & 0x03) == 0x03 && (epa & 0x80)) {       /* interrupt IN */
                            gHubIntEp = (UInt32)(epa & 0x0F); epFound = 1; break;
                        }
                    }
                    o += gEnumDesc[o];
                }
                if (!epFound) {
                    ehci_os_ilog("!! h10: the hub has no interrupt IN endpoint — it cannot tell us about port "
                                 "changes. Claiming anyway; devices attached NOW are still found, but hot-plug "
                                 "behind this hub will not work.");
                    gHubIntEp = 0;
                } else {
                    ehci_os_ilogx("  h10 hub status-change endpoint", gHubIntEp);
                }
            }
            /* h10: seed EVERY port as "changed" so anything already attached is found, then park the qTD.
             * From here on the hub tells us; we never poll the bus again. */
            gHub.changeMask = ((1UL << (gAs.hubPorts + 1)) - 2UL);   /* bits 1..nPorts */
            gHubIntLen = (UInt32)((gAs.hubPorts + 8) / 8);           /* ceil((nPorts+1)/8) */
            gHub.rootPort = gAs.port;
            gHub.addr     = gAs.addr;
            gHub.nPorts   = gAs.hubPorts;
            gHub.scanPort = 1;
            gHub.claimed  = 1;
            ehci_os_ilogx("=== h3 TIER 1: HUB CLAIMED — walking its downstream ports; nPorts", (UInt32)gHub.nPorts);
            ehci_os_ilogx("  h3 hub address", gHub.addr);
            /* ★ h10: hand the QH programming to TASK level (see gHubIntNeedArm). */
            if (gHubIntEp) {
                gHubIntNeedArm = 1;
                gAs.pc = 82; gAs.t0 = frame_ms(); return;
        case 82:
                if (gHubIntNeedArm) {
                    if (frame_ms() - gAs.t0 > 5000UL) {
                        ehci_os_ilog("!! h10: task level never armed the hub status endpoint (no pump?) — "
                                     "continuing without change notifications");
                        gHubIntNeedArm = 0; gHubIntEp = 0;
                    } else return;
                }
            }
            /* The hub occupied a gDev slot during its own enumeration; give it back. A hub needs an address
             * and control transfers, never a bulk QH pair, so holding a slot would waste one of only two. */
            if (gAs.dev >= 0) {
                int k;
                for (k = 0; k < NBULK; k++)
                    if (gBulkEP[k].used && gBulkEP[k].dev == (UInt8)gAs.dev) gBulkEP[k].used = 0;
                gDev[gAs.dev].inUse = 0; gDev[gAs.dev].probedPort = -1;
                gDev[gAs.dev].pOut = -1; gDev[gAs.dev].pIn = -1;
                ehci_os_ilogx("  h3 released the gDev slot the hub used during its own enumeration; slot",
                              (UInt32)gAs.dev);
            }
            /* ★★★★★★★ h71 — "THE ROOT PORT IS SETTLED" IS A PER-PORT STATEMENT. THIS SET A GLOBAL.
             * h39's defect on a FOURTH path (h39 cede, h56 success, h70 park, h71 hub-claim). The old comment
             * on this line said "the ROOT PORT is settled — do not re-arm on it" while the code stopped
             * enumeration for the ENTIRE CONTROLLER.
             * ★ AND THE PER-PORT PROTECTION ALREADY EXISTS: the h39 scan skips the hub's root port explicitly
             * (`if (gHub.claimed && p == gHub.rootPort) continue;`). The global was not merely wrong, it was
             * redundant.
             * ★★ MEASURED, the hub + two drives cold boot (2026-08-11): the display's hub was claimed on
             * port 1, this line fired, and the scan never looked at PORT 0 again -- so the keyboard stayed
             * APPLE_HIDden and dead for the whole boot. Its cede only appears at log line 4248, right after
             * the user PHYSICALLY REPLUGGED it, because a connect event is the one thing that clears
             * gSelfEnumDone (service_ports). Both drives mounted; the keyboard was collateral.
             * ⇒ The engine's ORDERING was never the problem -- the root-port arm already outranks the hub
             * sweep, and the scan runs before both. Enumeration was simply switched off. */
            /* ★★★★★★★ h72 — h71 WAS HALF A FIX AND THE OTHER HALF MATTERED.
             * ⚠ Removing `gSelfEnumDone = 1` was right in principle -- a per-port decision must not stop the
             * controller -- but that line was quietly doing a SECOND job its comment never mentioned: with the
             * global set, the root-port arm could not re-fire. Remove it and leave gSelfEnumPort STILL AIMED
             * AT THE HUB'S OWN PORT, and the root arm simply re-enumerates the hub for ever.
             * ★ MEASURED, m23: exactly 2 SELFENUM starts, BOTH on port 1 (the hub), ZERO h39 scans, ZERO
             * downstream walks, no drives. The hub was claimed and then re-enumerated instead of swept.
             * ⇒ THE CORRECT PER-PORT EXPRESSION OF "THIS ROOT PORT IS SETTLED" IS TO CLEAR THE AIM, not to
             * stop the world. With gSelfEnumPort = -1 the scan runs, skips the hub's own port (it already
             * does: `if (gHub.claimed && p == gHub.rootPort) continue;`), resolves any OTHER port -- the
             * keyboard cedes in ~600 ms -- and once nothing is eligible the hub sweep's else-if finally gets
             * the engine. All three things we need, from one assignment. */
            gSelfEnumPort = -1;
            ehci_os_ilog("  h72: hub claimed — AIM cleared for that port (not a controller-wide stop), so the "
                         "scan can resolve other ports and the hub sweep can then run");
            gAs.running = 0; gAs.pc = 0; gAs.dev = -1;
            return;
        }
        ehci_os_ilogx("  MSC interface", (UInt32)gAs.iface);
        ehci_os_ilogx("  bulk-IN ep / maxpkt",  ((UInt32)gAs.epIn  << 16) | (UInt32)gAs.mpIn);
        ehci_os_ilogx("  bulk-OUT ep / maxpkt", ((UInt32)gAs.epOut << 16) | (UInt32)gAs.mpOut);

        /* 7. Configure the device (bulk endpoints are dead until this is done). */
        ctl_begin(0x00, 0x09, (UInt16)gAs.cfgVal, 0, gAs.addr, gEnumDesc, 0, 0);
        AS_CTL(8);
        ehci_os_ilog("  SET_CONFIGURATION ok");

        /* 8. Register the bulk endpoints (what Apple's slot-6 CreateBulkEndpoint used to do).
         * ★★ HAND THIS TO TASK LEVEL. ehci_vhub_create_bulk() calls ase_quiesce() — a 200,000-iteration
         * MMIO spin waiting for the async schedule to stop — and then epq_program()s the bulk QHs.
         * Reprogramming a QH from the SIH is the documented r84/r85 freeze, and the spin alone is far too
         * long to sit in an interrupt handler. In n4c this only ever ran at TASK level, from selfenum_run;
         * moving enumeration to interrupt level in n5 quietly dragged it below task level with it. That is
         * what froze the machine on hot re-insert: the reinsert path had just quiesced and re-armed ASE via
         * reconnect_reset, so create_bulk hit a live ring from the SIH.
         * Same interrupt->task boundary as gAsProbeOK: park here, let selfprobe_tick do it, then resume. */
        gAsNeedBulk = 1;
        AS_TASK(15);
        { int ad = (gAs.dev >= 0) ? gAs.dev : 0;   /* n14 step 3: the slot THIS enumeration fills */
        if (gDev[ad].pOut < 0 || gDev[ad].pIn < 0) {
            as_fail("!! n5 SELFENUM: bulk endpoints registered but pb_find_eps found none; gPOut/gPIn",
                    ((UInt32)(gDev[ad].pOut & 0xFF) << 8) | (UInt32)(gDev[ad].pIn & 0xFF));
            return;
        }
        /* ★ n13: remember WHICH PORT this device is on. The driver used to infer that from
         * gSelfEnumPort, which any later connect on any other port overwrites — see gProbedPort. */
        gDev[gAs.dev >= 0 ? gAs.dev : 0].probedPort = gAs.port;
        /* ★ n15: this slot now owns a device. Its endpoints are registered on ITS OWN bulk QH pair
         * (create_bulk tagged gBulkEP[].dev = gCur), so transfers can never land on another device's
         * queue heads. Slot 0 goes on to publish 'Eusb' + AddDrive as before; a slot >= 1 stops here
         * and is only logged, because exposing it needs the paired gSvc/activator change (step 4). */
        gDev[ad].inUse = 1;
        /* ★ n27: record the negotiated speed at the moment we take ownership. The port's ENABLE bit after a
         * successful reset IS the EHCI speed determination (see the HPS_HIGHSPEED derivation in port_status). */
        gDev[ad].hiSpeed = (UInt8)((ehci_read32(gSoftc.opBase, EHCI_PORTSC(gAs.port)) & EHCI_PORT_ENABLE) ? 1 : 0);
        gNewDevMask |= (1UL << (ad & 0x0F));
        if (gAs.port >= 0 && gAs.port < 15) gBounceCnt[gAs.port] = 0;   /* h33: a success clears the streak */
        /* CRITICAL channel from here to the exposure latch: these are the lines that say a device was really
         * verified, and the m24 rate cap dropped 458 ring lines straight through the middle of them. */
        ehci_os_ilogc("=== n5 SELFENUM COMPLETE — we enumerated + own the device; starting BOT probe ===");
        ehci_os_ilogx("  outEp.addr/ep", ((UInt32)gBulkEP[gDev[ad].pOut].addr << 16) | gBulkEP[gDev[ad].pOut].endpt);
        ehci_os_ilogx("  inEp.addr/ep",  ((UInt32)gBulkEP[gDev[ad].pIn].addr  << 16) | gBulkEP[gDev[ad].pIn].endpt); }

        /* ---- 9. BOT probe: INQUIRY ---- */
        bot_begin(gAsCdbInq, 6, 36, gAsInq, 36);
        AS_BOT(9);
        if (gBot.csw != 0) { as_fail("!! n5 PROBE: INQUIRY CSW status", (UInt32)gBot.csw); return; }
        capture_device_name(gAs.dev >= 0 ? gAs.dev : 0, gAsInq);   /* n24: per-slot. n10: for Apple's device-named alerts */
        ehci_os_ilog("  n5 SELFPROBE INQUIRY ok:");
        ehci_os_ilogx("    periphType", gAsInq[0]);
        ehci_os_ilogx("    vendor0_3",  BUF_BE32(gAsInq, 8));
        ehci_os_ilogx("    prod0_3",    BUF_BE32(gAsInq, 16));

        /* ---- 10. TEST UNIT READY, clearing UNIT ATTENTION via REQUEST SENSE (up to 8 attempts) ---- */
        gAs.tur = 0;
    case 10:
        bot_begin(gAsCdbTur, 6, 0, 0, 0);
        AS_BOT(11);
        if (gBot.csw == 0) { ehci_os_ilogx("  unit ready after TUR attempts", (UInt32)(gAs.tur + 1)); goto ready; }
        gSense[2] = gSense[12] = gSense[13] = 0;
        bot_begin(gAsCdbSense, 6, 18, gSense, 18);
        AS_BOT(12);
        ehci_os_ilogx("  REQUEST SENSE senseKey/ASC/ASCQ",
                      ((UInt32)(gSense[2] & 0x0Fu) << 16) | ((UInt32)gSense[12] << 8) | gSense[13]);
        if (++gAs.tur < 8) { gAs.pc = 10; return; }
        as_fail("!! n5 PROBE: device never reported ready after TUR attempts", (UInt32)gAs.tur);
        return;
    ready:

        /* ---- 11. READ CAPACITY ---- */
        bot_begin(gAsCdbCap, 10, 8, gAsCap, 8);
        AS_BOT(13);
        if (gBot.csw != 0) { as_fail("!! n5 PROBE: READ CAPACITY CSW status", (UInt32)gBot.csw); return; }
        gAs.blocks  = BUF_BE32(gAsCap, 0) + 1UL;      /* READ CAPACITY returns the LAST LBA */
        gAs.blkSize = BUF_BE32(gAsCap, 4);
        ehci_os_ilogcx("  n5 SELFPROBE READ CAPACITY blocks",  gAs.blocks);
        ehci_os_ilogcx("                     blockSize", gAs.blkSize);
        if (gAs.blkSize != 512UL || gAs.blocks < 4UL) {
            as_fail("!! n5 PROBE: implausible geometry (stale/garbage data phase); blockSize", gAs.blkSize);
            return; }
        {   /* ★ n18 FIX 3: record geometry against the slot that was probed, not globally. */
            int gd = (gAs.dev >= 0) ? gAs.dev : 0;
            gDevBlkCnt[gd] = gAs.blocks; gDevBlkSize[gd] = gAs.blkSize;
            ehci_os_ilogcx("  n18 geometry recorded for slot", (UInt32)gd);
        }

        /* ---- 12. READ block 0 — the proof we can really read the medium ---- */
        gAsCdbRd[0] = 0x28;
        gAsCdbRd[1] = gAsCdbRd[2] = gAsCdbRd[3] = gAsCdbRd[4] = gAsCdbRd[5] = 0;
        gAsCdbRd[6] = 0; gAsCdbRd[7] = 0; gAsCdbRd[8] = 1; gAsCdbRd[9] = 0;
        bot_begin(gAsCdbRd, 10, 512, gAsBlk0, 512);
        AS_BOT(14);
        if (gBot.csw != 0) { as_fail("!! n5 PROBE: READ block 0 CSW status", (UInt32)gBot.csw); return; }
        ehci_os_ilogx("  n5 SELFPROBE READ block 0 b0_3", BUF_BE32(gAsBlk0, 0));

        ehci_os_ilogc("=== n5 SELFPROBE COMPLETE (async, interrupt-driven — no application involved) ===");
        gAs.running = 0; gAs.done = 1; gAs.pc = 0;
        gAs.dev = -1;              /* n14 step 3: sequence over; the next device allocates its own slot.
                                    * Deliberately NOT cleared at SELFENUM COMPLETE: the BOT probe below
                                    * runs after that point and needs gAs.dev to target the right one. */
        /* ★★★★★★ h56 — ENUMERATING ONE PORT MUST NOT END ENUMERATION FOR THE WHOLE CONTROLLER.
         * THIS IS h39's DEFECT, ONE LAYER OVER, AND IT SURVIVED h39 BECAUSE h39 ONLY FIXED THE CEDE PATH.
         *
         * ⚠ This line was `gSelfEnumDone = 1`. gSelfEnumDone is the GLOBAL "stop looking" flag, and as_tick's
         * scan is gated `if (gSelfEnumPort < 0 && !gSelfEnumDone && !gAs.running)` — so a SUCCESSFUL probe on
         * one port stopped the engine from ever looking at any other port, exactly as a CEDE used to.
         *
         * ★ THE m7 RUN IS THE PROOF, and it is unambiguous. Keyboard on port 0, drive on port 1, both at cold
         * boot. The drive enumerated CLEANLY — SELFENUM COMPLETE, INQUIRY ok, READ CAPACITY 0x780000, geometry,
         * SELFPROBE COMPLETE — and the keyboard's port was then NEVER LOOKED AT. h39's scan only found port 0
         * after the user PHYSICALLY PULLED the drive, which cleared the flag via the disconnect path. So the
         * keyboard was dead for the whole session because a DIFFERENT port's enumeration had succeeded.
         *
         * ★★ AND IT IS CIRCULAR ON THIS MACHINE: the volume exposure is deferred while a modal dialog is up
         * (h30/h44 logged isFinder<<8|dialogUp = 0x0101 — the Keychain dialog), and that dialog cannot be
         * dismissed without the keyboard. Fix this line and both unwind: the keyboard is ceded in ~600 ms, the
         * dialog is dismissed, the defer releases, the drive mounts.
         *
         * The per-port record already exists and is already consulted by the scan (gPortCeded, gPortParked,
         * and gDev[].probedPort via the `owned` test), so releasing the engine here loses nothing: the next
         * tick re-scans, skips this port because a live slot owns it, and picks up any OTHER port still
         * holding an unenumerated device. Same shape as h39, same reasoning, same log line. */
        gSelfEnumPort = -1;        /* release the aim so the scan can re-target */
        ehci_os_ilog("  h56: engine RELEASED after a successful probe (not gSelfEnumDone) — another port may "
                     "still hold an unenumerated device");
        gFenceApple   = 1;         /* belt-and-braces: Apple must never touch our endpoints */
        /* h59: latch the slot NOW, while gEnumDev still describes the probe that just finished. The exposure
         * may sit in the h30/h44 defer for up to 45 s, and gEnumDev will not survive another enumeration. */
        gExposeDev    = gEnumDev;
        ehci_os_ilogcx("  h59: exposure slot LATCHED at probe completion (read at release instead of "
                       "gEnumDev, which a later enumeration would overwrite); slot", (UInt32)gExposeDev);
        gAsProbeOK    = 1;         /* task level picks this up to publish 'Eusb' + hand the mount over */
        return;
    }
}
#undef AS_AWAIT
#undef AS_DELAY
#undef AS_CTL
#undef AS_BOT

/* Kick the engine. Safe to call from ANY level; called from the heartbeat and (harmlessly) from uim23.
 * ★ ARMING HAPPENS HERE, not at task level. service_ports sets gSelfEnumPort at interrupt level when a
 * hidden port connects; if arming lived in selfprobe_tick then discovery would still need slot 23 and n5
 * would have achieved nothing. This is the line that actually makes the driver self-starting. */
/* ★★★★ n15 step 3: enumerate a SECOND device into its OWN slot.
 *
 * ⚠⚠ THE BLOCKER, ESTABLISHED HERE AND NOT YET REMOVED — READ BEFORE TOUCHING gCur.
 *
 * The obvious implementation is to point gCur at the slot being enumerated for the duration of an
 * as_advance() step and restore it after. That is WRONG and was reverted after being written:
 *   - as_tick is called from the heartbeat (INTERRUPT level) AND from ehci_vhub_selfprobe_tick (TASK level,
 *     slot 23). gAsBusy makes as_tick non-reentrant, but it does NOT stop the heartbeat ISR running
 *     bio_kick while a TASK-level as_tick has gCur moved.
 *   - the endpoint indices were macro aliases onto gDev[gCur], and the bio engine reads them at interrupt
 *     level. So an interrupt landing in that window would issue device A's transfer against device B's
 *     endpoints. With n12's unregistered-endpoint guard the request is dropped rather than corrupting
 *     memory, but dropping an I/O request mid-copy is still a data-integrity failure.
 *
 * ⇒ STEP 1 OF THAT PREREQUISITE IS NOW DONE: the aliases are gone and every site names its device
 * explicitly as gDev[0], which no interrupt can retarget. What REMAINS is step 2, threading an explicit
 * `int d` through the pb_* and bio_* primitives so those literal 0s become the real device. Only then can
 * this function safely enumerate into a slot >= 1. Enumeration itself is already serial (one gAs, one
 * shared ctrl ep0), so only where the RESULT lands needs to change.
 *
 * ★★★★ h14: THE ABOVE IS HISTORY — BOTH STEPS ARE LONG DONE and this comment described the world as it was
 * at n14. Step 2 landed in n19/n20 (`BioReq.dev`, and `pb_*`/`bot_step` taking the device as a parameter),
 * multi-device was validated on hardware at n23/n27, and this function has enumerated into slots >= 1 ever
 * since. It is kept because the reasoning is still the right reasoning — the device must travel with the
 * work — but read "until then this stays single-slot" as a note about n14, NOT about the code below it.
 * A stale comment is a claim that expired silently, which this project has been bitten by before. */
static int dev_alloc(void)      /* n14 step 3: lowest free gDev[] slot, or -1 if all are taken */
{
    int d;
    for (d = 0; d < USB_MAX_DEV; d++) if (!gDev[d].inUse) return d;
    return -1;
}
/* h14: bit d = slot d in use. With four slots, "no free device slot" is no longer self-explanatory — the
 * useful question is WHICH slots are held, so a refusal can be read against what is actually plugged in. */
static UInt32 dev_inuse_mask(void)
{
    UInt32 m = 0; int d;
    for (d = 0; d < USB_MAX_DEV; d++) if (gDev[d].inUse) m |= (1UL << d);
    return m;
}
static void as_tick(void)
{
    if (gAsBusy) return;
    gAsBusy = 1;
    /* ★★★ n14 step 3: ENUMERATE INTO A FREE SLOT.
     *
     * The old `gDev[0].pstate != 10` term is gone: that was slot 0's state, so once a first drive mounted it
     * gated enumeration off for everything. The bound is now simply whether a slot is free.
     *
     * Note what is NOT here, because it was tried and reverted: gCur is not moved. Nothing in the I/O path
     * reads a mutable global to learn its device any more (BioReq carries dev, pb_* and bot_step take it as
     * a parameter), which is exactly what makes arming an enumeration for slot 1 safe while slot 0 is
     * mounted and transferring. Enumeration stays serial, one gAs and one shared ctrl ep0, so only WHERE the
     * result lands is per-device. */
    /* ★★★ n18 FIX 4: DO NOT START AN ENUMERATION WHILE A MOUNTED DEVICE HAS I/O PENDING.
     *
     * The engine runs one transfer at a time (the CBW/CSW/bounce buffers are shared), so a second device's
     * enumeration and probe - port reset, SET_ADDRESS, descriptors, SET_CONFIGURATION, then INQUIRY, READ
     * CAPACITY and a block-0 read - sits in front of every queued block request for drive A. Each of those
     * steps is allowed up to a 10s transfer watchdog. In n17 that is part of why the Finder waited ~30s per
     * action and then reported a disk error: A's File Manager requests were being starved, not just
     * misdirected. Waiting for the bio ring to drain costs a newcomer a little latency and protects a
     * MOUNTED volume from timing out, which is the right trade. gBioHead/gBioTail are the ring's producer
     * and consumer indices; equal means nothing in flight and nothing queued. */
    if (gBioHead != gBioTail) {
        /* ★★★★ h16: COUNT A DEFERRAL ONLY WHEN SOMETHING IS ACTUALLY WAITING.
         * h14 incremented this unconditionally, so it counted every heartbeat during ANY file copy rather than
         * every occasion a device was held back. The h15 run reached 8,390 with nothing waiting at all, which
         * made the number worthless for the one question it exists to answer: is the four-drive case starving a
         * newcomer's enumeration behind mounted-volume I/O? Gate it on there being a pending arrival — a root
         * port with an unfinished enumeration, or a hub port the hub has flagged as changed.
         * ⇒ n24 sweep finding 3 again: a diagnostic that lies has cost this project cycles, and a counter that
         * measures the wrong thing is exactly that. */
        if ((gSelfEnumPort >= 0 && !gSelfEnumDone) || (gHub.claimed && gHub.changeMask))
            gEnumDeferBusy++;
        gAsBusy = 0; return;
    }
    /* ★★★★★★ h39 part 2 — WHEN THERE IS NO AIM, SCAN FOR ONE. gSelfEnumPort holds ONE port and is set from a
     * CONNECT EVENT, so with several devices attached at cold boot service_ports overwrites it and only the
     * LAST connect survives; every earlier device is forgotten. The h38+v7 run is exactly that: the mouse's
     * connect landed last, the engine worked port 4 only, and the drive on port 2 was never looked at even
     * though it sat there connected and unenumerated (final PORTMAP 0x1801).
     * ★ Event-driven arming stays the primary path (unchanged). This only fills the gap when there is no
     * pending aim — which covers both "I just ceded a port, what else is out there" (h39 part 1) and
     * "several devices were already attached before we existed". A port qualifies only if it is connected,
     * not already ceded to Apple, not parked, and not owned by a live slot — all facts that are recorded
     * PER PORT and simply were not being consulted here. */
    /* ★★★★★★ h58 — THE h9 GUARD, WHICH THIS SCAN NEVER HAD, AND WHICH h56 MADE LOAD-BEARING.
     *
     * ⚠ gAsProbeOK and gAsNeedBulk are parks the interrupt engine sets for TASK LEVEL to finish, and both read
     * gEnumDev to know which slot they are completing. Arming another port here overwrites it. h9 wrote that
     * guard for the HUB sweep (a few lines below) and h39's port scan was added later without it — harmlessly,
     * because a successful probe set gSelfEnumDone = 1 and this scan could never run afterwards anyway.
     * ★ h56 REMOVED THAT BLOCK ON PURPOSE (so a second occupied port still gets serviced) AND THEREBY EXPOSED
     * THE UNGUARDED PATH. The m9 run is h9's symptom, verbatim, three lines apart:
     *     === n5 SELFPROBE COMPLETE ===
     *       h56: engine RELEASED after a successful probe
     *       h39: no pending aim — SCANNED and found a connected, unenumerated port; port 0
     *     === n5 SELFENUM ... ===
     * The keyboard was ceded correctly and the drive that had JUST PROBED was never exposed: no
     * exposure-DEFERRED line, no AddDrive, no mount. Exactly what h9 recorded — "both drives enumerated, both
     * recorded geometry, neither was ever EXPOSED".
     * ⇒ The engine runs on the 8 ms heartbeat and the handoff only when task level gets a turn, so the scan
     * wins this race every time. Wait for the handoff; it clears in a fraction of a second (selfprobe_tick
     * sets gAsProbeOK = 0), and the other port is still serviced immediately afterwards — h56's whole point is
     * preserved, it just no longer overtakes the mount it was released by. */
    /* ★★★★★★ h59 — AND NOW THE GUARD CAN BE NARROWED, WHICH IS WHAT BREAKS THE DEADLOCK.
     *
     * ⚠ THE DEADLOCK, measured on m10: (1) the keyboard's port is not serviced until the drive's handoff
     * completes; (2) the handoff is deferred until the modal dialog clears; (3) the Keychain dialog cannot be
     * dismissed without the keyboard. So h58's blanket `!gAsProbeOK` held the scan for the FULL 45 s defer,
     * the log shows "h44: defer CAP reached (45 s) — exposing anyway; dialogUp 1", and the user had to swap
     * ports. m9 (no guard) freed the keyboard early but lost the mount. We were trading one for the other.
     *
     * ★ THE DISTINCTION THE CODEBASE ALREADY DRAWS: gExposureDeferred separates "task level must finish this
     * NOW" from "task level has taken it and is parked waiting on the Finder". The NM re-arm gate has used
     * exactly this predicate for the same reason since h35: `(gAsProbeOK && !gExposureDeferred)`.
     * ⇒ h9's hazard is real only in the first state. In the second, the handoff is not about to read anything
     * — and with h59 latching the slot it could not be hurt if it did.
     * ⇒ The keyboard is now ceded while the exposure waits, the user dismisses the dialog, the defer releases
     * in seconds instead of capping at 45, and the drive mounts. Both halves, instead of one. */
    if (gSelfEnumPort < 0 && !gSelfEnumDone && !gAs.running
        && !(gAsProbeOK && !gExposureDeferred) && !gAsNeedBulk) {
        int p, np = (int)gSoftc.nPorts;
        for (p = 0; p < np && p < 15; p++) {
            int owned = 0, o;
            if (!gPortConn[p] || gPortCeded[p] || gPortParked[p]) continue;
            if (gHub.claimed && p == gHub.rootPort) continue;     /* the hub owns this one */
            for (o = 0; o < USB_MAX_DEV; o++)
                if (gDev[o].inUse && gDev[o].probedPort == p) { owned = 1; break; }
            if (owned) continue;
            gSelfEnumPort = p; gSelfEnumTries = 0;
            ehci_os_ilogx("  h39: no pending aim — SCANNED and found a connected, unenumerated port; port",
                          (UInt32)p);
            break;
        }
    }
    /* ★★★ n21: NEVER enumerate a port a live slot already owns.
     * gSelfEnumPort is a single "which port next" variable and it is only cleared when THAT port's device is
     * pulled (service_ports: `if (p == gSelfEnumPort)`). Pull a device on a DIFFERENT port and gSelfEnumPort
     * keeps pointing at the survivor's port, while the rearm clears gSelfEnumDone — so enumeration re-runs
     * against a port whose device is already enumerated, probed and owned by another slot, and dev_alloc
     * hands it the slot the pull just freed. That is the n20 hardware bug: eject both drives, pull one, and
     * the OTHER re-enumerated into the freed slot and was re-announced under the PULLED drive's drive number.
     * gDev[].probedPort has recorded the answer since n13; it simply was not consulted here. Same lesson as
     * n11's gPortCeded and n19's r->dev: where we have already established a fact, consult it. */
    if (gSelfEnumPort >= 0 && gAs.dev < 0) {
        int o;
        for (o = 0; o < USB_MAX_DEV; o++)
            if (gDev[o].inUse && gDev[o].probedPort == gSelfEnumPort) {
                gPortOwnedMask |= (1UL << (gSelfEnumPort & 0x0F));   /* task level logs it */
                gSelfEnumPort = -1; gSelfEnumDone = 1;               /* nothing to enumerate here */
                break;
            }
    }
    /* ★★★★★★★ h61 — THE h60 SETTLE IS REMOVED, AND THE REASON IS A HARD RULE NOW.
     *
     * ⛔ h60 GATED THIS ARM ON A 500 ms TIMER AND USED A BARE `return;` — WITHOUT CLEARING gAsBusy, WHICH
     * as_tick SETS ON ENTRY AND EVERY OTHER EXIT PATH CLEARS. The guard latched on the first hold and as_tick
     * returned at its first line for the rest of the session. No enumeration ever armed, so no port was ever
     * ceded, while service_ports kept APPLE_HIDE-ing every port the user plugged into: hidden from Apple AND
     * unused by us, i.e. the "bricked port" state this driver was written to eliminate. The machine could not
     * be recovered by unplugging or by changing ports; it took an OS 9 CD boot and a stock-ROM restore.
     *
     * ★★ TWO LESSONS, AND THE SECOND MATTERS MORE THAN THE TYPO:
     *  1. An early return from a function that holds a re-entrancy flag must release it. The compiler cannot
     *     see this; only the reader can. (Same family as n12's "an abandoned attempt must not leave a transfer
     *     in flight".)
     *  2. ★ THE CEDE PATH IS THE USER'S ONLY ROUTE BACK TO A KEYBOARD, AND IT RUNS THROUGH THIS ARM. Anything
     *     placed in front of it must FAIL OPEN. A timing gate that can stall — for any reason, including a
     *     clock that stops advancing — must never be able to stand between the user and their input. If a
     *     settle is ever wanted here again, it must be expressed as "arm anyway, but delay the first control
     *     transfer", never as "do not arm".
     * The settle hypothesis itself was never tested: the gate never opened, so h60 proved nothing about it.
     * TRSTRCY stays at h60's 50 ms, which is a plain delay INSIDE the sequence and gates nothing. */
    if (!gAs.running && !gSelfEnumDone && gSelfEnumPort >= 0 && gSelfEnumTries < 3u) {
        int slot = (gAs.dev >= 0) ? gAs.dev : dev_alloc();
        if (slot >= 0) {
            gAs.dev = slot; gEnumDev = slot;
            gAs.pc = 0; gAs.port = gSelfEnumPort; gAs.running = 1; gAs.done = 0; gAs.failed = 0;
            /* ★★★★★★★ h69 — ARMING AN ENUMERATION MUST NOT DESTROY A PENDING EXPOSURE.
             * ⚠ gAsProbeOK is the interrupt->task handoff that says "a probed volume is waiting to be
             * exposed". Clearing it here means "cancel any probe-complete signal", which was right when one
             * enumeration at a time owned everything. It is WRONG once h56/h59 let the engine service ANOTHER
             * port while an exposure sits in the h30/h44 defer: arming the keyboard's port wiped the drive's
             * pending exposure, so selfprobe_tick's `if (gAsProbeOK)` block never ran again -- no re-check, no
             * 45 s cap, NO MOUNT.
             * ★ THE CROSS-RUN PROOF: m10 (h58, BLANKET guard) blocked the scan during the defer, so nothing
             * armed, gAsProbeOK survived, and it is THE ONLY COLD-BOOT RUN THAT EVER REACHED "defer CAP
             * reached" AND "EXPOSED". Every run from m14 on carries h59's narrowed guard, which frees the
             * keyboard during the defer -- and lost the mount here, silently.
             * ⇒ h59's gExposeDev latch already protects WHICH SLOT the exposure targets against gEnumDev being
             * overwritten just above. This protects the exposure's EXISTENCE. Both are needed; neither alone
             * is sufficient. Same family as every other bug in this driver: state cleared by an event that is
             * not the event it cares about. */
            if (!gExposureDeferred) gAsProbeOK = 0;
            /* ★★★★ h15 THE h14 LIVELOCK. THIS IS A ROOT-PORT RUN, SO SAY SO. `gAs.hubAddr` is the switch that
             * sends every reset and port query through the hub, and it was cleared in exactly ONE place —
             * as_fail. A hub-downstream enumeration that SUCCEEDED left it set, and this arm did not clear it,
             * so the next ROOT-port device was enumerated as though it were behind the hub: the run read the
             * hub's downstream port (stale gAs.hubPort), found that port already owned by a live slot, took
             * `goto h3_next_port` — which clears gAs.running but touches neither gSelfEnumPort, gSelfEnumDone
             * nor gSelfEnumTries — and as_tick armed the identical run again. Unbounded, ~3 log lines per pass,
             * and every one of those is an FSWrite + FlushVol at task level, so the File Manager saturated and
             * the machine showed a wristwatch cursor as soon as the user asked the Finder to do anything.
             * ⇒ On the h14 run this is BOTH reported symptoms at once: drive 4 never mounted (its enumeration
             * was being aimed at the hub) and dragging the other three to the Trash appeared to hang.
             * ★ WHY IT NEEDED h14 TO SURFACE: the arm only runs when a slot is free. At USB_MAX_DEV = 2 a
             * root-port connect after two hub drives always hit "no free slot" and never armed, so the stale
             * switch was never acted on. Raising the limit to 4 made a pre-existing latent bug REACHABLE — it
             * was not introduced by h14. Same family as every other defect here: per-operation state that is
             * correct, and not reset at one transition. */
            gAs.hubAddr = 0; gAs.hubPort = 0;
        } else {
            gSecondDevMask |= (1UL << (gSelfEnumPort & 0x0F));   /* task level logs it */
            gSelfEnumDone = 1;                                   /* stop re-arming until a slot frees */
        }
    }
    /* ★★★★ h3 TIER 1: arm the next DOWNSTREAM enumeration behind the claimed hub.
     * Same shape as the root-port arm above, and deliberately after it so a root-port device never waits on a
     * hub walk. One port at a time is not just simpler, it is REQUIRED: a freshly reset downstream port is
     * enabled and its device answers at address 0, so two ports enabled at once would both hear address-0
     * traffic. The serial walk is what keeps that safe. */
    /* ★★★★ h9: DO NOT ARM THE NEXT PORT WHILE A TASK-LEVEL HANDOFF IS PENDING.
     * gAsProbeOK and gAsNeedBulk are parks that selfprobe_tick completes AT TASK LEVEL, and both read
     * gEnumDev to know WHICH slot they are finishing. Arming the next sweep port overwrites gEnumDev.
     * The engine runs on the 8 ms heartbeat and selfprobe_tick only when the app polls (~10/sec), so the
     * sweep reliably won the race: h8's log shows SELFPROBE COMPLETE followed immediately by the next
     * enumeration, and the publish/AddDrive for the device that had just probed never happened. Both drives
     * enumerated, both recorded geometry, neither was ever EXPOSED.
     * h6 never hit this because it SKIPPED ports that already held a device, so it armed rarely. h7 made the
     * sweep visit every port every pass — correct in itself, and it closed a window the handoff had been
     * quietly relying on. */
    else if (!gAs.running && !gAsProbeOK && !gAsNeedBulk &&
             gHub.claimed && gHub.rootPort >= 0 && !gPortParked[gHub.rootPort]) {
        /* ★★★★ h4: REPEATING sweep of the claimed hub's ports. h3 swept once and missed both drives because
         * they had not finished powering up 120 ms after PORT_POWER. Sweeping forever, with a gap, is both the
         * fix for that and the fix for hot-plug behind the hub — a later arrival is just seen on a later pass.
         * A port that ALREADY has a device is skipped, so a mounted drive is never re-enumerated out from
         * under the File Manager. */
        /* Sweep bookkeeping. scanPort > nPorts means a sweep just finished, so arm the gap and go idle (0);
         * scanPort == 0 means we are waiting out that gap. Doing it HERE, in one place, is deliberate: several
         * skip paths advance scanPort and none of them should have to remember to arm the gap. Without this
         * the sweep restarts on the very next 8 ms heartbeat and hammers the hub with control traffic, which
         * would starve a mounted drive's I/O — the documented ~30 s File Manager patience problem. */
        /* ★★★★ h10: THE SWEEP IS NO LONGER ON A TIMER. Consume whatever the hub has told us, then visit
         * only the ports it flagged. When nothing has changed, hub_int_poll() is a single memory load and we
         * return having touched the bus not at all — which is the property h6 had by accident, h7 destroyed,
         * and this design makes explicit. */
        {   UInt32 bits = hub_int_poll();
            if (bits) {
                /* bit 0 = the hub itself, bits 1..N = port N. Keep port numbers as bit positions. */
                gHub.changeMask |= (bits == 0xFFFFFFFFUL)
                                     ? ((1UL << (gHub.nPorts + 1)) - 2UL)
                                     : (bits & ~1UL);
                hub_int_arm();                       /* re-park at once so no change is missed */
            }
        }
        if (!gHub.changeMask) { gAsBusy = 0; return; }     /* nothing changed — this is the common case */
        {   int p;
            for (p = 1; p <= gHub.nPorts; p++) if (gHub.changeMask & (1UL << p)) break;
            if (p > gHub.nPorts) { gHub.changeMask = 0; gAsBusy = 0; return; }
            gHub.changeMask &= ~(1UL << p);          /* consume it; the hub will tell us again if need be */
            gHub.scanPort = p;
        }
        gAs.dev = -1;            /* h9: gEnumDev is deliberately NOT touched here — it still names the slot
                                  * whose handoff may be in flight, and nothing in the port QUERY needs it.
                                  * It is set when a slot is actually allocated, below. */
        gAs.pc = 0; gAs.running = 1; gAs.done = 0; gAs.failed = 0;
        if (!gExposureDeferred) gAsProbeOK = 0;   /* h69: same protection on the hub-sweep arm */
        gAs.port = gHub.rootPort;      /* still our root port for logging/ownership purposes */
        gAs.hubAddr = gHub.addr;       /* ★ the switch that makes reset go via the hub */
        gAs.hubPort = gHub.scanPort;
    }
    /* f4: the overlap itself, independent of whether it did any harm — the enumeration engine is about to
     * advance while a BLOCK transfer is live in hardware. If this reads 0 across the m24 topology then the
     * two never coincide and F4 is refuted outright, whatever the other counters say. */
    if (gAs.running && gDpBusy && gF4Owner == F4_OWNER_BIO) gF4EnumArmedWhileBio++;
    if (gAs.running) as_advance();
    gAsBusy = 0;
}
#endif /* APPLE_HIDE */

/* ★★★★★★ h26 APP-LESS: the task-level pump, and the counters that say whether it is being starved.
 *
 * THE DESIGN, and it is Apple's own, not ours: `NMInstall` with **`nmStr = 0`** displays nothing and exists
 * solely to get `nmResp` called at task level, inside whatever process is running its event loop (normally
 * the Finder). Established by the RE of `USBMassStorageSupport` (`docs/APPLE-UMSS-RE.md`): their `nmResp` at
 * code 0x01e00 does `GetVCBQHdr` → `UnmountVol` → `Eject`, which is very nearly line-for-line our
 * `gSelfEnumRearm` block. `NMInstall` only ENQUEUES, so it is legal from where our detection happens
 * (interrupt level), and we already import `NMInstall`/`NMRemove`.
 *
 * ★★ WHY THIS IS NOW BUILDABLE — all four recorded caveats of that RE are closed:
 *   1. `USLPolledProcessDoneQueue` is **not needed at all** — n4h ran the whole stack with it compiled out,
 *      and n4i also dropped `ExpertIdleTask`. Both passed. So there is nothing left to POLL, which was the
 *      one thing `NMInstall` could not have supplied.
 *   2. `ase_quiesce` inside an `nmResp` is **measured and bounded**: 10 ms by construction, worst observed
 *      0.47-0.80 ms across three runs. That was the single place our needs exceed Apple's.
 *   3. Our trigger stays our own interrupt-level detection (Apple's `USBInstallDeviceNotification` can never
 *      fire for our devices — we hide the ports).
 *   4. "App-less" means **no app of OURS**. Some process must still run an event loop for the NM queue; the
 *      Finder always does. Unchanged from the `_SystemTask` sketch this replaces, but say it out loud.
 *
 * ★ WHY THE RESPONSE CALLS `selfprobe_tick` WHOLE rather than a factored-out subset. The four things needing
 * task level are parks the interrupt engine sets — `gAsNeedBulk`, `gAsProbeOK`, `gSelfEnumRearm`,
 * `gHubIntNeedArm` — and `selfprobe_tick` is exactly the function that drains them. Re-using it means the
 * task-level path an app drove for 130 builds is byte-for-byte the path the NM now drives, so a pass here is
 * evidence about the VEHICLE and not about a fresh copy of the logic. Splitting the function is a refactor
 * worth doing on its own terms, not while changing the thing that calls it.
 *
 * ⚠ A SEPARATE NMRec FROM THE USER-VISIBLE ALERTS. `gNM` carries the eject / reconnect notifications; if the
 * pump shared it, a mount could retract an alert the user is still reading, or vice versa. `gNMT` is ours.
 *
 * ★ THE COUNTERS ANSWER THE OPEN n4i QUESTION. n4i left two 300 ms same-qTD stalls whose cause could not be
 * separated: a flash write-latency spike, or a starved task-level pump. `gTickGap*` measures the gap between
 * consecutive task-level ticks and `gNmLat*` the delay from arming a request to `nmResp` actually running.
 * Under load, a ~300 ms figure in either supports starvation; a few ms in both leaves the device as the
 * explanation. This is the discriminator that run could not carry because there was no clock in the log. */
static volatile UInt32 gTickCalls = 0, gTickGapLast = 0, gTickGapMax = 0, gTickLastExit = 0;
/* h59: gExposureDeferred moved up beside gExposeDev so as_tick can read it. */
static volatile UInt32 gNmArmed = 0, gNmFired = 0, gNmFailed = 0, gNmDeclined = 0;
static volatile UInt32 gNmStuckRearms = 0;        /* h63: lost NM requests recovered by the watchdog */
static volatile UInt32 gTickSeenLast = 0, gTickSeenMs = 0;
static volatile UInt32 gTicksStallMaxMs = 0;      /* h64: longest observed freeze of lowmem Ticks, in ms */
static volatile UInt32 gHbRuns = 0;              /* h65: heartbeat callbacks, vs gVhubTick = SIH passes */
/* ★★★ h66 — THE PORT-POWER / OVER-CURRENT WATCH. Per port: last PORTSC seen, POWER 1->0 transitions, and
 * over-current sightings. Sampled on every SIH pass and BEFORE service_ports, so an OCC latch is recorded
 * before our own read-modify-write clears it. */
static volatile UInt32 gPwPrev[16];
static volatile UInt32 gPwLost[16], gOcSeen[16];
static volatile int    gPwInit = 0;
static volatile UInt32 gRepower[16];             /* h67: re-power attempts per port, hard-capped */
#define H67_MAX_REPOWER 3UL
static volatile UInt32 gNmLatLast = 0, gNmLatMax = 0;   /* ⚠ MILLISECONDS (frame_ms), NOT ticks — see h83 */
static volatile UInt32 gNmArmTick = 0;            /* frame_ms() when the pending request was posted. ⚠ The
                                                   * comment here said "lowmem Ticks" for eleven builds after
                                                   * h64 changed it to frame_ms; that stale word is what made
                                                   * a reader divide the latency by 60 and report a 50 s stall
                                                   * that was really 3 s. Units belong next to the variable. */
/* ★★★★★★★ h83 — THE STUCK-REQUEST WATCHDOG HAS BEEN DEAD SINCE h65, AND IT IS DEAD BY A UNIT MISMATCH.
 *
 * h63's check is `(long)(nowK - gNmArmTick) < NM_STUCK_TICKS`. h65 then changed `nowK` to **gVhubTick**
 * (SIH passes, ~125/s) and NM_STUCK_TICKS to "7500 SIH passes", but left `gNmArmTick` as **frame_ms()**
 * (milliseconds, 1000/s). Two clocks, two origins, one subtraction.
 *
 * frame_ms outruns gVhubTick by ~8x from the first second, so `nowK - gNmArmTick` is hugely NEGATIVE for
 * the whole session, the compare is always true, and task_work_arm RETURNS ON ITS FIRST LINE forever once
 * gNMTPosted is set. That is precisely the permanent-death case h63 was written to recover from — quoted in
 * its own comment: "this function returns on its first line for the rest of the session and the app-less
 * task-level pump is dead — permanently, with no counter to say so."
 *
 * ★ CONFIRMED BY DATA, not inferred: gNmStuckRearms is 0 in EVERY run ever logged, including the m27 and
 * 2026-08-12 hangs where a lost request is the leading explanation. A watchdog that has never once fired
 * across a hundred builds is not a quiet watchdog; it is a broken one.
 *
 * ★ THE FIX IS TO STAMP THE ARM IN THE SAME CLOCK THE CHECK READS. gNmArmTick stays frame_ms so the
 * LATENCY instrument is untouched — the two uses wanted different clocks and were sharing one variable. */
static volatile UInt32 gNmArmTickK = 0;           /* gVhubTick at the arm — the h63 check's OWN clock */
/* ★★★★★★★ h84 — THE PUMP BODY'S DURATION HAS NEVER BEEN MEASURED, AND IT IS THE QUANTITY THAT DECIDES
 * WHETHER THE MACHINE SURVIVES.
 *
 * m32/h83 settled what the pump does: at all three paints ARMED == FIRED with POSTED == 1, which can only
 * mean we were INSIDE the response and it had been running longer than the watchdog's 5 s trigger. The
 * 2026-08-12 hang is the same picture taken further along — the h76 dump read gTaskPumpN 0x0a, so we were
 * inside body #10 when the second AddDrive happened, and it never reached 11. The response borrows the
 * FINDER'S OWN THREAD. A body that does not return IS a wedged desktop, a wristwatch cursor and a frozen
 * cyan number: one cause, every symptom.
 *
 * ⚠ AND NOTHING WE HAVE CAN SEE IT. task_work_arm returns on its first line while gNMTPosted is set, so the
 * next arm can only happen AFTER the body ends. gNmLat (arm -> response) therefore measures the GAP;
 * gTickGap (body exit -> next entry) measures the GAP by construction. Two instruments, same blind spot.
 * Arithmetic across m32's paints (4315 SIH passes, ~34 s, for TWO pumps, gaps capped at 7.2 s) puts the
 * bodies at 10 s or more during enumeration — inferred, never measured.
 *
 * ★ A HIGH-WATER MARK ALONE WOULD BE USELESS HERE, and that is the trap worth naming: it is written at body
 * EXIT, and the failure is a body that never exits. So the load-bearing fields are the LIVE ones —
 * gBodyPhase (where we are) and gBodyStartMs (since when) — which the paint reads at interrupt level and
 * turns into "stuck in section N for M ms". The completed-body maxima are the supporting cast.
 *
 * ★ CLOCK: frame_ms(). It is driven by frame_time_update() in the heartbeat, it demonstrably kept advancing
 * through the hang (the h61 traces carry ms values), and it is the clock every other timeout here trusts.
 * If it ever DID stop, green gVhubTick would still climb while the elapsed sat still — itself readable.
 *
 * ⚠ COST: one UInt32 subtract and two stores per checkpoint, no logging, no allocation. That matters more
 * than usual, because an instrument that lengthens the body is measuring itself (I10). */
#define BP_N            16u
#define BP_IDLE          0u   /* not inside the body at all */
#define BP_ENTRY         1u   /* entry + compl_drain */
#define BP_LOGDRAIN      2u   /* h78 bulk ring drain (160 lines, FSWrite + FlushVol each) */
#define BP_LATEDUMP      3u   /* h41 late dump + f4 verdict */
#define BP_PERIODIC      4u   /* the ~40-line periodic dump — the biggest known File Manager block */
#define BP_NEEDBULK      5u   /* gAsNeedBulk: create the bulk endpoints */
#define BP_HUBARM        6u   /* gHubIntNeedArm: program + park the hub status QH */
#define BP_EXPOSE        7u   /* gAsProbeOK: THE SUSPECT — install, AddDrive, diskEvt */
#define BP_REARM         8u   /* gSelfEnumRearm: unmount + reconnect_reset */
#define BP_DRAINS        9u   /* the mask drains (n15/n13/h15/n21/h7/h3/n24/h53/n4b) */
#define BP_TAIL         10u   /* as_tick + the legacy tail */
/* ★★★★ h84b — SPLIT THE EXPOSURE, because the prior art makes a specific prediction and a lumped section 07
 * could not test it. `docs/APPLE-UMSS-RE.md` establishes that Apple's block driver "imports no File Manager,
 * no Notification Manager and no dialog APIs" and issues AddDrive + PostEvent from its OWN DoDriverIO, while
 * Apple's nmResp does only GetVCBQHdr -> UnmountVol -> Eject. We do AddDrive from inside the nmResp AND wrap
 * it in ~30 lines of synchronous FSWrite + FlushVol on the boot volume (h41 before, h76, h41 after).
 * ⇒ PREDICTION: the cost is OUR LOGGING around the handover, not the handover. h78 already caught this exact
 * pattern once (the DoDriverIO drain) and cut it; these dumps were never cut because they are what made the
 * exposure legible in the first place. Four extra checkpoints settle it in the SAME run instead of the next
 * one — and if 0C is the slow one instead, the prediction is wrong and the OS's mount path is the problem. */
#define BP_EXP_PRE      11u   /* h41 BEFORE dump — ours */
#define BP_EXP_ADD      12u   /* install_block_driver: InstallDriverFromMemory + AddDrive + diskEvt — the OS */
#define BP_EXP_POST     13u   /* n19 line + the h76 dump — ours */
#define BP_EXP_AFTER    14u   /* h41 AFTER dump — ours */
static volatile UInt8  gBodyPhase   = BP_IDLE;
static volatile UInt32 gBodyStartMs = 0;      /* frame_ms at body ENTRY — live, the one that matters */
static volatile UInt32 gPhaseAtMs   = 0;      /* frame_ms when the CURRENT phase began */
static volatile UInt32 gBodyLastMs  = 0, gBodyMaxMs = 0;   /* COMPLETED bodies only */
static volatile UInt32 gBodySlowN   = 0;      /* completed bodies over BODY_SLOW_MS — frequency without a photo */
#define BODY_SLOW_MS  1000UL
static volatile UInt32 gPhaseMaxMs[BP_N];     /* per-phase high-water, so "which section" is answerable */
static void bphase(UInt8 p)
{
    UInt32 now = frame_ms();
    UInt8  prev = gBodyPhase;
    if (prev < BP_N) {
        UInt32 d = now - gPhaseAtMs;
        if (d > gPhaseMaxMs[prev]) gPhaseMaxMs[prev] = d;
    }
    gPhaseAtMs = now;
    gBodyPhase = p;
}
static NMRec   gNMT;                              /* the PUMP's own request — never gNM, see above */
static NMUPP   gNMTUpp   = 0;
static volatile int gNMTPosted = 0;
/* ★ h27: a COUNTER, not a flag. The body can legitimately nest (it calls into Apple's USL via compl_drain,
 * and into the File Manager), and a flag would be cleared by an inner exit while the outer call is still
 * inside — leaving the NM response free to run in the middle of it. Depth counts nest correctly. */
static volatile int gInTaskWork = 0;              /* >0 = a tick is somewhere on the stack */

/* Is there task-level work parked? The exact set of parks selfprobe_tick drains.
 * ★ h30: a DEFERRED exposure does not count — otherwise the pending gAsProbeOK would re-arm the NM at
 * event-loop rate for the whole defer window (hundreds of no-display notifications during Finder startup).
 * The ~2 s keepalive owns the re-check; worst case the mount lands one keepalive after the Finder settles. */
/* ★★★★★★ h41 — MEASURE, DO NOT PATCH. Who owns a drive-queue entry for this medium, and when?
 *
 * ⚠ h40 restored CONFIGFLAG microseconds after HCReset to deny Apple an enumeration window, and the run came
 * back LINE-FOR-LINE identical to h39: same scan, same 2.0 mount, same "initialize" prompt. So the window
 * theory is unsupported, and that is the second patch aimed at a mechanism I cannot observe. Worse, EVERY T4
 * log ends at "device EXPOSED" — there has never been a single line of evidence about the mount itself.
 *
 * Two possibilities remain and they call for opposite fixes:
 *   (A) APPLE'S STALE ENTRY — Apple enumerated the drive at some point, registered a drive-queue entry, and we
 *       took the device back. The prompt is Apple's orphan. Fix = keep Apple away from the drive entirely.
 *   (B) OUR OWN BAD EXPOSURE — our scan/geometry produces a volume the File Manager cannot read. The prompt is
 *       about OUR entry. Fix = the scan, and Apple is innocent.
 * Both produce "prompt, then Eject, then our 2.0 mount", so no amount of re-reading the existing logs
 * separates them. ONE dump does: walk the drive queue and the VCB queue immediately BEFORE we install, and
 * again AFTER. A foreign drive-queue entry present BEFORE our install proves (A); nothing foreign before, and
 * only our own entry after, proves (B).
 * ★ Cheap, always-printed, task level (both queues are System Globals lists), and it answers the question the
 * "USB Disk Log" would have answered — that file has never appeared in any app-less run, so it is not a
 * reliable channel here and this does not depend on it. */
static void h41_queue_dump(const char *when)
{
    QHdrPtr dq = GetDrvQHdr();
    QHdrPtr vq = GetVCBQHdr();
    void *e; int n;
    ehci_os_log(when);
    for (e = (void *)(dq ? dq->qHead : 0), n = 0; e && n < 12; e = (void *)((DrvQElPtr)e)->qLink, n++) {
        DrvQElPtr d = (DrvQElPtr)e;
        /* drive number and the DRIVER refNum that owns it — ours is gBlkDref once installed, so anything
         * else on this medium is Apple's. Negative refNums are unit-table drivers (ours reads 0xffffffc8-ish). */
        ehci_os_logx("  h41 drvq: drive<<16|refNum", (((UInt32)(UInt16)d->dQDrive) << 16)
                                                    | ((UInt32)(UInt16)d->dQRefNum));
        ehci_os_logx("  h41       fsid<<16|qType",   (((UInt32)(UInt16)d->dQFSID) << 16)
                                                    | ((UInt32)(UInt16)d->qType));
    }
    ehci_os_logx("  h41 drive-queue entries", (UInt32)n);
    for (e = (void *)(vq ? vq->qHead : 0), n = 0; e && n < 12; e = (void *)((VCB *)e)->qLink, n++) {
        VCB *v = (VCB *)e;
        ehci_os_logx("  h41 vcb: drvNum<<16|drvRefNum", (((UInt32)(UInt16)v->vcbDrvNum) << 16)
                                                       | ((UInt32)(UInt16)v->vcbDRefNum));
    }
    ehci_os_logx("  h41 mounted volumes", (UInt32)n);
    ehci_os_logx("  h41 our block driver refNum (0 = not installed yet)", (UInt32)(UInt16)gBlkDref);
}

static int task_work_pending(void)
{
    /* h35: gAsProbeOK is excluded while its exposure is deferred, so the keepalive (~2 s) owns the re-check
     * during the Finder-settle window rather than the NM re-arming at event-loop rate — same reasoning h30
     * used, restored with the split's revert. */
    return (gAsNeedBulk || (gAsProbeOK && !gExposureDeferred)
            || gSelfEnumRearm || gHubIntNeedArm) ? 1 : 0;
}

/* ★★★★★★ h27 — THE h26 FREEZE, AND IT WAS MINE. Run A (h26 + n4g, 2026-08-08) froze the activator during a
 * drive insertion, right after AddDrive/diskEvt. Two coupled defects here, both from one wrong assumption:
 *
 * ⚠ **h26 asserted "the NM has dequeued us by the time the response runs; NMRemove here would be wrong."
 * THAT IS FALSE**, and this codebase already contained the proof both ways:
 *   - the alert posters set `nmResp = (NMUPP)-1L`, whose comment reads "the NM dequeues it once the user
 *     dismisses" — the magic -1 is what buys auto-dequeue;
 *   - `resident/bootmain.c`'s own NM response, with a REAL UPP like ours, calls `NMRemove(nmReqPtr)` and its
 *     comment says "NMRemove's self".
 * With a real UPP the request STAYS QUEUED until removed. h26 never removed it.
 *
 * ★★ THE FREEZE MECHANISM: h26 also cleared `gNMTPosted` at the TOP of the response, so while this function
 * was still doing its (long, File-Manager-heavy) work, the 8 ms heartbeat could see a park pending and call
 * `NMInstall(&gNMT)` on a record that was **still in the Notification Manager's queue**. An OS queue element
 * installed twice links through its own `qLink` — a cycle — and the NM's queue walk then never terminates.
 * Task level dies; interrupt level carries on, which is exactly what the log shows: it stops mid-body with
 * the engine having been healthy a line earlier.
 * ★ Why the earlier part of the run was clean, and why the counters misled me at first: the last periodic dump
 * before the insertion read `gNmArmed/gNmFired = 0`, because with a fast app tick no park survives long enough
 * at heartbeat time to arm anything. The arming window only opens during an INSERTION — which is precisely
 * where it froze. A counter snapshot from before the window says nothing about the window.
 *
 * THE THREE FIXES:
 *   1. `NMRemove` FIRST, before anything else, so the record is legally re-installable from that instant.
 *   2. `gNMTPosted` stays SET for the whole of the work and is cleared only at the end, so an arm can never
 *      overlap a response in progress. A park that survives is picked up by the next heartbeat.
 *   3. The re-entrancy guard moved OFF the app path (see the wrapper) and INTO this response, so the path an
 *      app has driven for 130 builds keeps h25 semantics byte for byte. h26 changed a validated path for a
 *      benefit only the new path needed. */
static pascal void ehci_nm_task_resp(NMRecPtr r)
{
    UInt32 now = frame_ms();               /* h64: same clock as gNmArmTick, so the latency stays meaningful
                                            * even when VBL (and therefore lowmem Ticks) has stopped. */
    if (r) (void)NMRemove(r);              /* h27 fix 1: dequeue BEFORE any work */
    gNmFired++;
    gNmLatLast = now - gNmArmTick;
    if (gNmLatLast > gNmLatMax) gNmLatMax = gNmLatLast;
    /* h27 fix 3: never run the body nested inside a tick that is already in progress. */
    if (gInTaskWork) { gNmDeclined++; gNMTPosted = 0; return; }
    /* ★ THE WHOLE POINT: task level, in some application's event loop, with no application of ours. */
    ehci_vhub_selfprobe_tick();
    gNMTPosted = 0;                        /* h27 fix 2: only NOW may the heartbeat arm a fresh request */
}

/* Post a request if work is parked and one is not already outstanding. Called from ehci_vhub_service, i.e.
 * interrupt level, so a park is noticed within one 8 ms heartbeat. NMInstall only enqueues — that is
 * precisely why Apple's design puts it here. */
/* ★★★★★★ h28 — ARM ON A KEEPALIVE TOO, NOT ONLY ON PARKED WORK. Two reasons, one of them a diagnostic
 * failure of my own making.
 *
 * ⚠ THE BLIND SPOT. Run B (h27 + n4j) produced a 73-line log that stopped at "pump armed" and said nothing
 * else: `gTickCalls` 0, no PORTMAP, no counters. Three causes were possible — the engine never started, it
 * parked but `task_work_arm` never posted, or `NMInstall` posted and the NM never called back — and **none of
 * them could be told apart, because `gNmArmed`/`gNmFired`/`gNmFailed` print only in the periodic dump, the
 * periodic dump runs only from a tick, and a tick is exactly what was missing.** The instrument was reachable
 * only through the thing being measured. That is the h24 chicken-and-egg again, and I built it twice.
 * ⇒ A keepalive makes the pump SELF-REPORTING: with no app at all, the counters and the interrupt-log ring
 * reach the file on their own, so a failed run explains itself instead of needing a manual flush afterwards.
 *
 * ★ AND IT IS BETTER ENGINEERING INDEPENDENTLY. Event-only arming makes the whole task-level path depend on
 * the park-detection predicate being exactly right; a park nobody notices is a mount that never happens, with
 * no trace. The keepalive is the backstop: worst case, work waits one interval instead of forever.
 *
 * ⚠ Cost is small and bounded: one no-display NM request every ~2 s. The response is a single `selfprobe_tick`,
 * which is strictly less work than the continuous ticking an application did — and the periodic dump inside it
 * is rate-limited by CALL count (every 64), so at 2 s per call that is one dump every ~2 minutes, not a flood. */
#define NM_KEEPALIVE_TICKS  250UL                  /* h65: SIH passes. ~2 s at the 8 ms heartbeat, and
                                                    * sooner when the real IRQ is also driving it. */
/* h63: how long a posted NM request may go unanswered before we treat it as LOST and re-arm. Deliberately far
 * beyond any latency ever observed (phase A recorded a response arriving 1829 ticks / 30.5 s late), because
 * re-arming while one is genuinely still queued would double-post. */
#define NM_STUCK_TICKS      7500UL                 /* h65: SIH passes, ~60 s at 8 ms */
static volatile UInt32 gNmKeepDue = 0, gNmKeepArms = 0;

static void task_work_arm(void)
{
    /* ★★★★★★★ h64 — THE KEEPALIVE'S CLOCK WAS lowmem Ticks, AND lowmem Ticks IS DRIVEN BY VBL.
     *
     * ⚠ m15 refuted the h63 theory and pointed straight here. Its counters: gNmArmed/gNmFired = 1/1 with a
     * latency of 49 ticks, i.e. the request was armed once, ANSWERED PROMPTLY, and then nothing ever armed
     * again — gNmStuckRearms 0, so it was never "lost". task_work_arm runs on the 8 ms heartbeat and cannot
     * be starved, and gNMTPosted was clear. The ONLY remaining way to not re-arm is for `keep` never to go
     * true — and `keep` was computed from lowmem Ticks (0x016A), which the VBL interrupt increments.
     *
     * ★★ THIS MACHINE HAS A KNOWN, ONLY PARTIALLY FIXED VBL BUG. It is the whole subject of the sibling VBL
     * project; the user keeps a VBLFix app on the desktop for it and hit a freeze during the m13 run. When VBL
     * dies, Ticks stops — so a cadence keyed off Ticks stops with it, permanently and silently.
     * ⇒ THE COLD-BOOT USB MOUNT FAILURE AND THE VBL BUG ARE THE SAME BUG. It also explains why HOT-PLUG ALWAYS
     * WORKS: by then the desktop has settled (or the user has run VBLFix), Ticks advances, and the pump runs.
     *
     * ★ THE FIX: use frame_ms(), the driver's own clock, derived from the controller's FRINDEX. It advances
     * for as long as the EHCI controller is running and is independent of VBL, the Time Manager and task
     * level. Every control and BOT timeout in this driver already trusts it; the app-less pump was the one
     * liveness path still keyed to a clock the machine cannot guarantee.
     * ⚠ TICKS_NOW is still used by the down-engine stall watchdog (r54). That is a BACKSTOP, not a liveness
     * path — a frozen Ticks makes it late, not fatal — so it is deliberately left alone rather than swept up
     * in this change. */
    /* ★★★★★★★ h65 — STOP GUESSING WHICH CLOCK DIED. DRIVE THE CADENCE OFF THE ONE THING DEMONSTRABLY ALIVE.
     *
     * ⚠ TWO CLOCK THEORIES HAVE NOW BEEN REFUTED BY THEIR OWN INSTRUMENTATION, which is the process working,
     * but it is also the signal to stop substituting clocks one at a time:
     *   h63 said the NM request was LOST -> refuted: m15 read gNmArmed/gNmFired 1/1, answered in 0.8 s,
     *       gNmStuckRearms 0.
     *   h64 said lowmem Ticks froze because VBL died -> refuted: m16 read gTicksStallMaxMs = 16 ms. VBL fine.
     * Both runs still ended with `callN 1` -- armed once, answered, never re-armed.
     *
     * ★ WHAT IS LEFT, AND IT IS NARROW: task_work_arm runs from ehci_vhub_service on every SIH pass and
     * gNMTPosted was clear, so the ONLY way not to re-arm is `keep` never going true -- i.e. whatever clock
     * the cadence reads is not advancing. Ticks is fine. That leaves frame_ms(), which is advanced ONLY by
     * frame_time_update() inside vhub_heartbeat -- the Time Manager one-shot. The SIH has a SECOND driver (the
     * real EHCI IRQ), which is why port events keep flowing and gVhubTick keeps climbing even if the heartbeat
     * has stopped re-arming itself. That asymmetry fits every run we have.
     *
     * ★★ SO USE gVhubTick: it is incremented on every SIH pass, and the SIH is fed by TWO independent sources.
     * It is the most robust liveness signal in the driver, and unlike a wall clock it cannot be stopped by one
     * subsystem failing. A cadence of SIH passes is not a precise 2 s, and it does not need to be -- this is a
     * keepalive, not a deadline.
     * ⚠ It also removes the LAST dependency of the app-less pump on anything outside the SIH, which is the
     * point: the pump is a liveness path, and a liveness path must not be able to outlive its own clock. */
    UInt32 nowK = gVhubTick;
    int    keep = ((long)(nowK - gNmKeepDue) >= 0);
    /* ★★★★★★★ h63 — THE KEEPALIVE IS A ONE-IN-FLIGHT CHAIN, AND ITS ONLY RECOVERY WAS THE RESPONSE FIRING.
     *
     * ⚠ gNMTPosted is set when NMInstall succeeds and cleared ONLY inside the NM response handler. So if that
     * response is never delivered, this function returns on its first line for the rest of the session and the
     * app-less task-level pump is dead — permanently, with no counter to say so.
     *
     * ★ MEASURED, m14 2026-08-11: `callN 0x00000001`. The pump ran EXACTLY ONCE in a whole session. That one
     * fact explained everything the run showed — the deferred exposure never re-evaluated, the 45 s cap never
     * fired, the log stopped draining, and dismissing the dialog changed nothing because nothing was left
     * running to notice it.
     * ★★ THE TRIGGER IS ENVIRONMENTAL AND NOT OURS TO PREVENT: an AppleShare alias in System Folder/Servers
     * makes the OS block task level at boot while it tries to reach the server. The user measured ~15 s with
     * the server absent and ~30 s with the Keychain prompt up, and during that window APPLE'S OWN HIDs are
     * frozen too — this is the whole system's task level, not our driver. Phase A's gTickGapMax of 1855 ticks
     * (30.9 s) was the same thing seen from the other side.
     * ⇒ WE CANNOT STOP THE BLOCK. WE CAN SURVIVE IT. A user must be able to boot with Ethernet connected;
     * that is a normal configuration, not one to design around.
     *
     * ★ THE WATCHDOG: if a request has been posted far longer than any latency ever observed, treat it as lost,
     * NMRemove it and re-arm. 3600 ticks (60 s) is deliberately generous — Phase A saw a response arrive 1829
     * ticks (30.5 s) late, and re-arming while one is genuinely still queued would double-post.
     * ⚠ This can only ever ADD an arm, never remove one, so it cannot make the pump worse than it is. */
    if (gNMTPosted) {
        /* h83: gNmArmTickK, not gNmArmTick. Same clock as nowK and as NM_STUCK_TICKS — see the block by the
         * declaration. This comparison has been reading milliseconds against SIH passes since h65, which is
         * why gNmStuckRearms has been 0 in every run: the watchdog could never reach its own body. */
        if ((long)(nowK - gNmArmTickK) < (long)NM_STUCK_TICKS) return;
        (void)NMRemove(&gNMT);                 /* drop the lost request before posting a replacement */
        gNMTPosted = 0; gNmStuckRearms++;
    }
    if (!task_work_pending() && !keep) return;
    if (!gNMTUpp) return;                          /* UPP build failed at init; the app tick still works */
    if (keep) { gNmKeepDue = nowK + NM_KEEPALIVE_TICKS; gNmKeepArms++; }
    gNMT.qType   = nmType;
    gNMT.nmMark  = 0;                              /* no Application-menu mark */
    gNMT.nmIcon  = 0;
    gNMT.nmSound = 0;
    gNMT.nmStr   = 0;                              /* ★ nmStr = 0: displays NOTHING. This is the trampoline */
    gNMT.nmResp  = gNMTUpp;
    gNMT.nmRefCon = 0;
    gNmArmTick  = frame_ms();     /* h64: the LATENCY clock — ms, matches ehci_nm_task_resp's frame_ms() */
    gNmArmTickK = nowK;           /* h83: the WATCHDOG clock — gVhubTick, matches NM_STUCK_TICKS */
    if (NMInstall(&gNMT) == noErr) { gNMTPosted = 1; gNmArmed++; }
    else                            gNmFailed++;
}

/* ★★★★★ h31 — SWEEP COMPANION-OWNED PORTS AT BRING-UP, because our own HCReset just gave them away.
 *
 * ⚠ THE h30 T2 RESULT (2026-08-09): boot with a drive on the card → it mounted at USB 1.1, Apple's stack.
 * The INIT's early claim WORKED — its own log shows the port going 0x2800 (companion) → 0x1803 (ours, CCS=1,
 * drive visible). Then activation ran ehci_hc_reset, and **HCReset resets CONFIGFLAG to 0**, routing every
 * port to the companions for the bring-up window. Apple's hot stack took the connected drive, and when
 * hc_start set CONFIGFLAG=1 milliseconds later this NEC chip did NOT evict the active companion connection —
 * the first PORTMAP reads 0x3800 (owner=1) and we are blind there (CCS reads 0 under owner=1). Apple mounted
 * it at desktop time, at 1.1. The early claim was real; OUR OWN RESET un-did it nine seconds later.
 *
 * ★ THE EVICTION WRITE IS HARDWARE-PROVEN SAFE-AND-EFFECTIVE on this chip: the INIT claim's owner=0 write is
 * exactly what produced 0x2800→0x1803 against an ACTIVE companion connection. n11's scar was clearing Port
 * ENABLE; owner=0 with power preserved is the claim this driver has always made at CONFIGFLAG time.
 * ★ Timing makes the yank clean: at bring-up Apple cannot have MOUNTED anything (its boot-attached mounts
 * defer to the desktop, per the user's own observation), so an evicted device was enumerated-at-most, never
 * a live volume. An FS/LS device swept here fails to chirp and is ceded straight back — the proven path.
 * ★ The chip AUTO-REVERTS ownership on disconnect (observed this run: 0x3800→0x1000 on unplug with no PORTSC
 * write of ours) — so this sweep is only needed for devices CONNECTED ACROSS bring-up, which is exactly the
 * cold-boot-attached case T2 tests. */
/* h43: public wrapper so uimInitialize can take the earliest snapshot (h41_queue_dump is static). */
void ehci_vhub_queue_dump_public(const char *when) { h41_queue_dump(when); }

void ehci_vhub_bringup_owner_sweep(void)
{
    int p, np = (int)gSoftc.nPorts, swept = 0;
    if (!gSoftc.opBase || np <= 0 || np > 15) return;
    for (p = 0; p < np; p++) {
        UInt32 v = ehci_read32(gSoftc.opBase, EHCI_PORTSC(p));
        if (v & EHCI_PORT_OWNER) {
            /* ★★★★ h33 — NEVER EVICT A LOW-SPEED DEVICE. K-state on the line (bits 11:10 = 01) means a
             * low-speed device is attached, and we can NEVER drive one on a root port (EHCI has no root-port
             * splits) — evicting it just kills a working keyboard until the cede wanders back, or strands it
             * outright (the T4-on-v4 dead keyboard, port read 0x3400 = K). The line state reads through
             * Owner=1. ⚠ The companion may be actively DRIVING the device, and line state toggles during a
             * transfer — an LS transfer lasts long enough that consecutive µs-reads can all land inside one
             * packet reading J. So sample ACROSS a window longer than any LS transfer: 16 samples spaced
             * ~100 µs; ANY K ⇒ low-speed (an FS/HS idle line never shows K). */
            int k = 0, s, d;
            for (s = 0; s < 16; s++) {
                if (((ehci_read32(gSoftc.opBase, EHCI_PORTSC(p)) >> 10) & 3) == 1) k++;
                for (d = 0; d < 100; d++) (void)ehci_read32(gSoftc.opBase, EHCI_FRINDEX);  /* ~100 µs spacer */
            }
            /* ★ h34 MAJORITY, not any-K (the h33 filter was wrong for busy lines): FULL-SPEED signalling
             * toggles J/K per packet, so a drive Apple is actively driving shows K on some samples — any-K
             * would have skipped the drive's port and produced a 1.1 mount. LS idles at K (≥14/16); a busy
             * FS line samples ≈50/50 in packets, J between (≤8/16). Threshold 12. K-count logged so hardware
             * tells us the real distributions. */
            if (k >= 12) {
                ehci_os_logx("h34: K-majority (LOW-SPEED device) — LEFT WITH APPLE; port<<8|Kcount",
                             ((UInt32)p << 8) | (UInt32)k);
                continue;
            }
            ehci_os_logx("h34: K-samples of 16 (below 12 = not low-speed, evicting); port<<8|Kcount",
                         ((UInt32)p << 8) | (UInt32)k);
            ehci_os_logx("h31: port still COMPANION-OWNED after hc_start (our HCReset gave it away) — "
                         "evicting; port<<28|PORTSC[before]", ((UInt32)p << 28) | (v & 0xFFFFFFF));
            v &= ~EHCI_PORTSC_RW1C; v &= ~EHCI_PORT_OWNER; v |= EHCI_PORT_POWER;
            ehci_write32(gSoftc.opBase, EHCI_PORTSC(p), v);
            ehci_os_logx("h31:   PORTSC[after]", ehci_read32(gSoftc.opBase, EHCI_PORTSC(p)));
            swept++;
        }
    }
    if (swept) ehci_os_logx("h31: owner sweep complete — ports evicted back to EHCI", (UInt32)swept);
}

void ehci_vhub_appless_init(void)
{
    /* ★★★★★★★ h96 — THE PUMP DESCRIPTOR MUST LIVE IN THE SYSTEM ZONE. NewNMUPP allocates in TheZone,
     * and this runs at task level inside whatever context hosts the extension's NM response — a zone
     * with no obligation to outlive boot. The 2026-08-14 MDD desktop-load crash was this exact class:
     * a persistent descriptor in evaporating boot-era memory, recycled into the Helvetica FOND, then
     * called — and THIS descriptor is the most-called pointer in the whole driver (every task tick,
     * forever). h90 already carried the rule and the idiom ("the RD must outlive whichever app hosts
     * boot"); the lesson existed in one place and was not consulted here. Now it is. */
    if (!gNMTUpp) {
        THz oldZone = GetZone();
        SetZone(SystemZone());
        gNMTUpp = NewNMUPP(ehci_nm_task_resp);
        SetZone(oldZone);
    }
    ehci_os_logx("=== h26 APP-LESS: NMInstall(nmStr=0) task-level pump armed; UPP non-zero = ready (h96: SysZone-forced)", (UInt32)gNMTUpp);
}

static void selfprobe_tick_body(void);

/* The public tick is a WRAPPER: the body keeps its several early returns, so the depth counter is maintained
 * here where there is exactly one exit (inline set/clear around those returns would leak it).
 *
 * ⚠⚠ h27: THIS PATH NEVER SHORT-CIRCUITS. h26 put `if (gInTaskWork) return;` here and that made an app tick
 * capable of being silently dropped — a behavioural change to the exact path an application has driven for 130
 * builds, for the benefit of a new path that could have been guarded on its own. Run A froze. Whatever else
 * was wrong (see ehci_nm_task_resp), changing a validated path to protect a new one was the wrong shape:
 * ★ GUARD THE NEW CALLER, NOT THE SHARED CALLEE. */
void ehci_vhub_selfprobe_tick(void)
{
    /* ★★ h28: measure IDLE time — previous body EXIT to this entry — not entry-to-entry.
     * ⚠ h26/h27 measured entry-to-entry, which INCLUDES the body's own duration, and the body writes a ~40-line
     * periodic dump with an FSWrite + FlushVol per line to the boot volume while USB copying is in flight. So
     * h27's headline "gTickGapMax = 245 ticks = 4.08 s" may have been the instrument timing ITSELF rather than
     * any starvation, and it could not distinguish the two. Idle time can only be time we were not called. */
    {   UInt32 nowT = *(volatile UInt32 *)0x016AUL;      /* lowmem Ticks (60 Hz) */
        if (gTickCalls) { gTickGapLast = nowT - gTickLastExit;
                          if (gTickGapLast > gTickGapMax) gTickGapMax = gTickGapLast; }
        gTickCalls++;
    }
    gInTaskWork++;
    /* ★ h88: the DCE fix, retried from PROVEN task level until it lands. The uimInitialize call ran before
     * the USL had registered the unit entry (the leading read of the m36 miss), so nothing was found. This
     * runs seconds later, after all boot registration, and is a latched no-op once the plant succeeds. */
    { extern void ehci_os_fix_native_dce(void); ehci_os_fix_native_dce(); }
    h92_check();   /* h92: sweep the DMA guard zones — reads only, latched one-shot reporting */
    /* h84: bracket the body. gBodyStartMs is the LIVE field — the paint reads frame_ms() - gBodyStartMs at
     * interrupt level, so a body that never returns still reports how long it has been gone. */
    gBodyStartMs = frame_ms();
    bphase(BP_ENTRY);
    selfprobe_tick_body();
    {   UInt32 d;
        bphase(BP_IDLE);                       /* closes the last phase's high-water on the way out */
        d = frame_ms() - gBodyStartMs;
        gBodyLastMs = d;
        if (d > gBodyMaxMs) gBodyMaxMs = d;    /* COMPLETED bodies only — a wedge never reaches this line */
        if (d >= BODY_SLOW_MS) gBodySlowN++;
    }
    gInTaskWork--;
    gTickLastExit = *(volatile UInt32 *)0x016AUL;
}

static void selfprobe_tick_body(void)
{
    static const UInt8 cdbInq[6]  = {0x12,0,0,0,36,0};
    static const UInt8 cdbCap[10] = {0x25,0,0,0,0,0,0,0,0,0};
    UInt8 cdbRd[10]; int i;
    compl_drain();
    if (PHASE0_TRANSPARENT) return;   /* Phase 0: transparent UIM — no self-probe / fence / takeover.
                                       * compl_drain (above) still delivers Apple's bulk completions and
                                       * OS 9's own stack mounts the device. */
#if APPLE_HIDE
    /* ★ n5: this function is now only the TASK-LEVEL half of the async design, and only these things
     * need it.
     * (1) Flush whatever the interrupt-level engine logged into the ring. With no app running nothing
     *     drains it, so the ring simply ACCUMULATES and appears in the log the next time anything pumps
     *     us — which is exactly how to read back an appless run: insert with nothing running, then launch
     *     the activator afterwards and the whole enumeration trace lands in EHCIUIM_init.log. */
    /* h78: the BULK drain. This is selfprobe_tick, running in the NM response — NOT inside a File
     * Manager call chain — so a generous budget here is safe, and it is what keeps the ring from
     * overflowing now that the DoDriverIO drain is capped at a handful of lines. */
    bphase(BP_LOGDRAIN);
    ehci_os_ilog_drain_bulk();
    /* ★★★★★★ h42 — THE h41 LATE DUMP BELONGS **HERE**, AT TASK LEVEL. IT WAS IN as_tick, AT INTERRUPT LEVEL.
     *
     * ⚠⚠ h41 crashed the Finder into MacsBug: "Privilege Violation at 01E79BFC 'FOND 0015 02D6 Helvetica'" —
     * the PC inside a FONT resource, i.e. data executed as code, from an "instrument only" build. Cause: I put
     * h41_queue_dump (which calls ehci_os_log -> synchronous FSWrite + FlushVol) inside as_tick, and as_tick
     * runs from the 8 ms HEARTBEAT at INTERRUPT LEVEL as well as from the task-level tick.
     * ★ That is this project's FIRST rule — never call the File Manager below task level — and it has now bitten
     * four times (r18, n4, r95, this). The interrupt-safe ring (ehci_os_ilog*) exists for exactly that path and
     * the h39 scan a few lines below correctly uses it; I reached for the wrong logger and shipped it.
     * ⇒ selfprobe_tick_body is task-level by construction (it is the NM response / slot-23 path, and it opens
     * with the File-Manager drain above), so the dump is safe here and nowhere near the heartbeat. */
    /* ★★★★★★ f4 VERDICT — PRINTED TWICE, AND ON THE **DIRECT** CHANNEL. BOTH CHOICES ARE THE POINT.
     *
     * ⚠ WHY TWICE. m24 DIED INSIDE THE +15 s WINDOW — that dump never fired, which is precisely how we know
     * the crash was in there. Hanging the verdict off that one deadline would have reproduced the failure
     * this whole run exists to avoid: the evidence dying with the machine. +5 s is the floor (the mount
     * attempt has begun, so any clobber during it is already counted), +15 s is the full window.
     *
     * ⚠⚠ WHY ehci_os_logx AND NOT THE CRITICAL RING. I first wrote this on ehci_os_ilogc — reaching for
     * "critical = rate-cap exempt" when the requirement here is actually CRASH SURVIVAL, which is a
     * different property. A ring line sits in MEMORY until some task-level caller drains it; a machine that
     * dies first takes it with it. ehci_os_log/logx is a synchronous FSWrite + FlushVol — ON DISK before the
     * next line runs. This block is task level by construction (see the note above), so the direct channel
     * is available, and at task level it is ALSO uncapped. Strictly better here on every axis.
     * ⇒ The general lesson, and it is the §0 lesson again from the other side: pick the channel by what the
     * line has to survive, not by which one has the word "critical" on it.
     *
     * ★ HOW TO READ IT — the three outcomes are mutually exclusive and the log states which one it is:
     *   f4 clobberProbe > 0  => DIRECTION B. Block I/O armed the shared slot over a live PROBE transfer.
     *                           The victim's qTD is orphaned AND both were pointed at the same bounce.
     *                           This is the only mechanism that can corrupt the MOUNT'S OWN data, so it
     *                           is the answer if the volume came up unreadable or the machine crashed.
     *   f4 pumpBlockedByBio > 0 and maxWaitMs large => DIRECTION A. The probe was starved: its queued
     *                           transfer was never issued while the mount re-armed the slot on every
     *                           completion. Expect a failed/timed-out probe and a HEALTHY mount.
     *   all zero, and enumWhileBio 0 => NEITHER. The two never coincided; F4 is refuted and the
     *                           CTL/BULK split stays off the critical path. That is a real result.
     * ⚠ enumWhileBio > 0 with both harm counters 0 means they overlapped WITHOUT contending — worth
     * knowing, because it says the window is real and only the timing spared us.
     *
     * ZERO lines are added to the hot path — the whole instrument is counters until this moment. That
     * discipline is h47's: 705 dumps inside one defer window were "noise and a plausible confound". */
    bphase(BP_LATEDUMP);
    {   int f4early = (gF4EarlyDue && (long)(*(volatile UInt32 *)0x016AUL - gF4EarlyDue) >= 0);
        int f4late  = (gH41LateDue && (long)(*(volatile UInt32 *)0x016AUL - gH41LateDue) >= 0);
        if (f4early) gF4EarlyDue = 0;
        if (f4late) {
            gH41LateDue = 0;
            h41_queue_dump("=== h41 QUEUES **~15 s AFTER** the exposure (post-prompt end state: ONE entry or two?) ===");
        }
        if (f4early || f4late) {
            ehci_os_log(f4late
                ? "=== f4 SHARED IN-FLIGHT SLOT — VERDICT at ~15 s (the FULL mount window) ==="
                : "=== f4 SHARED IN-FLIGHT SLOT — VERDICT at ~5 s (EARLY floor; the ~15 s one follows "
                  "unless the machine dies first, which is exactly what m24 did) ===");
            ehci_os_logx("  f4 snapshot AT AddDrive: asRunning<<24|asPc<<16|bioDepth<<8|dpBusy<<4|owner "
                         "(owner 1=ctl 2=probe 3=bio)", gF4Snap);
            ehci_os_logx("  f4   downQ depth at that moment", gF4SnapDownQ);
            ehci_os_logx("* f4 B: CLOBBERS — bio armed over a BUSY slot (total)", gF4Clobber);
            ehci_os_logx("* f4 B:   ...of a PROBE/enum bulk transfer (THE DANGEROUS ONE)", gF4ClobberProbe);
            ehci_os_logx("  f4 B:   ...of a control transfer", gF4ClobberCtl);
            ehci_os_logx("  f4 B:   ...of another block transfer (nonzero = a bio-engine bug)", gF4ClobberBio);
            ehci_os_logx("  f4 B:   ...while an enumeration sequence was running", gF4ClobberEnum);
            ehci_os_logx("  f4 B:   frame_ms of the FIRST clobber (0 = never happened)", gF4ClobberFirstMs);
            ehci_os_logx("* f4 A: down_pump BLOCKED with work queued (total)", gF4PumpBlocked);
            ehci_os_logx("* f4 A:   ...specifically because BLOCK I/O held the slot", gF4PumpBlockedByBio);
            ehci_os_logx("* f4 A:   worst wait, ms, a queued transfer sat unissued", gF4PumpMaxWaitMs);
            ehci_os_logx("  f4 A:   owner of the slot at that worst wait (1=ctl 2=probe 3=bio)",
                         gF4PumpMaxWaitOwner);
            ehci_os_logx("* f4 OVERLAP: heartbeats with an enumeration running AND a block transfer in flight",
                         gF4EnumArmedWhileBio);
            ehci_os_logx("  f4 context: gDownQDrop (queue overflowed) / gDownTimeouts",
                         ((gDownQDrop & 0xFFFFUL) << 16) | (gDownTimeouts & 0xFFFFUL));
            /* ★★★★★★ THE SPLIT'S OWN VERDICT. Read these three together:
             *   splitSaved > 0 AND clobbers 0  => the split WORKED, and this says how many times.
             *   splitSaved 0                   => the two never coincided this boot; proves nothing either
             *                                     way, exactly like the single-drive m25 run. Try again with
             *                                     the hub + two drives.
             *   any clobber > 0                => block I/O armed over BLOCK I/O. The engine can no longer
             *                                     be the victim, so this would be a NEW, different bug in
             *                                     bio_kick/bio_advance serialisation. Do not ignore it.
             *   egOversize > 0                 => a caller asked the engine for more than ENG_BUF_MAX and
             *                                     was refused. Must be 0; if not, find the caller. */
            ehci_os_logx("* SPLIT: times block I/O armed while the ENGINE was busy (each one WAS a clobber "
                         "before this build; now harmless)", gSplitSaved);
            ehci_os_logx("* SPLIT: engine transfers REFUSED for exceeding ENG_BUF_MAX (MUST be 0)", gEgOversize);
            ehci_os_logx("  SPLIT: engine slot busy<<8 | block-I/O slot busy, right now",
                         ((UInt32)(gEgBusy ? 1 : 0) << 8) | (UInt32)(gDpBusy ? 1 : 0));
        }
    }
    /* ★★ n5d LIVENESS PROBE — the one diagnostic that settles the n5r failure.
     * Symptom being chased: the app stays RESPONSIVE (so its pump loop is returning) yet nothing from this
     * function reaches the log after Apple's root-hub enumeration — no PORTMAP idle reminders, no port
     * events. Exactly two explanations remain and they need opposite fixes:
     *   (a) uim23 is NOT being called at all -> these lines will be ABSENT, and the fault is upstream of us
     *       (the USL has stopped servicing our bus).
     *   (b) uim23 IS being called and the state gates are wrong -> these lines APPEAR, and the values say
     *       which gate: gSelfEnumDone silences portmap_tick; gSelfEnumPort<0 means service_ports never saw
     *       the connect; a frozen gVhubTick means the heartbeat/SIH is dead so nothing is polling the ports.
     * Deliberately NOT EHCI_VERBOSE: that flag restores the whole per-tick v18-v42 trace, and its per-tick
     * FSWrite+FlushVol is itself a known enumeration-timing aggravator. This is one line per 512 calls. */
    bphase(BP_PERIODIC);        /* h84: the ~40-line dump, every line an FSWrite + FlushVol on the boot volume */
    { static UInt32 gLiveN = 0;
      /* n5d2: every 64 calls, not 512. The first run produced exactly ONE block, which told us the state
       * was clean at call 1 but gave no TIME SERIES — and the decisive question is whether gVhubTick keeps
       * CLIMBING (heartbeat alive) or freezes (heartbeat dead, so service_ports never sees the insert,
       * which would explain both the missing port event and the 1.1 mount). ~10 calls/sec means a block
       * every ~6 s. */
      /* ★★★★★★★ h68 — THE INSTRUMENT WAS THE BOTTLENECK, NOT THE BUG.
       * The periodic dump fired every 64 pump passes, and app-less the pump is paced by the keepalive at 250
       * SIH passes -- so a dump landed roughly every 16,000 SIH passes, about every two minutes. That is why
       * every run since m14 has yielded ONE OR TWO samples, and why four successive theories each died for
       * lack of data rather than being settled by it. Every one of them would have been answered in a single
       * run at a usable sample rate.
       * ⇒ every 4 passes. 16x the samples, and the cost is log volume on a run we are reading anyway. */
      /* ★★★★★★ h78 FIX 3 — PACE THIS BY THE WALL CLOCK, NOT BY PUMP PASSES.
       *
       * ⚠ MEASURED, m26 re-boot: this dump fired 62 TIMES in one session at ~86 lines each — 5365 lines
       * AFTER the mount, every one a synchronous FSWrite + FlushVol at task level, landing exactly where
       * the Finder is building the desktop. At the documented 1-5 ms per write that is 5-27 SECONDS of
       * pure File Manager time. The defer that run was 38.8 s, the longest ever recorded.
       * ★ THE DEFECT IS THE PACING BASIS, NOT h68's NUMBER. h68 changed the divisor from 64 to 4 for an
       * excellent reason — the old rate gave ONE SAMPLE PER TWO MINUTES and four consecutive theories
       * died for want of data — and its own comment accepted "the cost is log volume on a run we are
       * reading anyway". But PUMP PASSES are not a clock: the pump rate swings enormously between
       * app-driven and app-less, idle and busy, so the log rate ended up being an artifact of how busy
       * the machine was rather than anything we chose.
       * ⇒ Wall clock, and DENSE WHERE THE ACTION IS: every 2 s while we are still enumerating and
       * probing, every 15 s once a volume has mounted and the interesting part is over. h68's sample
       * rate is preserved exactly where h68 needed it, and the steady-state flood — which is what was
       * actually hurting us — drops by roughly an order of magnitude. */
      UInt32 nowDump = *(volatile UInt32 *)0x016AUL;          /* lowmem Ticks, 60 Hz */
      static UInt32 sNextDump = 0;
      gLiveN++;
      if ((long)(nowDump - sNextDump) >= 0) {
          /* ★★★★★★★ h79 — h78's GATE WAS WRONG AND MADE THE IDLE CASE TEN TIMES WORSE.
           * ⚠ `gMountedOnce ? 900 : 120` reads as "2 s until something mounts, then 15 s". But with NO
           * DRIVES ATTACHED gMountedOnce is NEVER SET, so it means 2 s FOR EVER. Measured on a no-drive
           * boot: 18 dumps, ~1548 of the session's 1677 lines, every one a synchronous FSWrite+FlushVol
           * at task level during desktop construction. Before h78 the gate was every 4 PUMP passes, and
           * a pump that turns once per tens of seconds gave 1-2 dumps a boot — so h78 made the quiet
           * case an order of magnitude WORSE, which is the exact opposite of its purpose, and the user
           * felt it immediately as "desktop icons take much longer to load than before".
           * ⇒ THE CONDITION IS "ARE WE ACTUALLY DOING SOMETHING", NOT "HAS ANYTHING EVER MOUNTED".
           * h68 needed dense sampling DURING enumeration and probing; that is preserved exactly. What
           * goes away is hammering the File Manager while the driver sits idle with nothing plugged in.
           * ★ AND IT MATTERS MORE THAN LOG TIDINESS: 72.6 s of NM latency was measured on a boot with NO
           * USB DEVICES AT ALL, where our only activity IS this logging. We are not a bystander to the
           * task-level starvation — we are feeding it. */
          {   int busy = gAs.running || gAsProbeOK || gAsNeedBulk || gBioPhase || gHubIntNeedArm;
              sNextDump = nowDump + (busy ? 120UL : 1800UL);   /* 2 s working, 30 s idle */
          }
          ehci_os_log("n5d uim23 ALIVE:");
          ehci_os_logx("  callN",         gLiveN);
          ehci_os_logx("  gVhubTick (heartbeat/SIH alive if climbing)", gVhubTick);
          ehci_os_logx("  gServiceStop (1 = heartbeat suppressed!)", (UInt32)gServiceStop);
          ehci_os_logx("  gIsrHits",      gIsrHits);
          ehci_os_logx("  gPState",       (UInt32)gDev[0].pstate);
          ehci_os_logx("  gSelfEnumDone (1 silences PORTMAP)", (UInt32)gSelfEnumDone);
          ehci_os_logx("  gSelfEnumPort (-1 = no connect seen)", (UInt32)(long)gSelfEnumPort);
          ehci_os_logx("  gSelfEnumTries", gSelfEnumTries);
          ehci_os_logx("  gAs.running",   (UInt32)gAs.running);
          ehci_os_logx("  gAs.pc",        (UInt32)gAs.pc);
          ehci_os_logx("  gHideLogMask",  gHideLogMask);
          /* ★★★★★ h36 — THE TRANSFER-ENGINE HEALTH COUNTERS BELONG IN *THIS* DUMP, not only in the v47 stall
           * dump. Four T4 runs failed with "the volume needs to be initialized" and I could not say whether a
           * transfer had errored, timed out, or been starved — because gDownErr/gDownTimeouts/gIsrConsecMax
           * print ONLY from the v47 dump, which is gated on gDpBusy and never fired in any of those runs. So
           * the periodic dump reported a healthy-looking engine while saying nothing about the failure at all.
           * ⚠ That is the same defect shape as the h28 keepalive: a diagnostic reachable only through a path
           * the failure does not take. These are four cheap reads; they belong where the dump always runs.
           * ★ gIsrConsecMax is the shared-IRQ storm peak, which is the counter that matters most for the T4
           * configuration — HIDs on the card mean Apple's companion shares our PCI interrupt line and fires
           * constantly, and a storm that masks USBINTR long enough would starve a File Manager read into
           * ioErr, i.e. exactly "unreadable volume". This makes that hypothesis measurable instead of argued. */
          ehci_os_logx("  h36 gDownErr / gDownTimeouts (both MUST be 0)",
                       ((gDownErr & 0xFFFFUL) << 16) | (gDownTimeouts & 0xFFFFUL));
          ehci_os_logx("  h36 gIsrConsecMax (shared-IRQ storm peak; high = companion flooding our line)",
                       gIsrConsecMax);
          ehci_os_logx("  h36 gDownDone (retirements; frozen while busy = engine stalled)", gDownDone);
          /* h45: non-zero = Apple got onto our bus despite APPLE_HIDE and we refused it. */
          ehci_os_logx("!! h45 gH45Refused (foreign-address bulk refusals; MUST be 0)", gH45Refused);
          /* h46: ONE per low-speed device ceded. 78 for one mouse was the h45 spin. */
          ehci_os_logx("!! h46 gH46CedeSpinGuard (h33 K-cedes; MUST equal the number of LS devices ceded)",
                       gH46CedeSpinGuard);
          ehci_os_logx("!! h53 gH53Unceded (ceded ports taken back after the companion released OWNER)",
                       gH53Unceded);
          /* h57: the QH-overlay halt. These two must MATCH — every halt seen is a halt cleared. A non-zero
           * count is not itself a failure (a transaction error on the wire is a device/timing event); it means
           * the endpoint recovered instead of staying dead for the session, which is the whole fix. */
          ehci_os_logx("!! h57 gQhHalted<<16|gQhUnhalted (QH overlay halts seen | cleared — MUST be equal)",
                       ((gQhHalted & 0xFFFFUL) << 16) | (gQhUnhalted & 0xFFFFUL));
          h48_probe();      /* h48: the _SystemTask trap watch + the Expert's own account. Task level. */
          h52_probe();      /* h52: the display-driver swap watch. Task level, pure memory reads. */
          ehci_os_logx("  h36 sharedCompanion (1 = our ISR chains Apple's on the same line)",
                       (UInt32)gSoftc.sharedCompanion);
          /* ★★★★★ ase_quiesce cost — THE app-less gating number (see the note on ase_quiesce).
           * Read it as: iterMax against the ASE_QUIESCE_ITER_CAP backstop (20000), the real limit being the
           * 10 ms TIME bound, and tbMax converted with
           *   microseconds = tbTicks * 125 * ufSpan / tbSpan
           * ⚠ aseTimeouts MUST be 0. Non-zero means the schedule never stopped and we reprogrammed a LIVE
           * ring anyway — the r84/r85 freeze shape, silent until this build.
           * ⚠⚠ h25: the label on aseIterMax used to read "bound is 200000" and was left behind when the bound
           * became a time bound with a 20000 backstop — a reader comparing 1870 against 200000 would conclude
           * there was 100x of headroom where there is ~10x. Fixed here; the counters themselves are unchanged
           * so old logs stay comparable.
           * ★★ AND THE us CONVERSION IS NOW KNOWN GOOD, which CORRECTS a recorded number: the 2026-08-07 run
           * reported 2557.7 ticks per microframe = 20.46 MHz, and p1/p2 on 2026-08-08 both measured 5204 =
           * 41.67 MHz. The new figure is the right one — an MDD's timebase is bus/4, and 166.7/4 = 41.67 MHz
           * exactly, where 20.46 MHz implies an 82 MHz bus that no MDD has. ⇒ the recorded "worst 1718
           * iterations = 1.55 ms" was 2x too high and is really ~0.77 ms, which agrees with p1's independent
           * 1870 iterations = 0.803 ms. The DECISION is untouched: at 0.43 us/iter the retired 200000 bound
           * was still ~86 ms of frozen event loop, so replacing it with the 10 ms time bound was right anyway.
           * ⚠ The earlier "trustworthy because last and max agree (0.87 vs 0.90 us)" check could not have
           * caught this: both derived from the SAME conversion, so it tested self-consistency, not the clock. */
          ehci_os_logx("  m4 aseCalls",        gAseCalls);
          ehci_os_logx("  m4 aseSkip (already stopped; cost 0)", gAseSkip);
          ehci_os_logx("  m4 aseSpun (actually waited)",         gAseSpun);
          ehci_os_logx("  m4 aseIterLast",     gAseIterLast);
          ehci_os_logx("  m4 aseIterMax  (10 ms time bound; 20000-iter backstop)", gAseIterMax);
          ehci_os_logx("  m4 aseIterSum  (/aseSpun = mean)",     gAseIterSum);
          ehci_os_logx("  m4 aseTbLast   (timebase ticks)",      gAseTbLast);
          ehci_os_logx("  m4 aseTbMax    (timebase ticks)",      gAseTbMax);
          ehci_os_logx("  m4 calib tbSpan",    gAseTbSpan);
          ehci_os_logx("  m4 calib ufSpan (125us each)",         gAseUfSpan);
          /* ★★★★★ h26: is the task-level pump being STARVED? These are the discriminator the n4i run could
           * not carry, because there was no clock in the log. gTickGapMax is the worst gap between task-level
           * ticks; gNmLatMax is the worst delay from posting an NM request to nmResp running. Under a copy,
           * ~18 ticks (300 ms) in either says the n4i same-qTD stalls were a starved pump; a few ticks in both
           * leaves the device's own write latency as the explanation. gNmArmed vs gNmFired should track;
           * gNmFailed MUST be 0 (a refused NMInstall means a parked job waits for the next arm). */
          ehci_os_logx("  h26 gTickCalls (task-level ticks so far)",          gTickCalls);
          ehci_os_logx("  h26 gTickGapLast / gTickGapMax (60 ticks = 1 s)",
                       ((gTickGapLast & 0xFFFFUL) << 16) | (gTickGapMax & 0xFFFFUL));
          ehci_os_logx("  h26 gNmArmed / gNmFired (should track)",
                       ((gNmArmed & 0xFFFFUL) << 16) | (gNmFired & 0xFFFFUL));
          /* ★ h27: declines are HEALTHY — the response found a tick already in progress and stood down. A
           * park that survives is re-armed by the next heartbeat. Non-zero here just means the app tick and
           * the pump were both live, which is exactly the Run A configuration. */
          ehci_os_logx("  h27 gNmDeclined (response stood down; benign)", gNmDeclined);
          /* ★ h28: keepalive arms. With NO app this is the only thing that gets the log written at all, so a
           * non-zero value here in an app-less run is itself the proof that the NM route is alive. */
          ehci_os_logx("  h28 gNmKeepArms (keepalive arms; app-less proof of life)", gNmKeepArms);
          /* h63: non-zero means the pump was found DEAD and recovered. The trigger is environmental (an
           * AppleShare alias blocking task level at boot), so on a networked machine this firing is EXPECTED
           * and is the fix working -- it is not a fault indicator. */
          ehci_os_logx("!! h63 gNmStuckRearms (lost NM requests recovered; non-zero = the pump was revived)",
                       gNmStuckRearms);
          /* h64: the VBL evidence. A large value means lowmem Ticks -- which the VBL interrupt drives --
           * froze for that long, which is exactly what used to kill the keepalive when its cadence was keyed
           * to Ticks. Ties this to the sibling VBL project. */
          ehci_os_logx("!! h64 gTicksStallMaxMs (longest lowmem-Ticks freeze, ms; VBL drives that counter)",
                       gTicksStallMaxMs);
          /* ★ h65 THE DISCRIMINATOR: gHbRuns counts HEARTBEAT callbacks only; gVhubTick counts SIH passes,
           * which the real EHCI IRQ also drives. gVhubTick climbing while gHbRuns is FLAT = the Time Manager
           * one-shot stopped re-arming itself, freezing frame_ms() -- the last standing explanation for the
           * pump arming exactly once. */
          ehci_os_logx("!! h65 gHbRuns (heartbeat callbacks; compare with gVhubTick above)", gHbRuns);
          /* ★ h66: the Vbus / over-current summary, per port. gPwLost non-zero means the port went DARK
           * (the user's LED going out); gOcSeen non-zero means the controller reported over-current, which
           * per EHCI 2.3.9 is itself a reason it would drop PORT_POWER. Either one names a physical cause
           * that no amount of driver logic could have worked around. */
          /* ★★★★★★ h80 — PRINT THE PER-PORT GUARDS ONLY WHEN THEY HAVE SOMETHING TO SAY.
           * ⚠ These two loops emitted 10 LINES EVERY DUMP, and on the m29 run-4 session the dump fired 30
           * times: 300 synchronous FSWrite + FlushVol at TASK LEVEL reporting, almost always, nothing but
           * zeros. That is spent from the exact budget that was starving — the pump was taking 106.9 s a
           * turn — so these lines were actively making the fault they were meant to help diagnose worse.
           * ★ h66/h67 have read ZERO on every Mini run since h67 disproved the Vbus theory (m19). Suppress
           * the all-clear and say so in ONE line, so a reader still knows the check RAN. Anything non-zero
           * still prints per port, in full, unchanged — the diagnostic keeps all of its power and loses
           * only its cost. Same lesson as h16/h68: a diagnostic nobody can afford to leave on is worthless
           * in the run that matters. */
          {   int _p, any = 0;
              for (_p = 0; _p < (int)gSoftc.nPorts && _p < 15; _p++)
                  if (gPwLost[_p] || gOcSeen[_p] || gRepower[_p]) any = 1;
              if (!any) {
                  ehci_os_log("  h66/h67 Vbus + over-current + re-power: ALL PORTS CLEAN (checked, all zero)");
              } else {
                  for (_p = 0; _p < (int)gSoftc.nPorts && _p < 15; _p++)
                      if (gPwLost[_p] || gOcSeen[_p])
                          ehci_os_logx("!! h66 port<<28|powerLost<<16|overCurrentSeen",
                                       ((UInt32)_p << 28) | ((gPwLost[_p] & 0xFFUL) << 16) | (gOcSeen[_p] & 0xFFFFUL));
                  for (_p = 0; _p < (int)gSoftc.nPorts && _p < 15; _p++)
                      if (gRepower[_p])
                          ehci_os_logx("!! h67 port<<28|rePowerAttempts (0 = never needed)",
                                       ((UInt32)_p << 28) | (gRepower[_p] & 0xFFFFUL));
              }
          }
          /* h83: the unit in this label was WRONG — h64 moved both ends to frame_ms() and the word "ticks"
           * stayed. Reading 0x0b81 as ticks reports a 49 s stall; it is 2945 ms. Say the unit correctly. */
          /* ★★★ h84: the pump BODY, in milliseconds. Three lines, deliberately — this dump runs INSIDE the
           * body it is measuring (phase BP_PERIODIC), so every line added here inflates its own reading.
           * The per-phase table is on screen (row 5 MAXPH/PHMS) rather than here for the same reason. */
          ehci_os_logx("★ h84 body ms: LAST<<16|MAX (completed bodies; a wedged body never records)",
                       ((gBodyLastMs & 0xFFFFUL) << 16) | (gBodyMaxMs > 0xFFFFUL ? 0xFFFFUL : gBodyMaxMs));
          /* h92: the corruption instruments, on the '!!' channel so LEVEL 1 carries them every dump. */
          ehci_os_logx("!! h92 gH92Mask (DMA guard zones violated; MUST be 0 — nonzero = OUR DMA overran)",
                       gH92Mask);
          {   extern volatile UInt32 gN0Kind[3], gH93Count, gH93CcBad;
              extern void ehci_os_h93_dump(void);
              ehci_os_logx("!! h92 DoDriverIO kinds imm<<16|sync<<8|other (the h91 completion-tail watch)",
                           ((gN0Kind[0] & 0xFFUL) << 16) | ((gN0Kind[1] & 0xFFUL) << 8) | (gN0Kind[2] & 0xFFUL));
              ehci_os_logx("!! h93 DM calls<<16 | IOCommandIsComplete NONZERO returns (low; MUST be 0)",
                           ((gH93Count & 0xFFFFUL) << 16) | (gH93CcBad & 0xFFFFUL));
              ehci_os_h93_dump();   /* prints each new DM call: code/kind/cmdID + the DM's own verdict */
          }
          ehci_os_logx("!! h94 retired<<16 | refused (rude-removal client transfers made safe)",
                       ((gH94Retired & 0xFFFFu) << 16) | (gH94Refused & 0xFFFFu));
          ehci_os_logx("!! h94 gH94Orphaned (reap-gate saves; MUST be 0 — nonzero = the funnel has a hole)",
                       gH94Orphaned);
          /* b12: the engine's health was invisible at LEVEL1 (gDownErr/gDownTimeouts lived only in
           * LEVEL2 blocks) — the B&W's full-speed stalls cost a boot to that blindness. Promoted. */
          ehci_os_logx("!! b12 gDownErr<<16 | gDownTimeouts (nonzero = transfers failing/expiring)",
                       ((gDownErr & 0xFFFFu) << 16) | (gDownTimeouts & 0xFFFFu));
          ehci_os_logx("!! b12 gDownDone (completed transfers) / next: gMaxStallTicks", gDownDone);
          ehci_os_logx("!! b12 gMaxStallTicks (worst single-transfer latency, ticks; ~16ms each — a slow "
                       "flash-GC device shows LARGE values here BEFORE it starts timing out)", gMaxStallTicks);
          /* b12: the block driver's DoDriverIO ring ('Ucs2') records every Read/Write/Control/Status it
           * receives with the csCode and our returned err — the exact trail DFA/ASP queries leave. It was
           * only ever dumped on a bio STALL; print NEW entries per periodic dump instead (capped), so a
           * clean-but-odd session (the DFA garbage string, the skipped second drive) is reconstructible.
           * codes: 5=Open 6=Close 7=Read 8=Write 9=Control 10=Status; err 1 = kIOBusyStatus (accepted). */
          {   static UInt32 sDioLast = 0;
              if (!gDioLogPtr) { long _dv; if (Gestalt('Ucs2', &_dv) == noErr && _dv) {
                  DioLogMirror *_dp = (DioLogMirror *)_dv; if (_dp->magic == 0x44696f4cUL) gDioLogPtr = _dp; } }
              if (gDioLogPtr && gDioLogPtr->count != sDioLast) {
                  UInt32 _n = gDioLogPtr->count, _i, _new = _n - sDioLast, _show = (_new > 24u) ? 24u : _new;
                  ehci_os_logx("!! b12 DoDriverIO ring: NEW calls since last dump (showing latest, capped 24)",
                               _new);
                  for (_i = 0; _i < _show; _i++) {
                      DioRecMirror *_r = &gDioLogPtr->recs[(_n - 1u - _i) & 127u];
                      ehci_os_logx("!!   b12 code<<24|err16 / next line: aux (csCode for ctl+stat)",
                                   ((UInt32)(_r->code & 0xFF) << 24) | ((UInt32)_r->err & 0xFFFFu));
                      ehci_os_logx("!!     b12 aux", (UInt32)_r->aux);
                  }
                  sDioLast = _n;
              }
          }
          ehci_os_logx("★ h84 gBodySlowN (completed bodies over 1000 ms — how OFTEN, not just how bad)",
                       gBodySlowN);
          {   UInt32 i3, best3 = 0, bestPh3 = 0;
              for (i3 = 0; i3 < BP_N; i3++)
                  if (gPhaseMaxMs[i3] > best3) { best3 = gPhaseMaxMs[i3]; bestPh3 = i3; }
              ehci_os_logx("★ h84 worst SECTION: phase<<16|ms (1=entry 2=logdrain 3=latedump 4=periodic "
                           "5=needbulk 6=hubarm 7=expose 8=rearm 9=drains 10=tail; 11=h41-BEFORE "
                           "12=INSTALL+AddDrive 13=n19+h76 14=h41-AFTER — 11/13/14 are OUR LOGGING, "
                           "12 is the OS)",
                           ((bestPh3 & 0xFFFFUL) << 16) | (best3 > 0xFFFFUL ? 0xFFFFUL : best3));
          }
          ehci_os_logx("  h26 gNmLatLast / gNmLatMax (arm -> nmResp, MILLISECONDS)",
                       ((gNmLatLast & 0xFFFFUL) << 16) | (gNmLatMax & 0xFFFFUL));
          ehci_os_logx("!! h26 gNmFailed (MUST be 0 — a refused NMInstall parks the job)", gNmFailed);
          ehci_os_logx("!! m4 aseTimeouts (MUST be 0)",          gAseTimeouts);
          ehci_os_logx("!! m4 aseTimeoutKind (1=10ms time bound, 2=iteration cap)", gAseTimeoutKind);
          /* ★★★★ h13 PORT-AGNOSTIC DIAGNOSTICS. This dumped ports 0 and 4 and nothing else — two literals from
           * whichever ports happened to be interesting on some earlier run. Every root port is equivalent
           * hardware and the user is entitled to plug into any of them and get identical behaviour, so a
           * diagnostic that can only see two of them makes the driver LOOK port-dependent even when it is not:
           * on the h12 run the hub sat on port 4 and was visible, but on h11 it sat on port 2 and this block
           * was blind to it. Dump every port the controller reports. */
          /* ★ h80: PORTSC only when a port has actually CHANGED since the last dump. Five lines every dump
           * is another 150 File-Manager writes across a session that mostly reports the same values. The
           * mask ignores the RW1C change bits, exactly as portmap_tick does, so ordinary churn does not
           * count as a change. First dump of a session always prints, so a baseline is never missing. */
          { UInt32 pi, pn = gSoftc.nPorts; static UInt32 sLast[15]; static int sPrimed = 0; int changed = !sPrimed;
            for (pi = 0; pi < pn && pi < 15; pi++) {
                UInt32 v = ehci_read32(gSoftc.opBase, EHCI_PORTSC(pi));
                if ((v & 0x3005UL) != (sLast[pi] & 0x3005UL)) changed = 1;
                sLast[pi] = v;
            }
            sPrimed = 1;
            if (changed)
                for (pi = 0; pi < pn && pi < 15; pi++)
                    ehci_os_logx("  h13 port|PORTSC (hi nibble = port)",
                                 (pi << 28) | (sLast[pi] & 0x0FFFFFFFUL));
            else
                ehci_os_log("  h13 PORTSC unchanged since the last dump (all ports)"); }
      } }
    /* (2) Complete the handoff the engine cannot do itself: NewGestaltValue and InstallDriverFromMemory
     *     are both task-only, so the engine sets gAsProbeOK and we finish the job here. ⚠ THIS is the
     *     remaining app dependency — the probe is appless, the first MOUNT is not yet. Closing it means
     *     installing once at activation (no media) and reducing the runtime path to AddDrive-once +
     *     PostEvent, both of which are interrupt-safe. */
    /* n6b: the engine parks and asks US to register the bulk endpoints, because create_bulk does
     * ase_quiesce() + epq_program() and neither may run from the SIH (the hot-re-insert freeze). */
    /* ★ m3: count the turn BEFORE the check, so an advance in gTaskPumpN proves this check was evaluated
     * rather than merely that selfprobe_tick was entered. AS_TASK's fail condition reads it. */
    gTaskPumpN++;
    bphase(BP_NEEDBULK);
    if (gAsNeedBulk) {
        /* ★★★★ h14: CHECK THE REGISTRATIONS. These two results were discarded with (void), so a failure here
         * became "the drive never appeared" with nothing naming the cause. Both endpoints are required; if
         * either is refused, fail the enumeration LOUDLY rather than handing the probe a slot with no
         * endpoints and letting it die further downstream. */
        {   long e0 = ehci_vhub_create_bulk(gAs.addr, (UInt32)gAs.epOut, 0, (UInt32)gAs.mpOut);
            long e1 = ehci_vhub_create_bulk(gAs.addr, (UInt32)gAs.epIn,  1, (UInt32)gAs.mpIn);
            if (e0 != 0 || e1 != 0)
                ehci_os_logx("!! h14: bulk endpoint registration FAILED for this slot — the drive will not "
                             "appear; slot<<16|outErr<<8|inErr",
                             ((UInt32)gEnumDev << 16) | (((UInt32)e0 & 0xFFu) << 8) | ((UInt32)e1 & 0xFFu));
        }
        pb_find_eps(gEnumDev);   /* ★ n17: the slot BEING ENUMERATED. Hardcoding 0 here wrote drive B's
                                  * endpoint indices into slot 0, corrupting drive A's, and left slot 1 at
                                  * -1 so as_advance's check failed with 0x0000ffff three times over.em */
        ehci_os_logx("  n6b bulk endpoints registered at task level; slot", (UInt32)gEnumDev);
        ehci_os_logx("    gPOut/gPIn for THAT slot",
                     ((UInt32)(gDev[gEnumDev].pOut & 0xFF) << 8) | (UInt32)(gDev[gEnumDev].pIn & 0xFF));
        gAsNeedBulk = 0;        /* releases the engine's AS_TASK park */
    }
    /* ★ h10: program and park the hub's status-change QH here, at TASK level. create_bulk sets the contract:
     * quiesce the async schedule before touching a QH that is spliced into the live ring. */
    bphase(BP_HUBARM);
    if (gHubIntNeedArm) {
        ase_quiesce();
        epq_program(&gHubIntQ, gHub.addr, gHubIntEp, 64, 0);
        epq_arm_idle(&gHubIntQ);
        hub_int_arm();
        gHubIntNeedArm = 0;
        ehci_os_logx("h10: status-change qTD PARKED at task level — the hub reports changes from here; endpoint",
                     gHubIntEp);
    }
    bphase(BP_EXPOSE);          /* h84: the suspect. The 2026-08-12 body entered here and never returned. */
    if (gAsProbeOK) {
        /* ★★★★★ h30 — DO NOT EXPOSE A VOLUME INTO A FINDER THAT IS STILL BUILDING THE DESKTOP.
         *
         * ⚠ THE T2/T4 CRASH (2026-08-08): cold boot with a drive on the card ran this whole path correctly —
         * enumerate, probe, publish, install, AddDrive, diskEvt — and the FINDER then died with an illegal
         * instruction mid-startup, CurApName=Finder. The Finder mounts boot-time drives by walking the drive
         * queue DURING its startup, so an AddDrive landing in that window hands a half-initialised Finder a
         * volume. Post-desktop insertions have always been fine (Run D: hours). The model is Apple's own —
         * OHCI drives present at cold boot do not mount until the desktop finishes either.
         *
         * Gate: CurApName == "Finder" (lowmem Str31 at 0x910) continuously for FINDER_SETTLE_TICKS. Name
         * equality alone is NOT enough — the crash had CurApName already reading Finder — hence the settle
         * window. gAsProbeOK stays SET while deferred, so the NM keepalive (~2 s) re-evaluates until the gate
         * opens; the hold also blocks as_tick from starting another device's enumeration mid-defer.
         *
         * ★★★★★ h35 — publish_service AND install_block_driver run TOGETHER here, the h30/h31 structure that
         * PASSED T2 with a clean 2.0 cold-boot mount. h33 split them (publish at handoff, install ~6.5 s
         * later) to free the engine during the defer; T4-on-h34 then regressed — the driver's own probe read
         * block 0 cleanly, but the Finder called the volume UNREADABLE and a replug fixed it, a mount-path
         * regression whose only delta from the passing T2 was that split. The publish→install gap is the
         * suspect, so the two are adjacent again. The split's purpose is MOOT: it kept an HID port serviced
         * during a drive's defer, but the K-filter cedes HIDs to Apple BEFORE enumeration touches them (this
         * run: the FS mouse ceded before the drive handed off), so parking the engine strands nothing — worst
         * case a second DRIVE waits one ~6.5 s defer at cold boot, exactly as h31 did. */
        {
            /* ★★★★★★ h44 — WAIT FOR THE FINDER TO BE *READY*, NOT MERELY FOR TIME TO PASS.
             *
             * ⚠ THE USER'S OBSERVATION THAT CRACKED THIS (2026-08-10): the "initialize" prompt appears at the
             * same instant as the KEYCHAIN RECONNECT prompt for a file server, just behind it, right before the
             * desktop icons load. That is the Finder's NETWORK-VOLUME RESTORATION phase - modal dialogs up,
             * File Manager busy - and it explains the whole T2-versus-T4 split that four builds could not:
             *   T2 (drive only): the drive enumerates at once, so our AddDrive lands BEFORE that phase. Clean.
             *   T4 (drive + HIDs): the FS mouse burns three failed enumeration attempts first (~seconds), which
             *     pushes our AddDrive + diskEvt straight INTO it - handing a volume to a Finder sitting behind
             *     a modal prompt. h42's dumps proved our entry and geometry are correct, so the medium was never
             *     the problem; the MOMENT was.
             * ⇒ The old gate measured the wrong thing: elapsed time since CurApName read "Finder", which says
             * nothing about whether the Finder can accept a volume right now.
             *
             * ★ ADDED CONDITION: no modal dialog frontmost. Read from low memory only - WindowList (0x09D6) is
             * the frontmost window, and windowKind sits at +108, just past the 108-byte GrafPort in a
             * WindowRecord. dialogKind is 2. This is a pure memory read: NO Toolbox call, no allocation, no
             * event loop - the driver has never touched the Window Manager and this does not start.
             * ⚠ HARD CAP so a dialog left up forever cannot mean "no drive ever": past DEFER_CAP_TICKS we
             * expose anyway and say so in the log. A late mount beats no mount. */
            enum { FINDER_SETTLE_TICKS = 300 };            /* 5 s after the Finder name first appears */
            enum { DEFER_CAP_TICKS     = 2700 };           /* 45 s absolute ceiling on the whole defer */
            static UInt32 sFndSince = 0, sDeferStart = 0; static int sDeferLogged = 0;
            static int sDeferDumps = 0;        /* h47: cap the in-window queue dumps (705 in one window) */
            volatile UInt8 *ap = (volatile UInt8 *)0x0910UL;   /* CurApName, Str31 */
            int isFinder = (ap[0] == 6 && ap[1]=='F' && ap[2]=='i' && ap[3]=='n' &&
                            ap[4]=='d' && ap[5]=='e' && ap[6]=='r');
            UInt32 nowT = *(volatile UInt32 *)0x016AUL;
            int dialogUp = 0;
            {   void *fw = *(void * volatile *)0x09D6UL;    /* WindowList = frontmost window */
                if (fw) { short kind = *(volatile short *)((volatile char *)fw + 108);
                          if (kind == 2) dialogUp = 1; }    /* 2 = dialogKind */
            }
            /* ★★★★★★ h46 — sDeferStart MUST BE PER-EXPOSURE. IT WAS PER-BOOT, SO THE GATE WAS A ONE-SHOT.
             *
             * ⚠ THE h45 RUN (2026-08-10): four of five mounts logged "defer CAP reached (45 s) — exposing
             * anyway" and only the COLD BOOT logged a real release. sDeferStart was set once under
             * `if (!sDeferStart)` and never cleared, so it measured uptime since the FIRST defer this boot,
             * not the elapsed time of THIS one. Past 45 s of uptime `nowT - sDeferStart >= DEFER_CAP_TICKS`
             * is already true on the FIRST evaluation of every later exposure ⇒ the cap branch fires
             * immediately and the readiness gate is bypassed entirely for every hot-plug.
             *
             * ★ Two separate harms, and the second is the one this project keeps paying for: the gate stopped
             * protecting anything after the first 45 s, AND the log line claimed a 45-second wait that never
             * happened. A lying diagnostic has cost cycles here before (the v36 write instrumentation, h41's
             * counters). Clearing it on BOTH release paths makes the message true again in either direction.
             *
             * ⚠⚠ DO **NOT** also reset sFndSince here. The per-exposure clock is sDeferStart alone.
             *
             * ★★★★★★ h47 — AND `isFinder` WAS THE WRONG QUESTION, WHICH FIX 2 TURNED INTO A 45 s REGRESSION.
             *
             * ⚠ THE n4g RUN (2026-08-10, app-driven, no INIT): `exposure RELEASED` appears ZERO times and the
             * defer ended only via `defer CAP reached (45 s)`, with `isFinder<<8|dialogUp` logged as 0x0000 —
             * isFinder was **0**. 705 h43 queue dumps fired inside that window. The user saw a ~20 s stall
             * before the drive mounted.
             *
             * ★ WHY: `isFinder` reads CurApName (0x0910), which is the **CURRENT** process. In the APP-DRIVEN
             * path this whole check runs from the activator's own slot-23 pump, so CurApName is the ACTIVATOR
             * and isFinder can never be true — `SetFrontProcess` hands the front to the Finder for event
             * routing but does NOT change CurApName while our code is executing. So the release branch was
             * unreachable and only the cap could end the defer. In the APP-LESS path the NM response borrows
             * the Finder's context, CurApName reads "Finder", and the gate works — which is exactly why v8
             * and v4b released properly at 335 and 555 ticks.
             * ⇒ Before h46, the never-reset sDeferStart made the cap fire on the first evaluation, so the
             * app-driven path exposed INSTANTLY *by accident*. Making the cap real turned that into a genuine
             * 45-second wait. **Fix 2 is right for app-less and was a regression for app-driven.**
             *
             * ★★ THE FIX, and it is the same shape as every other bug this session — a variable cleared by an
             * event that is not the event it cares about. `if (!isFinder) sFndSince = 0;` wiped the latch
             * whenever ANY other process happened to be current, including our own activator. So:
             *   1. sFndSince now LATCHES: set once the Finder has been seen as the current process, never
             *      cleared because something else is running.
             *   2. the defer condition asks `!sFndSince` ("the Finder has not appeared yet") instead of
             *      `!isFinder` ("the Finder is not running right now").
             * Cold-boot protection is unchanged: before the Finder ever appears sFndSince is 0, so we still
             * defer, and we still serve FINDER_SETTLE_TICKS after that first sighting. What goes away is
             * penalising a path merely because OUR code is the one asking.
             * ★ h44's own comment argued this already: CurApName timing "says nothing about whether the Finder
             * can accept a volume right now", and the ADDED condition — no modal dialog frontmost — is the one
             * that actually fixed T4. `dialogUp` still carries that, and it is read from the global
             * WindowList, so it works whichever process is current. */
            if (!sDeferStart) sDeferStart = nowT ? nowT : 1;
            if (isFinder && !sFndSince) sFndSince = nowT ? nowT : 1;   /* h47: LATCH — never cleared */
            if ((nowT - sDeferStart) >= DEFER_CAP_TICKS) {
                ehci_os_logx("h44: defer CAP reached (45 s) — exposing anyway; dialogUp", (UInt32)dialogUp);
                sDeferStart = 0;                 /* h46: this defer is over; the next one times itself */
                sDeferDumps = 0;                 /* h47: each defer window gets its own dump budget */
                gExposureDeferred = 0;
            } else if (!sFndSince || (nowT - sFndSince) < FINDER_SETTLE_TICKS || dialogUp) {
                if (!sDeferLogged) {
                    sDeferLogged = 1;
                    ehci_os_logx("h30/h44: volume exposure DEFERRED — isFinder<<8|dialogUp (h44: a modal "
                                 "dialog frontmost means the Finder cannot take a volume yet)",
                                 ((UInt32)isFinder << 8) | (UInt32)dialogUp);
                }
                /* ★★★★★★ h43 — DUMP THE QUEUES *INSIDE THE DEFER WINDOW*, which is where the prompt lives.
                 * h42's "BEFORE our install" dump was too late by construction: our install waits ~6 s for the
                 * Finder to settle, and the "initialize" prompt appears DURING that wait, so by dump time the
                 * offending entry has already been created AND dismissed by the user's Eject. h42 therefore
                 * proved only that the END state is clean (one entry, ours, correctly mounted) - it could not
                 * see the crime. This fires on every re-check (~2 s apart) for the whole window, so an entry
                 * that appears and vanishes inside it is caught. Task level: this is selfprobe_tick_body.
                 *
                 * ⚠⚠ h47 CAPS IT. The n4g run fired this **705 times** in one 45 s window — several
                 * FSWrite + FlushVol per call, at task level, sustained, and in the SAME window as the boot
                 * crash we are chasing. That is both diagnostic noise and a plausible confound: this project
                 * has already wedged the File Manager with an unthrottled driver log loop, which is why the
                 * h16 token-bucket cap exists. The hunt this dump was built for (h41→h44, "which stack
                 * registered this medium") is CLOSED — the answer was that our entry is correct and the prompt
                 * is a configuration limit. A handful of frames still catches an entry that appears and
                 * vanishes; 705 adds nothing. */
                if (sDeferDumps < 24) { sDeferDumps++;   /* h68: 6 -> 24; the defer window is where
                                                         * the answer lives and it was the most starved */
                    h41_queue_dump("=== h43 QUEUES **DURING THE DEFER WINDOW** (the prompt happens in here) ==="); }
                gExposureDeferred = 1;                     /* keepalive re-checks in ~2 s; park stays set */
            } else {
                if (sDeferLogged) {
                    sDeferLogged = 0;
                    ehci_os_logx("h30/h44: exposure RELEASED — Finder settled AND no modal dialog; ticks "
                                 "since Finder appeared", nowT - sFndSince);
                    ehci_os_logx("  h46 ticks this defer actually took (the cap is 2700 = 45 s)",
                                 sDeferStart ? (nowT - sDeferStart) : 0);
                }
                sDeferStart = 0;                 /* h46: per-exposure clock — see the note above */
                sDeferDumps = 0;                 /* h47: each defer window gets its own dump budget */
                gExposureDeferred = 0;
            }
        }
        if (!gExposureDeferred) {
            /* h59: the LATCHED slot, not gEnumDev. gEnumDev is rewritten by every slot allocation, and this
             * line can run up to 45 s after the probe that set it — see the note at gExposeDev. */
            int hd = (gExposeDev >= 0) ? gExposeDev : gEnumDev;
            gAsProbeOK = 0; gExposeDev = -1;
            pb_find_eps(hd);
            ehci_os_logx("  n5 handoff at task level; gIsrHits (real IRQ during probe; 0 = heartbeat-only)", gIsrHits);
            /* ★★★★★★ I15 AS A CHECK, NOT A COMMENT — docs/ENGINE-STATE-MACHINE.md §7 F2.
             *
             * ⚠ WHAT THIS DEFENDS. Exposing a device we have not fully probed hands the Finder a volume it
             * cannot read, and the user-visible result is an "initialize this disk?" prompt over a drive that
             * is perfectly fine — destructive-looking, and indistinguishable in the log from several other
             * faults. m24 produced exactly that prompt and I spent a full analysis pass unable to rule this
             * in or out, because nothing in the code ASSERTED it and the probe's log lines had been dropped.
             *
             * ★ Today the property holds by construction: gAsProbeOK is set at ONE site, downstream of
             * READ CAPACITY, the geometry plausibility check and a successful read of block 0. So this guard
             * should never fire. That is the point — it is one refactor away from being false, and this
             * driver's entire bug history is state that USED to be guaranteed by a call ordering nobody had
             * written down. Cheap to hold, and it converts a silent wrong mount into a named refusal.
             *
             * ⚠ THE ENDPOINT TEST BELOW IS NOT THIS TEST. pOut/pIn are registered at SELFENUM COMPLETE,
             * before the BOT probe runs at all, so it proves the device was ENUMERATED, never that its medium
             * was READ. gDevBlkCnt[] is written at exactly one place (READ CAPACITY, after the plausibility
             * check), which makes it the only honest witness to a completed probe.
             * ⚠ inUse is required TOO: reconnect_reset clears a slot's state but does NOT zero its geometry
             * (fixed below), so on its own a stale block count could outlive the device that produced it. */
            if (!gDev[hd].inUse || gDevBlkCnt[hd] == 0) {
                ehci_os_logx("!! I15 VIOLATED — refusing to expose a slot that has NOT completed a probe. "
                             "This must never fire; if it has, the probe/exposure coupling is broken. "
                             "slot<<16|inUse<<8|blkCntNonZero",
                             ((UInt32)hd << 16) | ((UInt32)(gDev[hd].inUse ? 1 : 0) << 8) |
                             (UInt32)(gDevBlkCnt[hd] ? 1 : 0));
                gSelfEnumDone = 0;          /* let discovery re-run; do NOT strand the controller (I1/I5) */
            } else if (gDev[hd].pOut >= 0 && gDev[hd].pIn >= 0) {
                gDev[hd].pstate = 10;
                /* publish + install ADJACENT (the proven order): install_block_driver installs once and
                 * thereafter just announces, so calling it per device adds a drive not a second driver. */
                /* ★★★★★★ h78 FIX 2 — STAND THE DoDriverIO DRAIN DOWN FOR THE WHOLE MOUNT.
                 * Everything from here until the Finder has finished taking the volume is File Manager
                 * work on OUR drive, and every one of those calls reaches usb_disk.c's DoDriverIO, which
                 * drains our log ring with synchronous FSWrite + FlushVol ON THE BOOT VOLUME. That is
                 * File Manager work issued from inside a File Manager call chain on a different volume,
                 * and on the m27 hub run it is where task level died — gTaskPumpN froze at the exact
                 * value this dump records, and never moved again.
                 * ⚠ WALL-CLOCK AND SELF-CLEARING (30 s). It must expire without task-level help, because
                 * the state it is protecting against is precisely "task level is dead". An inhibit that
                 * needed the pump to lift it would be permanent in exactly the case that matters. */
                ehci_os_ilog_inhibit_drain(1800UL);
                ehci_vhub_publish_service(); gMountedOnce = 1;
                /* ★ h41: the discriminating pair of dumps — see h41_queue_dump. */
                bphase(BP_EXP_PRE);
                h41_queue_dump("=== h41 QUEUES **BEFORE** our install (a foreign entry here = APPLE already "
                               "registered this medium) ===");
                /* ★★★ f4: SNAPSHOT THE ENGINE AT THE INSTANT WE HAND THE VOLUME OVER. Everything after this
                 * line is the Finder mounting through our brand-new block driver, and the F4 question is
                 * whether another device's probe is live on the shared slot while that happens. Taken BEFORE
                 * install_block_driver so it describes the moment of handover, not the aftermath. */
                /* SPLIT: report BOTH slots — bit 5 = the ENGINE is busy, bit 4 = block I/O is busy. Before
                 * the split those were the same bit, which is precisely the confusion being removed. */
                gF4Snap = ((UInt32)(gAs.running ? 1 : 0) << 24) | ((UInt32)(gAs.pc & 0xFF) << 16)
                        | (((gBioHead - gBioTail) & 0xFFUL) << 8)
                        | ((UInt32)(gEgBusy ? 1 : 0) << 5)
                        | ((UInt32)(gDpBusy ? 1 : 0) << 4) | (UInt32)(gF4Owner & 0x0F);
                gF4SnapDownQ = gDownQHead - gDownQTail;
                bphase(BP_EXP_ADD);      /* h84b: the OS's own work — install, AddDrive, diskEvt. Nothing of ours. */
                install_block_driver(hd);
                bphase(BP_EXP_POST);     /* h84b: back to our logging */
                ehci_os_logx("n19: device EXPOSED to the OS as its own drive; slot", (UInt32)hd);
                /* ★★★★★★ h76 — DUMP THE STATE **HERE**, SYNCHRONOUSLY, NOT ON A TIMER.
                 * ⚠ THE m26 LESSON. The f4 verdict was hung off +5 s and +15 s deadlines, and in run 2 the
                 * File Manager wedged before EITHER fired, so the single most important run of this hunt
                 * produced no state at all. This instant — immediately after AddDrive returned — is the LAST
                 * MOMENT WE KNOW THE FILE MANAGER WORKS, because AddDrive itself just succeeded. Anything
                 * scheduled for later is a bet that the machine survives, and that bet has now lost once.
                 * The timed dumps stay (they show what changed AFTER the mount); this one guarantees a
                 * floor. */
#if H76_DIAGNOSTICS
                ehci_os_log("=== h76 STATE AT AddDrive (synchronous — the last instant the File Manager is "
                            "known good; the timed dumps may never fire) ===");
                ehci_os_logx("  h76 gBioPhase (>=20 = in BOT recovery)", (UInt32)gBioPhase);
                ehci_os_logx("  h76 egBusy<<8|dpBusy (the two in-flight slots)",
                             ((UInt32)(gEgBusy ? 1 : 0) << 8) | (UInt32)(gDpBusy ? 1 : 0));
                ehci_os_logx("  h76 bio ring depth (queued block requests)", gBioHead - gBioTail);
                ehci_os_logx("  h76 gAs.running<<8|gAs.pc (concurrent enumeration?)",
                             ((UInt32)(gAs.running ? 1 : 0) << 8) | ((UInt32)gAs.pc & 0xFFu));
                ehci_os_logx("  h76 gDownErr<<16|gDownTimeouts", ((gDownErr & 0xFFFFUL) << 16) | (gDownTimeouts & 0xFFFFUL));
                ehci_os_logx("  h76 gSplitSaved<<16|gEgRecovCompl",
                             ((gSplitSaved & 0xFFFFUL) << 16) | (gEgRecovCompl & 0xFFFFUL));
                ehci_os_logx("  h76 gTaskPumpN (the pump whose death the screen watchdog reports)", gTaskPumpN);
                /* ★ ARM THE SCREEN WATCHDOG. From here on, if task level goes silent for 5 s the heartbeat
                 * paints the live state to the top-left of the screen once a second. That is the readout
                 * that survives the File-Manager wedge this build exists to diagnose. */
                gPwPumpLast = gTaskPumpN; gPwPumpAtTick = TICKS_NOW; gPwArmed = 1;
                /* ★★★★★★ PROOF OF LIFE — PAINT ONCE, RIGHT NOW, UNCONDITIONALLY.
                 * ⚠ WITHOUT THIS THE INSTRUMENT IS UNFALSIFIABLE. The watchdog only paints when task level
                 * stalls, so a blank screen would mean EITHER "no stall happened" OR "the painter is
                 * broken" — and we would have no way to tell which, on the one run that matters. This
                 * project has paid for that mistake three times already (m20's sampling artifact, m24's 458
                 * dropped lines, and the m26 timed dump that never fired), and every time the instrument
                 * was believed until it was checked.
                 * ★ So paint the state box here, at a moment we KNOW is healthy. The user sees it work.
                 * On a good boot the Finder simply draws over it a moment later as it lays out the desktop,
                 * so it costs a brief flicker and buys certainty about the readout. */
                paint_watchdog_state();
#endif  /* H76_DIAGNOSTICS */
                bphase(BP_EXP_AFTER);
                h41_queue_dump("=== h41 QUEUES **AFTER** our install (ours should now appear; compare refNums) ===");
                /* ★ h41: and once more ~15 s later, by which time the Finder has put up the prompt and the user
                 * has answered it. That is the state no log has ever captured — every T4 log to date ends at
                 * the line above — and it shows whether the end state holds ONE entry or two. */
                gH41LateDue = *(volatile UInt32 *)0x016AUL + 900UL;
                gF4EarlyDue = *(volatile UInt32 *)0x016AUL + 300UL;   /* f4: +5 s floor — m24 died before +15 s */
            } else {
                ehci_os_log("!! n5: probe reported OK but the bulk endpoints are missing — not publishing 'Eusb'");
                gSelfEnumDone = 0;
            }
        }
    }
    /* p1a: an enumerated device was unplugged. Reset the probe state HERE (task level — reconnect_reset
     * does File-Mgr logging and must never run at interrupt level) so gDev[0].pstate returns to 0 and the case-0
     * hook can re-enumerate on the next insert. */
    bphase(BP_REARM);
    if (gSelfEnumRearm) {
        gSelfEnumRearm = 0;
        ehci_os_log("=== SELFENUM: enumerated device was pulled — resetting probe state for re-enumeration ===");
        /* ★★★★ h12: only tear a DRIVE down if a drive of ours actually left. gRearmDev is now -1 when the
         * disconnect freed no slot — e.g. a device we refused for want of a free slot being unplugged again.
         * Running the per-device path on a stale index is what unmounted an unrelated drive on hardware. The
         * engine reset and the re-arm below still run: those are about the PORT, not about any one device. */
        if (gRearmDev >= 0 && gRearmDev < USB_MAX_DEV) {
            nm_retract_for_dev(gRearmDev);    /* ★ n26: "you may now remove the cartridge" — it has been removed. */
            blk_notify_media(gRearmDev, 0);   /* n4c: mark the media gone BEFORE we tear the endpoints down, so kStatus
                                * stops reporting "disk present" for a volume that is no longer there. */
            blk_unmount_removed_volumes();   /* n7: and actually UNMOUNT it, as Apple's extension does. MUST
                                          * follow blk_notify_media(0) — gDiskInPlace has to be 0 first so
                                          * UnmountVol's flush fails fast with offLinErr rather than
                                          * stalling in the transfer engine against absent media. */
        } else {
            ehci_os_log("  h12: that disconnect freed no slot of ours — NOT touching any drive "
                        "(a stale index here is what unmounted an unrelated volume in h11)");
        }
        ase_quiesce();         /* n4c: reconnect_reset's own contract is that the async schedule must
                                * already be STOPPED before it epq_arm_idles the bulk QHs — create_bulk
                                * honours that and this path did not. It is the documented r84/r85 freeze
                                * shape, so close it rather than keep relying on luck. */
        /* ★★ n21: reset the slot that was actually PULLED, not slot 0.
         * This was `reconnect_reset(0)` — the third instance in this driver of a device index being plumbed
         * in and then discarded at one call site (n14's `r->dev = 0`, n19's three, this). gRearmDev is set
         * right beside the pull, and blk_notify_media above already honours it; only this line ignored it.
         * Latent rather than observed: every hardware pull so far has been slot 0's, where gRearmDev == 0 and
         * this is a no-op. Pull the SECOND drive first and it would reset the state of the drive still
         * PLUGGED IN. Behaviour for the proven slot-0 path is bit-identical. */
        if (gRearmDev >= 0 && gRearmDev < USB_MAX_DEV) reconnect_reset(gRearmDev);   /* h12: never gDev[-1] */
        gSelfEnumDone = 0; gSelfEnumTries = 0;
    }
    /* n15: drain the new-device and no-free-slot notices (same interrupt-set / task-logged discipline). */
    bphase(BP_DRAINS);
    if (gNewDevMask) {
        UInt32 m = gNewDevMask; int q;
        gNewDevMask = 0;
        for (q = 0; q < USB_MAX_DEV; q++)
            if (m & (1UL << q)) {
                ehci_os_logx("n15: device ENUMERATED into its own slot, endpoints on that slot's own bulk QH "
                             "pair; slot", (UInt32)q);
                ehci_os_logx("  on root port", (UInt32)gDev[q].probedPort);
                if (q != 0)
                    ehci_os_log("  n15: slot != 0 is NOT exposed yet — publishing 'Eusb' + AddDrive is still "
                                "single-device and needs the paired gSvc/activator change (step 4). The point "
                                "of this build is that it enumerated cleanly WITHOUT disturbing slot 0.");
            }
    }
    /* n13: drain the second-device notices set by service_ports at interrupt level. Logged here (task
     * level) because ehci_os_log is File-Manager I/O and must never run below task level. */
    if (gSecondDevMask) {
        UInt32 m = gSecondDevMask; int q;
        gSecondDevMask = 0;
        for (q = 0; q < 15; q++)
            if (m & (1UL << q))
                /* h14: name the port AND which slots are held. With four slots a bare "all in use" left the
                 * reader unable to tell a genuine full house from a leaked slot — and on the h13 run this
                 * exact line was mistaken for a fault when it was the 2-slot limit working correctly. */
                ehci_os_logx("h14: no free device slot — device left unenumerated, nothing else disturbed; "
                             "port<<24|USB_MAX_DEV<<16|inUseMask",
                             ((UInt32)q << 24) | ((UInt32)USB_MAX_DEV << 16) | dev_inuse_mask());
        /* h15: and SAY SO on screen. This drain already runs at task level and does File Manager work, which
         * is exactly what NMInstall requires. Posted once per refusal batch, not per port, so plugging a fifth
         * and sixth drive in quickly cannot stack alerts. */
        notify_slot_limit();
    }
    /* h15: drain the "an arm kept achieving nothing" notice. Seeing this line means the ARM_NOPROGRESS_MAX
     * backstop fired — enumeration stopped re-arming rather than spinning the heartbeat. That is a symptom
     * report, not the bug: something re-armed an identical no-op, and the log above it says what. */
    if (gArmStuckMask) {
        gArmStuckMask = 0;
        ehci_os_logx("!! h15: an enumeration arm achieved nothing repeatedly — STOPPED re-arming to protect "
                     "the File Manager (a real port event will resume it). Consecutive no-ops allowed",
                     (UInt32)ARM_NOPROGRESS_MAX);
    }
    /* n21: drain the "enumeration re-armed on a port a live slot already owns" notices. Seeing this line is
     * the fix DOING ITS JOB — the stale gSelfEnumPort was caught instead of stealing the survivor's port. */
    if (gPortOwnedMask) {
        UInt32 m = gPortOwnedMask; int q;
        gPortOwnedMask = 0;
        for (q = 0; q < 15; q++)
            if (m & (1UL << q))
                ehci_os_logx("n21: enumeration re-armed on a port a LIVE slot already owns — refused (this is "
                             "the n20 eject-both-then-pull-one bug being caught); port", (UInt32)q);
    }
    /* h7: drain the downstream-removal notices. */
    if (gHubPortGoneMask) {
        UInt32 m = gHubPortGoneMask; int q;
        gHubPortGoneMask = 0;
        for (q = 0; q < 31; q++)
            if (m & (1UL << q))
                ehci_os_logx("h7: a device behind the hub was UNPLUGGED — slot freed and volume unmounted; "
                             "downstream port", (UInt32)q);
    }
    /* h3: drain the hub-removed notice. */
    if (gHubGoneMask) {
        UInt32 m = gHubGoneMask; int q;
        gHubGoneMask = 0;
        for (q = 0; q < 15; q++)
            if (m & (1UL << q))
                ehci_os_logx("h3: the claimed HUB was unplugged — every slot behind it freed and unmounted; "
                             "root port", (UInt32)q);
    }
    /* n24: drain the un-park notices. Seeing this means a port that had been given up on is usable again. */
    if (gPortUnparkMask) {
        UInt32 m = gPortUnparkMask; int q;
        gPortUnparkMask = 0;
        for (q = 0; q < 15; q++)
            if (m & (1UL << q))
                ehci_os_logx("n24: parked port UN-PARKED (its device left, so the reason to park is gone; "
                             "a new device here gets a fresh 3 attempts); port", (UInt32)q);
    }
    /* h53: drain the un-cede notices. Seeing one means the companion handed a ceded port back and we have
     * stopped showing it to Apple -- which is what stops Apple enumerating the next device plugged in there. */
    if (gPortUncedeMask) {
        UInt32 m = gPortUncedeMask; int q;
        gPortUncedeMask = 0;
        for (q = 0; q < 15; q++)
            if (m & (1UL << q))
                ehci_os_logx("!! h53: ceded port UN-CEDED (companion released OWNER, so it is OURS again and is "
                             "hidden from Apple once more -- this is the APPLE_HIDE leak closing); port", (UInt32)q);
    }
    /* n4b: drain the hidden-connect log flags set by service_ports at interrupt level. */
    if (gHideLogMask) {
        UInt32 m = gHideLogMask; int q;
        gHideLogMask = 0;
        for (q = 0; q < 15; q++)
            if (m & (1UL << q))
                ehci_os_logx("APPLE_HIDE ph0b: port connect hidden from Apple (reported empty-but-changed) (port)",
                             (UInt32)q);
    }
    portmap_tick();   /* n3b: log port state changes so an insert is always visible */
    /* v47 TARGETED STALL DUMP (task level — the frozen log proved the task stays alive here: slot27 keeps
     * ticking steadily to the end). The Mini freeze stalls in the self-probe read sequence (seen at
     * gDev[0].pstate=1, post-takeover INQUIRY) with gDpBusy=1 = a bulk transfer issued whose completion never
     * arrives. Log the engine state ~1/sec while stuck so we see WHY, with NO per-tick flood. Reads:
     *   gIsrHits climbing = EHCI IRQs still firing (-> reap/routing bug) vs FLAT = no IRQ for our completion;
     *   gDownDone advancing = transfers retire on the wire vs FROZEN = nothing completing;
     *   gDownTimeouts climbing = the down watchdog IS firing (recovery failing) vs 0 = watchdog never runs;
     *   USBCMD bit5 (ASE 0x20) / USBSTS bit15 (ASS 0x8000) = is the async schedule actually RUNNING;
     *   USBSTS 0x1000 (HCHalted) / 0x10 (host system error) = controller faulted. */
    /* ★★★★ h25: the trigger is THE SAME TRANSFER OUTSTANDING FOR 250 ms, not merely "busy at the sampling
     * instant". The old test was `if (gDpBusy)` alone, and it cried wolf.
     *
     * ⚠ PROVED BY THE p2 RUN (2026-08-08, the noUSL pump): it fired ELEVEN times in a run where every test
     * passed, and its own dumps showed a perfectly healthy engine — `gPState` 10 (probe COMPLETE, so there was
     * no self-probe transfer to not-complete), `gIsrHits` climbing 23 -> 1399, `gDownDone` climbing 11 -> 667,
     * FRINDEX moving, `gDownErr`/`gDownTimeouts`/`gDownRelink` all 0, and `gDpBusy` reading **0** in the dump
     * body because the transfer retired during the log write. It was reporting ordinary file-copy traffic as a
     * stall: `gDpBusy` is the BULK slot, which during a copy is legitimately set almost all the time, so a
     * 1 Hz sample catching it set says nothing at all. p1 scored 0 only because the USL route happens not to
     * tick us mid-copy — i.e. the count measured WHEN WE SAMPLE, not whether anything was wrong.
     *
     * ★ A stall is one transfer that stops retiring, so track the qTD: same `gDpTd` still in flight 250 ms
     * later. Transfers flowing normally replace `gDpTd` constantly and never accumulate age, while a genuinely
     * wedged one trips it well inside the old 1 s cadence. Keeps every reader the dump had, and keeps the 1 Hz
     * rate limit for the repeat case. ⚠ This is a DIAGNOSTIC gate only — nothing here changes engine
     * behaviour, and the counters it prints are unchanged, so old logs stay comparable.
     * ★ Why it matters beyond noise: this project's rule is that a lying diagnostic costs cycles, and this one
     * would have made any future real stall indistinguishable from normal copying. */
    /* ★★★ THE SPLIT WIDENS THIS WATCHDOG, AND IT HAD TO. It watched gDp*, which is now BLOCK I/O ONLY — so
     * left alone it would have gone HALF BLIND on the day the split shipped: an ENGINE transfer could wedge
     * for ever and this, the one stall detector, would report a perfectly idle block-I/O slot and say
     * nothing. That is worse than before the split, because the gap would be invisible rather than merely
     * unfixed. Watch whichever slot is busy, and SAY WHICH ONE it is. */
    {   static UInt32 sSameTd = 0, sSameSince = 0; static int sSameIsEng = 0;
        UInt32 nowT0 = *(volatile UInt32 *)0x016AUL;
        int      egB  = gEgBusy != 0;
        UInt32   curTd = egB ? (UInt32)gEgTd : (gDpBusy ? (UInt32)gDpTd : 0);
        int      anyB  = egB || gDpBusy;
        if (!anyB || curTd != sSameTd) { sSameTd = anyB ? curTd : 0; sSameSince = nowT0; sSameIsEng = egB; }
    if (anyB && sSameTd && (long)(nowT0 - sSameSince) >= 15L) {   /* same qTD outstanding >= 250 ms */
        static UInt32 nextStall = 0;
        UInt32 nowT = nowT0;
        if ((long)(nowT - nextStall) >= 0) {
            nextStall = nowT + 60UL;                         /* ~1 s */
            ehci_os_log("!! v47 STALL — the SAME transfer has been outstanding for 250 ms (h25: not merely busy):");
            ehci_os_logx("  SPLIT: which slot is stalled (1 = ENGINE ctl/probe, 0 = BLOCK I/O)",
                         (UInt32)(sSameIsEng ? 1 : 0));
            ehci_os_logx("  h25 ticks the same qTD has been in flight (60 = 1 s)", nowT - sSameSince);
            ehci_os_logx("  gPState", (UInt32)gDev[0].pstate);
            ehci_os_logx("  gDpBusy", (UInt32)gDpBusy);
            ehci_os_logx("  gDpIsIn", (UInt32)gDpIsIn);
            ehci_os_logx("  gDpBulkEp", (UInt32)gDpBulkEp);
            ehci_os_logx("  gIsrHits (climbing=IRQ firing / flat=no IRQ)", gIsrHits);
            ehci_os_logx("  gIsrConsec (now)", gIsrConsec);
            ehci_os_logx("  gIsrConsecMax", gIsrConsecMax);
            ehci_os_logx("  gDownDone (advancing=xfers retire)", gDownDone);
            ehci_os_logx("  gDownErr", gDownErr);
            ehci_os_logx("  gDownTimeouts (watchdog fired)", gDownTimeouts);
            ehci_os_logx("  USBCMD (0x20=ASE async-enable)", ehci_read32(gSoftc.opBase, EHCI_USBCMD));
            ehci_os_logx("  USBSTS (0x8000=ASS run/0x1000=halt/0x10=hosterr)", ehci_read32(gSoftc.opBase, EHCI_USBSTS));
            /* ★★★★ h13 ENGINE-DEATH INSTRUMENTATION. On the h12 run this dump said the engine had stopped —
             * gDownDone frozen at 0xc6 and gIsrHits flat at 0x1c4 across every stall, where h11's 133 stalls
             * all had both counters climbing — but it could not say WHY, so the cause was left undetermined
             * rather than guessed at. USBSTS had Reclamation set, meaning the controller walked the whole async
             * list and found nothing to execute, while our own bookkeeping still believed a transfer was in
             * flight. Exactly three things can produce that, and these fields separate them:
             *   (a) the qTD was never activated  -> qTD token Active = 0 and the QH overlay does not point at it
             *   (b) the qTD is active but unreachable -> token Active = 1, but the ring does not lead to the QH
             *   (c) the controller itself has stopped -> FRINDEX frozen across two dumps
             * ASYNCLISTADDR + the anchor link + every QH address let the ring be walked by hand from the log.
             * gLastAnchorLink and gDownRelink are r60's own diagnostics, which have existed all along and were
             * never printed here. */
            ehci_os_logx("  h13 FRINDEX (frozen across dumps = controller stopped)",
                         ehci_read32(gSoftc.opBase, EHCI_FRINDEX));
            ehci_os_logx("  h13 ASYNCLISTADDR", ehci_read32(gSoftc.opBase, EHCI_ASYNCLISTADDR));
            ehci_os_logx("  h13 anchor->hlink (r60 diag, masked)", gLastAnchorLink);
            ehci_os_logx("  h13 gDownRelink (r60 ring repairs; MUST be 0)", gDownRelink);
            ehci_os_logx("  h13 gDpActual (bytes the last reap delivered)", gDpActual);
            ehci_os_logx("  h13 gCtlActual / gCtlStat of the last CONTROL completion",
                         (gCtlActual << 16) | ((UInt32)gCtlStat & 0xFFFFUL));
            ehci_os_logx("  h13 gHubShortSt (short GET_PORT_STATUS replies)", gHubShortSt);
            ehci_os_logx("  h14 gEnumDeferBusy (deferrals with an arrival ACTUALLY waiting — h16 gating)",
                         gEnumDeferBusy);
            /* h16: bio-ring pressure. BIOQ_N is 16 request slots shared by every device, so four busy drives
             * press on it far harder than two — and these counters existed while being printed NOWHERE, which
             * makes them unable to answer the question they were added for. hiWater against BIOQ_N says how
             * close the ring came to full; reject counts the submits it had to refuse. */
            ehci_os_logx("  h16 bio ring: hiWater<<16|reject (ring is BIOQ_N=16 deep, shared by all devices)",
                         ((gBioHiWater & 0xFFFFUL) << 16) | (gBioReject & 0xFFFFUL));
            ehci_os_logx("  h14 USB_MAX_DEV|NBULK|slots in use",
                         ((UInt32)USB_MAX_DEV << 24) | ((UInt32)NBULK << 16) | dev_inuse_mask());
            ehci_os_logx("  h13 ctrlQH phys", gCtrlQ.qhP);
            ehci_os_logx("  h13 hubIntQH phys", gHubIntQ.qhP);
            if (gSoftc.asyncAnchor)
                ehci_os_logx("  h13 anchor->hlink LIVE (re-read now)",
                             ehci_le32_to_cpu(gSoftc.asyncAnchor->hlink));
            /* The QH the stalled transfer was issued on, straight out of the DMA page. */
            if (gDpQ && gDpQ->qh) {
                ehci_os_logx("  h13 stalled QH phys", gDpQ->qhP);
                ehci_os_logx("  h13   qh->hlink",     ehci_le32_to_cpu(gDpQ->qh->hlink));
                ehci_os_logx("  h13   qh->epChar",    ehci_le32_to_cpu(gDpQ->qh->epChar));
                ehci_os_logx("  h13   qh->curQtd",    ehci_le32_to_cpu(gDpQ->qh->curQtd));
                ehci_os_logx("  h13   qh->ovlNext",   ehci_le32_to_cpu(gDpQ->qh->ovlNext));
                ehci_os_logx("  h13   qh->ovlToken (0x80=Active 0x40=Halted)",
                             ehci_le32_to_cpu(gDpQ->qh->ovlToken));
            }
            if (gDpTd) {
                ehci_os_logx("  h13 in-flight qTD token (0x80=Active)", ehci_le32_to_cpu(gDpTd->token));
                ehci_os_logx("  h13 in-flight qTD next",  ehci_le32_to_cpu(gDpTd->next));
            }
            /* The parked hub qTD shares this ring; if it went Halted it is a suspect for wedging the ring. */
            if (gHubIntQ.qh) {
                ehci_os_logx("  h13 hubIntQH ovlToken", ehci_le32_to_cpu(gHubIntQ.qh->ovlToken));
                ehci_os_logx("  h13 hubIntQH hlink",    ehci_le32_to_cpu(gHubIntQ.qh->hlink));
                ehci_os_logx("  h13 hubInt parked qTD token",
                             ehci_le32_to_cpu(gHubIntQ.td[gHubIntSlot]->token));
                ehci_os_logx("  h13 gHubIntArmed / gHubIntErrs",
                             ((UInt32)gHubIntArmed << 16) | (gHubIntErrs & 0xFFFFUL));
            }
        }
    }
    }   /* h25: closes the same-qTD age tracker opened above the dump */
    bphase(BP_TAIL);
    /* n5: give the engine a step from here too. Harmless duplication of the heartbeat's call (as_tick is
     * idempotent and re-entrancy-guarded); it just saves a heartbeat of latency when an app IS pumping. */
    as_tick();
    if (gAs.running) return;          /* engine mid-sequence: leave the legacy machine alone */
#endif
    if (EHCI_VERBOSE) {   /* r88 diag: log gDev[0].pstate transitions so we can SEE the post-reconnect re-probe path (or lack of it) */
        static UInt32 lastSt = 0xFFFFFFFFUL;
        if (gDev[0].pstate != lastSt) { lastSt = gDev[0].pstate;
            ehci_os_log("selfprobe_tick state:"); ehci_os_logx("  gPState", gDev[0].pstate);
            ehci_os_logx("  gReprobe", (UInt32)gReprobe); ehci_os_logx("  gPOut", (UInt32)(long)gDev[0].pOut); }
    }
    if (EHCI_VERBOSE) {   /* r89 diag: UNCONDITIONAL periodic entry log — proves selfprobe_tick is CALLED + the gDev[0].pstate it SEES */
        static UInt32 stN = 0;
        if ((stN++ & 0x1FFUL) == 0) {   /* every 512 calls */
            ehci_os_log("r89 selfprobe entry:");
            ehci_os_logx("  callN", stN);        ehci_os_logx("  gPState", (UInt32)gDev[0].pstate);
            ehci_os_logx("  &gPState", (unsigned long)(void *)&gDev[0].pstate);
            ehci_os_logx("  gReprobe", (UInt32)gReprobe);
            ehci_os_logx("  gPOut", (UInt32)(long)gDev[0].pOut); ehci_os_logx("  gPIn", (UInt32)(long)gDev[0].pIn);
            ehci_os_logx("  v18 gSubmitReentry", (UInt32)gSubmitReentry);
            ehci_os_logx("  v18 gSubmitMaxDepth", (UInt32)gSubmitMaxDepth);
            /* v36: the FM-race smoking gun. gSuspN>0 with flag bit0 => the File Manager re-issued a WRITE from
             * inside our completion (the re-entrancy the OS 9 File Manager source model predicts); bit1 => that
             * write's LBA was off the device (a clobbered File-Mgr Params offset reached us). Never-wraps, so it
             * survives the whole folder copy even though the 'Ucsl' ring holds only the last 512 I/Os. */
            ehci_os_logx("  v36 gInCompletion (now)", (UInt32)gInCompletion);
            ehci_os_logx("  v36 gWrTotal (writes seen)", (UInt32)gWrTotal);
            ehci_os_logx("  v36 gSuspN (SUSPICIOUS writes)", (UInt32)gSuspN);
            { UInt32 _si, _sm = (gSuspN < 16u) ? gSuspN : 16u;
              for (_si = 0; _si < _sm; _si++) {
                ehci_os_log("  v36 SUSPICIOUS WRITE (flags 1=reentrant 2=off-device):");
                ehci_os_logx("    lba",   gSuspLba[_si]);
                ehci_os_logx("    nblk",  gSuspCnt[_si]);
                ehci_os_logx("    flags", gSuspFlags[_si]);
              } }
            ehci_os_logx("  v18 gComplDrop", (UInt32)gComplDrop);
            /* v19 diag: ring occupancy + queue-full rejections + engine drain progress.
             * hiWater==BIOQ_N(16) => the ring is the bottleneck; reject>0 => DM over-issued (grow ring);
             * hiWater==16 with reject==0 while WrOk/RdOk FLATLINE across samples => engine stall (dropped kick);
             * hiWater stays low + dialog still fires => neither, look above the ring (Status/Control calls). */
            ehci_os_logx("  v19 gBioHiWater", (UInt32)gBioHiWater);
            ehci_os_logx("  v19 gBioReject", (UInt32)gBioReject);
            ehci_os_logx("  v19 gBioWrOk", (UInt32)gBioWrOk);
            ehci_os_logx("  v19 gBioRdOk", (UInt32)gBioRdOk);
            /* v21 engine-wedge discriminator (the copy stalls with NO transport error -> read the engine state):
             * gBioPhase!=0 & gDpBusy=0 => a transfer COMPLETED but bio_advance never ran (routing/completion bug);
             * gBioPhase!=0 & gDpBusy=1 => transfer STUCK in flight (watchdog should have fired but didn't);
             * gBioPhase=0 & gBioHead!=gBioTail => DROPPED KICK (a request sits in the ring, never started). */
            ehci_os_logx("  v21 gBioPhase", (UInt32)gBioPhase);
            ehci_os_logx("  v21 gBioHead", (UInt32)gBioHead);
            ehci_os_logx("  v21 gBioTail", (UInt32)gBioTail);
            ehci_os_logx("  v21 gDpBusy", (UInt32)gDpBusy);
            /* v22: FM-level Prime trace from the block driver's gCsLog (Gestalt 'Ucsl'). nWrites > gBioWrOk
             * ⇒ the failing write REACHED us and was lost (our path); nWrites == gBioWrOk ⇒ it never reached
             * us ⇒ error is ABOVE us (HFS / wrong-read-data). */
            if (!gCsLogPtr) { long _v; if (Gestalt('Ucsl', &_v) == noErr && _v) {
                CsLogMirror *_p = (CsLogMirror *)_v; if (_p->magic == 0x5563736cUL) gCsLogPtr = _p; } }
            if (gCsLogPtr) {
                UInt32 _io = (UInt32)(gBioWrOk + gBioRdOk);
                ehci_os_logx("  v22 FM nReads", (UInt32)gCsLogPtr->nReads);
                ehci_os_logx("  v22 FM nWrites", (UInt32)gCsLogPtr->nWrites);
                ehci_os_logx("  v22 FM count", (UInt32)gCsLogPtr->count);
                if (!gCsDumped && gBioPhase == 0 && gBioHead == gBioTail && _io > 0 && _io == gCsLastIo) {
                    UInt32 _n = gCsLogPtr->count, _i, _show = 16u;   /* stalled: dump the last records ONCE */
                    gCsDumped = 1;
                    ehci_os_log("!! v22 STALL — last FM Primes (kind 1=stat 2=ctl 3=rd 4=wr / blk / nblk / iokind):");
                    for (_i = 0; _i < _show && _i < _n; _i++) {
                        CsRecMirror *_r = &gCsLogPtr->recs[(_n - 1u - _i) & 511u];
                        ehci_os_logx("  kind", (UInt32)(short)_r->kind);
                        ehci_os_logx("   blk", (UInt32)_r->p0);
                        ehci_os_logx("   nblk", (UInt32)(short)_r->csCode);
                        ehci_os_logx("   iokind", (UInt32)_r->p1);
                    }
                    /* v25: also dump the block driver's DoDriverIO control-plane trace (Gestalt 'Ucs2').
                     * codes: 5=Open 6=Close 7=Read 8=Write 9=Control 10=Status 11=KillIO (1=Init 2=Finalize).
                     * err 1 = kIOBusyStatus (accepted async). Look for a Status/Control returning non-0, or a
                     * KillIO, right before the abort. */
                    if (!gDioLogPtr) { long _dv; if (Gestalt('Ucs2', &_dv) == noErr && _dv) {
                        DioLogMirror *_dp = (DioLogMirror *)_dv; if (_dp->magic == 0x44696f4cUL) gDioLogPtr = _dp; } }
                    if (gDioLogPtr) {
                        UInt32 _dn = gDioLogPtr->count, _di, _dshow = 48u;
                        ehci_os_log("!! v25 DoDriverIO trace (code / err / aux=csCode for ctl+stat), most-recent first:");
                        for (_di = 0; _di < _dshow && _di < _dn; _di++) {
                            DioRecMirror *_dr = &gDioLogPtr->recs[(_dn - 1u - _di) & 127u];
                            ehci_os_logx("  code", (UInt32)(short)_dr->code);
                            ehci_os_logx("   err", (UInt32)(short)_dr->err);
                            ehci_os_logx("   aux", (UInt32)_dr->aux);
                        }
                    }
                }
                gCsLastIo = _io;
            }
            ehci_os_logx("  v23 gRawChecks", (UInt32)gRawChecks);
            ehci_os_logx("  v23 gRawMismatch", (UInt32)gRawMismatch);
            ehci_os_logx("  v24 gStaleN", (UInt32)gStaleN);
            if (gRawMPending) {
                ehci_os_log("!! v23 READ-AFTER-WRITE MISMATCH (read-back != written data — HFS's just-written block came back stale/wrong):");
                ehci_os_logx("  lba", gRawMLba); ehci_os_logx("  wroteFP", gRawMExp); ehci_os_logx("  readFP", gRawMAct);
                gRawMPending = 0;
            }
            if (gStalePending) {
                ehci_os_log("!! v24 STALE READ (read returned the LAST WRITE's data for a DIFFERENT block = stale gDownBuf):");
                ehci_os_logx("  readLba", gStaleRLba); ehci_os_logx("  wroteLba", gStaleWLba);
                ehci_os_logx("  readWord0", gStaleRW0); ehci_os_logx("  wroteWord0", gStaleWW0);
                gStalePending = 0;
            }
        }
    }
    if (EHCI_VERBOSE && gBurst > 0) {   /* r90: reconnect-triggered burst — log the self-probe's actual post-reconnect path */
        gBurst--;
        ehci_os_log("r90 burst:"); ehci_os_logx("  gPState", (UInt32)gDev[0].pstate);
        ehci_os_logx("  gReprobe", (UInt32)gReprobe); ehci_os_logx("  gFence", (UInt32)gFenceApple);
        ehci_os_logx("  gPOut", (UInt32)(long)gDev[0].pOut); ehci_os_logx("  gPIn", (UInt32)(long)gDev[0].pIn);
    }
    switch (gDev[0].pstate) {
    case 0:   /* wait for the mounter to exercise both bulk eps then go permanently quiet (parked) */
        pb_find_eps(0);
        if (gReprobe && gDev[0].pOut >= 0 && gDev[0].pIn >= 0) {   /* r95: the SIH watcher (sih_reconnect_arm) armed this. It fires ONLY
                                                      * after Apple has gone QUIET (parked) AND been fenced, so taking over
                                                      * now is safe — no active Apple transfer to collide with. This runs the
                                                      * instant ExpertIdleTask releases the task level after the SIH fenced.
                                                      * (r93 disabled the old r88 assertive path because it fired while Apple
                                                      * was still active; the SIH arm-gate is what makes it safe now.) */
            gReprobe = 0; gFenceApple = 1;
            ehci_os_log("=== SELFPROBE: SIH-armed reconnect takeover (Apple parked+fenced) ===");
            ehci_os_logx("  gSihArmTick", (UInt32)gSihArmTick); ehci_os_logx("  gVhubTick(now)", (UInt32)gVhubTick);
            ehci_os_logx("  outEp.addr", gBulkEP[gDev[0].pOut].addr); ehci_os_logx("  inEp.addr", gBulkEP[gDev[0].pIn].addr);
            /* v49: the device may be mid-BOT from Apple's abandoned probe, so reset the transport and put
             * both bulk toggles back to DATA0 before we issue our first CBW on the taken-over endpoints. */
            ehci_os_logx("  v49 takeover BOT reset rc (0=ok)", (UInt32)(long)pb_bot_reset());
            pb_cbw(0, cdbInq, 6, 36); gDev[0].pstate = 1; break;
        }
        if (EHCI_VERBOSE) {   /* r31 DIAGNOSTIC: periodically log WHY we haven't parked — cracks the intermittent no-mount
             * (user confirms identical insertion every time, so the variance is intrinsic timing). Read
             * on a stalled run: gDev[0].pOut/gDev[0].pIn = ffffffff => endpoints never created (enum didn't start);
             * bulkCnt stuck + gPIdle NOT climbing + vhubTick FROZEN => heartbeat died (root cause);
             * gPIdle climbing but never parks => park-detection logic. Bounded to ~every 256 ticks. */
            static UInt32 gDbgN = 0;
            if ((gDbgN++ & 0xFF) == 0) {
                ehci_os_log("SELFPROBE wait(state0):");
                ehci_os_logx("  gPOut", (UInt32)(long)gDev[0].pOut);
                ehci_os_logx("  gPIn",  (UInt32)(long)gDev[0].pIn);
                ehci_os_logx("  bulkCnt", gBulkDoneN + gBulkErrN);
                ehci_os_logx("  gPIdle", gPIdle);
                ehci_os_logx("  vhubTick", gVhubTick);
            }
        }
        if (gDev[0].pOut >= 0 && gDev[0].pIn >= 0) {
            UInt32 c = gBulkDoneN + gBulkErrN;
            if (c == gPLastCnt) gPIdle++; else { gPIdle = 0; gPLastCnt = c; }
            if (c >= 2 && gPIdle > 40) {          /* BOT ran (>=2 xfers) + ~40 quiet ticks => parked */
                gFenceApple = 1;                  /* r32: from here on, fence the Apple driver off our shared endpoints */
                ehci_os_log("=== SELFPROBE: mounter parked; taking over bulk endpoints (Apple fenced) ===");
                ehci_os_logx("  outEp.addr", gBulkEP[gDev[0].pOut].addr); ehci_os_logx("  outEp.ep", gBulkEP[gDev[0].pOut].endpt);
                ehci_os_logx("  inEp.addr",  gBulkEP[gDev[0].pIn].addr);  ehci_os_logx("  inEp.ep",  gBulkEP[gDev[0].pIn].endpt);
                pb_cbw(0, cdbInq, 6, 36); gDev[0].pstate = 1;
            }
        }
        break;
    case 1: if (pb_ready()) { pb_in(0, 36); gDev[0].pstate = 2; } break;                 /* INQUIRY data */
    case 2: if (pb_ready()) {
                ehci_os_log("SELFPROBE INQUIRY:");
                ehci_os_logx("  periphType", gPB[0]);
                ehci_os_logx("  vendor0_3", PB_BE32(8));  ehci_os_logx("  vendor4_7", PB_BE32(12));
                ehci_os_logx("  prod0_3",   PB_BE32(16)); ehci_os_logx("  prod4_7",   PB_BE32(20));
                pb_in(0, 13); gDev[0].pstate = 3;                                        /* INQUIRY CSW */
            } break;
    case 3: if (pb_ready()) { pb_cbw(0, cdbCap, 10, 8); gDev[0].pstate = 4; } break;     /* READ CAPACITY */
    case 4: if (pb_ready()) { pb_in(0, 8); gDev[0].pstate = 5; } break;
    case 5: if (pb_ready()) {
                gPBlkCnt  = PB_BE32(0) + 1;   /* returned last-LBA + 1 */
                gPBlkSize = PB_BE32(4);
                if (gPBlkSize == 0 || gPBlkSize > 512) gPBlkSize = 512;        /* clamp the read */
                ehci_os_log("SELFPROBE READ CAPACITY:");
                ehci_os_logx("  blocks", gPBlkCnt); ehci_os_logx("  blockSize", gPBlkSize);
                pb_in(0, 13); gDev[0].pstate = 6;                                        /* CAPACITY CSW */
            } break;
    case 6: if (pb_ready()) {
                for (i = 0; i < 10; i++) cdbRd[i] = 0;
                cdbRd[0] = 0x28; cdbRd[8] = 1;    /* READ(10), LBA 0, transfer length 1 block */
                pb_cbw(0, cdbRd, 10, gPBlkSize); gDev[0].pstate = 7;
            } break;
    case 7: if (pb_ready()) { pb_in(0, gPBlkSize); gDev[0].pstate = 8; } break;          /* block-0 data */
    case 8: if (pb_ready()) {
                ehci_os_log("SELFPROBE READ block 0:");
                ehci_os_logx("  b0_3",   PB_BE32(0));
                ehci_os_logx("  sig510", ((UInt32)gPB[510]<<8)|gPB[511]);      /* FAT boot sig 55 AA */
                pb_in(0, 13); gDev[0].pstate = 9;                                        /* block-0 CSW */
            } break;
    case 9: if (pb_ready()) { ehci_os_log("=== SELFPROBE COMPLETE — we read the disk ourselves ===");
                              /* r35 R2a: after ~10 self-probe transfers, did the real EHCI IRQ fire? */
                              ehci_os_logx("  gIsrHits (real IRQ fired during selfprobe; 0 = heartbeat-only)", gIsrHits);
                              ehci_os_logx("  v46 gIsrConsecMax (shared-IRQ storm peak)", gIsrConsecMax);
                              ehci_vhub_publish_service(); gDev[0].pstate = 10; gMountedOnce = 1; } break;   /* m2: expose block-read; r87: arm reconnect logic */
    default: break;   /* 10 = done */
    }
}

/* ==================== dispatch slot 3: control transfer ====================
 * devAddr == root hub  -> simulate the root hub (fabricate descriptors, service port requests).
 * devAddr != root hub  -> a real downstream device: run the per-phase transfer on the EHCI.
 * Per-phase (pid 2=SETUP / 1=IN / 0=OUT), one completion each (proven contract). */
long ehci_vhub_control_xfer(void *pipe, void *complUPP, volatile UInt8 *buf, UInt32 devAddr, UInt32 len, UInt32 pid)
{
    if (devAddr == gRootHubAddr) {
        if (pid == 2 && (UInt32)buf >= 0x1000UL) {                  /* SETUP */
            int i; for (i = 0; i < 8; i++) gSetup[i] = buf[i]; gHaveSetup = 1;
            if (gSetup[0] == 0x00 && gSetup[1] == 0x05) gRootHubAddr = gSetup[2];   /* SET_ADDRESS */
            else if (gSetup[0] == 0x23 && (gSetup[1] == 0x03 || gSetup[1] == 0x01)) { /* SET/CLEAR_FEATURE(port) */
                UInt8 feat = gSetup[2], port = gSetup[4]; int p = port - 1;
                if (p >= 0 && p < gSoftc.nPorts) {
                    if (gSetup[1] == 0x03) {                        /* SET_FEATURE */
                        if (feat == FEAT_PORT_RESET) {
                            UInt32 v = ehci_read32(gSoftc.opBase, EHCI_PORTSC(p)) & ~EHCI_PORTSC_RW1C;
                            v &= ~EHCI_PORT_ENABLE; v |= EHCI_PORT_RESET;
                            ehci_write32(gSoftc.opBase, EHCI_PORTSC(p), v);
                            gResetAtFrame[p] = frame_ms() + 50; gResetPending[p] = 1; gResetSeen = 1;
                            gResetEnabling[p] = 0;   /* r38: a fresh reset supersedes any pending enable-wait */
                            pevt((UInt8)p, PEV_RST_ASSERT, ehci_read32(gSoftc.opBase, EHCI_PORTSC(p)));   /* r36 diag */
                            VLOG("vhub: SET_FEATURE(PORT_RESET)");
                        } else if (feat == FEAT_PORT_POWER) {
                            UInt32 v = ehci_read32(gSoftc.opBase, EHCI_PORTSC(p)) & ~EHCI_PORTSC_RW1C;
                            v |= EHCI_PORT_POWER; ehci_write32(gSoftc.opBase, EHCI_PORTSC(p), v);
                        }
                    } else {                                        /* CLEAR_FEATURE */
                        if (feat == FEAT_C_PORT_CONNECTION) gPortChange[p] &= ~HPC_CONNECTION;
                        else if (feat == FEAT_C_PORT_RESET) gPortChange[p] &= ~HPC_RESET;
                        else if (feat == FEAT_C_PORT_ENABLE) gPortChange[p] &= ~HPC_ENABLE;
                    }
                }
            }
        } else if (pid == 1 && (UInt32)buf >= 0x1000UL && gHaveSetup) { /* IN data: fabricate */
            int filled = 0;                                          /* ★ h30, see the else below */
            if (gSetup[1] == 0x06) {                                /* GET_DESCRIPTOR */
                if      (gSetup[0] == 0x80 && gSetup[3] == 0x01) { fill(buf, gDevDesc, 18, len); filled = 1; }
                else if (gSetup[0] == 0x80 && gSetup[3] == 0x02) { fill(buf, gCfgDesc, 25, len); filled = 1; }
                else if (gSetup[0] == 0xA0 && gSetup[3] == 0x29) { fill(buf, gHubDesc, 9, len); filled = 1; }
                else if (gSetup[0] == 0x80 && gSetup[3] == 0x03) { /* STRING */
                    if (gSetup[2] == 0) { if (len >= 4) { buf[0]=0x04; buf[1]=0x03; buf[2]=0x09; buf[3]=0x04; } }
                    else                { if (len >= 2) { buf[0]=0x02; buf[1]=0x03; } }
                    filled = 1;
                }
            } else if (gSetup[1] == 0x00) {                         /* GET_STATUS */
                if (gSetup[0] == 0xA3) { int p = gSetup[4] - 1; if (p >= 0 && p < gSoftc.nPorts) ehci_vhub_port_status(p, buf); }
                else if (gSetup[0] == 0xA0) { buf[0]=buf[1]=buf[2]=buf[3]=0; }        /* hub status */
                else { buf[0] = 0x01; buf[1] = 0; }                 /* device status: SELF-POWERED (v0.19 fix) */
                filled = 1;
            }
            /* ★★★★★ h30 — AN UNHANDLED IN REQUEST MUST NOT COMPLETE WITH THE CALLER'S BUFFER UNTOUCHED.
             * Until now, any request this fabricator did not recognise — GET_DESCRIPTOR types beyond
             * device/config/hub-class/string (DEVICE_QUALIFIER…), GET_CONFIGURATION, GET_INTERFACE, any
             * class request the boot path never issues — fell through to enq_compl and reported SUCCESS
             * while the caller's buffer still held whatever garbage was there. A caller that then PARSES
             * the "descriptor" (Apple System Profiler's Devices & Volumes scan walks every USB bus asking
             * exactly such questions) chases fabricated lengths and pointers into an illegal instruction.
             * ⚠ This is the SAME disease the block driver cured in r37 ("returning noErr … and leaving it
             * unset was the original DriverGestalt crash"), one layer down, and it is in every shipped ROM
             * incl. m3 — which matches ASP crashing on BOTH machines.
             * ZERO the buffer instead: bLength=0 parses as "no descriptor here" and every standard caller
             * stops cleanly. And LOG the request (interrupt-safe ring), so the very next ASP run tells us
             * exactly what it asked for — instrumentation and safety in one change. */
            if (!filled) {
                UInt32 z; for (z = 0; z < len; z++) buf[z] = 0;
                ehci_os_ilogx("h30: UNHANDLED root-hub IN request answered with ZEROS (was: garbage); "
                              "bmRT|bReq|wValLo|wValHi",
                              ((UInt32)gSetup[0] << 24) | ((UInt32)gSetup[1] << 16) |
                              ((UInt32)gSetup[2] << 8)  |  (UInt32)gSetup[3]);
            }
        } else if (pid == 0) { gHaveSetup = 0; }                    /* status stage */
    }
    else if (gResetSeen) {                                          /* real downstream device */
        if (gDownReady && ((pid == 2) ? ((UInt32)buf >= 0x1000UL) : (len == 0 || (UInt32)buf >= 0x1000UL))) {
            down_submit(pipe, complUPP, buf, devAddr, len, pid, -1);  /* -1 = control (ep0) */
            return 0;                                               /* DEFER: completed by the service */
        }
    }
    enq_compl(complUPP, pipe);                                      /* root-hub phase completes immediately */
    return 0;
}

/* ==================== dispatch slot 11: hold the root hub's status-change interrupt pipe ==================== */
long ehci_vhub_int_xfer(UInt32 devAddr, UInt32 endpt, void *refcon, void *callback, volatile UInt8 *buf, UInt32 len)
{
    (void)endpt;
    if (devAddr == gRootHubAddr) {
        gIntRefcon = refcon; gIntCB = callback; gIntBuf = (void *)buf; gIntLen = len; gIntPending = 1;
    }
    /* r97 REVERTED (2026-07-20): delivering an already-pending port change here (to make Apple
     * enumerate a device carried over from the companion) HUNG the machine at boot — heartbeat froze
     * at vhubTick ~41 (interrupt-level hang), likely firing the status-change completion into Apple's
     * hub driver at a moment it couldn't take it. This is the SAME companion<->EHCI transition wall we
     * hit for hot-plug (ExpertIdleTask monopoly) and INIT-preempt (MMIO crash). See topic memory. */
    return 0;
}

/* ==================== interrupt/timer service ====================
 * Called from the real EHCI interrupt handler AND a periodic timer (both installed by the ndrv):
 * acknowledge controller events, run the root-port state machine (connect detect + reset deassert),
 * then deliver queued completions (root-hub phases, the held status-change poll, downstream reaps). */
static UInt32 hub_int_ack(void)
{
    UInt32 sts = ehci_read32(gSoftc.opBase, EHCI_USBSTS);
    UInt32 evt = sts & (EHCI_STS_USBINT | EHCI_STS_USBERRINT | EHCI_STS_PCD | EHCI_STS_HSE | EHCI_STS_IAA);
    if (evt) ehci_write32(gSoftc.opBase, EHCI_USBSTS, evt);   /* RW1C ack */
    return evt;
}
static void service_ports(void)
{
    int p, changed = 0, np = gSoftc.nPorts;
    for (p = 0; p < np && p < 15; p++) {
        UInt32 pv = ehci_read32(gSoftc.opBase, EHCI_PORTSC(p));
        UInt8 conn = (pv & EHCI_PORT_CONNECT) ? 1 : 0;
        if (conn != gPortConn[p]) { gPortConn[p] = conn; gPortChange[p] |= HPC_CONNECTION; changed = 1; gPortEvent[p] = conn ? 1 : 2;
            pevt((UInt8)p, conn ? PEV_CONNECT : PEV_DISCONN, pv);   /* r36 diag: a DISCONN straddling reset = the bounce */
#if APPLE_HIDE
            /* n4b: NO File-Manager logging here — this is INTERRUPT level and ehci_os_log is synchronous
             * File Manager I/O (the r18 hang; it hung the machine on first connect in n4). Flag it and let
             * selfprobe_tick log it at task level. */
            if (conn && apple_hidden_port(p)) gHideLogMask |= (1UL << p);
            /* p1a: arm/disarm OUR OWN enumeration. Interrupt level: flags only, no work here — the n5
             * engine (as_tick, driven from the heartbeat) does the reset/descriptors/SET_ADDRESS. A
             * reinsert re-arms it, which is how hot-plug re-enumeration falls out for free.
             *
             * ★★★ n11: SKIP A PORT WE HAVE CEDED. This block is the engine of the hub thrash loop. Once the
             * port belongs to the companion, EHCI reports CCS = 0 for it — which arrives here as a connect
             * DISCONNECT transition and used to mean "our enumerated device was pulled": it set
             * gSelfEnumRearm (spurious media-gone + probe reset at task level) and cleared gSelfEnumDone,
             * so the next pass re-armed enumeration on a port we had deliberately given away. 148 rounds of
             * that in one session. A ceded port generates no arm, no disarm and no rearm — the device is
             * Apple's now and its comings and goings are not our business. */
            /* ★★★★ h17 BUG 2: UN-PARK A PARKED PORT WHOSE DEVICE HAS LEFT — FROM OUTSIDE THE CEDED GATE.
             * n24 sweep finding 2 added exactly this, and it has been DEAD CODE since the day it was written.
             * The un-park sat inside the `apple_hidden_port(p) && !port_ceded(p)` block below, and
             * port_ceded() returns 1 when gPortParked[p] is set (see its definition) — so the instant a port
             * was parked, the gate went false and every later event on that port was skipped, INCLUDING the
             * un-park meant to revive it. A parked port could never be un-parked.
             * The n24 comment states the very reasoning this violates: "unlike a CEDED port … a PARKED port is
             * still OURS and its disconnect IS visible right here — that asymmetry is what makes this
             * fixable." port_ceded() erases the asymmetry by conflating parked with ceded, so the fix could
             * not see the disconnect it was written to catch.
             * Seen on hardware (h16): the fourth drive's enumeration failed 3x while a copy was starving its
             * control transfers, the port was parked — and it then stayed dead for the rest of the session,
             * through a physical re-plug, because this code was unreachable.
             * ⇒ Third time in this project a fix turned out to be unreachable code, after DoDriverIO and
             * ehci_os_boot_quiesce. A `park` and a `cede` are DIFFERENT STATES and must not share a test. */
            if (!conn && gPortParked[p]) {
                gPortParked[p] = 0;
                gPortUnparkMask |= (1UL << (p & 0x0F));   /* task level logs it */
            }
            /* ★★★★★★ h53 — UN-CEDE A PORT THE COMPANION HAS HANDED BACK. THE APPLE_HIDE LEAK, FOUND.
             *
             * ⚠ THE T5b PHASE 3b RUN (2026-08-10) caught it: cede port 1 to the keyboard (PORTSC 0x3800),
             * unplug the keyboard (0x3800 -> 0x1000, ownership auto-reverted with no write of ours), then plug
             * a DRIVE into that same port. We took the port and enabled it at high speed (0x1005) — and
             * APPLE ENUMERATED THE SAME DRIVE, assigning address 0x2F and asking for bulk endpoints 1 and 2,
             * the Bulk-Only mass-storage pair. h45's guard refused both (gH45Refused 0 -> 2) so it was a clean
             * non-mount instead of the h44-era FREEZE, but two stacks reached one medium.
             *
             * ★ WHY. apple_hidden_port() reads:
             *       if (gPortParked[p]) return 1;
             *       if (gPortCeded[p])  return 0;      <- a CEDED port is deliberately VISIBLE to Apple
             *       return (PORTSC & OWNER) ? 0 : 1;
             * gPortCeded[p] is set when we cede and, per the n11 lesson (keep the intent in software, never
             * re-read the decision out of a register), is never cleared by a hardware disagreement. That was
             * right for the case n11 fixed. But THIS CHIP AUTO-REVERTS OWNERSHIP ON DISCONNECT, so after the
             * HID leaves, the hardware says "yours again" while our software still says "Apple's" — the port
             * stays UN-HIDDEN and Apple enumerates whatever is plugged in next.
             *
             * ★★ n24 hit this exact asymmetry ONE VARIABLE OVER — gPortParked was permanent and needed the
             * un-park directly above — and reasoned that a CEDED port's removal was invisible "because CCS
             * reads 0 either way". The h30 chip fact overturns that premise: **bit 13 CLEARING is the visible
             * signal**, so the hand-back IS detectable, right here, from the same PORTSC read.
             *
             * ⚠ NOTE THE CONDITION IS THE OWNER BIT, NOT !conn. A ceded port with a device still attached
             * reads CCS 0 anyway (that is the n11 blindness), so keying off `conn` would fire while the
             * keyboard was still plugged in and un-hide a port that really is Apple's. Only the companion
             * releasing OWNER means it is ours again.
             * ★ And this sits OUTSIDE the `apple_hidden_port(p) && !port_ceded(p)` gate below — deliberately.
             * port_ceded() returns 1 for a ceded port, so anything inside that gate is unreachable for exactly
             * the ports this fix exists to rescue. That was h17's bug 2 and it made n24's un-park dead code for
             * builds; the same trap, one variable over, and this is the fourth unreachable-fix in the project. */
            if (gPortCeded[p] && !(pv & EHCI_PORT_OWNER)) {
                gPortCeded[p] = 0;
                gPortUncedeMask |= (1UL << (p & 0x0F));   /* task level logs it */
                gH53Unceded++;
            }
            if (apple_hidden_port(p) && !port_ceded(p)) {
                if (conn) {
                    /* ★ n13: do NOT let a newcomer seize the state of a device we have already probed.
                     * This engine holds exactly one device's worth of state (one gAs, one endpoint pair,
                     * one gDev[0].pstate), so a second drive cannot be driven yet — but it must not CORRUPT the
                     * first one either, which is what used to happen and what killed the whole session. */
                    /* ★ n14 step 3: a newcomer on a different port now gets ENUMERATED into its own
                     * slot, rather than merely counted (n13) or allowed to corrupt the mounted device's
                     * state (n12). Only when every slot is taken do we fall back to ignoring it.
                     * gSelfEnumPort/gSelfEnumDone are enumeration-scoped, not "the" device: they schedule
                     * which port is enumerated next, and the mounted device's own slot is untouched. */
                    dead_ring_clear();                       /* h94 FAIL-OPEN: a new arrival may be assigned a
                                                              * recycled address; a stale "dead" entry must
                                                              * never refuse a live device's traffic */
                    if (dev_alloc() < 0) {
                        gSecondDevMask |= (1UL << p);        /* no free slot; state left untouched */
                    } else {
                        gSelfEnumPort = p; gSelfEnumDone = 0; gSelfEnumTries = 0;
                    }
                } else {
                    /* ★ n13: the disconnect that requires cleanup is the one on the port we actually
                     * PROBED — keyed off gProbedPort, not off gSelfEnumDone, which a second device's
                     * connect can clear out from under us. Missing this left gDev[0].pstate latched at 10. */
                    /* ★ n15: free whichever SLOT was on this port, and release its endpoint registrations,
                     * so the slot and its bulk QH pair can serve the next device. Keyed off the slot's own
                     * probedPort — the n13 lesson — so a newcomer cannot make us clean up the wrong one. */
                    { int d, k, freed = -1;
                      for (d = 0; d < USB_MAX_DEV; d++) {
                          UInt32 dAddr;                      /* h94: the address the departed device answered to */
                          if (!gDev[d].inUse || gDev[d].probedPort != p) continue;
                          dAddr = gDev[d].curAddr;
                          for (k = 0; k < NBULK; k++)
                              if (gBulkEP[k].used && gBulkEP[k].dev == (UInt8)d) {
                                  if (!dAddr) dAddr = gBulkEP[k].addr;   /* Apple-era device: the endpoint knows */
                                  gBulkEP[k].used = 0;
                              }
                          gRearmDev = d;      /* n19 step 3: tell the task-level rearm WHICH drive went */
                          freed = d;          /* n23: remember that a slot WE OWNED just lost its device */
                          gDev[d].inUse = 0; gDev[d].probedPort = -1;
                          gDev[d].pOut = -1; gDev[d].pIn = -1;
                          if (d != 0) gDev[d].pstate = 0;    /* slot 0's teardown is the proven path below */
                          usl_retire_device(d, dAddr);       /* h94: retire the dead device's client transfers
                                                              * BEFORE the USL hears of the removal and frees */
                      }
                    /* ★★★★ n23 FIX B: report the removal whenever a slot we owned lost its device.
                     * This used to hinge on gSelfEnumDone for any slot other than 0 — and the PREVIOUS rearm
                     * clears gSelfEnumDone, so in practice a disconnect on slot 1's port reported NOTHING:
                     * no media-gone to the block driver, no unmount, and gRearmDev left pointing at the old
                     * slot. The n22 log shows it plainly: every disconnect on port 4 (slot 0) produced a
                     * rearm, and all four on port 2 (slot 1) produced silence. `freed` is the honest
                     * condition — we know we owned that port, so we do not need to infer it from a global.
                     * (The old slot-0 test `p == gDev[0].probedPort` was also already dead by this point,
                     * because the loop above has just set that field to -1.) */
                      /* ★★★★ h12: gRearmDev MUST say "nothing of ours" when nothing of ours was on this port.
                       * The loop above only assigns gRearmDev when it finds a slot whose probedPort matches,
                       * so on a disconnect that frees NO slot it kept whatever value the PREVIOUS removal
                       * left — and the task-level rearm then ran blk_notify_media(stale, 0), marking some
                       * OTHER drive's media gone, after which the (correct) selective unmount dutifully
                       * unmounted it.
                       * Seen on hardware: with both slots full, a third drive was plugged into a ROOT port and
                       * refused for want of a slot (so it owned no slot); pulling it out again unmounted one of
                       * the two drives behind the HUB. I identified this staleness while writing n23 and did
                       * not act on it. Making it explicit is the fix: -1 means "no device of ours left". */
                      gRearmDev = freed;
                      if (freed >= 0) {
                          gSelfEnumRearm = 1;
                          if (freed == 0) gDev[0].probedPort = -1;
                      } else if (gSelfEnumDone) {
                          gSelfEnumRearm = 1;                /* re-arm enumeration, but there is no drive to
                                                              * tear down — gRearmDev = -1 says exactly that */
                      } }
                    /* ★★★★ n24 SWEEP FINDING 2 USED TO LIVE HERE — and could never run. This block is inside
                     * `apple_hidden_port(p) && !port_ceded(p)`, and port_ceded() reports a PARKED port as
                     * ceded, so by the time gPortParked[p] could be set the gate excluding us was already
                     * false. h17 moved the un-park ABOVE that gate, where a parked port's disconnect is
                     * actually visible; see the long note there. Deleted rather than left in place, because a
                     * second copy that cannot run is precisely the trap that cost the h16 hardware cycle. */
                    /* ★★★★ h3 TIER 1: THE HUB'S OWN ROOT PORT WENT AWAY. Everything behind it went with it,
                     * so free every slot marked viaHub and forget the hub. Written at the same time as the
                     * claim rather than discovered on hardware — "who resets this when the device leaves" is
                     * the question that cost n19-n23 five builds, so it gets answered up front here. */
                    if (gHub.claimed && p == gHub.rootPort) {
                        int d, k;
                        for (d = 0; d < USB_MAX_DEV; d++) {
                            UInt32 dAddr;                  /* h94: as in the root-port loop above */
                            if (!gDev[d].viaHub) continue;
                            dAddr = gDev[d].curAddr;
                            for (k = 0; k < NBULK; k++)
                                if (gBulkEP[k].used && gBulkEP[k].dev == (UInt8)d) {
                                    if (!dAddr) dAddr = gBulkEP[k].addr;
                                    gBulkEP[k].used = 0;
                                }
                            gRearmDev = d;                 /* so the task-level rearm unmounts THIS drive */
                            gSelfEnumRearm = 1;
                            gDev[d].inUse = 0; gDev[d].viaHub = 0; gDev[d].hubPort = 0;
                            gDev[d].probedPort = -1; gDev[d].pOut = -1; gDev[d].pIn = -1;
                            if (d != 0) gDev[d].pstate = 0;
                            usl_retire_device(d, dAddr);   /* h94: everything behind the hub left too */
                        }
                        gHub.claimed = 0; gHub.rootPort = -1; gHub.nPorts = 0; gHub.scanPort = 0; gHub.skipMask = 0;
                        hub_int_stop();   /* h10 */
                        gHubGoneMask |= (1UL << (p & 0x0F));       /* task level logs it */
                    }
                    if (p == gSelfEnumPort) { gSelfEnumPort = -1; gSelfEnumDone = 0; gSelfEnumTries = 0; }
                }
            }
#endif
            }
        if (pv & (EHCI_PORT_CONNECT_CH | EHCI_PORT_ENABLE_CH))     /* clear hardware change bits (RW1C) */
            ehci_write32(gSoftc.opBase, EHCI_PORTSC(p), (pv & ~EHCI_PORTSC_RW1C) | (pv & (EHCI_PORT_CONNECT_CH | EHCI_PORT_ENABLE_CH)));
        if (gResetPending[p] && frame_ms() >= gResetAtFrame[p]) {  /* ~50 ms elapsed -> deassert reset */
            UInt32 w = ehci_read32(gSoftc.opBase, EHCI_PORTSC(p)) & ~EHCI_PORTSC_RW1C; w &= ~EHCI_PORT_RESET;
            ehci_write32(gSoftc.opBase, EHCI_PORTSC(p), w);
            gResetPending[p] = 0;
            gResetEnabling[p] = 1; gEnableDeadline[p] = frame_ms() + 30;   /* r38: now wait for the port to ENABLE */
            pevt((UInt8)p, PEV_RST_DEASS, ehci_read32(gSoftc.opBase, EHCI_PORTSC(p)));   /* r36 diag */
        }
        /* r38 FIX (root cause of the r37-run2 no-mount: every downstream transfer timed out). After
         * deasserting reset, report reset-complete to the hub driver (HPC_RESET) ONLY once the port has
         * actually ENABLED — i.e. the EHCI high-speed chirp handshake finished and the device is on the
         * HS bus — or the port was released to a companion (FS/LS), or we hit a ~30ms safety cap. The old
         * code set HPC_RESET the instant it deasserted, so the hub driver's SET_ADDRESS/GET_DESCRIPTOR
         * went out BEFORE the device was reachable => all transfers timed out => no enumeration => no
         * mount. Intermittent because the enable sometimes landed within the old zero-wait window. */
        else if (gResetEnabling[p]) {
            UInt32 pe = ehci_read32(gSoftc.opBase, EHCI_PORTSC(p));
            if (((pe & (EHCI_PORT_RESET | EHCI_PORT_ENABLE)) == EHCI_PORT_ENABLE) ||   /* reset done + HS-enabled */
                (pe & EHCI_PORT_OWNER) ||                                              /* released to a companion */
                frame_ms() >= gEnableDeadline[p]) {                                    /* safety cap */
                gResetEnabling[p] = 0; gPortChange[p] |= HPC_RESET; changed = 1; gPortResetDone[p] = 1;
                pevt((UInt8)p, PEV_ENABLED, pe);   /* r38 diag: PORTSC when we finally report reset-complete */
            }
        }
    }
    if (changed && gIntPending) gIntTrigger = 1;
}
static void deliver_completions(void)
{
    down_reap();                                                   /* finish any downstream transfer */
    while (gCTail < gCHead) {                                      /* root-hub control-phase completions */
        volatile Compl *cc = &gC[gCTail % C_N]; void *upp = cc->upp, *pp = cc->pipe; gCTail++;
        if (upp) ((ehci_usl_complete)upp)(pp, 0);
    }
    if (gH94Hold) return;   /* h94: substitute completions for a rude removal are still queued for the
                             * task-level drain. Holding the status-change report here (one task round-trip,
                             * ~ms) is what guarantees the USL cannot free its pipe/transfer structures
                             * before the clients' completions have run against them. Fail-open: compl_drain
                             * runs unconditionally every uim23 tick and always clears the hold. */
    if (gIntTrigger && gIntPending && gIntBuf && gIntCB) {         /* status-change interrupt completion */
        UInt8 bmp = 0; int q, np = gSoftc.nPorts;
        for (q = 0; q < np && q < 15; q++) if (gPortChange[q]) bmp |= (UInt8)(1u << (q + 1));
        ((volatile UInt8 *)gIntBuf)[0] = bmp;
        gIntPending = 0; gIntTrigger = 0;
        ((ehci_usl_intcomplete)gIntCB)((void *)gIntRefcon, 0, gIntLen);
    }
}
/* r95: interrupt-level reconnect takeover-arm (see the gSih* declarations). Called once per heartbeat from
 * ehci_vhub_service (SIH level), so it runs THROUGH Apple's post-reconnect ExpertIdleTask task-level monopoly.
 * SAFE: reads counters + gBulkEP (read-only), sets flags only; NO transfers, NO File-Mgr logging; fences ONLY
 * once Apple has gone quiet (nothing in flight — the boot fence timing, never the r92 active-probe fence). */
static void sih_reconnect_arm(void)
{
    int i, haveIn = 0, haveOut = 0; UInt32 c;
    if (!gMountedOnce || gDev[0].pstate >= 10 || gSihArmed) return;   /* active only between a reconnect and re-completion */
    for (i = 0; i < NBULK; i++) if (gBulkEP[i].used) { if (gBulkEP[i].dirIn) haveIn = 1; else haveOut = 1; }
    if (!haveIn || !haveOut) return;                           /* wait until Apple has re-registered BOTH bulk eps */
    c = gBulkDoneN + gBulkErrN;
    if (c != gSihLastCnt) { gSihLastCnt = c; gSihQuiet = 0; return; }   /* Apple still active — must NOT fence (r92 freeze) */
    if (++gSihQuiet > 80) {              /* ~640ms (80 x 8ms heartbeat) with no new bulk completion => Apple parked */
        gFenceApple = 1;                /* safe now: Apple is quiet, nothing in flight (mirrors the boot fence) */
        gReprobe = 1;                   /* arm the task-level takeover — fires when ExpertIdleTask finally releases */
        gSihArmed = 1; gSihArmTick = gVhubTick;
    }
}
/* ★★★★★★★ h66 — WATCH Vbus AND OVER-CURRENT. THE USER'S LED OBSERVATION, INSTRUMENTED.
 *
 * ⚠ THE OBSERVATION THAT PROMPTED THIS (2026-08-11, and it is better evidence than any of the last three
 * theories): on boots where the keyboard has to be moved to another port, THE DRIVE'S RED POWER LED IS OFF;
 * on boots where everything keeps working, it is LIT. That is Vbus -- PORT_POWER, PORTSC bit 12.
 * ★ EHCI 2.3.9: on an over-current condition THE CONTROLLER ITSELF CLEARS PORT_POWER. So a port can go dark
 * with no write of ours, the device disappears, and until now nothing in the log could say why -- because OCC
 * sits in EHCI_PORTSC_RW1C, so every read-modify-write we perform has been CLEARING the over-current latch
 * without anyone ever reading it. We have been destroying this evidence on every port write since r1.
 * ⇒ This runs FIRST in ehci_vhub_service, before service_ports, so the latch is recorded before we wipe it.
 * ★ PURE READS. It never writes PORTSC, so it cannot perturb what it is measuring and is safe at this level. */
static void h66_port_watch(void)
{
    int p, np = (int)gSoftc.nPorts;
    if (!gSoftc.opBase || np <= 0 || np > 15) return;
    for (p = 0; p < np; p++) {
        UInt32 v = ehci_read32(gSoftc.opBase, EHCI_PORTSC(p));
        if (!gPwInit) { gPwPrev[p] = v; continue; }
        if ((v & (EHCI_PORT_OCA | EHCI_PORT_OCC)) && gOcSeen[p] < 0xFFFFUL) {
            gOcSeen[p]++;
            if (gOcSeen[p] == 1)
                ehci_os_ilogx("!! h66 OVER-CURRENT on a port — the controller clears PORT_POWER itself; "
                              "port<<28|PORTSC", ((UInt32)p << 28) | (v & 0x0FFFFFFFUL));
        }
        if ((gPwPrev[p] & EHCI_PORT_POWER) && !(v & EHCI_PORT_POWER)) {
            gPwLost[p]++;
            ehci_os_ilogx("!! h66 ★ PORT POWER LOST (Vbus off — this is the LED going dark); port<<28|PORTSC",
                          ((UInt32)p << 28) | (v & 0x0FFFFFFFUL));
            ehci_os_ilogx("!! h66   was", gPwPrev[p]);
            /* ★★★★★★ h67 — GATED SELF-HEAL. The diagnostic above says a port went dark; this puts it back,
             * but ONLY when doing so is safe and meaningful:
             *   · NOT if over-current is indicated. Per EHCI 2.3.9 the controller drops PORT_POWER ITSELF on
             *     an over-current condition, and re-powering into one means fighting a hardware protection in
             *     a loop. If OCA/OCC is set we deliberately LEAVE IT OFF and let the log say why -- that case
             *     has no software fix and the right answer is a powered hub or a less hungry drive.
             *   · NOT if the companion owns the port. Our PORTSC writes are ignored under Owner=1 (measured:
             *     v8's power-down read back 0x2800 unchanged on three boot logs), so a write there would be a
             *     silent no-op that only confuses the next reader.
             *   · RATE-LIMITED to H67_MAX_REPOWER per port per session, so this can never become a loop.
             * ⚠ It cannot strand anything: an unpowered port is already dead to both stacks, so restoring
             * power strictly widens what can work -- including the cede path, which is the user's route back
             * to a keyboard. This is the opposite shape to h60, which BLOCKED a path. */
            if ((v & (EHCI_PORT_OCA | EHCI_PORT_OCC)) != 0) {
                ehci_os_ilog("!! h67   OVER-CURRENT indicated — NOT re-powering; this is a hardware "
                             "protection and has no software fix");
            } else if ((v & EHCI_PORT_OWNER) != 0) {
                ehci_os_ilog("!! h67   the companion owns this port — our PORTSC writes are ignored there, "
                             "so not attempting a re-power");
            } else if (gRepower[p] < H67_MAX_REPOWER) {
                UInt32 w = (v & ~EHCI_PORTSC_RW1C) | EHCI_PORT_POWER;
                ehci_write32(gSoftc.opBase, EHCI_PORTSC(p), w);
                gRepower[p]++;
                ehci_os_ilogx("!! h67 ★ RE-POWERED the port (no over-current, ours); port<<28|PORTSC[after]",
                              ((UInt32)p << 28)
                              | (ehci_read32(gSoftc.opBase, EHCI_PORTSC(p)) & 0x0FFFFFFFUL));
            } else {
                ehci_os_ilogx("!! h67   re-power budget spent for this port; leaving it off; port", (UInt32)p);
            }
        } else if (!(gPwPrev[p] & EHCI_PORT_POWER) && (v & EHCI_PORT_POWER)) {
            ehci_os_ilogx("!! h66 port power RESTORED; port<<28|PORTSC",
                          ((UInt32)p << 28) | (v & 0x0FFFFFFFUL));
        }
        gPwPrev[p] = v;
    }
    gPwInit = 1;
}
void ehci_vhub_service(void)
{
    h66_port_watch();   /* h66: FIRST, so an over-current latch is seen before our own writes clear it */
    gVhubTick++;
    (void)hub_int_ack();
    down_relink_if_needed();   /* r60: heal the async ring BEFORE reaping — restores reachability so a
                                * stuck-but-now-linked transfer can complete instead of freezing the machine */
    service_ports();
    deliver_completions();
#if APPLE_HIDE
    /* ★ n5: THE POINT OF THE WHOLE EXERCISE — drive enumeration + probe from our OWN heartbeat, so
     * discovery no longer depends on slot 23 and therefore no longer depends on an application existing.
     * Must come AFTER deliver_completions() so a transfer that retired in this same pass is already
     * visible to pb_ready(), which lets the state machine advance a step per heartbeat, not per two. */
    as_tick();
#endif
    bio_kick();          /* r48: SOLE task-independent driver of the async block-I/O engine. Runs at
                          * interrupt level (real EHCI ISR + the 8ms heartbeat SIH), so starting/advancing
                          * a bio request never races a task-level submit. deliver_completions() already
                          * drove bio_advance for in-flight requests; this starts a freshly-appended one
                          * when the engine is idle (worst-case latency = one heartbeat, ~8ms). */
    sih_reconnect_arm();   /* r95: watch for Apple's post-reconnect park + arm the takeover, from interrupt level */
    /* ★★★★★ h26 APP-LESS: notice a parked task-level job and post the NM trampoline for it. This runs on the
     * 8 ms heartbeat and the real IRQ, so a park is picked up within one heartbeat with no application of ours
     * involved. NMInstall only ENQUEUES, which is what makes it legal from here — see ehci_nm_task_resp. */
    /* ★ h64 MEASUREMENT: how long does lowmem Ticks itself stall? Sampled here on the 8 ms heartbeat and
     * timed with frame_ms(), which VBL cannot stop. If the theory above is right this reports a large number
     * on a boot that used to fail — the fix restores the pump, and the pump is what lets the dump be written,
     * so this build proves its own diagnosis instead of asserting it. */
    {   UInt32 t = TICKS_NOW, ms = frame_ms();
        if (t != gTickSeenLast) { gTickSeenLast = t; gTickSeenMs = ms; }
        else if ((UInt32)(ms - gTickSeenMs) > gTicksStallMaxMs) gTicksStallMaxMs = (UInt32)(ms - gTickSeenMs);
    }
    task_work_arm();
}

/* ==================== real EHCI interrupt + periodic timer ====================
 * A resident UIM services its own controller (as Apple's OHCI UIM does): install a handler on
 * the node's driver-ist interrupt-set member, enable EHCI interrupts, and defer the work to a
 * secondary-interrupt handler; also arm a periodic timer so reset-deassert timing and completion
 * reaping advance on a cadence even between controller interrupts. HW-proven in the app leg (v0.24,
 * a2=1). Storm-safe: the primary ISR returns not-complete when USBSTS is clear (shared PCI IRQ),
 * else masks USBINTR and queues the SIH, which clears the source and re-unmasks. */
#define VHUB_HB_MS 8
static UInt32 gIntrEnabled = (EHCI_STS_PCD | EHCI_STS_USBINT | EHCI_STS_USBERRINT);
static int gA2Live = 0;   /* h54: gSihQueued moved up beside gIsrConsec so ctl_step's probe can read it */
static TimerID gHbTimer = 0;
static InterruptSetID gSetID = 0; static InterruptMemberNumber gMember = 0;
static void *gSavedRefcon = 0; static InterruptHandler gSavedHandler = 0;
static InterruptEnabler gSavedEnabler = 0; static InterruptDisabler gSavedDisabler = 0;
/* v46: paint a bright bar to the top of the main screen straight from the ISR. ScrnBase (lowmem 0x0824)
 * holds the base of the main screen buffer; a generous run of solid 0xFF longs shows a white band at any
 * depth/rowBytes. Best-effort — if the base looks bogus, do nothing (never fault). This is the ONLY
 * readout that survives an interrupt-level CPU lockup. RECONSTRUCTED verbatim. */
static void storm_paint(void)
{
    UInt32 *fb = *(UInt32 * volatile *)0x0824UL;     /* ScrnBase */
    if (fb && (unsigned long)fb >= 0x1000UL) {
        UInt32 i;
        fb += 1024u;                                 /* nudge in ~4KB from the top-left corner */
        for (i = 0; i < 8192u; i++) fb[i] = 0xFFFFFFFFUL;   /* ~32KB = a visible bright band */
    }
}

/* ================================================================================================
 * ★★★★★★★ h76 — THE SCREEN WATCHDOG. EVIDENCE THAT DOES NOT DIE WITH THE FILE MANAGER.
 *
 * ⚠ WHY THIS EXISTS, and it was predicted in this file after h18 and never acted on: "the next attempt
 * needs evidence that survives a stall rather than more counters written through FSWrite."
 * EVERY log line in this driver is a synchronous FSWrite + FlushVol. The m26 run-2 failure WEDGED THE FILE
 * MANAGER — desktop drawn, wristwatch cursor, not one icon, for ever — so logging stopped instantly and
 * silently, mid-line, with no error recorded. The +5 s state dump never printed because the File Manager
 * was already gone before the deadline. Our entire diagnostic channel is destroyed by the exact fault we
 * are trying to diagnose, and no amount of extra logging can fix that.
 * ⇒ Paint to the SCREEN. It touches no Toolbox call, no allocator, no File Manager — just stores into the
 * framebuffer, which is why storm_paint (v46) has always been "the ONLY readout that survives an
 * interrupt-level CPU lockup". This extends that from a blank white band to actual NUMBERS.
 *
 * ★ AND IT REPAINTS ~1/s, WHICH IS THE POINT. A frozen gVhubTick means the driver died; a CLIMBING
 * gVhubTick beside a FROZEN gTaskPumpN means the driver is fine and TASK LEVEL is what stopped — the
 * single most valuable distinction in this whole hunt, and one no log could ever have given us, because a
 * log cannot be written by the thing whose death it is trying to report.
 *
 * SAFETY, since this runs at INTERRUPT level and writes raw screen memory:
 *   · base from ScrnBase (0x0824), exactly as storm_paint proves works on this hardware;
 *   · rowBytes read defensively through MainDevice -> gdPMap -> PixMap, every pointer NULL-checked, and
 *     REJECTED unless plausible — on failure we fall back to storm_paint's plain band, so the diagnostic
 *     degrades instead of faulting;
 *   · every write is bounded to a small fixed box near the top-left, so even a wrong stride can only make
 *     an ugly pattern, never scribble memory;
 *   · stores only. No Toolbox, no allocation, no File Manager. The interrupt audit stays clean. */
static const UInt8 gHexFont[16][6] = {
    {0x6,0x9,0x9,0x9,0x9,0x6}, {0x2,0x6,0x2,0x2,0x2,0x7}, {0x6,0x9,0x1,0x2,0x4,0xF},
    {0xE,0x1,0x6,0x1,0x9,0x6}, {0x9,0x9,0xF,0x1,0x1,0x1}, {0xF,0x8,0xE,0x1,0x9,0x6},
    {0x6,0x8,0xE,0x9,0x9,0x6}, {0xF,0x1,0x2,0x4,0x4,0x4}, {0x6,0x9,0x6,0x9,0x9,0x6},
    {0x6,0x9,0x9,0x7,0x1,0x6}, {0x6,0x9,0x9,0xF,0x9,0x9}, {0xE,0x9,0xE,0x9,0x9,0xE},
    {0x6,0x9,0x8,0x8,0x9,0x6}, {0xE,0x9,0x9,0x9,0x9,0xE}, {0xF,0x8,0xE,0x8,0x8,0xF},
    {0xF,0x8,0xE,0x8,0x8,0x8}
};
/* h85: painted so it READS as the build number — 0x85 shows as "85". ⚠ KEEP IN SYNC WITH
 * EHCI_BUILD_TAG in ehci_uim.c; they are the same fact told to two different readers. */
#define EHCI_BUILD_PAINT 0x94u
#define PW_SCALE   3                     /* pixels per font pixel */
#define PW_GW      (5 * PW_SCALE)        /* glyph cell width  (4 px glyph + 1 px gap) */
#define PW_GH      (8 * PW_SCALE)        /* glyph cell height (6 px glyph + 2 px gap) */
#define PW_MAXX    600u                  /* hard bound: never write past this column ... */
#define PW_MAXY    200u                  /* ... or this line. Any screen is at least 640x480. */

/* longs-per-row, or 0 if we cannot establish it safely. */
static UInt32 pw_row_longs(void)
{
    UInt8 **gdh; UInt8 *gd; UInt8 **pmh; UInt8 *pm; UInt32 rb;
    gdh = (UInt8 **)*(volatile unsigned long *)0x08A4UL;   /* MainDevice: GDHandle */
    if (!gdh || (unsigned long)gdh < 0x1000UL) return 0;
    gd = *gdh;  if (!gd || (unsigned long)gd < 0x1000UL) return 0;
    pmh = *(UInt8 ***)(gd + 0x16);                         /* GDevice.gdPMap */
    if (!pmh || (unsigned long)pmh < 0x1000UL) return 0;
    pm = *pmh;  if (!pm || (unsigned long)pm < 0x1000UL) return 0;
    rb = (UInt32)(*(volatile UInt16 *)(pm + 4) & 0x3FFFu); /* PixMap.rowBytes, minus the flag bits */
    if (rb < 1024u || rb > 32768u || (rb & 3u)) return 0;  /* implausible => caller falls back */
    return rb >> 2;
}
static void pw_glyph(UInt32 *fb, UInt32 rowLongs, UInt32 x0, UInt32 y0, UInt8 g, UInt32 colour)
{
    int r, c, sy, sx;
    if (x0 + 4u * PW_SCALE >= PW_MAXX || y0 + 6u * PW_SCALE >= PW_MAXY) return;   /* bounded, always */
    for (r = 0; r < 6; r++) {
        UInt8 bits = gHexFont[g & 0x0F][r];
        for (c = 0; c < 4; c++) {
            if (!(bits & (8 >> c))) continue;
            for (sy = 0; sy < PW_SCALE; sy++)
                for (sx = 0; sx < PW_SCALE; sx++)
                    fb[(y0 + (UInt32)(r * PW_SCALE + sy)) * rowLongs + (x0 + (UInt32)(c * PW_SCALE + sx))] = colour;
        }
    }
}
/* Paint `n` hex digits of `v` at character cell (cx, cy). */
static void pw_hex(UInt32 *fb, UInt32 rowLongs, UInt32 cx, UInt32 cy, UInt32 v, int n, UInt32 colour)
{
    int i;
    for (i = 0; i < n; i++)
        pw_glyph(fb, rowLongs, (cx + (UInt32)i) * PW_GW, cy * PW_GH,
                 (UInt8)((v >> (4 * (n - 1 - i))) & 0x0Fu), colour);
}
static void paint_watchdog_state(void)
{
    UInt32 *fb = *(UInt32 * volatile *)0x0824UL;           /* ScrnBase — storm_paint's proven source */
    UInt32 rl = pw_row_longs();
    UInt32 i;
    if (!fb || (unsigned long)fb < 0x1000UL) return;
    if (!rl) { storm_paint(); return; }                    /* cannot lay out -> the plain band still says "fired" */
    /* Black backing box so the digits are legible over whatever the Finder drew. */
    for (i = 0; i < PW_MAXY; i++) {
        UInt32 j; UInt32 *row = fb + i * rl;
        for (j = 0; j < PW_MAXX; j++) row[j] = 0x00000000UL;
    }
    /* Row 0 — a bright red banner so it is unmistakable on a photograph. */
    for (i = 0; i < PW_GH; i++) {
        UInt32 j; UInt32 *row = fb + i * rl;
        for (j = 0; j < PW_MAXX; j++) row[j] = 0x00FF0000UL;
    }
    /* Row 1: the liveness pair. TICK climbing + PUMP frozen == the driver is alive, TASK LEVEL is dead.
     * Row 2: the block-I/O state machine and both in-flight slots.
     * Row 3: the error counters we could never see, because the log died before they were dumped. */
    pw_hex(fb, rl, 0,  1, gVhubTick,                     8, 0x0000FF00UL);   /* green: must CLIMB */
    pw_hex(fb, rl, 9,  1, gTaskPumpN,                    8, 0x0000FFFFUL);   /* cyan:  frozen == task level dead */
    pw_hex(fb, rl, 0,  2, (UInt32)gBioPhase,             2, 0x00FFFFFFUL);   /* >= 20 == stuck in BOT recovery */
    pw_hex(fb, rl, 3,  2, (UInt32)(gEgBusy ? 1 : 0),     1, 0x00FFFFFFUL);
    pw_hex(fb, rl, 5,  2, (UInt32)(gDpBusy ? 1 : 0),     1, 0x00FFFFFFUL);
    pw_hex(fb, rl, 7,  2, (gBioHead - gBioTail),         2, 0x00FFFFFFUL);   /* queued and not completing */
    pw_hex(fb, rl, 10, 2, (UInt32)gAs.pc,                2, 0x00FFFFFFUL);
    pw_hex(fb, rl, 13, 2, (UInt32)(gAs.running ? 1 : 0), 1, 0x00FFFFFFUL);
    pw_hex(fb, rl, 0,  3, gDownErr,                      4, 0x00FFFF00UL);
    pw_hex(fb, rl, 5,  3, gDownTimeouts,                 4, 0x00FFFF00UL);
    pw_hex(fb, rl, 10, 3, gEgRecovCompl,                 4, 0x00FFFF00UL);
    pw_hex(fb, rl, 15, 3, gSplitSaved,                   4, 0x00FFFF00UL);
    /* ★★★★ h83 ROW 4 (magenta) — WHY the pump stopped, which rows 1-3 cannot say.
     * The 2026-08-12 Mini hang froze gTaskPumpN at 0x0a with the engine completely idle and every error
     * counter zero, and the last dump read gNmArmed == gNmFired == 10. Balanced. Two incompatible stories
     * fit that, they need opposite fixes, and no counter on screen could separate them:
     *   A — we stopped ASKING: no further arm was ever issued, so nothing woke the pump.
     *   B — we asked and NOBODY ANSWERED: a request went out and the Finder never ran it.
     * These four fields decide it on sight, with no log and no working task level:
     *   ARMED > FIRED  -> B. A request is outstanding and unanswered.
     *   ARMED == FIRED and both frozen, POSTED 0 -> A. We are not asking.
     *   POSTED 1 forever -> the gNMTPosted latch is stuck, which is the ONLY way task_work_arm can
     *                       return on its first line for the rest of the session (see h83 at gNmArmTickK).
     *   STUCK climbing -> h63 is alive for the first time ever and is re-posting; if the machine then
     *                     recovers, B is confirmed AND fixed. If it climbs and the machine stays wedged,
     *                     the Finder is dead to us and the fault is not the request at all.
     *   KEEP climbing  -> the ~2 s keepalive is still reaching NMInstall, so interrupt level is healthy
     *                     and the break is downstream of us. */
    pw_hex(fb, rl, 0,  4, gNmArmed,                      4, 0x00FF00FFUL);   /* ARMED */
    pw_hex(fb, rl, 5,  4, gNmFired,                      4, 0x00FF00FFUL);   /* FIRED */
    pw_hex(fb, rl, 10, 4, gNmStuckRearms,                4, 0x00FF00FFUL);   /* STUCK (h63 revivals) */
    pw_hex(fb, rl, 15, 4, (UInt32)(gNMTPosted ? 1 : 0),  1, 0x00FF00FFUL);   /* POSTED latch */
    pw_hex(fb, rl, 17, 4, gNmKeepArms,                   4, 0x00FF00FFUL);   /* KEEP (keepalive arms) */
    /* ★★★★ h84 ROW 5 (orange) — WHERE the body is, and HOW LONG it has been there.
     * Row 4 established that a wedge is a body that never returns. This row names the section and times it
     * LIVE, which is the only form that works: a high-water mark is written at body exit, and the failure
     * is a body that never exits. PH is the answer; NOWMS is the evidence it is not merely slow.
     *   PH    current section, 0 = not in the body (see the BP_* table)
     *   NOWMS ms since this body started, clamped at FFFF. Climbing across two paints = wedged HERE.
     *   MAXMS worst COMPLETED body this session
     *   MAXPH / PHMS  the section with the worst high-water, and that figure — "which part is the long pole"
     * ⚠ MAXMS and PHMS are healthy-path statistics. On a wedge read PH and NOWMS and ignore the rest. */
    {   UInt32 el = 0, i2, best = 0, bestPh = 0;
        if (gBodyPhase != BP_IDLE) {
            el = frame_ms() - gBodyStartMs;
            if (el > 0xFFFFUL) el = 0xFFFFUL;          /* pin rather than wrap — a wrapped field lies (I10) */
        }
        for (i2 = 0; i2 < BP_N; i2++)
            if (gPhaseMaxMs[i2] > best) { best = gPhaseMaxMs[i2]; bestPh = i2; }
        if (best > 0xFFFFUL) best = 0xFFFFUL;
        pw_hex(fb, rl, 0,  5, (UInt32)gBodyPhase, 2, 0x00FF8000UL);   /* PH    */
        pw_hex(fb, rl, 3,  5, el,                 4, 0x00FF8000UL);   /* NOWMS */
        pw_hex(fb, rl, 8,  5, gBodyMaxMs > 0xFFFFUL ? 0xFFFFUL : gBodyMaxMs,
                                                  4, 0x00FF8000UL);   /* MAXMS */
        pw_hex(fb, rl, 13, 5, bestPh,             2, 0x00FF8000UL);   /* MAXPH */
        pw_hex(fb, rl, 16, 5, best,               4, 0x00FF8000UL);   /* PHMS  */
        /* ★★ h85: BUILD and LOG LEVEL, in white, at the end of row 5. With the logging compiled out the log
         * file holds two lines, so the screen has to be able to identify the run on its own — otherwise a
         * level-0 build and a level-2 build are indistinguishable from a photograph, and this project has
         * already lost a cycle to a stale log and a wrong app version. BUILD is painted so it READS as the
         * decimal build number (0x85 -> "85"), not as hex arithmetic. */
        pw_hex(fb, rl, 21, 5, EHCI_BUILD_PAINT, 2, 0x00FFFFFFUL);              /* "85" */
        pw_hex(fb, rl, 24, 5, (UInt32)ehci_os_log_level(), 1, 0x00FFFFFFUL);   /* log level */
        /* h92: the canary mask, red so a dead guard zone is unmissable. 00 = all guards intact. */
        pw_hex(fb, rl, 26, 5, gH92Mask, 2, 0x00FF0000UL);
    }
}
/* Interrupt level, from the heartbeat. Arms only after an exposure, fires only once task level has been
 * silent for 5 s, repaints ~1/s, and stops after 120 paints so it can never become the problem itself. */
static void paint_watchdog_tick(void)
{
    UInt32 now = TICKS_NOW;
    if (!gPwArmed) return;
    if (gTaskPumpN != gPwPumpLast) { gPwPumpLast = gTaskPumpN; gPwPumpAtTick = now; return; }
    if ((long)(now - gPwPumpAtTick) < 300L) return;        /* task level quiet < 5 s: normal, say nothing */
    if (gPwPaints >= 120u) return;                         /* ~2 minutes of evidence is plenty */
    {   static UInt32 sNext = 0;
        if ((long)(now - sNext) < 0) return;
        sNext = now + 60UL;                                /* ~1 s between repaints */
    }
    gPwPaints++;
    paint_watchdog_state();
}

static OSStatus vhub_sih(void *p1, void *p2)
{
    (void)p1; (void)p2;
    gSihRuns++;                 /* h54: diagnostic only — the probe reads it to see whether the SIH is starved */
    ehci_vhub_service();
    /* SHARED-IRQ FIX #1 (Mini mount): clear gSihQueued BEFORE re-unmasking. The old order (unmask then
     * clear) left a window where a completion arriving after the unmask re-entered vhub_isr, which masked
     * again but saw gSihQueued still 1 and did NOT queue a fresh SIH -> that completion was dropped until
     * the 8ms heartbeat. Clearing first means any post-unmask IRQ sees gSihQueued==0 and queues a new SIH.
     * ⚠ RESTORED 2026-08-01: the Jul 24 base still had the OLD order. This is a LOGIC-ONLY fix with no log
     * string attached, so the string/function gates could never have flagged it — see the reconstruction
     * plan's note about that blind spot. */
    gSihQueued = 0;
    gIsrConsec = 0;                                                         /* v46: storm depth resets */
    if (gA2Live) ehci_write32(gSoftc.opBase, EHCI_USBINTR, gIntrEnabled);   /* re-unmask */
    return noErr;
}
static OSStatus vhub_heartbeat(void *p1, void *p2)
{
    AbsoluteTime when;
    (void)p1; (void)p2;
    /* ★ lc1 (restored 2026-08-01 from the disassembly): the teardown flag is checked HERE, first. Without
     * it ehci_vhub_stop_service cancels the pending timer but this handler re-arms a new one on its way
     * out, so the heartbeat never actually stops and the driver keeps touching hardware after kFinalize.
     * Returning before frame_time_update() is what makes the driver go quiet. */
    if (gServiceStop) return noErr;
    /* ★ h76: the screen watchdog. Deliberately HERE, in the timer handler, and not in the SIH — this is the
     * one path that keeps running when everything above it has stopped, which is exactly the condition it
     * exists to report. Cheap: a counter compare on all but ~1 pass per second. */
#if H76_DIAGNOSTICS
    paint_watchdog_tick();
#endif
    gHbRuns++;                  /* h65: heartbeat-ONLY liveness. Compare with gVhubTick (SIH passes,
                                 * which the real IRQ also drives): gVhubTick climbing while gHbRuns is
                                 * flat means the Time Manager one-shot stopped re-arming, which would
                                 * freeze frame_ms() and is the last standing explanation for callN 1. */
    frame_time_update();        /* advance the USL frame clock on a steady 8 ms cadence (single writer) */
    /* QUEUE the secondary handler (like the ISR path) rather than running the service inline at timer
     * level — so EVERY transfer completion is delivered to the USL/class driver at secondary-interrupt
     * level, matching Apple's OHCI UIM. (A completion arriving at timer level may be why the mass-
     * storage driver won't re-issue after REQUEST SENSE.) */
    if (!gSihQueued) { gSihQueued = 1; QueueSecondaryInterruptHandler(vhub_sih, NULL, NULL, NULL); }
    /* ★ ase_quiesce timebase calibration, free of charge: this path already reads UpTime() to re-arm, and
     * frame_time_update() just advanced the microframe accumulator. Accumulating both spans lets the log
     * convert ase_quiesce's raw tick deltas into microseconds without a clock-rate assumption and without
     * spinning anywhere to calibrate. Skips the first pass (no previous sample) and any pass where the
     * accumulator did not move. */
    {   UInt32 tbNow = UpTime().lo;
        static UInt32 sTbPrev = 0, sUfPrev = 0; static int sHave = 0;
        if (sHave) {
            UInt32 dtb = tbNow - sTbPrev, duf = gMicroAcc - sUfPrev;
            if (duf && dtb) {
                /* ★ HALVE BOTH BEFORE EITHER CAN WRAP. The first run of this instrumentation reached 96.6%
                 * of a 32-bit wrap in 202.8 s: seven more seconds and the accumulator would have rolled over
                 * and silently DEFLATED the microsecond conversion, reporting a smaller number that looked
                 * perfectly plausible. Only the RATIO matters, and halving both preserves it exactly, so the
                 * calibration now survives a session of any length. */
                if (gAseTbSpan > 0x40000000UL || gAseUfSpan > 0x40000000UL) {
                    gAseTbSpan >>= 1; gAseUfSpan >>= 1;
                }
                gAseTbSpan += dtb; gAseUfSpan += duf;
            }
        }
        sTbPrev = tbNow; sUfPrev = gMicroAcc; sHave = 1; }
    when = AddDurationToAbsolute((Duration)VHUB_HB_MS, UpTime());
    SetInterruptTimer(&when, vhub_heartbeat, 0, &gHbTimer);   /* one-shot: re-arm */
    return noErr;
}
static InterruptMemberNumber vhub_isr(InterruptSetMember m, void *refcon, UInt32 cnt)
{
    UInt32 sts;
    (void)refcon;
    /* ★ v46 SHARED-IRQ STORM DETECTOR (restored 2026-08-01 from the disassembly of the surviving binary —
     * this whole block was lost with the source and no string or symbol pointed at it).
     * On a SHARED line EVERY companion interrupt also enters here, so count consecutive entries BEFORE
     * looking at USBSTS — that is what measures the storm. The SIH clears gIsrConsec, so a climbing value
     * means the SIH is not getting to run. Past ~500 we are wedged at interrupt level, where no log can be
     * written; paint the screen ONCE instead, which is the only readout that survives a lockup. */
    if (gSoftc.sharedCompanion) {
        gIsrConsec++;
        if (gIsrConsec > gIsrConsecMax) gIsrConsecMax = gIsrConsec;
#if EHCI_PAINT   /* h86: the release build must never draw on the screen — not even in a storm.
                  * The counters (gIsrConsec/Max) stay; a field report of a wedge gets a PAINT=1 build. */
        if (gIsrConsec > 499u && gStormPainted == 0) { gStormPainted = 1; storm_paint(); }
#endif
    }
    sts = ehci_read32(gSoftc.opBase, EHCI_USBSTS) & gIntrEnabled;
    if (sts) {                                                 /* OUR (EHCI) interrupt */
        gIsrHits++;                                            /* r35: this IRQ was ours */
        ehci_write32(gSoftc.opBase, EHCI_USBINTR, 0);          /* mask until the SIH clears it */
        if (!gSihQueued) { gSihQueued = 1; QueueSecondaryInterruptHandler(vhub_sih, NULL, NULL, NULL); }
        /* Shared line (on-board chip: the claim released occupied ports -> sharedCompanion): also run
         * the companion handler so the keyboard/mouse are serviced. On a dedicated line (PCI card)
         * sharedCompanion is 0, so we never touch it (that call stalled the MDD completion path). */
        if (gSoftc.sharedCompanion && gSavedHandler) (void)gSavedHandler(m, gSavedRefcon, cnt);
        return kIsrIsComplete;
    }
    if (gSoftc.sharedCompanion && gSavedHandler) return gSavedHandler(m, gSavedRefcon, cnt);
    return kIsrIsNotComplete;
}
void ehci_vhub_start_service(EHCIRegEntryIDPtr node)
{
    ISTProperty ist; RegPropertyValueSize sz = sizeof(ist);
    OSErr pe = RegistryPropertyGet((RegEntryID *)node, kISTPropertyName, ist, &sz);
    /* r35 R2a: log EXACTLY why the real EHCI IRQ does/doesn't install. Suspicion: a force-loaded
     * (LoadUIMForEntry) node has NO "driver-ist" interrupt-set property -> pe != noErr -> we never
     * install the ISR and fall back to the 8ms timer (= the slow, heartbeat-paced I/O observed). */
    /* ★ Restored 2026-08-01 from the disassembly: clear the teardown flag on (re)start. stop_service sets
     * it to 1 to make the heartbeat and the SIH go quiet; without clearing it here, a driver that is
     * stopped and started again never ticks again. */
    gServiceStop = 0;
    ehci_os_log("vhub_start_service: real-EHCI-IRQ install");
    ehci_os_logx("  driver-ist get", (unsigned long)(long)pe);      /* 0 = found; negative = absent */
    ehci_os_logx("  driver-ist sz",  (unsigned long)sz);
    /* Install our EHCI interrupt handler for real-IRQ throughput: the self-probe/down-engine needs
     * PROMPT completions, and heartbeat-only pacing (gA2Live=0) is far too slow (v5: ~5s per transfer,
     * mount never completed). On a SHARED line (on-board chip, sharedCompanion==1) vhub_isr ALSO runs
     * the displaced companion handler so the keyboard/mouse stay serviced; the launcher's settle window
     * keeps their re-enumeration from colliding with the drive's enumeration. On a DEDICATED line (PCI
     * card, sharedCompanion==0) vhub_isr never touches the companion handler. */
#if EHCI_FORCE_POLLED
    /* b9: the B&W G3 NMI capture (2026-08-14) proved an unserviced interrupt storm at the first PCD
     * assertion — the driver-ist install "succeeds" on the Heathrow-era tree but the ISR is never
     * dispatched. In forced-polled mode we never install the ISR and never write USBINTR, so the
     * controller can never assert the line at all; the 8ms heartbeat carries all completions. */
    pe = -1;
    ehci_os_log("  b9 EHCI_FORCE_POLLED: skipping ISR install + USBINTR enable BY BUILD CONFIG");
#endif
    if (pe == noErr && sz >= sizeof(InterruptSetMember)) {
        OSErr ge, ie = -1;
        gSetID = ist[0].setID; gMember = ist[0].member;
        ehci_os_logx("  setID",  (unsigned long)gSetID);
        ehci_os_logx("  member", (unsigned long)gMember);
        ge = GetInterruptFunctions(gSetID, gMember, &gSavedRefcon, &gSavedHandler, &gSavedEnabler, &gSavedDisabler);
        ehci_os_logx("  GetInterruptFunctions", (unsigned long)(long)ge);
        if (ge == noErr) {
            ie = InstallInterruptFunctions(gSetID, gMember, NULL, vhub_isr, NULL, NULL);
            ehci_os_logx("  InstallInterruptFunctions", (unsigned long)(long)ie);
        }
        if (ge == noErr && ie == noErr) {
            if (gSavedEnabler) { InterruptSetMember mm; mm.setID = gSetID; mm.member = gMember; gSavedEnabler(mm, NULL); }
            ehci_write32(gSoftc.opBase, EHCI_USBINTR, gIntrEnabled);
            gA2Live = 1;
        }
    }
    ehci_os_logx("  gA2Live (1=real IRQ, 0=heartbeat-only)", (unsigned long)gA2Live);
    ehci_os_logx("  sharedCompanion (1=shared line->ISR also chains companion, 0=dedicated)", (unsigned long)gSoftc.sharedCompanion);
    /* ★ v50 (restored 2026-08-01 from the disassembly — the reconstruction had only the log line below,
     * not this write). On the Mac mini's SHARED on-board controller, force USBCMD's Interrupt Threshold
     * Control to 1 = interrupt on every microframe, no coalescing. Coalescing there delays our completions
     * behind the companion's traffic, which is what made the Mini's transfers crawl. PCI-card (dedicated
     * line) machines are left at whatever the controller came up with. */
    if (gSoftc.sharedCompanion) {
        UInt32 cmd = ehci_read32(gSoftc.opBase, EHCI_USBCMD);
        cmd = (cmd & ~0x00FF0000UL) | 0x00010000UL;      /* ITC field = 0x01 */
        ehci_write32(gSoftc.opBase, EHCI_USBCMD, cmd);
    }
    ehci_os_logx("  v50 Mini ITC (0x01=stock/no coalescing); USBCMD now", ehci_read32(gSoftc.opBase, EHCI_USBCMD));
    ehci_os_log("=== PATH A: ROM-integrated self-probe/mount (headless). This driver ships in the Mac OS ROM via a driver,AAPL,MacOS,PowerPC parcel on the EHCI node; the mount vehicle activates it with LoadUIMForEntry, EHCI comes up, then our self-probe fences Apple's parked mounter, takes the bulk endpoints, and does INQUIRY / READ CAPACITY / mount. WATCH: SELFPROBE INQUIRY + READ CAPACITY, then MOUNTED; timeouts should stay 0. Block-driver LBA-math + re-entrancy fixes as of 33d0f45. ===");
    { AbsoluteTime when = AddDurationToAbsolute((Duration)VHUB_HB_MS, UpTime());
      SetInterruptTimer(&when, vhub_heartbeat, 0, &gHbTimer); }
}

/* ============================================================================================
 * RECONSTRUCTED 2026-08-01 — the three functions ehci_vhub.h declares and the Jul 24 base lacked.
 * They were the only unresolved symbols when the base was compiled against the current tree, so
 * their signatures and call sites are known exactly (ehci_uim.c:138/311/320, ehci_os.c:454); the
 * bodies are re-derived.
 * ============================================================================================ */

/* lc1: clean teardown for kFinalize/kClose. Mirror of ehci_vhub_start_service above, in reverse:
 * stop the heartbeat re-arming, mask EHCI interrupts, hand the interrupt-set member back to whoever
 * owned it before us, and stop the schedules. Order matters — quiesce the source BEFORE restoring the
 * handler, so no interrupt can arrive addressed to a handler we have already given away. */
/* ★★★ n10: WARM-REBOOT TEARDOWN — the fix for the frozen mouse cursor after Special > Restart.
 *
 * ROOT CAUSE: ehci_os_boot_quiesce() exists precisely to tame a controller left HOT by a previous session,
 * and it is called from kInitialize in DoDriverIO — which n0 PROVED is never called. So it has never once
 * run. Nothing else tears us down either: kFinalize never fires, and the activator deliberately does not
 * release the ports. A warm boot therefore inherits a controller that is still RUNNING, still bus-mastering
 * DMA into memory the new boot is about to reuse, and still asserting interrupts — with the old ISR gone.
 * That is a textbook recipe for the wedged desktop the user saw, and it is cured by a cold boot, which
 * matches exactly.
 *
 * FIX: hook the Shutdown Manager at activation, so we quiesce BEFORE the restart instead of hoping the next
 * boot cleans up after us. This is the standard mechanism and it runs at task level during shutdown.
 *
 * ⚠ The proc must be MINIMAL and must NOT touch the File Manager — the system is going down. So: set
 * gServiceStop first (the restored heartbeat guard makes the timer stop re-arming without us cancelling
 * it), mask every interrupt source, halt the schedules, and hand the ports back so the next boot — and
 * Apple's 1.1 companion — get a clean controller. No logging, no allocation, no Device Manager. */
static ShutDwnUPP gShutUPP = 0;
static pascal void ehci_shutdown_proc(short stage)
{
    (void)stage;
    gServiceStop = 1;                       /* heartbeat returns immediately and stops re-arming */
    if (gSoftc.opBase) {
        UInt32 cmd;
        ehci_write32(gSoftc.opBase, EHCI_USBINTR, 0);            /* mask all interrupt sources */
        (void)ehci_read32(gSoftc.opBase, EHCI_USBSTS);           /* posted-write flush */
        cmd = ehci_read32(gSoftc.opBase, EHCI_USBCMD);
        ehci_write32(gSoftc.opBase, EHCI_USBCMD,
                     cmd & ~(EHCI_CMD_ASE | EHCI_CMD_PSE | EHCI_CMD_RUN));   /* stop schedules + halt */
        (void)ehci_read32(gSoftc.opBase, EHCI_USBSTS);
    }
    ehci_hc_release_ports(&gSoftc);         /* ports back to the 1.1 companion, CONFIGFLAG cleared */
}
void ehci_vhub_install_shutdown_hook(void)
{
    if (gShutUPP) return;                                        /* once only */
    /* h96: same rule as the pump UPP — the Shutdown Manager calls this descriptor at restart time,
     * long after whatever zone hosted this call may have evaporated. A stale descriptor HERE means
     * the quiesce never runs and the jump lands in recycled memory at the worst possible moment
     * (mid-shutdown, no MacsBug). SysZone, explicitly, like h90. */
    {   THz oldZone = GetZone();
        SetZone(SystemZone());
        gShutUPP = NewShutDwnUPP(ehci_shutdown_proc);
        SetZone(oldZone);
    }
    if (gShutUPP) {                                  /* ShutDwnInstall returns void — the UPP is the only
                                                      * thing that can fail here */
        ShutDwnInstall(gShutUPP, sdRestartOrPower);
        ehci_os_log("n10 shutdown hook installed — the controller will be quiesced before restart/power-off");
    } else {
        ehci_os_log("!! n10 NewShutDwnUPP FAILED — a warm reboot may inherit a hot controller");
    }
}
void ehci_vhub_stop_service(void)
{
    gServiceStop = 1;
    if (gSoftc.opBase) {
        ehci_write32(gSoftc.opBase, EHCI_USBINTR, 0);           /* mask every EHCI interrupt source */
        (void)ehci_read32(gSoftc.opBase, EHCI_USBSTS);          /* posted-write flush */
    }
    if (gHbTimer) { (void)CancelTimer(gHbTimer, NULL); gHbTimer = 0; }
    if (gA2Live) {                                              /* give the interrupt member back */
        if (gSavedDisabler) { InterruptSetMember mm; mm.setID = gSetID; mm.member = gMember;
                              gSavedDisabler(mm, gSavedRefcon); }
        (void)InstallInterruptFunctions(gSetID, gMember, gSavedRefcon, gSavedHandler,
                                        gSavedEnabler, gSavedDisabler);
        if (gSavedEnabler)  { InterruptSetMember mm; mm.setID = gSetID; mm.member = gMember;
                              gSavedEnabler(mm, gSavedRefcon); }
        gA2Live = 0;
    }
    if (gSoftc.opBase) {                                        /* stop the async + periodic schedules */
        UInt32 cmd = ehci_read32(gSoftc.opBase, EHCI_USBCMD);
        ehci_write32(gSoftc.opBase, EHCI_USBCMD, cmd & ~(EHCI_CMD_ASE | EHCI_CMD_PSE));
    }
    gDownAseOn = 0;
    ehci_os_log("EHCIUIM: vhub_stop_service — timer cancelled, IRQ restored, schedules stopped");
}

/* p0i3: descriptor-spoof patch counters. ⚠ RECONSTRUCTION NOTE — the spoof CAPTURE side (which
 * increments these from inside the control-transfer path) has NOT been restored yet, so these read 0
 * until it is. The accessor is real and its contract is right; only the producer is outstanding.
 * Under APPLE_HIDE the spoof is believed vestigial anyway (Apple never sees our device), which is why
 * it is not on the critical path for the rebuild. */
static UInt32 gSpoofBcd = 0, gSpoofMp = 0;
void ehci_vhub_spoof_stats(UInt32 *bcd, UInt32 *mp)
{
    if (bcd) *bcd = gSpoofBcd;
    if (mp)  *mp  = gSpoofMp;
}

/* p0i3b: config-descriptor diagnostic capture. Same reconstruction caveat as above — the producers
 * live in the control-transfer path and are not restored yet, so this reports zeros. Shapes come from
 * the header contract and the call site at ehci_uim.c:320. */
static UInt32 gGdDev = 0, gGdCfg = 0, gGdStr = 0, gGdOther = 0, gCfgFull = 0;
static UInt32 gCfgMaxActual = 0, gCfgSetupLen = 0, gCfgEpN = 0, gCfgEp4[4];
static UInt8  gCfgSnap48[48];
void ehci_vhub_cfgcap(UInt32 *gdDev, UInt32 *gdCfg, UInt32 *gdStr, UInt32 *gdOther, UInt32 *cfgFull,
                      UInt32 *maxActual, UInt32 *setupLen, UInt32 *epN, UInt32 *ep4, UInt8 *snap48)
{
    int i;
    if (gdDev)     *gdDev     = gGdDev;
    if (gdCfg)     *gdCfg     = gGdCfg;
    if (gdStr)     *gdStr     = gGdStr;
    if (gdOther)   *gdOther   = gGdOther;
    if (cfgFull)   *cfgFull   = gCfgFull;
    if (maxActual) *maxActual = gCfgMaxActual;
    if (setupLen)  *setupLen  = gCfgSetupLen;
    if (epN)       *epN       = gCfgEpN;
    if (ep4)  for (i = 0; i < 4;  i++) ep4[i]    = gCfgEp4[i];
    if (snap48) for (i = 0; i < 48; i++) snap48[i] = gCfgSnap48[i];
}

/* r35: task-context accessor so the trigger can show whether the real EHCI IRQ is FIRING (isrHits
 * climbing) or heartbeat-only (isrHits stuck at 0 while I/O still completes via the timer). */
void ehci_vhub_irq_stats(unsigned long *isrHits, int *a2live)
{
    if (isrHits) *isrHits = gIsrHits;
    if (a2live)  *a2live  = gA2Live;
}
