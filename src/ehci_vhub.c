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

/* Have we deliberately given port p to the 1.1 companion? Software intent first, hardware bit second, so
 * a disagreement with the controller can never resurrect a port we have already handed over. */
static int port_ceded(int p)
{
    if (p < 0 || p >= 15) return 0;
    if (gPortCeded[p]) return 1;
    if (p >= (int)gSoftc.nPorts || gSoftc.opBase == 0) return 0;
    return (ehci_read32(gSoftc.opBase, EHCI_PORTSC(p)) & EHCI_PORT_OWNER) ? 1 : 0;
}

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
    /* n11: a port we have deliberately ceded stays VISIBLE to Apple no matter what the Owner bit reads
     * back as. Re-hiding a ceded port was step 4 of the thrash loop. (Checked before the register read so
     * this stays one MMIO access — service_ports calls this per port per heartbeat.) */
    if (gPortCeded[p]) return 0;
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
static volatile int    gStormPainted = 0;   /* v46: storm_paint is a ONE-SHOT */
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
 * self-probe never gets bulk eps (gPOut/gPIn=ffffffff). This ring timestamps every port CONNECT/
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
static EpQ gCtrlQ;        /* device control endpoint 0 */
static EpQ gBulkQ[2];     /* [0] = bulk-OUT (dirIn 0), [1] = bulk-IN (dirIn 1) */
static EpQ *gDpQ = 0;             /* the endpoint queue the in-flight transfer was issued on */
static ehci_qtd *gDpTd = 0;       /* the in-flight (activated) qTD to reap */
static volatile UInt8 *gDownBuf = 0;                 /* r46: 20KB (5-page) DMA bounce for big data chunks */
static UInt32 gDownBufPhys[DOWN_DATA_PAGES];         /* physical addr of each bounce page (may be non-contiguous) */
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
static volatile UInt32 gDownRelink = 0;      /* r60: async-ring repairs performed (the QH-unlink freeze fix) */
static volatile UInt32 gLastAnchorLink = 0;  /* r60 diag: anchor->hlink (masked) at the last ring check; compare to gDownQHP */
/* r67 THROUGHPUT DIAGNOSTIC: measure the PURE data-phase rate. Two sticks both cap ~1.5-1.9 MB/s read =>
 * engine-limited, not device. Time each big (>=2KB) bulk DATA transfer with FRINDEX (125µs microframes) to
 * separate the data-phase rate from per-command overhead → decides Phase 2 (pre-queue) vs park-mode/RL. */
static volatile UInt32 gDataBytes = 0, gDataFrames = 0, gDataFr0 = 0; static volatile int gDpMeasured = 0;
/* Bulk endpoints (mass storage). uim6 records each endpoint (addr, endpt, dir 0=OUT/1=IN, maxpkt); uim7
 * routes a transfer by (addr, endpt). r63: each registered bulk endpoint is bound to its own resident QH
 * (gBulkQ[dirIn]) with HARDWARE toggle, so the `toggle` field below is now LEGACY/unused (the controller
 * owns the toggle) — kept only so the currently-disabled recovery code still compiles; removed in A2. */
#define NBULK 6
static struct { UInt32 addr, endpt; UInt8 dirIn, toggle; UInt16 maxpkt; UInt8 used; } gBulkEP[NBULK];
static volatile int gDpBulkEp = -1; static volatile UInt32 gDpNpkt = 1;
/* MINI FIX: Apple's BULK completion UPP deadlocks at interrupt level on the Mini's shared-interrupt
 * controller -> defer it to TASK level (compl_drain from selfprobe_tick). Dedicated line (MDD) = inline. */
typedef struct { void *upp, *pipe; long status; UInt32 actual; } ComplDef;
#define NCOMPL 16u
static volatile ComplDef gComplQ[NCOMPL];
static volatile UInt32 gComplHead = 0, gComplTail = 0, gComplDrop = 0;
/* r10 diagnostic: last bulk completion snapshot (interrupt-safe stores in down_reap; task-ctx reads
 * via ehci_vhub_bulk_stats from uim7, since down_reap runs at interrupt level where File Mgr is unsafe). */
static volatile long gBulkLastStat = 0; static volatile UInt32 gBulkDoneN = 0, gBulkErrN = 0;
static volatile UInt8 gLastData[16];
typedef struct { void *upp; void *pipe; void *dest; UInt32 addr, len; UInt8 pid; UInt8 obuf[64]; UInt32 olen;
                 void *obig; UInt32 obiglen; int bulkEp; } DownReq;   /* obig = large OUT source (write data > 64B) */
