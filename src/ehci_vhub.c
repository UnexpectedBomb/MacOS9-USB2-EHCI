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
#include "ehci_vhub.h"

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

void ehci_vhub_port_status(int p, volatile UInt8 *out)
{
    UInt32 pv = ehci_read32(gSoftc.opBase, EHCI_PORTSC(p));
    UInt16 st = 0, ch = gPortChange[p];
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
#define DOWN_DATA_PAGES 5                        /* r46: one qTD spans 5 buffer pointers = 20KB */
#define DOWN_BUF_MAX (DOWN_DATA_PAGES * 0x1000)  /* r46: 20480 = 40 blocks/BOT command (was 0xF00 = 7 blocks) */
#define DOWN_MAX_BLOCKS 7     /* r47: REVERTED to 7. r46's 40-block/20KB chunks BACKFIRED — 0.14 MB/s (5x SLOWER),
                               * failed mid-write, and choppy: a 20KB data copy AT INTERRUPT LEVEL (down_reap/bio_advance)
                               * starves the Finder, and the 5-page scatter path was unreliable. Restores the proven r45
                               * behaviour (7-block, single-page transfers, small interrupt-level copies, 0.76 MB/s reliable).
                               * The 20KB bounce + 5-buffer-pointer infra above stays DORMANT (chunks <=7 blocks = <1 page,
                               * so only buffer[0] is used) — reusable IF a future rework first moves the copies off the
                               * interrupt path (the real prerequisite for big chunks). */
static volatile UInt8 *gDownPg = 0; static UInt32 gDownPgP = 0;
static ehci_qh  *gDownQH = 0;  static UInt32 gDownQHP = 0;
static ehci_qtd *gDownQTD = 0; static UInt32 gDownQTDP = 0;
static volatile UInt8 *gDownBuf = 0;                 /* r46: 20KB (5-page) DMA bounce for big data chunks */
static UInt32 gDownBufPhys[DOWN_DATA_PAGES];         /* physical addr of each bounce page (may be non-contiguous) */
static int    gDownReady = 0, gDownInitErr = 0, gDownAseOn = 0;
static UInt32 gDownQHAddr = 0xFFFFUL;
static volatile int    gDpBusy = 0, gDpIsIn = 0;
static volatile void  *gDpUPP = 0, *gDpPipe = 0, *gDpDest = 0;
static volatile UInt32 gDpLen = 0, gDpArmTick = 0;
static volatile UInt32 gDownDone = 0, gDownErr = 0, gDownTimeouts = 0;
/* Bulk endpoints (mass storage). We reuse the single serialized gDownQH, reprogramming its epChar
 * per transfer (control ep0 <-> bulk ep), and keep DTC=1 with a SOFTWARE per-endpoint data toggle
 * (bulk needs toggle continuity across transfers; control uses the SETUP=0/else=1 rule). uim6 records
 * the endpoint (addr, endpt, dir 0=OUT/1=IN, maxpkt); uim7 routes by (addr, endpt). */
#define NBULK 6
static struct { UInt32 addr, endpt; UInt8 dirIn, toggle; UInt16 maxpkt; UInt8 used; } gBulkEP[NBULK];
static volatile int gDpBulkEp = -1; static volatile UInt32 gDpNpkt = 1;
/* r10 diagnostic: last bulk completion snapshot (interrupt-safe stores in down_reap; task-ctx reads
 * via ehci_vhub_bulk_stats from uim7, since down_reap runs at interrupt level where File Mgr is unsafe). */
static volatile long gBulkLastStat = 0; static volatile UInt32 gBulkDoneN = 0, gBulkErrN = 0;
static volatile UInt8 gLastData[16];
typedef struct { void *upp; void *pipe; void *dest; UInt32 addr, len; UInt8 pid; UInt8 obuf[64]; UInt32 olen;
                 void *obig; UInt32 obiglen; int bulkEp; } DownReq;   /* obig = large OUT source (write data > 64B) */
#define DOWNQ_N 48
static volatile DownReq gDownQ[DOWNQ_N];
static volatile UInt32 gDownQHead = 0, gDownQTail = 0, gDownQDrop = 0;

static void down_prog_addr(UInt32 a)
{
    gDownQH->epChar = ehci_cpu_to_le32(EHCI_QH_DEVADDR(a) | EHCI_QH_ENDPT(0) | EHCI_QH_EPS_HIGH |
                        EHCI_QH_MAXLEN(64) | EHCI_QH_DTC | EHCI_QH_RL(4));   /* r45: NO EHCI_QH_HEAD — only the anchor heads the ring; this QH is a spliced member */
    gDownQHAddr = a;
}
int ehci_vhub_xfer_init(void)
{
    Ptr raw; LogicalAddress pg; LogicalToPhysicalTable t; unsigned long cnt = 1; UInt32 pgP; int i;
    raw = NewPtrSysClear(0x2000);
    if (!raw) { gDownInitErr = 1; return -1; }
    pg = (LogicalAddress)(((UInt32)raw + 0xFFFUL) & ~0xFFFUL);
    t.logical.address = pg; t.logical.count = 0x1000;
    if (LockMemory(pg, 0x1000) != noErr) { gDownInitErr = 2; return -1; }
    if (GetPhysical(&t, &cnt) != noErr)  { gDownInitErr = 3; return -1; }
    pgP = (UInt32)t.physical[0].address;
    gDownPg = (volatile UInt8 *)pg; gDownPgP = pgP;
    gDownQH  = (ehci_qh  *)((UInt8 *)pg + DOWN_QH_OFF);  gDownQHP  = pgP + DOWN_QH_OFF;
    gDownQTD = (ehci_qtd *)((UInt8 *)pg + DOWN_QTD_OFF); gDownQTDP = pgP + DOWN_QTD_OFF;
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
    /* r45 ROOT-CAUSE FIX for the intermittent enum wall (the r44 capture: ASYNCLISTADDR pointed at the
     * anchor, not our QH, on ~2/3 of boots → controller looped the empty anchor → every downstream
     * transfer timed out = "device unreachable"). Do NOT self-link this QH and do NOT overwrite
     * ASYNCLISTADDR (unreliable while the schedule runs). Instead SPLICE it into the async anchor ring
     * and leave ASYNCLISTADDR = the anchor (set once by ehci_hc_start). The controller then always
     * reaches us: anchor -> gDownQH -> anchor. This is what ehci_qh_link_async was built for. */
    down_prog_addr(0);
    gDownQH->epCaps  = ehci_cpu_to_le32(EHCI_QH_MULT(1));
    gDownQH->curQtd  = 0;
    gDownQH->ovlNext = ehci_cpu_to_le32(EHCI_LINK_TERMINATE);
    gDownQH->ovlAltNext = ehci_cpu_to_le32(EHCI_LINK_TERMINATE);
    gDownQH->ovlToken = ehci_cpu_to_le32(EHCI_QTD_STATUS_HALTED);
    ehci_qh_link_async(&gSoftc, gDownQH, gDownQHP);   /* sets gDownQH->hlink + publishes into the anchor ring */
    gDownReady = 1;
    return 0;
}
static void down_arm_ase(void)
{
    if (gDownAseOn) return;
    /* r45: do NOT write ASYNCLISTADDR here — it stays = the anchor (set once by ehci_hc_start), and our
     * QH is spliced into the anchor ring in xfer_init. Overwriting it while the schedule ran was the
     * intermittent enum wall. Just ensure the async schedule is enabled (it already is, from init). */
    ehci_write32(gSoftc.opBase, EHCI_USBCMD, ehci_read32(gSoftc.opBase, EHCI_USBCMD) | EHCI_CMD_ASE);
    gDownAseOn = 1;
}
static void down_issue(volatile DownReq *r)
{
    UInt32 tok, i, dt, m; int useBounce = 0;   /* r46: useBounce flag replaces the single bufP address */
    down_arm_ase();
    if (r->bulkEp < 0) {                                    /* CONTROL (ep0): SETUP=DATA0, data/status=DATA1 */
        if (r->addr != gDownQHAddr) down_prog_addr(r->addr);
        dt = (r->pid == 2) ? 0u : 1u;
        gDpNpkt = 1;
    } else {                                                /* BULK: reprogram the shared QH for this endpoint */
        m = gBulkEP[r->bulkEp].maxpkt ? gBulkEP[r->bulkEp].maxpkt : 512;
        gDownQH->epChar = ehci_cpu_to_le32(EHCI_QH_DEVADDR(r->addr) | EHCI_QH_ENDPT(gBulkEP[r->bulkEp].endpt) |
                            EHCI_QH_EPS_HIGH | EHCI_QH_MAXLEN(m) | EHCI_QH_DTC | EHCI_QH_RL(4));   /* r45: no HEAD (spliced member, not the ring head) */
        gDownQHAddr = 0xFFFFUL;                             /* force a control re-program on the next ep0 xfer */
        dt = gBulkEP[r->bulkEp].toggle;                     /* software per-endpoint data toggle */
        gDpNpkt = (r->len + m - 1) / m; if (gDpNpkt == 0) gDpNpkt = 1;
    }
    if (r->obig) { UInt32 nn = (r->obiglen > DOWN_BUF_MAX) ? DOWN_BUF_MAX : r->obiglen;   /* large OUT: copy write data to the bounce */
                   for (i = 0; i < nn; i++) gDownBuf[i] = ((volatile UInt8 *)r->obig)[i]; useBounce = 1; }
    else if (r->pid == 2 || (r->pid == 0 && r->olen)) { for (i = 0; i < r->olen; i++) gDownBuf[i] = r->obuf[i]; useBounce = 1; }
    else if (r->pid == 1 && r->len) { useBounce = 1; }        /* IN: HC DMAs into the bounce */
    tok = EHCI_QTD_STATUS_ACTIVE | EHCI_QTD_CERR(3) | EHCI_QTD_IOC | EHCI_QTD_BYTES(r->len) |
          (r->pid == 2 ? EHCI_QTD_PID_SETUP : (r->pid == 1 ? EHCI_QTD_PID_IN : EHCI_QTD_PID_OUT)) |
          (dt ? EHCI_QTD_TOGGLE : 0u);
    gDownQTD->next    = ehci_cpu_to_le32(EHCI_LINK_TERMINATE);
    gDownQTD->altNext = ehci_cpu_to_le32(EHCI_LINK_TERMINATE);
    for (i = 0; i < 5; i++) gDownQTD->buffer[i] = useBounce ? ehci_cpu_to_le32(gDownBufPhys[i]) : 0;   /* r46: 5 pages -> up to 20KB per qTD */
    gDownQTD->token = ehci_cpu_to_le32(tok);
    gDpUPP = r->upp; gDpPipe = r->pipe; gDpDest = r->dest; gDpLen = r->len; gDpIsIn = (r->pid == 1);
    gDpBulkEp = r->bulkEp;
    gDpLastAddr = r->addr; gDpLastPid = r->pid;             /* r36 diag: name the in-flight downstream xfer */
    gDpArmTick = gVhubTick; gDpBusy = 1;
    gDownQH->curQtd  = 0;
    gDownQH->ovlNext = ehci_cpu_to_le32(gDownQTDP);
    gDownQH->ovlAltNext = ehci_cpu_to_le32(EHCI_LINK_TERMINATE);
    for (i = 0; i < 5; i++) gDownQH->ovlBuffer[i] = 0;
    __asm__ __volatile__("eieio");
    gDownQH->ovlToken = 0;                       /* clear Active/Halted -> HC executes ovlNext */
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
typedef struct { IOCommandID cmdID; UInt32 lba, count, reqBytes; UInt8 *buf; long *actCount; UInt8 isWrite; } BioReq;
static BioReq gBioQ[BIOQ_N];
static UInt32 gBioHead = 0, gBioTail = 0;      /* ring: [tail]=current in-flight request, head=enqueue */
static int    gBioPhase = 0;                   /* 0 idle; 1 CBW issued; 2 data issued; 3 CSW issued */
static UInt32 gBioChunk = 0;                    /* blocks in the current READ(10)/WRITE(10) */
static long   gBioResult = 0;
static void bio_advance(void);                  /* forward: driven from down_reap on each completion */

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
    gToQhP      = gDownQHP;
    gToQhEpChar = ehci_le32_to_cpu(gDownQH->epChar);
    gToQhCurQtd = ehci_le32_to_cpu(gDownQH->curQtd);
    gToQhOvlTok = ehci_le32_to_cpu(gDownQH->ovlToken);
    gToQtdTok   = ehci_le32_to_cpu(gDownQTD->token);
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

static void down_reap(void)
{
    UInt32 tok; long status; UInt32 actual = 0;
    if (gDpBusy) {
        tok = ehci_le32_to_cpu(gDownQTD->token);
        if (tok & EHCI_QTD_STATUS_ACTIVE) {
            if ((gVhubTick - gDpArmTick) > 200UL) { gDownTimeouts++; capture_timeout_state(); status = -6640L; }  /* watchdog + r39 snapshot */
            else return;
        } else if (tok & (EHCI_QTD_STATUS_HALTED | EHCI_QTD_STATUS_XACTERR | EHCI_QTD_STATUS_BABBLE | EHCI_QTD_STATUS_DBERR)) {
            gDownErr++; status = -6640L;
        } else {
            UInt32 resid = EHCI_QTD_BYTES_GET(tok);
            status = 0; gDownDone++;
            actual = (resid <= gDpLen) ? (gDpLen - resid) : gDpLen;
            if (gDpIsIn && gDpLen) {
                volatile UInt8 *dd = (volatile UInt8 *)gDpDest; UInt32 i;
                for (i = 0; i < actual; i++) dd[i] = gDownBuf[i];
            }
            if (gDpBulkEp >= 0) gBulkEP[gDpBulkEp].toggle ^= (UInt8)(gDpNpkt & 1u);  /* advance bulk data toggle */
        }
        if (gDpBulkEp >= 0) {   /* snapshot for the task-context diagnostic log */
            UInt32 i; gBulkLastStat = status;
            if (status == 0) gBulkDoneN++; else gBulkErrN++;
            for (i = 0; i < 16; i++) gLastData[i] = gDownBuf[i];   /* IN: received CSW/data; OUT: sent CBW */
        }
        gDpLastStat = status;                                  /* r36 diag: last downstream reap status */
        gDpBusy = 0;
        if (gDpUPP) {
            if (gDpBulkEp >= 0)   /* bulk: completion(cmdBlock, status, actualCount) — 3 args (RE-confirmed) */
                ((ehci_usl_intcomplete)gDpUPP)((void *)gDpPipe, status, (unsigned long)actual);
            else                  /* control: completion(pipeRef, status) — 2 args */
                ((ehci_usl_complete)gDpUPP)((void *)gDpPipe, (unsigned long)status);
        }
        if (gBioPhase && !gDpUPP) bio_advance();   /* r34: advance the async block-I/O state machine */
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
 * Slot 7 BulkTransfer enqueues onto the same serialized FIFO with a valid endpoint index; down_issue
 * reprograms the shared QH + applies the software toggle; down_reap fires the 3-arg completion. */
long ehci_vhub_create_bulk(UInt32 addr, UInt32 endpt, UInt32 dirIn, UInt32 maxpkt)
{
    int i, freeSlot = -1;
    for (i = 0; i < NBULK; i++) {
        if (gBulkEP[i].used && gBulkEP[i].addr == addr && gBulkEP[i].endpt == endpt) {
            gBulkEP[i].dirIn = (UInt8)(dirIn ? 1 : 0);
            gBulkEP[i].maxpkt = (UInt16)(maxpkt ? maxpkt : 512);
            gBulkEP[i].toggle = 0;                       /* fresh endpoint => DATA0 */
            return 0;
        }
        if (!gBulkEP[i].used && freeSlot < 0) freeSlot = i;
    }
    if (freeSlot < 0) return -1;
    gBulkEP[freeSlot].addr = addr; gBulkEP[freeSlot].endpt = endpt;
    gBulkEP[freeSlot].dirIn = (UInt8)(dirIn ? 1 : 0);
    gBulkEP[freeSlot].maxpkt = (UInt16)(maxpkt ? maxpkt : 512);
    gBulkEP[freeSlot].toggle = 0; gBulkEP[freeSlot].used = 1;
    return 0;
}
/* r32 ROOT-CAUSE FIX: set to 1 once our self-probe takes over the endpoints. While set,
 * ehci_vhub_bulk_xfer REJECTS Apple-USL bulk transfers (they arrive with a real completion UPP;
 * our own self-probe/'Eusb' transfers pass complUPP==0), so the still-loaded Apple USB Mass Storage
 * driver can't share our single QH and intermittently corrupt our reads (the audio-CD misID) or
 * collide with a Finder copy (the freeze). Returning an error = a synchronous "submit failed" the
 * USL handles without waiting for a completion UPP. */
static int gFenceApple = 0;
long ehci_vhub_bulk_xfer(void *pipe, void *complUPP, volatile UInt8 *buf,
                         UInt32 addr, UInt32 endpt, UInt32 len, UInt32 dirIn)
{
    int i;
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
static UInt8  gPB[DOWN_BUF_MAX];        /* r46: CBW / data / CSW scratch — now up to 40 blocks (20KB) per BOT command */
static int    gPState = 0;              /* 0 = wait for the mounter to park; then per-phase */
static UInt32 gPIdle = 0, gPLastCnt = 0, gPMark = 0, gPTag = 0x50524231UL; /* 'PRB1' */
static UInt32 gPBlkSize = 512, gPBlkCnt = 0;
static int    gPOut = -1, gPIn = -1;    /* gBulkEP indices: OUT (CBW) + IN (data/CSW) endpoints */

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
    gPMark = gBulkDoneN + gBulkErrN;
    (void)ehci_vhub_bulk_xfer(0, 0, gPB, gBulkEP[gPOut].addr, gBulkEP[gPOut].endpt, 31, 0);   /* OUT (31B, via obuf) */
}
static void pb_cbw(const UInt8 *cdb, int cdbLen, UInt32 dataLen) { pb_cbw_dir(cdb, cdbLen, dataLen, 0x80); }  /* data-IN CBW */
static void pb_in(UInt32 len)   /* read len bytes on the IN endpoint into gPB */
{
    if (gPIn < 0) return;
    gPMark = gBulkDoneN + gBulkErrN;
    (void)ehci_vhub_bulk_xfer(0, 0, gPB, gBulkEP[gPIn].addr, gBulkEP[gPIn].endpt, len, 1);     /* IN */
}
static void pb_out(UInt32 len)  /* write len bytes from gPB on the OUT endpoint (large OUT via obig) */
{
    if (gPOut < 0) return;
    gPMark = gBulkDoneN + gBulkErrN;
    (void)ehci_vhub_bulk_xfer(0, 0, gPB, gBulkEP[gPOut].addr, gBulkEP[gPOut].endpt, len, 0);   /* OUT */
}
static int pb_ready(void) { return (gBulkDoneN + gBulkErrN) != gPMark; }      /* prior transfer finished */
#define PB_BE32(o) (((UInt32)gPB[o]<<24)|((UInt32)gPB[(o)+1]<<16)|((UInt32)gPB[(o)+2]<<8)|gPB[(o)+3])

/* ==================== r22 (BYPASS m2): synchronous block-read SERVICE for our own disk driver ==========
 * Our block driver is a separate native PEF (installed post-selfprobe, like the eSATA driver) and can't
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
static volatile UInt16 gFailChunk = 0; static volatile UInt8 gFailIsWrite = 0, gFailCswStat = 0;
static volatile UInt32 gBioWrOk = 0, gBioRdOk = 0, gBioReject = 0;
static void biofail(UInt8 isWrite, UInt32 lba, UInt16 chunk)
{
    gFailIsWrite = isWrite; gFailLba = lba; gFailChunk = chunk;
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

/* ---- r34 async block-I/O state machine (see the block comment at gBioQ). Uses the same pb_* BOT
 * primitives as the self-probe but advances on completions instead of spinning in pb_wait. ---- */
static void bio_start_chunk(void)                 /* issue the CBW for the current request's next chunk */
{
    BioReq *r = &gBioQ[gBioTail % BIOQ_N];
    UInt8 cdb[10]; UInt32 n = (r->count > DOWN_MAX_BLOCKS) ? DOWN_MAX_BLOCKS : r->count, nbytes = n * 512; int i;   /* r46: 40-block chunks */
    gBioChunk = n;
    for (i = 0; i < 10; i++) cdb[i] = 0;
    cdb[0] = r->isWrite ? 0x2A : 0x28;            /* WRITE(10) / READ(10) */
    cdb[2]=(UInt8)(r->lba>>24); cdb[3]=(UInt8)(r->lba>>16); cdb[4]=(UInt8)(r->lba>>8); cdb[5]=(UInt8)r->lba;
    cdb[7]=(UInt8)(n>>8); cdb[8]=(UInt8)n;
    pb_cbw_dir(cdb, 10, nbytes, r->isWrite ? 0x00 : 0x80);   /* CBW into gPB + issue (data-OUT/IN flag) */
    gBioPhase = 1;
}
static void bio_kick(void)                        /* if idle and the queue is non-empty, start the next request */
{
    if (gBioPhase != 0 || gBioHead == gBioTail) return;
    gBioResult = 0;
    bio_start_chunk();
}
static void bio_advance(void)                     /* called from down_reap after each self-driven completion */
{
    BioReq *r = &gBioQ[gBioTail % BIOQ_N];
    UInt32 nbytes = gBioChunk * 512, i;
    switch (gBioPhase) {
    case 1:                                       /* CBW done -> data phase */
        if (r->isWrite) { for (i = 0; i < nbytes; i++) gPB[i] = r->buf[i]; pb_out(nbytes); }  /* stage+send write data */
        else pb_in(nbytes);                                                                    /* read data into gPB */
        gBioPhase = 2; break;
    case 2:                                       /* data done -> (read: copy in) then CSW */
        if (!r->isWrite) { for (i = 0; i < nbytes; i++) r->buf[i] = gPB[i]; }
        pb_in(13); gBioPhase = 3; break;
    case 3:                                       /* CSW done -> check, advance chunk, or complete */
        if (gPB[12] != 0) { gBioResult = -36L; biofail(r->isWrite, r->lba, (UInt16)gBioChunk); }  /* r41: name the failing CSW (r43 interrupt-level DebugStr reverted — hung MacsBug) */
        else { if (r->isWrite) gBioWrOk++; else gBioRdOk++; }                                     /* r41: context counts */
        r->lba += gBioChunk; r->buf += gBioChunk * 512; r->count -= gBioChunk;
        if (gBioResult == 0 && r->count > 0) { bio_start_chunk(); }        /* more chunks in this request */
        else {
            IOCommandID done = r->cmdID; long res = gBioResult;
            if (r->actCount) *r->actCount = (res == 0) ? (long)r->reqBytes : (long)(r->reqBytes - r->count * 512);
            gBioTail++; gBioPhase = 0;             /* dequeue BEFORE completing (completion may enqueue more) */
            (void)IOCommandIsComplete(done, (OSErr)res);   /* interrupt-safe: hand the block I/O back to the File Mgr */
            bio_kick();                            /* start the next queued request */
        }
        break;
    }
}
/* Async submit — the block driver's kRead/kWrite call this and return kIOBusyStatus. 0=accepted,
 * 1=nothing-to-do (complete noErr), -1=queue full (complete with error). */
static long ehci_usb_submit(IOCommandID cmdID, UInt32 lba, UInt32 count, void *buf, int isWrite, long *actCount)
{
    UInt32 depth = gBioHead - gBioTail; BioReq *r;
    if (count == 0) return 1;
    if (depth >= BIOQ_N) { gBioReject++; return -1; }   /* r41: count queue-full submit rejections */
    r = &gBioQ[gBioHead % BIOQ_N];
    r->cmdID = cmdID; r->lba = lba; r->count = count; r->reqBytes = count * 512;
    r->buf = (UInt8 *)buf; r->actCount = actCount; r->isWrite = (UInt8)(isWrite ? 1 : 0);
    gBioHead++;
    bio_kick();
    return 0;
}

typedef long (*ehci_usb_rw_fn)(UInt32 lba, UInt32 count, void *buf);
typedef long (*ehci_usb_submit_fn)(IOCommandID cmdID, UInt32 lba, UInt32 count, void *buf, int isWrite, long *actCount);
static struct { UInt32 magic; ehci_usb_rw_fn readFn; ehci_usb_rw_fn writeFn; UInt32 blkSize, blkCnt; ehci_usb_submit_fn submitFn; } gSvc;
static void ehci_vhub_publish_service(void)
{
    gSvc.magic = 0x45555342UL;  /* 'EUSB' */
    gSvc.readFn = ehci_usb_read;
    gSvc.writeFn = ehci_usb_write;
    gSvc.blkSize = 512; gSvc.blkCnt = gPBlkCnt;
    gSvc.submitFn = ehci_usb_submit;               /* r34: async path for kRead/kWrite */
    (void)NewGestaltValue('Eusb', (long)&gSvc);
    ehci_os_log("=== r34: USB block service published via Gestalt 'Eusb' (sync rw + async submit) ===");
}

/* Task-level state machine; call once per uim23. */
void ehci_vhub_selfprobe_tick(void)
{
    static const UInt8 cdbInq[6]  = {0x12,0,0,0,36,0};
    static const UInt8 cdbCap[10] = {0x25,0,0,0,0,0,0,0,0,0};
    UInt8 cdbRd[10]; int i;
    switch (gPState) {
    case 0:   /* wait for the mounter to exercise both bulk eps then go permanently quiet (parked) */
        pb_find_eps();
        {   /* r31 DIAGNOSTIC: periodically log WHY we haven't parked — cracks the intermittent no-mount
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
                              ehci_vhub_publish_service(); gPState = 10; } break;   /* m2: expose block-read to our disk driver */
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
            pevt((UInt8)p, conn ? PEV_CONNECT : PEV_DISCONN, pv); }   /* r36 diag: a DISCONN straddling reset = the bounce */
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
void ehci_vhub_service(void)
{
    gVhubTick++;
    (void)hub_int_ack();
    service_ports();
    deliver_completions();
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

static OSStatus vhub_sih(void *p1, void *p2)
{
    (void)p1; (void)p2;
    ehci_vhub_service();
    if (gA2Live) ehci_write32(gSoftc.opBase, EHCI_USBINTR, gIntrEnabled);   /* re-unmask */
    gSihQueued = 0;
    return noErr;
}
static OSStatus vhub_heartbeat(void *p1, void *p2)
{
    AbsoluteTime when;
    (void)p1; (void)p2;
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
    (void)m; (void)refcon; (void)cnt;
    sts = ehci_read32(gSoftc.opBase, EHCI_USBSTS) & gIntrEnabled;
    if (!sts) return kIsrIsNotComplete;                        /* not ours -> keep chaining */
    gIsrHits++;                                                /* r35: this IRQ was ours */
    ehci_write32(gSoftc.opBase, EHCI_USBINTR, 0);              /* mask until the SIH clears it */
    if (!gSihQueued) { gSihQueued = 1; QueueSecondaryInterruptHandler(vhub_sih, NULL, NULL, NULL); }
    return kIsrIsComplete;
}
void ehci_vhub_start_service(EHCIRegEntryIDPtr node)
{
    ISTProperty ist; RegPropertyValueSize sz = sizeof(ist);
    OSErr pe = RegistryPropertyGet((RegEntryID *)node, kISTPropertyName, ist, &sz);
    /* r35 R2a: log EXACTLY why the real EHCI IRQ does/doesn't install. Suspicion: a force-loaded
     * (LoadUIMForEntry) node has NO "driver-ist" interrupt-set property -> pe != noErr -> we never
     * install the ISR and fall back to the 8ms timer (= the slow, heartbeat-paced I/O observed). */
    ehci_os_log("vhub_start_service: real-EHCI-IRQ install");
    ehci_os_logx("  driver-ist get", (unsigned long)(long)pe);      /* 0 = found; negative = absent */
    ehci_os_logx("  driver-ist sz",  (unsigned long)sz);
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
    { AbsoluteTime when = AddDurationToAbsolute((Duration)VHUB_HB_MS, UpTime());
      SetInterruptTimer(&when, vhub_heartbeat, 0, &gHbTimer); }
}

/* r35: task-context accessor so the trigger can show whether the real EHCI IRQ is FIRING (isrHits
 * climbing) or heartbeat-only (isrHits stuck at 0 while I/O still completes via the timer). */
void ehci_vhub_irq_stats(unsigned long *isrHits, int *a2live)
{
    if (isrHits) *isrHits = gIsrHits;
    if (a2live)  *a2live  = gA2Live;
}