#define DOWNQ_N 48
static volatile DownReq gDownQ[DOWNQ_N];
static volatile UInt32 gDownQHead = 0, gDownQTail = 0, gDownQDrop = 0;

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
        for (i = 0; i < DOWN_DATA_PAGES; i++) {
            LogicalToPhysicalTable t2; unsigned long c2 = 1;
            t2.logical.address = (LogicalAddress)((UInt8 *)b + i * 0x1000);
            t2.logical.count   = 0x1000;
            if (GetPhysical(&t2, &c2) != noErr) { gDownInitErr = 6; return -1; }
            gDownBufPhys[i] = (UInt32)t2.physical[0].address;
        }
    }
    /* r63: three resident per-endpoint QHs, each with 2 alternating qTDs, all spliced into the async ring.
     * 32-byte-aligned page layout: QHs at 0x000/0x040/0x080; qTD pairs at 0x0C0.../0x100.../0x140...
     * (r45 lesson retained: we SPLICE into the anchor ring and never touch ASYNCLISTADDR, which stays = the
     * anchor set once by ehci_hc_start; overwriting it live was the old intermittent-enum wall.) */
    p = (UInt8 *)pg;
    epq_init(&gCtrlQ,    p, pgP, 0x000, 0x0C0, 0, 0,  64, 1);   /* control ep0 (addr set at SET_ADDRESS), DTC=1; qTD pool 0x0C0-0x1FF */
    epq_init(&gBulkQ[0], p, pgP, 0x040, 0x200, 0, 0, 512, 0);   /* bulk-OUT, DTC=0 (HW toggle); qTD pool 0x200-0x33F */
    epq_init(&gBulkQ[1], p, pgP, 0x080, 0x340, 0, 0, 512, 0);   /* bulk-IN,  DTC=0 (HW toggle); qTD pool 0x340-0x47F */
    gCbwBuf = (volatile UInt8 *)(p + 0x480); gCbwPhys = pgP + 0x480;   /* r69: CBW DMA buffer (spare page space) */
    gCswBuf = (volatile UInt8 *)(p + 0x4C0); gCswPhys = pgP + 0x4C0;   /* r69: CSW DMA buffer (13 bytes) */
    ehci_os_log("=== r63 (A1): per-endpoint QHs spliced (ctrl ep0 + bulk-OUT + bulk-IN); bulk toggle in HARDWARE (DTC=0) ===");
    ehci_os_logx("  ctrlQH phys",  gCtrlQ.qhP);
    ehci_os_logx("  bulkOUT phys", gBulkQ[0].qhP);
    ehci_os_logx("  bulkIN phys",  gBulkQ[1].qhP);
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
static int is_our_qh(UInt32 p) { return p == gCtrlQ.qhP || p == gBulkQ[0].qhP || p == gBulkQ[1].qhP; }
static void down_relink_if_needed(void)
{
    UInt32 aLink;
    if (!gDownReady || gSoftc.asyncAnchor == 0) return;
    aLink = QH_LINK_PTR(gSoftc.asyncAnchor->hlink);
    gLastAnchorLink = aLink;                        /* diag */
    if (is_our_qh(aLink)) return;                   /* anchor heads into our QHs -> intact (happy path, no writes) */
    ehci_qh_link_async(&gSoftc, gCtrlQ.qh, gCtrlQ.qhP);       /* should never fire — re-splice all three */
    ehci_qh_link_async(&gSoftc, gBulkQ[0].qh, gBulkQ[0].qhP);
    ehci_qh_link_async(&gSoftc, gBulkQ[1].qh, gBulkQ[1].qhP);
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
    for (i = 0; i < 5; i++) cur->buffer[i] = useBounce ? ehci_cpu_to_le32(gDownBufPhys[i]) : 0;   /* r46: 5 pages -> up to 20KB */
    __asm__ __volatile__("eieio");                          /* body + new dummy visible BEFORE Active */
    cur->token = ehci_cpu_to_le32(tok);                     /* ACTIVATE — the single Active-setting store, LAST */
    __asm__ __volatile__("eieio");
    q->dummy = (UInt8)nd;
    gDpQ = q; gDpTd = cur;                                  /* the qTD down_reap polls for completion */
}
static void down_issue(volatile DownReq *r)
{
    EpQ *q; UInt32 dt, i; int useBounce = 0;
    down_arm_ase();
    if (r->bulkEp < 0) {                                    /* CONTROL (ep0): SETUP=DATA0, data/status=DATA1 */
        q = &gCtrlQ;
        if (q->addr != r->addr) epq_program(q, r->addr, 0, 64, 1);   /* (re)point ep0 at this device addr (QH idle) */
        dt = (r->pid == 2) ? 0u : 1u;
        gDpNpkt = 1;
    } else {                                                /* BULK: the endpoint's OWN resident QH (HW toggle) */
        UInt32 m = gBulkEP[r->bulkEp].maxpkt ? gBulkEP[r->bulkEp].maxpkt : 512;
        q = &gBulkQ[gBulkEP[r->bulkEp].dirIn ? 1 : 0];
        dt = 0;                                             /* ignored for bulk (DTC=0 — HW owns the toggle) */
        gDpNpkt = (r->len + m - 1) / m; if (gDpNpkt == 0) gDpNpkt = 1;
    }
    if (r->obig) { UInt32 nn = (r->obiglen > DOWN_BUF_MAX) ? DOWN_BUF_MAX : r->obiglen;   /* large OUT: copy write data to the bounce */
                   for (i = 0; i < nn; i++) gDownBuf[i] = ((volatile UInt8 *)r->obig)[i]; useBounce = 1; }
    else if (r->pid == 2 || (r->pid == 0 && r->olen)) { for (i = 0; i < r->olen; i++) gDownBuf[i] = r->obuf[i]; useBounce = 1; }
    else if (r->pid == 1 && r->len) { useBounce = 1; }        /* IN: HC DMAs into the bounce */
    gDpUPP = r->upp; gDpPipe = r->pipe; gDpDest = r->dest; gDpLen = r->len; gDpIsIn = (r->pid == 1);
    gDpBulkEp = r->bulkEp;
    gDpLastAddr = r->addr; gDpLastPid = r->pid;             /* r36 diag: name the in-flight downstream xfer */
    gDpArmTick = *(volatile UInt32 *)0x016AUL; gDpBusy = 1;   /* r54: arm the stall watchdog with the 60Hz Ticks clock */
    gDpMeasured = 0;         /* r74: write/single-qTD data-phase measurement OFF — [dataphase] is now the READ pre-queue's on-the-wire per-command time (fed from down_reap for BIO_PH_PREREAD) */
    if (gDpMeasured) gDataFr0 = ehci_read32(gSoftc.opBase, EHCI_FRINDEX) & 0x3FFFUL;
    epq_issue(q, r->len, r->pid, useBounce, dt);
}
static void down_pump(void)
{
    volatile DownReq *r;
    if (gDpBusy) return;
    if (gDownQHead == gDownQTail) return;
    r = &gDownQ[gDownQTail % DOWNQ_N];
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
    if (len > DOWN_BUF_MAX) len = DOWN_BUF_MAX;
    r->upp = upp; r->pipe = pipe; r->dest = (void *)buf; r->addr = addr; r->pid = (UInt8)pid; r->len = len; r->olen = 0; r->obig = 0; r->bulkEp = bulkEp;
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
static void capture_timeout_state(void)
{
    gToCmd      = ehci_read32(gSoftc.opBase, EHCI_USBCMD);
    gToSts      = ehci_read32(gSoftc.opBase, EHCI_USBSTS);
    gToAsync    = ehci_read32(gSoftc.opBase, EHCI_ASYNCLISTADDR);
    gToQhP      = gDpQ ? gDpQ->qhP : 0;                                  /* r63: the ACTIVE endpoint QH/qTD */
    gToQhEpChar = gDpQ ? ehci_le32_to_cpu(gDpQ->qh->epChar) : 0;
    gToQhCurQtd = gDpQ ? ehci_le32_to_cpu(gDpQ->qh->curQtd) : 0;
    gToQhOvlTok = gDpQ ? ehci_le32_to_cpu(gDpQ->qh->ovlToken) : 0;
    gToQtdTok   = gDpTd ? ehci_le32_to_cpu(gDpTd->token) : 0;
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
static void down_reap(void)
{
    UInt32 tok; long status; UInt32 actual = 0;
    if (gDpBusy) {
        tok = ehci_le32_to_cpu(gDpTd->token);   /* r63: poll the qTD activated on the endpoint's own QH */
        if (tok & EHCI_QTD_STATUS_ACTIVE) {
            UInt32 el = TICKS_NOW - gDpArmTick;
            if (el > gMaxStallTicks) gMaxStallTicks = el;   /* r54: track the device's worst-case GC pause */
            if (el > DOWN_WATCHDOG_TICKS) { gDownTimeouts++; capture_timeout_state(); status = -6640L; }  /* watchdog + r39 snapshot */
            else return;
        } else if (tok & (EHCI_QTD_STATUS_HALTED | EHCI_QTD_STATUS_XACTERR | EHCI_QTD_STATUS_BABBLE | EHCI_QTD_STATUS_DBERR)) {
            gDownErr++; status = -6640L;
        } else {
            UInt32 resid = EHCI_QTD_BYTES_GET(tok);
            status = 0; gDownDone++;
            if (gDpMeasured) {   /* r67: data-phase rate — FRINDEX delta (125µs microframes), read BEFORE the copy */
                UInt32 fr = ehci_read32(gSoftc.opBase, EHCI_FRINDEX) & 0x3FFFUL;
                gDataFrames += (fr - gDataFr0) & 0x3FFFUL; gDataBytes += gDpLen; gDpMeasured = 0;
            } else if (gBioPhase == BIO_PH_PREREAD) {   /* r74: ON-THE-WIRE per-command READ time = issue->reap (device latency + xfer
                                                         * + CSW + the 1 IRQ); accumulated into the [dataphase] fields. Compare the
                                                         * resulting rate to [speed] end-to-end: [rcmd] >> [speed] => the File Mgr / above-us
                                                         * overhead is the wall (read-ahead is the lever); [rcmd] ~= [speed] => on-the-wire bound. */
                UInt32 fr = ehci_read32(gSoftc.opBase, EHCI_FRINDEX) & 0x3FFFUL;
                gDataFrames += (fr - gDataFr0) & 0x3FFFUL; gDataBytes += gDpLen;
            }
            actual = (resid <= gDpLen) ? (gDpLen - resid) : gDpLen;
            if (gDpIsIn && gDpLen) {
                volatile UInt8 *dd = (volatile UInt8 *)gDpDest; UInt32 i;
                for (i = 0; i < actual; i++) dd[i] = gDownBuf[i];
            }
            /* r63: NO software toggle advance — with DTC=0 the controller maintains the bulk data toggle
             * in the endpoint QH overlay across every transfer (the OHCI-style fix for the BOT desync). */
        }
        if (gDpBulkEp >= 0) {   /* snapshot for the task-context diagnostic log */
            UInt32 i; gBulkLastStat = status;
            if (status == 0) gBulkDoneN++; else gBulkErrN++;
            for (i = 0; i < 16; i++) gLastData[i] = gDownBuf[i];   /* IN: received CSW/data; OUT: sent CBW */
        }
        gDpLastStat = status;                                  /* r36 diag: last downstream reap status */
        gDpBusy = 0;
        if (gDpUPP) {
            if (gDpBulkEp >= 0) {   /* v20: defer bulk completion to task level on BOTH machines (was Mini-only via sharedCompanion). The MDD dedicated-line INLINE call intermittently DEADLOCKS Apple's bulk UPP at interrupt level = the pre-mount freeze v18/v19 hit. compl_drain (task level) delivers it. Control completions (gDpBulkEp<0) still go inline below. */
                UInt32 hh = gComplHead;
                if ((hh - gComplTail) < NCOMPL) {
                    gComplQ[hh & (NCOMPL - 1u)].upp = (void *)gDpUPP;
                    gComplQ[hh & (NCOMPL - 1u)].pipe = (void *)gDpPipe;
                    gComplQ[hh & (NCOMPL - 1u)].status = status;
                    gComplQ[hh & (NCOMPL - 1u)].actual = actual;
                    gComplHead = hh + 1u;
                } else gComplDrop++;
            } else if (gDpBulkEp >= 0)
                ((ehci_usl_intcomplete)gDpUPP)((void *)gDpPipe, status, (unsigned long)actual);
            else
                ((ehci_usl_complete)gDpUPP)((void *)gDpPipe, (unsigned long)status);
        }
        if (gBioPhase && !gDpUPP) bio_advance(status);   /* r34/r57: advance the async block-I/O state machine (pass the xfer status for recovery) */
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
 * an endpoint also programs its dedicated QH (gBulkQ[dirIn]); the QH's overlay was init'd to DATA0. */
/* r91 volatile: reconnect_reset writes these from the ISR-driven hub re-enum context; the task-level
 * self-probe / bulk_xfer read them. Non-volatile => readers got STALE values (the r86-r90 "reset doesn't
 * stick" bug). Boot worked only because its enumeration is task-level (same context). */
static volatile int    gMountedOnce = 0;    /* r87: set at the first self-probe completion — arms the reconnect logic */
static volatile int    gReprobe = 0;        /* r88: reconnect => state 0 takes over ASSERTIVELY (skip the passive park-wait) */
static volatile int    gBurst = 0;          /* r90: log the next N selfprobe_tick entries after a reconnect (diagnostic) */
static volatile int    gLoopDbg = 0;        /* r94: trace the app idle-loop for N passes after a reconnect — proves whether
                                             * the loop REACHES the tickFn call post-reconnect or stalls earlier (ExpertIdleTask/USL) */
static volatile UInt32 gCurAddr = 0;        /* r87: device address the engine is currently bound to */
static volatile int    gPState = 0;         /* r91 volatile: reconnect_reset (ISR ctx) resets it; self-probe (task) reads it.
                                             * r95: moved up from the self-probe globals so ehci_vhub_bulk_xfer's reconnect-window
                                             * diagnostic (below) can see it. */
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
static void reconnect_reset(void); /* r87 fwd decl: bounce-robust reset of stale device state */
long ehci_vhub_create_bulk(UInt32 addr, UInt32 endpt, UInt32 dirIn, UInt32 maxpkt)
{
    int i, freeSlot = -1;
    int reconn = (gMountedOnce && addr != gCurAddr);   /* r87: post-mount enum at a NEW address = reinsert or bounce */
    UInt32 mp = maxpkt ? maxpkt : 512;
    /* Phase 1 r87 (hot re-mount, bounce-robust). STOP the async schedule before the epq_program below
     * reprograms a QH (epq_program on a LIVE ring = the r84/r85 freeze). On a post-mount new-address enum
     * (a reinsert OR one of the SanDisk's re-enumeration bounces) reset the stale device state so the
     * self-probe re-runs on the CURRENT device — the r86 crash was the engine locked onto a bounced-past
     * stale address. Fires on EVERY address change (not once), so each bounce is handled cleanly. */
    ase_quiesce();
    if (reconn) reconnect_reset();
    gCurAddr = addr;
    for (i = 0; i < NBULK; i++) {
        if (gBulkEP[i].used && gBulkEP[i].addr == addr && gBulkEP[i].endpt == endpt) {
            gBulkEP[i].dirIn = (UInt8)(dirIn ? 1 : 0);
            gBulkEP[i].maxpkt = (UInt16)mp;
            gBulkEP[i].toggle = 0;                       /* legacy field; HW owns the toggle now */
            epq_program(&gBulkQ[dirIn ? 1 : 0], addr, endpt, mp, 0);   /* bind this endpoint to its resident bulk QH */
            return 0;
        }
        if (!gBulkEP[i].used && freeSlot < 0) freeSlot = i;
    }
    if (freeSlot < 0) return -1;
    gBulkEP[freeSlot].addr = addr; gBulkEP[freeSlot].endpt = endpt;
    gBulkEP[freeSlot].dirIn = (UInt8)(dirIn ? 1 : 0);
    gBulkEP[freeSlot].maxpkt = (UInt16)mp;
    gBulkEP[freeSlot].toggle = 0; gBulkEP[freeSlot].used = 1;
    epq_program(&gBulkQ[dirIn ? 1 : 0], addr, endpt, mp, 0);   /* program the endpoint's resident bulk QH (DTC=0) */
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
     * (gMountedOnce && gPState < 10) — which is open for the WHOLE of a hot re-insert, since
     * reconnect_reset sets gPState = 0. At 5 ring entries a call for the first 60 calls that is up to 300
     * of the ring's 384 slots, which would starve and DROP the enumeration's own messages exactly when we
     * need them. The r92 investigation it was written for is long closed. */
    if (EHCI_VERBOSE && gMountedOnce && gPState < 10) {   /* r95: reconnect-window trace. Shows whether Apple keeps issuing
                                           * (active) or stops (quiet), whether the heartbeat SIH is alive
                                           * (gVhubTick advancing), and whether/when the SIH armed the takeover.
                                           * ★★ n6b: these were ehci_os_log (FILE MANAGER). The original comment
                                           * asserted "this runs at TASK level inside Apple's ExpertIdleTask" —
                                           * true when it was written, FALSE since n5. The BOT probe's
                                           * pb_cbw/pb_in now reach here from bot_step at INTERRUPT level, and
                                           * this gate (gMountedOnce && gPState < 10) is OPEN during exactly one
                                           * window: a hot re-insert, because reconnect_reset sets gPState = 0
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
/* gPState moved up to the cross-context volatile group (r95) so bulk_xfer's diagnostic can see it. */
static volatile UInt32 gPIdle = 0, gPLastCnt = 0, gPMark = 0, gPTag = 0x50524231UL; /* 'PRB1' */
static volatile UInt32 gPErrMark = 0;   /* p1b: gBulkErrN snapshot at issue — lets pb_failed() see a STALL */
static UInt32 gPBlkSize = 512, gPBlkCnt = 0;
static volatile int gPOut = -1, gPIn = -1;    /* r91 volatile: reconnect_reset resets, self-probe/pb_* read (cross-ctx) */

static void pb_find_eps(void)
{
    int i; gPOut = gPIn = -1;
    for (i = 0; i < NBULK; i++) if (gBulkEP[i].used) {
        if (gBulkEP[i].dirIn) { if (gPIn  < 0) gPIn  = i; }
        else                  { if (gPOut < 0) gPOut = i; }
    }
}
static void pb_cbw_dir(const UInt8 *cdb, int cdbLen, UInt32 dataLen, UInt8 flags)   /* CBW; flags 0x80=data-IN, 0x00=data-OUT */
{
    int i; if (gPOut < 0) return;
    for (i = 0; i < 31; i++) gPB[i] = 0;
    gPB[0]=0x55; gPB[1]=0x53; gPB[2]=0x42; gPB[3]=0x43;                        /* 'USBC' */
    gPB[4]=(UInt8)gPTag; gPB[5]=(UInt8)(gPTag>>8); gPB[6]=(UInt8)(gPTag>>16); gPB[7]=(UInt8)(gPTag>>24);
    gPB[8]=(UInt8)dataLen; gPB[9]=(UInt8)(dataLen>>8); gPB[10]=(UInt8)(dataLen>>16); gPB[11]=(UInt8)(dataLen>>24);
    gPB[12]=flags;                                                            /* bmCBWFlags */
    gPB[13]=0;                                                                /* LUN 0 */
    gPB[14]=(UInt8)cdbLen;                                                    /* CDB length */
    for (i = 0; i < cdbLen && i < 16; i++) gPB[15+i] = cdb[i];
    gPTag++;
    gPMark = gBulkDoneN + gBulkErrN; gPErrMark = gBulkErrN;   /* p1b: also snapshot the ERROR count (see pb_failed) */
    (void)ehci_vhub_bulk_xfer(0, 0, gPB, gBulkEP[gPOut].addr, gBulkEP[gPOut].endpt, 31, 0);   /* OUT (31B, via obuf) */
}
static void pb_cbw(const UInt8 *cdb, int cdbLen, UInt32 dataLen) { pb_cbw_dir(cdb, cdbLen, dataLen, 0x80); }  /* data-IN CBW */
static void pb_in(UInt32 len)   /* read len bytes on the IN endpoint into gPB */
{
    if (gPIn < 0) return;
    gPMark = gBulkDoneN + gBulkErrN; gPErrMark = gBulkErrN;   /* p1b */
    (void)ehci_vhub_bulk_xfer(0, 0, gPB, gBulkEP[gPIn].addr, gBulkEP[gPIn].endpt, len, 1);     /* IN */
}
static void pb_out(UInt32 len)  /* write len bytes from gPB on the OUT endpoint (large OUT via obig) */
{
    if (gPOut < 0) return;
    gPMark = gBulkDoneN + gBulkErrN; gPErrMark = gBulkErrN;   /* p1b */
    (void)ehci_vhub_bulk_xfer(0, 0, gPB, gBulkEP[gPOut].addr, gBulkEP[gPOut].endpt, len, 0);   /* OUT */
}
static int pb_ready(void) { return (gBulkDoneN + gBulkErrN) != gPMark; }      /* prior transfer finished */
/* ★ p1b: pb_ready() means "the transfer FINISHED" — success OR error. That blindness is why the p1a probe
 * marched straight through a STALLed READ CAPACITY and still published 'Eusb': every state advanced on
 * pb_ready() alone, so a halted endpoint looked like progress and state 5 parsed the leftover CBW bytes
 * ('USBC' = 0x55534243, +1 = the bogus 0x55534244 "block count" in the log). pb_failed() distinguishes them. */
static int pb_failed(void) { return gBulkErrN != gPErrMark; }                 /* prior transfer ERRORED (e.g. STALL) */
#define PB_BE32(o) (((UInt32)gPB[o]<<24)|((UInt32)gPB[(o)+1]<<16)|((UInt32)gPB[(o)+2]<<8)|gPB[(o)+3])
#define BUF_BE32(b,o) (((UInt32)(b)[o]<<24)|((UInt32)(b)[(o)+1]<<16)|((UInt32)(b)[(o)+2]<<8)|(b)[(o)+3])

/* ==================== r22 (BYPASS m2): synchronous block-read SERVICE for our own disk driver ==========
 * Our block driver is a separate native PEF (installed post-selfprobe) and can't
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
static long ehci_usb_read(UInt32 lba, UInt32 count, void *buf)   /* synchronous BOT READ(10); >=0 = blocks read */
{
    UInt8 cdb[10]; UInt32 nbytes; int i;
    if (gPOut < 0 || gPIn < 0 || count == 0) return -1;
    if (count > DOWN_MAX_BLOCKS) count = DOWN_MAX_BLOCKS;          /* r46: up to 40*512=20480 = DOWN_BUF_MAX */
    nbytes = count * 512;
    for (i = 0; i < 10; i++) cdb[i] = 0;
    cdb[0] = 0x28;                                                /* READ(10) */
    cdb[2]=(UInt8)(lba>>24); cdb[3]=(UInt8)(lba>>16); cdb[4]=(UInt8)(lba>>8); cdb[5]=(UInt8)lba;
    cdb[7]=(UInt8)(count>>8); cdb[8]=(UInt8)count;
    pb_cbw(cdb, 10, nbytes);  if (pb_wait()) return -2;           /* CBW out */
    pb_in(nbytes);            if (pb_wait()) return -3;           /* data -> gPB */
    { UInt8 *d = (UInt8 *)buf; for (i = 0; i < (int)nbytes; i++) d[i] = gPB[i]; }
    pb_in(13);                if (pb_wait()) return -4;           /* CSW */
    return (gPB[12] == 0) ? (long)count : -5;                     /* CSW status 0 = passed */
}
/* synchronous BOT WRITE(10): CBW(OUT) -> DATA(OUT, large via obig) -> CSW(IN). >=0 = blocks written. */
static long ehci_usb_write(UInt32 lba, UInt32 count, void *buf)
{
    UInt8 cdb[10]; UInt32 nbytes; int i; UInt8 *src = (UInt8 *)buf;
    if (gPOut < 0 || gPIn < 0 || count == 0) return -1;
    if (count > DOWN_MAX_BLOCKS) count = DOWN_MAX_BLOCKS;          /* r46: up to 40 blocks */
    nbytes = count * 512;
    for (i = 0; i < 10; i++) cdb[i] = 0;
    cdb[0] = 0x2A;                                                /* WRITE(10) */
    cdb[2]=(UInt8)(lba>>24); cdb[3]=(UInt8)(lba>>16); cdb[4]=(UInt8)(lba>>8); cdb[5]=(UInt8)lba;
    cdb[7]=(UInt8)(count>>8); cdb[8]=(UInt8)count;
    pb_cbw_dir(cdb, 10, nbytes, 0x00);  if (pb_wait()) return -2; /* CBW out (bmCBWFlags data-OUT) */
    for (i = 0; i < (int)nbytes; i++) gPB[i] = src[i];           /* stage write data in gPB (obig source) */
    pb_out(nbytes);                     if (pb_wait()) return -3; /* data out */
    pb_in(13);                          if (pb_wait()) return -4; /* CSW (overwrites gPB — data already sent) */
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
#define REC_BASE          20
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
static void bio_issue_read(UInt32 nbytes)
{
    EpQ *qo = &gBulkQ[0];                                         /* bulk-OUT : CBW */
    EpQ *qi = &gBulkQ[1];                                         /* bulk-IN  : data chain -> CSW */
    int co = qo->dummy, no = (co + 1) % QTD_POOL;                 /* OUT: CBW slot, new tail dummy */
    int nData = (int)((nbytes + 0x4FFF) / 0x5000);               /* # of <=20KB(0x5000) data qTDs (ceil) */
    int base, cswSlot, dumSlot, i, j;
    ehci_qtd *cbw = qo->td[co], *cbwDum = qo->td[no], *csw, *inDum;
    UInt32 cbwTok = EHCI_QTD_STATUS_ACTIVE | EHCI_QTD_CERR(3) | EHCI_QTD_BYTES(31) | EHCI_QTD_PID_OUT;                 /* no IOC */
    UInt32 cswTok = EHCI_QTD_STATUS_ACTIVE | EHCI_QTD_CERR(3) | EHCI_QTD_BYTES(13) | EHCI_QTD_PID_IN | EHCI_QTD_IOC;
    if (nData < 1) nData = 1;
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
            gDpBulkEp = gPIn; gDpLastAddr = (gPIn >= 0) ? gBulkEP[gPIn].addr : 0; gDpLastPid = 1;
            gDpQ = qi; gDpTd = csw;                              /* reap the terminal CSW qTD */
            gDpArmTick = TICKS_NOW; gDpBusy = 1;                 /* arm watchdog; busy while the CSW qTD is ACTIVE */
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
static void bio_issue_write(const void *src, UInt32 nbytes)
{
    EpQ *qo = &gBulkQ[0];                                         /* bulk-OUT : CBW -> data chain */
    EpQ *qi = &gBulkQ[1];                                         /* bulk-IN  : CSW (terminal) */
    int nData = (int)((nbytes + 0x4FFF) / 0x5000);               /* # of <=20KB(0x5000) data qTDs (ceil) */
    int co, dn, ci, nci, i, j;
    ehci_qtd *cbw, *outDum, *csw, *inDum;
    UInt32 cbwTok = EHCI_QTD_STATUS_ACTIVE | EHCI_QTD_CERR(3) | EHCI_QTD_BYTES(31) | EHCI_QTD_PID_OUT;                 /* no IOC */
    UInt32 cswTok = EHCI_QTD_STATUS_ACTIVE | EHCI_QTD_CERR(3) | EHCI_QTD_BYTES(13) | EHCI_QTD_PID_IN | EHCI_QTD_IOC;
    if (nData < 1) nData = 1;
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
    gDpBulkEp = gPIn; gDpLastAddr = (gPOut >= 0) ? gBulkEP[gPOut].addr : 0; gDpLastPid = 0;
    gDpArmTick = TICKS_NOW; gDpBusy = 1;                          /* busy while the terminal CSW qTD is ACTIVE */
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
        bio_issue_read(nbytes);                              /* CBW-OUT + [data -> CSW]-IN, IOC on CSW only */
        gBioPhase = BIO_PH_PREREAD;
    } else {                                                 /* r77: WRITE -> pre-queue the whole command (1 interrupt) */
        bio_build_cbw(gCbwBuf, cdb, nbytes, 0x00);           /* CBW into gCbwBuf (data-OUT flag) */
        bio_issue_write(r->buf, nbytes);                     /* OUT:[CBW -> data-chain] + IN:[CSW], IOC on CSW only */
        gBioPhase = BIO_PH_PREWRITE;
    }
}
static void bio_kick(void)                        /* if idle and the queue is non-empty, start the next request */
{
    if (gBioPhase != 0 || gBioHead == gBioTail) return;
    if (!gBioQ[gBioTail % BIOQ_N].ready) return;   /* r+: slot atomically claimed but not yet filled — wait */
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
    if (r->actCount) *r->actCount = (res == 0) ? (long)r->reqBytes : (long)(r->reqBytes - r->count * 512);
    gBioQ[gBioTail % BIOQ_N].ready = 0;            /* r+: slot empty before dequeue (so it is 0 when reclaimed) */
    gBioTail++; gBioPhase = 0; gBioRetry = 0;      /* dequeue BEFORE completing (completion may enqueue more) */
    complete_now(done, res);                       /* v43: pacing retired — complete immediately (full speed) */
    bio_kick();                                    /* start the next queued request */
}
/* r57: begin BOT reset recovery for the current (timed-out) chunk — issue the Bulk-Only Mass Storage Reset. */
static void bio_recover_start(void)
{
    UInt32 addr;
    if (gPOut < 0 || gPIn < 0) return;
    addr = gBulkEP[gPOut].addr;
    gDownRecov++;
    recov_setup(0x21, 0xFF, 0x0000, 0x0000);       /* class, iface recipient; bRequest 0xFF = Bulk-Only Reset; iface 0 */
    down_submit(0, 0, gRecovSetup, addr, 8, 2, -1);   /* SETUP on ep0 */
    gBioPhase = REC_RESET_SETUP;
}
/* r64: reset the HOST bulk QH hardware toggles to DATA0, to match the device after a Bulk-Only Reset +
 * CLEAR_FEATURE(HALT). The toggle lives in each bulk QH's overlay and may only be rewritten while the QH is
 * quiescent, so briefly stop the async schedule (EHCI 4.8: clear ASE, wait ASS=0), re-arm both bulk QHs to
 * idle/DATA0, then re-enable (wait ASS=1). The control QH pauses too (~a few ms) — fine, this is the rare
 * recovery path. NOT the IAA doorbell (that's for REMOVING a QH and must not be rung while ASE is disabled).
 * Host and device toggles now agree; skipping this was the r57-r60 reset-loop bug. */
static void bot_reset_host_toggles(void)
{
    volatile void *op = gSoftc.opBase;
    long spin;
    ehci_write32(op, EHCI_USBCMD, ehci_read32(op, EHCI_USBCMD) & ~EHCI_CMD_ASE);
    for (spin = 0; spin < 200000; spin++)
        if (!(ehci_read32(op, EHCI_USBSTS) & EHCI_STS_ASS)) break;   /* async schedule stopped */
    epq_arm_idle(&gBulkQ[0]);           /* bulk-OUT: empty overlay, dt=0 */
    epq_arm_idle(&gBulkQ[1]);           /* bulk-IN:  empty overlay, dt=0 */
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
    UInt32 addr = (gPOut >= 0) ? gBulkEP[gPOut].addr : 0;
    if (status != 0) { bio_finish(r, status); return; }   /* ep0 itself unresponsive -> give up on this request */
    switch (gBioPhase) {
    case REC_RESET_SETUP:   down_submit(0, 0, gPB, addr, 0, 1, -1); gBioPhase = REC_RESET_STATUS; break;  /* reset STATUS-IN (0 len) */
    case REC_RESET_STATUS:  recov_setup(0x02, 0x01, 0x0000, (UInt16)(gBulkEP[gPIn].endpt | 0x80u));   /* CLEAR_FEATURE(HALT) bulk-IN (IN first) */
                            down_submit(0, 0, gRecovSetup, addr, 8, 2, -1); gBioPhase = REC_CLRIN_SETUP; break;
    case REC_CLRIN_SETUP:   down_submit(0, 0, gPB, addr, 0, 1, -1); gBioPhase = REC_CLRIN_STATUS; break;
    case REC_CLRIN_STATUS:  recov_setup(0x02, 0x01, 0x0000, gBulkEP[gPOut].endpt);            /* CLEAR_FEATURE(HALT) bulk-OUT */
                            down_submit(0, 0, gRecovSetup, addr, 8, 2, -1); gBioPhase = REC_CLROUT_SETUP; break;
    case REC_CLROUT_SETUP:  down_submit(0, 0, gPB, addr, 0, 1, -1); gBioPhase = REC_CLROUT_STATUS; break;
    case REC_CLROUT_STATUS: bot_reset_host_toggles();  /* r64: re-match host QH toggles to the device's post-reset DATA0 */
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
        if (BIO_BOT_RECOVERY && gBioRetry < BIO_MAX_RETRY && gPOut >= 0 && gPIn >= 0) { gBioRetry++; bio_recover_start(); }
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
    case 1:                                       /* CBW done -> data phase */
        if (r->isWrite) { for (i = 0; i < nbytes; i++) gPB[i] = r->buf[i]; pb_out(nbytes); }  /* stage+send write data */
        else pb_in(nbytes);                                                                    /* read data into gPB */
        gBioPhase = 2; break;
    case 2:                                       /* data done -> (read: copy in) then CSW */
        if (!r->isWrite) { for (i = 0; i < nbytes; i++) r->buf[i] = gPB[i]; }
        pb_in(13); gBioPhase = 3; break;
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
static long ehci_usb_submit(IOCommandID cmdID, UInt32 lba, UInt32 count, void *buf, int isWrite, long *actCount)
{
    UInt32 depth = gBioHead - gBioTail; BioReq *r;
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
        if (gPBlkCnt && (lba >= gPBlkCnt || (lba + count) > gPBlkCnt || (lba + count) < lba)) fl |= 2;  /* LBA off the device */
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

typedef long (*ehci_usb_rw_fn)(UInt32 lba, UInt32 count, void *buf);
typedef long (*ehci_usb_submit_fn)(IOCommandID cmdID, UInt32 lba, UInt32 count, void *buf, int isWrite, long *actCount);
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
void ehci_vhub_notify_ejected(void);   /* n8: clean-quit hook, appended LAST so the block
                                           * driver's shorter prefix view of this struct is unaffected */
long ehci_vhub_prepare_quit(void);
static struct { UInt32 magic; ehci_usb_rw_fn readFn; ehci_usb_rw_fn writeFn; UInt32 blkSize, blkCnt; ehci_usb_submit_fn submitFn; ehci_usb_health_fn healthFn; ehci_usb_tostate_fn toStateFn; ehci_usb_sim_fn simReplugFn; ehci_usb_arm_fn obsArmFn; ehci_usb_arm_fn tickFn; ehci_usb_crumb_fn loopFn; ehci_usb_quit_fn quitFn; ehci_usb_drain_fn drainFn; ehci_usb_drain_fn ejectFn; UInt32 magic2; } gSvc;
static void ehci_vhub_publish_service(void)
{
    gSvc.magic = 0x45555342UL;  /* 'EUSB' */
    gSvc.readFn = ehci_usb_read;
    gSvc.writeFn = ehci_usb_write;
    gSvc.blkSize = 512; gSvc.blkCnt = gPBlkCnt;
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
    gSvc.magic2  = 0x45555342UL;                   /* n9: END-marker. The block driver verifies THIS before
                                                    * touching drainFn, so if the two fragments' views of
                                                    * this struct ever drift it fails safe instead of
                                                    * calling a garbage pointer. */
    gSvc.quitFn = ehci_vhub_prepare_quit;          /* n8: activator calls this before exiting */
    gSvc.loopFn = ehci_vhub_loopcrumb;             /* r94: app idle-loop breadcrumb (armed only around a reconnect) */
    (void)NewGestaltValue('Eusb', (long)&gSvc);
    ehci_os_log("=== r34: USB block service published via Gestalt 'Eusb' (sync rw + async submit) ===");
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
        epq_arm_idle(&gCtrlQ); epq_arm_idle(&gBulkQ[0]); epq_arm_idle(&gBulkQ[1]);
    }
    __asm__ __volatile__("eieio");
    /* Engine + BOT state back to idle. Endpoint identity (gPOut/gPIn/gBulkEP) and the QH DMA pages
     * are preserved — enumeration owns those, not us. NOTE (C-phase TODO): the real disconnect path
     * must FAIL-COMPLETE any in-flight gBioQ request (IOCommandIsComplete with an error) rather than
     * just dropping it; here the caller guarantees the engine is idle first, so draining is safe. */
    gDpBusy = 0;
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
            epq_program(&gBulkQ[gBulkEP[i].dirIn ? 1 : 0], gBulkEP[i].addr, gBulkEP[i].endpt,
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
    if (gPOut < 0 || gPIn < 0) { ehci_os_log("[replug] no bulk endpoints — skipped"); return 0; }
    addr = gBulkEP[gPIn].addr;                 /* device address (ep0 + bulk share it) */
    r = ehci_usb_read(0, 1, blk);              /* baseline signature (engine is idle on entry) */
    if (r >= 0) sig0 = ((UInt32)blk[510] << 8) | blk[511];
    for (i = 0; i < n; i++) {
        ehci_vhub_engine_teardown(0);          /* quiesce; keep toggle (device still live) */
        ehci_vhub_engine_rebuild(addr);        /* re-enable the async schedule */
        r = ehci_usb_read(0, 1, blk);          /* one BOT READ(10) through the rebuilt engine */
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
static void ase_quiesce(void)
{
    volatile void *op = gSoftc.opBase;
    long spin;
    if (!gDownAseOn) return;
    ehci_write32(op, EHCI_USBCMD, ehci_read32(op, EHCI_USBCMD) & ~EHCI_CMD_ASE);
    for (spin = 0; spin < 200000; spin++)
        if (!(ehci_read32(op, EHCI_USBSTS) & EHCI_STS_ASS)) break;   /* wait for the schedule to actually stop */
    gDownAseOn = 0;
}
/* Phase 1 r87 (hot re-mount, bounce-robust): reset the stale device state on a POST-MOUNT re-enumeration at a
 * new address — a reinsert, OR one of the SanDisk's re-enumeration bounces (r86 showed the reinsert bounces
 * through several addresses, and the engine locking onto a bounced-past stale address is what crashed). Fires
 * on EVERY address change (create_bulk gate: gMountedOnce && addr != gCurAddr), so each bounce is handled and
 * only the CURRENT device's endpoints survive in gBulkEP. MUST run with the schedule already stopped
 * (create_bulk calls ase_quiesce first) so the epq_arm_idle below is safe. Leaves ctrl ep0 alone (per-qTD
 * toggle + per-transfer re-address) so the ongoing enumeration's control transfers are not disturbed. */
static void reconnect_reset(void)
{
    int i;
    ehci_os_log("=== RECONNECT: post-mount re-enumeration at a new address — reset stale state (r88) ===");
    epq_arm_idle(&gBulkQ[0]); epq_arm_idle(&gBulkQ[1]);   /* fresh device => bulk toggles back to DATA0 */
    gDpBusy = 0; gBioPhase = 0; gBioRetry = 0; gBioTail = gBioHead; { int _i; for (_i=0;_i<BIOQ_N;_i++) gBioQ[_i].ready = 0; }   /* idle the BOT/down engine */
    gFenceApple = 0;                                     /* r93: do NOT fence during Apple's reconnect probing. r90 set this to
                                                          * 1 immediately, to "free the pump" for a self-probe we now know was
                                                          * never being CALLED post-reconnect (not starved) — r92's tickFn is
                                                          * what fixes the driving. Fencing here rejected Apple's reconnect
                                                          * TUR/REQUEST SENSE with -6640, and Apple's error handling on that
                                                          * intermittently HUNG the machine (r92 froze there; r91 bounced past
                                                          * it). Boot never fences during probing — it lets Apple stall+park,
                                                          * THEN the self-probe takes over and fences. Mirror boot: stay
                                                          * unfenced until the passive park-wait sets the fence itself. */
    gPState = 0; gPIdle = 0; gPLastCnt = 0;              /* self-probe re-runs from the top (passive park-wait, as at boot) */
    gPOut = gPIn = -1;                                   /* pb_find_eps re-selects the CURRENT device's endpoints */
    for (i = 0; i < NBULK; i++) gBulkEP[i].used = 0;     /* drop ALL stale endpoint regs (incl. bounced-past addrs) */
    gDownReady = 1;                                      /* engine ready; ASE re-arms on the next transfer */
    gReprobe = 0;                                        /* r93: assertive re-take-over DISABLED (see selfprobe_tick case 0) —
                                                          * it issued a CBW while Apple was still probing the shared eps = collision */
    gBurst = 40;                                         /* r90: capture the next 40 selfprobe_tick entries (post-reconnect path) */
    gLoopDbg = 150;                                      /* r94: trace the app idle-loop for the next 150 passes after this reconnect */
    gSihArmed = 0; gSihQuiet = 0; gSihLastCnt = gBulkDoneN + gBulkErrN;   /* r95: re-arm the SIH takeover-watcher for this reconnect */
    ehci_os_logx("  r88 reset done — gPState now", gPState);   /* diag: confirm the reset took */
    ehci_os_logx("  r94 gLoopDbg armed", (unsigned long)gLoopDbg);   /* proves the arm ran (absence of APPLOOP crumbs => loop stalled) */
    ehci_os_logx("  r89 &gPState", (unsigned long)(void *)&gPState);   /* compare w/ the self-probe entry's &gPState */
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
 * Placed here because it needs recov_setup, bot_reset_host_toggles, ase_quiesce and
 * reconnect_reset, and must precede selfprobe_tick.
 * ================================================================================================ */

/* n5: split into ISSUE and POLL so the async engine can wait without blocking. Control phases retire on
 * the DOWN counters (gDownDone/Err/Timeouts) — NOT the bulk counters pb_ready() watches, which is a trap:
 * a control phase never moves gBulkDoneN, so polling pb_ready() for one waits forever. */
static UInt32 gCtlMark = 0;
static void ctl_issue(volatile UInt8 *buf, UInt32 addr, UInt32 len, UInt32 pid)
{
    gCtlMark = gDownDone + gDownErr + gDownTimeouts;
    down_submit(0, 0, buf, addr, len, pid, -1);      /* -1 = control (ep0) */
}
static int ctl_done(void) { return (gDownDone + gDownErr + gDownTimeouts) != gCtlMark; }
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
    if (gPOut < 0 || gPIn < 0) return -100;
    addr = gBulkEP[gPOut].addr;
    recov_setup(0x21, 0xFF, 0x0000, 0x0000);                                /* Bulk-Only Mass Storage Reset */
    if (pb_ctrl_phase(gRecovSetup, addr, 8, 2)) return -1;                   /*   SETUP */
    if (pb_ctrl_phase(gPB, addr, 0, 1))         return -2;                   /*   STATUS-IN (0 len) */
    recov_setup(0x02, 0x01, 0x0000, (UInt16)(gBulkEP[gPIn].endpt | 0x80u));  /* CLEAR_FEATURE(HALT) IN first */
    if (pb_ctrl_phase(gRecovSetup, addr, 8, 2)) return -3;
    if (pb_ctrl_phase(gPB, addr, 0, 1))         return -4;
    recov_setup(0x02, 0x01, 0x0000, gBulkEP[gPOut].endpt);                   /* CLEAR_FEATURE(HALT) OUT */
    if (pb_ctrl_phase(gRecovSetup, addr, 8, 2)) return -5;
    if (pb_ctrl_phase(gPB, addr, 0, 1))         return -6;
    bot_reset_host_toggles();                                               /* host bulk QHs -> DATA0 */
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
    pb_cbw(cdb, cdbLen, dataLen);
    if (pb_wait())   return -1;
    if (pb_failed()) return -2;                              /* the CBW itself never went out */
    if (dataLen) {
        pb_in(dataLen);
        if (pb_wait()) return -3;
        if (pb_failed()) {                                   /* data phase STALLED (BOT 5.3.4): clear the
                                                              * halt, then STILL read the CSW so the device
                                                              * stays in sync */
            (void)pb_clear_halt(gPIn);
        } else if (save && saveLen) {
            UInt32 k; for (k = 0; k < saveLen; k++) save[k] = gPB[k];
        }
    }
    pb_in(13);
    if (pb_wait()) return -4;
    if (pb_failed()) { (void)pb_clear_halt(gPIn); return -5; }
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
/* n4c: tell the ALREADY-INSTALLED block driver that the media state changed, via its private
 * kCsUsbDiskMedia control (magic-guarded; see usb_disk.c). This is the piece hot-plug was missing: the
 * re-enumeration and re-probe were always correct, but AddDrive + PostEvent(diskEvt) live in the block
 * driver's kInitialize, which only ever runs at install time — so on a re-insert nothing told the File
 * Manager the media was back. TASK LEVEL ONLY: the block driver re-scans the volume synchronously through
 * 'Eusb' and writes its log, exactly as kInitialize does from this same context. NEVER from the ISR. */
static void blk_notify_media(int present)
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
    e = PBControlSync(&pb);
    ehci_os_logx(present ? "n4c media-ARRIVED control -> block driver, err"
                         : "n4c media-GONE control -> block driver, err", (UInt32)(long)e);
}
/* ★★ n7: UNEXPECTED REMOVAL — unmount the volume and, if it is busy, tell the user, exactly as Apple's own
 * "USB Mass Storage Support" extension does. Reverse-engineering that extension (re-work/) shows precisely
 * this design: it imports UnmountVol, Eject, NMInstall and NMRemove, and carries the strings
 *   "Please reconnect the USB device …"
 *   "Before disconnecting the device, you must close all files and applications using it and choose
 *    [Eject] from the [Special] menu."
 * i.e. on removal it unmounts, and when the volume is BUSY it posts a Notification Manager alert asking the
 * user to plug the device back in. Without this the volume just stays mounted over absent media and the
 * File Manager eventually reports it as damaged, which is the behaviour this replaces.
 *
 * ⚠ TASK LEVEL ONLY, and deliberately NOT called from inside the block driver's own Control handler: doing
 * it there would re-enter the File Manager from inside a Device Manager call. We run here, in
 * selfprobe_tick, AFTER blk_notify_media(0)'s PBControlSync has fully returned — no nesting.
 * ⚠ Depends on the n6e offLinErr fix: UnmountVol flushes first, and that flush must FAIL FAST against the
 * absent device rather than stalling in the transfer engine. */
static NMRec  gNM;
static Str255 gNMMsg;
static int    gNMPosted = 0;
/* ★ n8: WORD-FOR-WORD Apple. Recovered from their USB Mass Storage Support extension's resource fork
 * (re-work/USBMassStorageSupport.rsrc), which carries this exact text with the device name substituted
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
static Str63 gDevName;
static void capture_device_name(const UInt8 *inq)
{
    int i, n = 0, end;
    for (end = 15; end >= 8 && inq[end] == ' '; end--) ;
    for (i = 8; i <= end && n < 40; i++) gDevName[++n] = inq[i];
    if (n) gDevName[++n] = ' ';
    for (end = 31; end >= 16 && inq[end] == ' '; end--) ;
    for (i = 16; i <= end && n < 60; i++) gDevName[++n] = inq[i];
    gDevName[0] = (unsigned char)n;
}
/* ★ n10: Apple's EJECT alert, word for word from their resource fork:
 *   "You may now remove the cartridge from the USB device ⟨name⟩ because your Macintosh is finished with it.
 *    The cartridge will not be remounted until it is removed from the drive."
 * Posted when the Finder ejects our volume — the case that previously produced no message at all. */
static void notify_reconnect(const unsigned char *volName);
void ehci_vhub_notify_ejected(void)
{
    int i = 0;
    if (gNMPosted) { (void)NMRemove(&gNM); gNMPosted = 0; }
    nm_cat(&i, "You may now remove the cartridge from the USB device ");
    gNMMsg[++i] = MACQ_OPEN;
    { int n; for (n = 1; n <= gDevName[0] && i < 200; n++) gNMMsg[++i] = gDevName[n]; }
    gNMMsg[++i] = MACQ_CLOSE;
    nm_cat(&i, " because your Macintosh is finished with it.");
    nm_cat(&i, "\r\rThe cartridge will not be remounted until it is removed from the drive.");
    gNMMsg[0] = (unsigned char)i;
    gNM.qType = nmType; gNM.nmMark = 0; gNM.nmIcon = 0; gNM.nmSound = 0;
    gNM.nmStr = gNMMsg; gNM.nmResp = (NMUPP)-1L; gNM.nmRefCon = 0;
    if (NMInstall(&gNM) == noErr) gNMPosted = 1;
}
static void notify_reconnect(const unsigned char *volName)
{
    int i = 0, n;
    if (gNMPosted) { (void)NMRemove(&gNM); gNMPosted = 0; }
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
    if (NMInstall(&gNM) == noErr) gNMPosted = 1;
}
/* Returns the number of OUR volumes that could NOT be unmounted (busy). notifyIfBusy posts Apple's
 * "please reconnect" alert, which is right for a REMOVAL but not for a clean quit — hence the flag. */
static long blk_unmount_our_volumes(int notifyIfBusy)
{
    QHdrPtr q = GetVCBQHdr();
    void *v, *next;
    long busy = 0;
    if (!gBlkDref) return 0;
    for (v = (void *)(q ? q->qHead : 0); v; v = next) {
        VCB *vcb = (VCB *)v;
        next = (void *)vcb->qLink;
        if (vcb->vcbDRefNum != gBlkDref) continue;     /* not one of ours */
        {
            HParamBlockRec pb; OSErr e;
            Str27 nm; int k;
            for (k = 0; k <= vcb->vcbVN[0] && k < 28; k++) nm[k] = vcb->vcbVN[k];
            pb.volumeParam.ioCompletion = 0;
            pb.volumeParam.ioNamePtr    = 0;
            pb.volumeParam.ioVRefNum    = vcb->vcbVRefNum;
            e = PBUnmountVol((ParmBlkPtr)&pb);
            ehci_os_logx("n7 UnmountVol on our volume, err", (UInt32)(long)e);
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
static void blk_unmount_removed_volumes(void) { (void)blk_unmount_our_volumes(1); }

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
    long busy = blk_unmount_our_volumes(0);
    ehci_os_logx("n8 prepare-quit: our volumes still busy (0 = safe to quit)", (UInt32)busy);
    return busy;
}
static void install_block_driver(void)
{
    OSErr e; short dref = 0;
    /* n4c: already installed = this is a RE-insert. Do not install twice (and do not AddDrive twice, which
     * would leak a drive number per insertion) — just re-announce the media. */
    if (gBlkInstalled) { blk_notify_media(1); return; }
    e = InstallDriverFromMemory((Ptr)gUsbDiskPef, (ByteCount)gUsbDiskPefLen, "\pUSBDisk",
                                (RegEntryID *)&gSoftc.node, 48, 127, &dref);
    ehci_os_logx("n3 InstallDriverFromMemory(block driver) err", (UInt32)(long)e);
    ehci_os_logx("  drefNum", (UInt32)dref);
    if (e == noErr && dref != 0) {
        gBlkInstalled = 1; gBlkDref = dref;   /* n4c: keep the refNum so re-inserts can reach the driver */
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
        gComplTail++;
        if (upp) ((ehci_usl_intcomplete)upp)(pipe, st, (unsigned long)act);
    }
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
#define SELFENUM_ADDR 1u                 /* the address we assign our device (our bus, our namespace) */
static volatile int   gSelfEnumDone = 0; /* 1 once endpoints are registered (or the port went to the companion) */
static int            gSelfEnumPort = -1;
static UInt32         gSelfEnumTries = 0;
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
    volatile UInt8 *buf;
} CtlState;
static CtlState gCtl;
static void ctl_begin(UInt8 bmRT, UInt8 bReq, UInt16 wValue, UInt16 wIndex,
                      UInt32 addr, volatile UInt8 *buf, UInt32 len, int dir)
{
    enum_setup(bmRT, bReq, wValue, wIndex, (UInt16)(dir ? len : 0));
    gCtl.pc = 0; gCtl.addr = addr; gCtl.buf = buf; gCtl.len = len; gCtl.dir = dir;
}
/* 0 = still running, 1 = complete, -1 = failed. Waits on ctl_done() — the DOWN counters — never pb_ready(). */
static int ctl_step(void)
{
    switch (gCtl.pc) {
    case 0:
        ctl_issue(gEnumSetup, gCtl.addr, 8, 2);                         /* SETUP */
        gCtl.pc = 1; gCtl.t0 = frame_ms(); return 0;
    case 1:
        if (!ctl_done()) return (frame_ms() - gCtl.t0 > 800UL) ? -1 : 0;
        if (gCtl.dir) {
            ctl_issue(gCtl.buf, gCtl.addr, gCtl.len, 1);                /* DATA-IN */
            gCtl.pc = 2; gCtl.t0 = frame_ms(); return 0;
        }
        ctl_issue(gEnumDesc, gCtl.addr, 0, 1);                          /* STATUS-IN (no-data control) */
        gCtl.pc = 3; gCtl.t0 = frame_ms(); return 0;
    case 2:
        if (!ctl_done()) return (frame_ms() - gCtl.t0 > 800UL) ? -1 : 0;
        ctl_issue(gEnumDesc, gCtl.addr, 0, 0);                          /* STATUS-OUT */
        gCtl.pc = 3; gCtl.t0 = frame_ms(); return 0;
    case 3:
        if (!ctl_done()) return (frame_ms() - gCtl.t0 > 800UL) ? -1 : 0;
        return 1;
    }
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
static int bot_step(void)
{
    switch (gBot.pc) {
    case 0:
        pb_cbw(gBot.cdb, gBot.cdbLen, gBot.dataLen);
        gBot.pc = 1; gBot.t0 = frame_ms(); return 0;
    case 1:
        if (!pb_ready()) return (frame_ms() - gBot.t0 > 800UL) ? -1 : 0;
        if (pb_failed()) return -1;                       /* the CBW itself never went out */
        if (gBot.dataLen) { pb_in(gBot.dataLen); gBot.pc = 2; gBot.t0 = frame_ms(); return 0; }
        pb_in(13); gBot.pc = 4; gBot.t0 = frame_ms(); return 0;
    case 2:
        if (!pb_ready()) return (frame_ms() - gBot.t0 > 800UL) ? -1 : 0;
        if (pb_failed()) {                                /* data phase STALLED (BOT 5.3.4): clear the halt,
                                                           * but STILL read the CSW so the device stays in sync */
            gBot.stalled = 1;
            ctl_begin(0x02, 0x01, 0x0000,
                      (UInt16)(gBulkEP[gPIn].endpt | 0x80u), gBulkEP[gPIn].addr, gPB, 0, 0);
            gBot.pc = 3; return 0;
        }
        if (gBot.save && gBot.saveLen) {
            UInt32 k; for (k = 0; k < gBot.saveLen; k++) gBot.save[k] = gPB[k];
        }
        pb_in(13); gBot.pc = 4; gBot.t0 = frame_ms(); return 0;
    case 3: {
        int r = ctl_step();
        if (r == 0) return 0;
        pb_in(13); gBot.pc = 4; gBot.t0 = frame_ms(); return 0;   /* recovery is advisory; read the CSW */
    }
    case 4:
        if (!pb_ready()) return (frame_ms() - gBot.t0 > 800UL) ? -1 : 0;
        if (pb_failed()) {
            ctl_begin(0x02, 0x01, 0x0000,
                      (UInt16)(gBulkEP[gPIn].endpt | 0x80u), gBulkEP[gPIn].addr, gPB, 0, 0);
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
    UInt32 addr, tot, o, t0;
    int    epIn, epOut, mpIn, mpOut, inMSC, iface, cfgVal, tur;
    UInt32 blocks, blkSize;
} AsState;
static AsState gAs;
static volatile int gAsBusy = 0;      /* re-entrancy guard: heartbeat and uim23 may both call in */
/* Set at interrupt level when the async probe has fully verified the medium; consumed at TASK level, which
 * is where 'Eusb' publication and the block-driver handoff have to happen (NewGestaltValue and
 * InstallDriverFromMemory are both task-only). This flag IS the interrupt->task boundary of the design. */
static volatile int gAsProbeOK = 0;
/* Set by the engine at interrupt level when it needs TASK level to register the bulk endpoints; cleared by
 * selfprobe_tick once done. create_bulk must not run from the SIH — see the note at its call site. */
static volatile int gAsNeedBulk = 0;

static void as_fail(const char *why, UInt32 v)
{
    ehci_os_ilogx(why, v);
    gAs.running = 0; gAs.failed = 1; gAs.pc = 0;
    gSelfEnumTries++;                                     /* bounded retry, same policy as the sync path */
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
/* Park until TASK level has done a piece of work the engine must not do from the SIH. */
#define AS_TASK(L) \
    do { gAs.pc = (L); gAs.t0 = frame_ms(); return; \
         case (L): \
            if (gAsNeedBulk) { \
                if (frame_ms() - gAs.t0 > 5000UL) { as_fail("!! n5: task-level endpoint registration never ran (no pump?)", (L)); } \
                return; \
            } \
    } while (0)
/* Drive the BOT sub-machine to completion. */
#define AS_BOT(L) \
    do { gAs.pc = (L); return; \
         case (L): { int _r = bot_step(); \
                     if (_r == 0) return; \
                     if (_r < 0) { as_fail("!! n5 PROBE: BOT transport failure at step", (L)); return; } } \
    } while (0)

static void as_advance(void)
{
    switch (gAs.pc) {
    case 0:
        ehci_os_ilog("=== n5 SELFENUM (ASYNC, no application): enumerating the hidden device ourselves ===");
        ehci_os_ilogx("  port", (UInt32)gAs.port);
        gAs.addr = SELFENUM_ADDR;
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
        gAs.t0 = frame_ms();
        gAs.pc = 20; return;
    case 20:
        if (frame_ms() - gAs.t0 < 100UL) return;          /* T_ATTDB */
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
         * bounce flag: abandon this attempt WITHOUT surrendering the port, and let the re-connect re-arm. */
        if (gSelfEnumPort != gAs.port) {
            ehci_os_ilog("  n5 SELFENUM: device bounced during reset — abandoning this attempt, port KEPT");
            gAs.running = 0; gAs.pc = 0;      /* deliberately NOT gSelfEnumDone, NOT a tries++ */
            return;
        }
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
                gSelfEnumDone = 1; gAs.running = 0; gAs.pc = 0;   /* companion owns it; do not retry */
                return;
            }
            as_fail("!! n5 SELFENUM: port did not enable and nothing is connected; portsc", portsc);
            return;
        }
        ehci_os_ilogx("  port ENABLED at high speed; portsc",
                      ehci_read32(gSoftc.opBase, EHCI_PORTSC(gAs.port)));

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
            /* ★★ n9 COEXISTENCE FIX. This device enumerated fine at high speed but is not Bulk-Only mass
             * storage — a USB 2.0 hub, webcam, printer, whatever. We cannot drive it and we never will.
             * The OLD behaviour was as_fail() -> three retries -> give up, which left the port HIDDEN from
             * Apple (Owner = 0 keeps apple_hidden_port() true) and unused by us: the device ended up
             * invisible to the OS *and* dead, which is STRICTLY WORSE than not installing this driver at
             * all — without us the companion would have run it at 1.1. Found by code review, not on
             * hardware, and it is any non-storage USB 2.0 device, so it is not an edge case.
             * Do exactly what the not-high-speed path does: give the port to the 1.1 companion and stop.
             *
             * ★★★ n11: DISABLE THE PORT BEFORE SURRENDERING IT. n9 set Owner = 1 on a port that was still
             * ENABLED at high speed, and the bit never read back as 1 — the device stayed on the HS bus and
             * the companion never saw it. Both cede paths that have always worked (the two not-high-speed
             * sites) surrender a port that is NOT enabled, which is precisely the EHCI-spec handoff point:
             * §2.3.9 says software releases ownership "when the attached device is not a high-speed device",
             * i.e. after a reset that did not enable the port. Here the device DID chirp high-speed and we
             * enumerated it, so we have to undo that first: clear Port Enable to take it off the HS bus, and
             * only then hand the port over, so the companion sees a fresh connect and enumerates it at full
             * speed. The read-backs are logged either side because this is a hardware-behaviour claim and the
             * next run should settle it rather than leave it inferred. */
            UInt32 pv = ehci_read32(gSoftc.opBase, EHCI_PORTSC(gAs.port));
            UInt32 w  = pv & ~EHCI_PORTSC_RW1C;
            w &= ~EHCI_PORT_ENABLE;                  /* off the high-speed bus first */
            ehci_write32(gSoftc.opBase, EHCI_PORTSC(gAs.port), w);
            (void)ehci_read32(gSoftc.opBase, EHCI_PORTSC(gAs.port));   /* posted-write flush */
            w = ehci_read32(gSoftc.opBase, EHCI_PORTSC(gAs.port)) & ~EHCI_PORTSC_RW1C;
            w |= EHCI_PORT_OWNER;                    /* now the spec's precondition holds */
            ehci_write32(gSoftc.opBase, EHCI_PORTSC(gAs.port), w);
            (void)ehci_read32(gSoftc.opBase, EHCI_PORTSC(gAs.port));
            gPortCeded[gAs.port] = 1;                /* n11: remember the DECISION — see the flag's comment */
            ehci_os_ilog("  n9 SELFENUM: high-speed but NOT Bulk-Only mass storage -> handed the port to the 1.1 companion");
            ehci_os_ilogx("  iface/epIn/epOut",
                          ((UInt32)(gAs.iface & 0xFF) << 16) | ((UInt32)(gAs.epIn & 0xFF) << 8) | (UInt32)(gAs.epOut & 0xFF));
            ehci_os_ilogx("  n11 PORTSC before cede", pv);
            ehci_os_ilogx("  n11 PORTSC after cede (bit13 = 0x2000 set means the handoff took)",
                          ehci_read32(gSoftc.opBase, EHCI_PORTSC(gAs.port)));
            gSelfEnumDone = 1; gAs.running = 0; gAs.pc = 0;   /* Apple owns it now; do not retry */
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
        if (gPOut < 0 || gPIn < 0) {
            as_fail("!! n5 SELFENUM: bulk endpoints registered but pb_find_eps found none; gPOut/gPIn",
                    ((UInt32)(gPOut & 0xFF) << 8) | (UInt32)(gPIn & 0xFF));
            return;
        }
        ehci_os_ilog("=== n5 SELFENUM COMPLETE — we enumerated + own the device; starting BOT probe ===");
        ehci_os_ilogx("  outEp.addr/ep", ((UInt32)gBulkEP[gPOut].addr << 16) | gBulkEP[gPOut].endpt);
        ehci_os_ilogx("  inEp.addr/ep",  ((UInt32)gBulkEP[gPIn].addr  << 16) | gBulkEP[gPIn].endpt);

        /* ---- 9. BOT probe: INQUIRY ---- */
        bot_begin(gAsCdbInq, 6, 36, gAsInq, 36);
        AS_BOT(9);
        if (gBot.csw != 0) { as_fail("!! n5 PROBE: INQUIRY CSW status", (UInt32)gBot.csw); return; }
        capture_device_name(gAsInq);   /* n10: for Apple's device-named alerts */
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
        ehci_os_ilogx("  n5 SELFPROBE READ CAPACITY blocks",  gAs.blocks);
        ehci_os_ilogx("                     blockSize", gAs.blkSize);
        if (gAs.blkSize != 512UL || gAs.blocks < 4UL) {
            as_fail("!! n5 PROBE: implausible geometry (stale/garbage data phase); blockSize", gAs.blkSize);
            return; }
        gPBlkCnt = gAs.blocks; gPBlkSize = gAs.blkSize;   /* feed the 'Eusb' geometry the block driver reads */

        /* ---- 12. READ block 0 — the proof we can really read the medium ---- */
        gAsCdbRd[0] = 0x28;
        gAsCdbRd[1] = gAsCdbRd[2] = gAsCdbRd[3] = gAsCdbRd[4] = gAsCdbRd[5] = 0;
        gAsCdbRd[6] = 0; gAsCdbRd[7] = 0; gAsCdbRd[8] = 1; gAsCdbRd[9] = 0;
        bot_begin(gAsCdbRd, 10, 512, gAsBlk0, 512);
        AS_BOT(14);
        if (gBot.csw != 0) { as_fail("!! n5 PROBE: READ block 0 CSW status", (UInt32)gBot.csw); return; }
        ehci_os_ilogx("  n5 SELFPROBE READ block 0 b0_3", BUF_BE32(gAsBlk0, 0));

        ehci_os_ilog("=== n5 SELFPROBE COMPLETE (async, interrupt-driven — no application involved) ===");
        gAs.running = 0; gAs.done = 1; gAs.pc = 0;
        gSelfEnumDone = 1;
        gFenceApple   = 1;         /* belt-and-braces: Apple must never touch our endpoints */
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
static void as_tick(void)
{
    if (gAsBusy) return;
    gAsBusy = 1;
    if (!gAs.running && !gSelfEnumDone && gSelfEnumPort >= 0 && gSelfEnumTries < 3u && gPState != 10) {
        gAs.pc = 0; gAs.port = gSelfEnumPort; gAs.running = 1; gAs.done = 0; gAs.failed = 0;
        gAsProbeOK = 0;
    }
    if (gAs.running) as_advance();
    gAsBusy = 0;
}
#endif /* APPLE_HIDE */

void ehci_vhub_selfprobe_tick(void)
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
    ehci_os_ilog_drain();
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
    { static UInt32 gLiveN = 0;
      /* n5d2: every 64 calls, not 512. The first run produced exactly ONE block, which told us the state
       * was clean at call 1 but gave no TIME SERIES — and the decisive question is whether gVhubTick keeps
       * CLIMBING (heartbeat alive) or freezes (heartbeat dead, so service_ports never sees the insert,
       * which would explain both the missing port event and the 1.1 mount). ~10 calls/sec means a block
       * every ~6 s. */
      if ((gLiveN++ & 0x3FUL) == 0) {
          ehci_os_log("n5d uim23 ALIVE:");
          ehci_os_logx("  callN",         gLiveN);
          ehci_os_logx("  gVhubTick (heartbeat/SIH alive if climbing)", gVhubTick);
          ehci_os_logx("  gServiceStop (1 = heartbeat suppressed!)", (UInt32)gServiceStop);
          ehci_os_logx("  gIsrHits",      gIsrHits);
          ehci_os_logx("  gPState",       (UInt32)gPState);
          ehci_os_logx("  gSelfEnumDone (1 silences PORTMAP)", (UInt32)gSelfEnumDone);
          ehci_os_logx("  gSelfEnumPort (-1 = no connect seen)", (UInt32)(long)gSelfEnumPort);
          ehci_os_logx("  gSelfEnumTries", gSelfEnumTries);
          ehci_os_logx("  gAs.running",   (UInt32)gAs.running);
          ehci_os_logx("  gAs.pc",        (UInt32)gAs.pc);
          ehci_os_logx("  gHideLogMask",  gHideLogMask);
          ehci_os_logx("  port0 PORTSC",  ehci_read32(gSoftc.opBase, EHCI_PORTSC(0)));
          ehci_os_logx("  port4 PORTSC",  ehci_read32(gSoftc.opBase, EHCI_PORTSC(4)));
      } }
    /* (2) Complete the handoff the engine cannot do itself: NewGestaltValue and InstallDriverFromMemory
     *     are both task-only, so the engine sets gAsProbeOK and we finish the job here. ⚠ THIS is the
     *     remaining app dependency — the probe is appless, the first MOUNT is not yet. Closing it means
     *     installing once at activation (no media) and reducing the runtime path to AddDrive-once +
     *     PostEvent, both of which are interrupt-safe. */
    /* n6b: the engine parks and asks US to register the bulk endpoints, because create_bulk does
     * ase_quiesce() + epq_program() and neither may run from the SIH (the hot-re-insert freeze). */
    if (gAsNeedBulk) {
        (void)ehci_vhub_create_bulk(gAs.addr, (UInt32)gAs.epOut, 0, (UInt32)gAs.mpOut);
        (void)ehci_vhub_create_bulk(gAs.addr, (UInt32)gAs.epIn,  1, (UInt32)gAs.mpIn);
        pb_find_eps();          /* pb_cbw/pb_in index gBulkEP via gPOut/gPIn; nothing else sets them */
        ehci_os_logx("  n6b bulk endpoints registered at task level; gPOut/gPIn",
                     ((UInt32)(gPOut & 0xFF) << 8) | (UInt32)(gPIn & 0xFF));
        gAsNeedBulk = 0;        /* releases the engine's AS_TASK park */
    }
    if (gAsProbeOK) {
        gAsProbeOK = 0;
        pb_find_eps();
        ehci_os_logx("  n5 handoff at task level; gIsrHits (real IRQ during probe; 0 = heartbeat-only)", gIsrHits);
        if (gPOut >= 0 && gPIn >= 0) {
            ehci_vhub_publish_service(); gPState = 10; gMountedOnce = 1;
            install_block_driver();   /* first insert: install + AddDrive + diskEvt. Later: re-announce. */
        } else {
            ehci_os_log("!! n5: probe reported OK but the bulk endpoints are missing — not publishing 'Eusb'");
            gSelfEnumDone = 0;
        }
    }
    /* p1a: an enumerated device was unplugged. Reset the probe state HERE (task level — reconnect_reset
     * does File-Mgr logging and must never run at interrupt level) so gPState returns to 0 and the case-0
     * hook can re-enumerate on the next insert. */
    if (gSelfEnumRearm) {
        gSelfEnumRearm = 0;
        ehci_os_log("=== SELFENUM: enumerated device was pulled — resetting probe state for re-enumeration ===");
        blk_notify_media(0);   /* n4c: mark the media gone BEFORE we tear the endpoints down, so kStatus
                                * stops reporting "disk present" for a volume that is no longer there. */
        blk_unmount_removed_volumes();   /* n7: and actually UNMOUNT it, as Apple's extension does. MUST
                                          * follow blk_notify_media(0) — gDiskInPlace has to be 0 first so
                                          * UnmountVol's flush fails fast with offLinErr rather than
                                          * stalling in the transfer engine against absent media. */
        ase_quiesce();         /* n4c: reconnect_reset's own contract is that the async schedule must
                                * already be STOPPED before it epq_arm_idles the bulk QHs — create_bulk
                                * honours that and this path did not. It is the documented r84/r85 freeze
                                * shape, so close it rather than keep relying on luck. */
        reconnect_reset();
        gSelfEnumDone = 0; gSelfEnumTries = 0;
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
     * gPState=1, post-takeover INQUIRY) with gDpBusy=1 = a bulk transfer issued whose completion never
     * arrives. Log the engine state ~1/sec while stuck so we see WHY, with NO per-tick flood. Reads:
     *   gIsrHits climbing = EHCI IRQs still firing (-> reap/routing bug) vs FLAT = no IRQ for our completion;
     *   gDownDone advancing = transfers retire on the wire vs FROZEN = nothing completing;
     *   gDownTimeouts climbing = the down watchdog IS firing (recovery failing) vs 0 = watchdog never runs;
     *   USBCMD bit5 (ASE 0x20) / USBSTS bit15 (ASS 0x8000) = is the async schedule actually RUNNING;
     *   USBSTS 0x1000 (HCHalted) / 0x10 (host system error) = controller faulted. */
    if (gDpBusy) {
        static UInt32 nextStall = 0;
        UInt32 nowT = *(volatile UInt32 *)0x016AUL;          /* lowmem Ticks (60 Hz) */
        if ((long)(nowT - nextStall) >= 0) {
            nextStall = nowT + 60UL;                         /* ~1 s */
            ehci_os_log("!! v47 STALL — self-probe transfer not completing:");
            ehci_os_logx("  gPState", (UInt32)gPState);
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
        }
    }
    /* n5: give the engine a step from here too. Harmless duplication of the heartbeat's call (as_tick is
     * idempotent and re-entrancy-guarded); it just saves a heartbeat of latency when an app IS pumping. */
    as_tick();
    if (gAs.running) return;          /* engine mid-sequence: leave the legacy machine alone */
#endif
    if (EHCI_VERBOSE) {   /* r88 diag: log gPState transitions so we can SEE the post-reconnect re-probe path (or lack of it) */
        static UInt32 lastSt = 0xFFFFFFFFUL;
        if (gPState != lastSt) { lastSt = gPState;
            ehci_os_log("selfprobe_tick state:"); ehci_os_logx("  gPState", gPState);
            ehci_os_logx("  gReprobe", (UInt32)gReprobe); ehci_os_logx("  gPOut", (UInt32)(long)gPOut); }
    }
    if (EHCI_VERBOSE) {   /* r89 diag: UNCONDITIONAL periodic entry log — proves selfprobe_tick is CALLED + the gPState it SEES */
        static UInt32 stN = 0;
        if ((stN++ & 0x1FFUL) == 0) {   /* every 512 calls */
            ehci_os_log("r89 selfprobe entry:");
            ehci_os_logx("  callN", stN);        ehci_os_logx("  gPState", (UInt32)gPState);
            ehci_os_logx("  &gPState", (unsigned long)(void *)&gPState);
            ehci_os_logx("  gReprobe", (UInt32)gReprobe);
            ehci_os_logx("  gPOut", (UInt32)(long)gPOut); ehci_os_logx("  gPIn", (UInt32)(long)gPIn);
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
        ehci_os_log("r90 burst:"); ehci_os_logx("  gPState", (UInt32)gPState);
        ehci_os_logx("  gReprobe", (UInt32)gReprobe); ehci_os_logx("  gFence", (UInt32)gFenceApple);
        ehci_os_logx("  gPOut", (UInt32)(long)gPOut); ehci_os_logx("  gPIn", (UInt32)(long)gPIn);
    }
    switch (gPState) {
    case 0:   /* wait for the mounter to exercise both bulk eps then go permanently quiet (parked) */
        pb_find_eps();
        if (gReprobe && gPOut >= 0 && gPIn >= 0) {   /* r95: the SIH watcher (sih_reconnect_arm) armed this. It fires ONLY
                                                      * after Apple has gone QUIET (parked) AND been fenced, so taking over
                                                      * now is safe — no active Apple transfer to collide with. This runs the
                                                      * instant ExpertIdleTask releases the task level after the SIH fenced.
                                                      * (r93 disabled the old r88 assertive path because it fired while Apple
                                                      * was still active; the SIH arm-gate is what makes it safe now.) */
            gReprobe = 0; gFenceApple = 1;
            ehci_os_log("=== SELFPROBE: SIH-armed reconnect takeover (Apple parked+fenced) ===");
            ehci_os_logx("  gSihArmTick", (UInt32)gSihArmTick); ehci_os_logx("  gVhubTick(now)", (UInt32)gVhubTick);
            ehci_os_logx("  outEp.addr", gBulkEP[gPOut].addr); ehci_os_logx("  inEp.addr", gBulkEP[gPIn].addr);
            /* v49: the device may be mid-BOT from Apple's abandoned probe, so reset the transport and put
             * both bulk toggles back to DATA0 before we issue our first CBW on the taken-over endpoints. */
            ehci_os_logx("  v49 takeover BOT reset rc (0=ok)", (UInt32)(long)pb_bot_reset());
            pb_cbw(cdbInq, 6, 36); gPState = 1; break;
        }
        if (EHCI_VERBOSE) {   /* r31 DIAGNOSTIC: periodically log WHY we haven't parked — cracks the intermittent no-mount
             * (user confirms identical insertion every time, so the variance is intrinsic timing). Read
             * on a stalled run: gPOut/gPIn = ffffffff => endpoints never created (enum didn't start);
             * bulkCnt stuck + gPIdle NOT climbing + vhubTick FROZEN => heartbeat died (root cause);
             * gPIdle climbing but never parks => park-detection logic. Bounded to ~every 256 ticks. */
            static UInt32 gDbgN = 0;
            if ((gDbgN++ & 0xFF) == 0) {
                ehci_os_log("SELFPROBE wait(state0):");
                ehci_os_logx("  gPOut", (UInt32)(long)gPOut);
                ehci_os_logx("  gPIn",  (UInt32)(long)gPIn);
                ehci_os_logx("  bulkCnt", gBulkDoneN + gBulkErrN);
                ehci_os_logx("  gPIdle", gPIdle);
                ehci_os_logx("  vhubTick", gVhubTick);
            }
        }
        if (gPOut >= 0 && gPIn >= 0) {
            UInt32 c = gBulkDoneN + gBulkErrN;
            if (c == gPLastCnt) gPIdle++; else { gPIdle = 0; gPLastCnt = c; }
            if (c >= 2 && gPIdle > 40) {          /* BOT ran (>=2 xfers) + ~40 quiet ticks => parked */
                gFenceApple = 1;                  /* r32: from here on, fence the Apple driver off our shared endpoints */
                ehci_os_log("=== SELFPROBE: mounter parked; taking over bulk endpoints (Apple fenced) ===");
                ehci_os_logx("  outEp.addr", gBulkEP[gPOut].addr); ehci_os_logx("  outEp.ep", gBulkEP[gPOut].endpt);
                ehci_os_logx("  inEp.addr",  gBulkEP[gPIn].addr);  ehci_os_logx("  inEp.ep",  gBulkEP[gPIn].endpt);
                pb_cbw(cdbInq, 6, 36); gPState = 1;
            }
        }
        break;
    case 1: if (pb_ready()) { pb_in(36); gPState = 2; } break;                 /* INQUIRY data */
    case 2: if (pb_ready()) {
                ehci_os_log("SELFPROBE INQUIRY:");
                ehci_os_logx("  periphType", gPB[0]);
                ehci_os_logx("  vendor0_3", PB_BE32(8));  ehci_os_logx("  vendor4_7", PB_BE32(12));
                ehci_os_logx("  prod0_3",   PB_BE32(16)); ehci_os_logx("  prod4_7",   PB_BE32(20));
                pb_in(13); gPState = 3;                                        /* INQUIRY CSW */
            } break;
    case 3: if (pb_ready()) { pb_cbw(cdbCap, 10, 8); gPState = 4; } break;     /* READ CAPACITY */
    case 4: if (pb_ready()) { pb_in(8); gPState = 5; } break;
    case 5: if (pb_ready()) {
                gPBlkCnt  = PB_BE32(0) + 1;   /* returned last-LBA + 1 */
                gPBlkSize = PB_BE32(4);
                if (gPBlkSize == 0 || gPBlkSize > 512) gPBlkSize = 512;        /* clamp the read */
                ehci_os_log("SELFPROBE READ CAPACITY:");
                ehci_os_logx("  blocks", gPBlkCnt); ehci_os_logx("  blockSize", gPBlkSize);
                pb_in(13); gPState = 6;                                        /* CAPACITY CSW */
            } break;
    case 6: if (pb_ready()) {
                for (i = 0; i < 10; i++) cdbRd[i] = 0;
                cdbRd[0] = 0x28; cdbRd[8] = 1;    /* READ(10), LBA 0, transfer length 1 block */
                pb_cbw(cdbRd, 10, gPBlkSize); gPState = 7;
            } break;
    case 7: if (pb_ready()) { pb_in(gPBlkSize); gPState = 8; } break;          /* block-0 data */
    case 8: if (pb_ready()) {
                ehci_os_log("SELFPROBE READ block 0:");
                ehci_os_logx("  b0_3",   PB_BE32(0));
                ehci_os_logx("  sig510", ((UInt32)gPB[510]<<8)|gPB[511]);      /* FAT boot sig 55 AA */
                pb_in(13); gPState = 9;                                        /* block-0 CSW */
            } break;
    case 9: if (pb_ready()) { ehci_os_log("=== SELFPROBE COMPLETE — we read the disk ourselves ===");
                              /* r35 R2a: after ~10 self-probe transfers, did the real EHCI IRQ fire? */
                              ehci_os_logx("  gIsrHits (real IRQ fired during selfprobe; 0 = heartbeat-only)", gIsrHits);
                              ehci_os_logx("  v46 gIsrConsecMax (shared-IRQ storm peak)", gIsrConsecMax);
                              ehci_vhub_publish_service(); gPState = 10; gMountedOnce = 1; } break;   /* m2: expose block-read; r87: arm reconnect logic */
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
            if (gSetup[1] == 0x06) {                                /* GET_DESCRIPTOR */
                if      (gSetup[0] == 0x80 && gSetup[3] == 0x01) fill(buf, gDevDesc, 18, len);
                else if (gSetup[0] == 0x80 && gSetup[3] == 0x02) fill(buf, gCfgDesc, 25, len);
                else if (gSetup[0] == 0xA0 && gSetup[3] == 0x29) fill(buf, gHubDesc, 9, len);
                else if (gSetup[0] == 0x80 && gSetup[3] == 0x03) { /* STRING */
                    if (gSetup[2] == 0) { if (len >= 4) { buf[0]=0x04; buf[1]=0x03; buf[2]=0x09; buf[3]=0x04; } }
                    else                { if (len >= 2) { buf[0]=0x02; buf[1]=0x03; } }
                }
            } else if (gSetup[1] == 0x00) {                         /* GET_STATUS */
                if (gSetup[0] == 0xA3) { int p = gSetup[4] - 1; if (p >= 0 && p < gSoftc.nPorts) ehci_vhub_port_status(p, buf); }
                else if (gSetup[0] == 0xA0) { buf[0]=buf[1]=buf[2]=buf[3]=0; }        /* hub status */
                else { buf[0] = 0x01; buf[1] = 0; }                 /* device status: SELF-POWERED (v0.19 fix) */
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
            if (apple_hidden_port(p) && !port_ceded(p)) {
                if (conn) { gSelfEnumPort = p; gSelfEnumDone = 0; gSelfEnumTries = 0; }
                else {
                    if (gSelfEnumDone) gSelfEnumRearm = 1;   /* an ENUMERATED device was pulled: ask task
                                                              * level to reset the probe state */
                    gSelfEnumPort = -1; gSelfEnumDone = 0; gSelfEnumTries = 0;
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
    if (!gMountedOnce || gPState >= 10 || gSihArmed) return;   /* active only between a reconnect and re-completion */
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
void ehci_vhub_service(void)
{
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
static int gA2Live = 0; static volatile UInt32 gSihQueued = 0;
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

static OSStatus vhub_sih(void *p1, void *p2)
{
    (void)p1; (void)p2;
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
    frame_time_update();        /* advance the USL frame clock on a steady 8 ms cadence (single writer) */
    /* QUEUE the secondary handler (like the ISR path) rather than running the service inline at timer
     * level — so EVERY transfer completion is delivered to the USL/class driver at secondary-interrupt
     * level, matching Apple's OHCI UIM. (A completion arriving at timer level may be why the mass-
     * storage driver won't re-issue after REQUEST SENSE.) */
    if (!gSihQueued) { gSihQueued = 1; QueueSecondaryInterruptHandler(vhub_sih, NULL, NULL, NULL); }
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
        if (gIsrConsec > 499u && gStormPainted == 0) { gStormPainted = 1; storm_paint(); }
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
 * Driver lifecycle: teardown, the warm-reboot shutdown hook, and stop-service. Call sites are
 * ehci_uim.c:138/311/320 and ehci_os.c:454.
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
 * That is a textbook recipe for the wedged desktop this produced, and it was cured by a cold boot, which
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
    gShutUPP = NewShutDwnUPP(ehci_shutdown_proc);
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
