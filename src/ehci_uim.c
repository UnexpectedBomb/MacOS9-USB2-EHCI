/*
 * ehci_uim.c — EHCI USB 2.0 host-controller driver (UIM) for Mac OS 9.
 *
 * Exports the two data symbols the USB Manager requires of a UIM:
 *   TheDriverDescription   — matched by the Driver Loader to the EHCI node
 *                            "pciclass,0c0320" (0C=serial-bus, 03=USB, 20=EHCI).
 *   ThePluginDispatchTable — version word (6) + 28 host-controller entry points.
 *
 * The dispatch slots are THIN WRAPPERS over the shared virtual-hub/transfer engine
 * (ehci_vhub.c) — the logic proven on real hardware in the app leg. The slot
 * signatures + table layout mirror that proven gDispatch (probe/ehci_hub.c): the USL
 * passes arguments in r3..r10, so a slot that needs them takes 8 UInt32 params;
 * slot 24 = the 64-bit frame-time clock, slot 23 = polled service, slot 22 = the
 * activation op that must return 0 so the USL promotes this bus.
 *
 * This UIM is brought up as the Expert's plugin via LoadUIMForEntry (USL-4.2), which
 * also builds the Name Registry parent-deviceRef entry that USBResetDevice needs.
 */
typedef unsigned char  UInt8;
typedef unsigned short UInt16;
typedef unsigned long  UInt32;
typedef long           OSStatus;
typedef unsigned long  OSType;

#define FOURCC(a,b,c,d) (((UInt32)(a)<<24)|((UInt32)(b)<<16)|((UInt32)(c)<<8)|(UInt32)(d))

typedef struct { UInt8 len; char s[31]; } Str31;
typedef struct { UInt8 majorRev, minorAndBugRev, stage, nonRelRev; } NumVersion;
typedef struct {
    OSType     sig;             /* 'mtej' */
    UInt32     descVersion;     /* 0 */
    Str31      nameInfoStr;     /* "pciclass,0c0320" <- EHCI class match */
    NumVersion typeVersion;
    UInt32     driverRuntime;   /* 0x04 = kDriverIsUnderExpertControl (loaded via LoadUIMForEntry) */
    Str31      driverName;      /* "EHCIUIM" */
    UInt32     reserved[8];
    UInt32     nServices;
} EHCIDriverDescription;

EHCIDriverDescription TheDriverDescription = {
    FOURCC('m','t','e','j'),
    0,
    { 15, "pciclass,0c0320" },
    { 1, 0, 0x80 /*final*/, 0 },
    0x00000004UL,   /* under Expert control: the Expert's LoadUIMForEntry loads us as a UIM plugin
                     * and builds the Name Registry entry (the reset-routing fix). The generic
                     * self-load path (0x03) was proven not to engage an EHCI node (HW tests #2/#3). */
    { 7, "EHCIUIM" },
    { 0,0,0,0,0,0,0,0 },
    0
};

#include "ehci.h"
#include "ehci_vhub.h"

#define noErr 0L

/* Shared controller soft state (DoDriverIO in ehci_os.c + the vhub also reference it). */
ehci_softc gSoftc;

/* slot 0 — Initialize: the USL/Expert hands us the controller's Name Registry node. Bring the
 * controller up (ehci_os.c: PCI enable, register map, reset, schedules, run) then initialize the
 * virtual-hub transfer engine (DMA page + control QH). */
static OSStatus uimInitialize(UInt32 a0, UInt32 a1, UInt32 a2, UInt32 a3,
                              UInt32 a4, UInt32 a5, UInt32 a6, UInt32 a7)
{
    long e;
    (void)a4; (void)a5; (void)a6; (void)a7;
    ehci_os_log("=== EHCIUIM r23: self-probe + block R/W service ('Eusb', read+WRITE) for our disk driver ===");
    ehci_os_log("uimInitialize: entered (dispatch slot 0)");
    ehci_os_logx("  arg0", a0); ehci_os_logx("  arg1", a1);   /* learn the slot-0 ABI */
    ehci_os_logx("  arg2", a2); ehci_os_logx("  arg3", a3);
    /* r42 MacsBug prep: log the UIM CODE BASE so a crash PC maps to a function via the nm map
     * (offset = PC - codeBase). A PPC function pointer is a TVector {codeAddr, TOC}; TVector[0] is the
     * code address, and uimInitialize sits at nm/PEF code offset 0, so ITS code address == the UIM base.
     * Also log &gSoftc so MacsBug `dm` can read live controller/engine state at the break. Task level
     * here (LoadUIMForEntry), so ehci_os_log (File Mgr) is safe. */
    {
        void *fp = (void *)uimInitialize;                    /* the TVector for this function */
        ehci_os_logx("r42 UIM codeBase (PC - this = nm offset)", ((UInt32 *)fp)[0]);
        ehci_os_logx("r42 UIM TVector(uimInit)@", (UInt32)fp);
        ehci_os_logx("r42 &gSoftc (dm this)", (UInt32)&gSoftc);
    }
    /* Do NOT dereference the (unverified) slot-0 argument — the app's uim0 was a stub and this
     * argument was never exercised. ehci_os_init self-finds the node instead (proven app path). */
    e = ehci_os_init(&gSoftc, (EHCIRegEntryIDPtr)0);
    if (e == 0) {
        ehci_os_log("  ehci_os_init OK; vhub_xfer_init...");
        (void)ehci_vhub_xfer_init();      /* DMA page + downstream control QH */
        ehci_os_log("  vhub_xfer_init done; vhub_start_service...");
        ehci_vhub_start_service(&gSoftc.node);   /* install the EHCI ISR + periodic timer */
        ehci_os_log("  vhub_start_service done — INIT COMPLETE");
    } else {
        ehci_os_logx("  ehci_os_init FAILED e=", (unsigned long)e);
    }
    return (OSStatus)e;
}
static OSStatus uimFinalize(void) { return noErr; }

/* ==================== r17 slot-call trace — close the post-REQUEST-SENSE blind spot ====================
 * r16 proved the completion LEVEL is not the wall: the bulk re-issues already reach uim7 at TASK level
 * (its File-Mgr logging succeeds), so the disk driver's next-command issue is already task-level. What
 * we have NEVER observed is whether, after the clean REQUEST SENSE, the disk driver calls one of the
 * dispatch slots we answer with a bare noErr — CreateControlEndpoint(2), Create{Int,Isoch}Endpoint(10/14),
 * IsochTransfer(15), AbortPipe/ClearPipeStall/GetPipeStatus(18/19/20), power(27), etc. Those stubs are
 * SILENT today, so a call into one is indistinguishable from "the driver went quiet" — the #1 RE-ranked
 * suspect. Record every stub call here (interrupt-safe; ANY context) into a ring, and drain it to the
 * disk log from uim23 at TASK level (File Mgr is task-only). Behaviour is unchanged — still returns noErr. */
typedef struct { UInt32 slot, a, b, c, d, e, f, g, h; } SlotCall;   /* r18: FULL r3..r10 ABI */
#define SLOTLOG_N 128   /* r36: larger ring — the un-starved downstream ctrl trace bursts during enum */
static volatile SlotCall gSlotLog[SLOTLOG_N];
static volatile UInt32 gSlotHead = 0, gSlotTail = 0;
static volatile UInt32 gSlotCount[32];                    /* r18: total calls per slot (all phases) */
static volatile UInt32 gCtrlRec = 0;                      /* r19: uim3 control-xfer records emitted (rate-limit) */
#define CTRL_SENTINEL 0xC3UL                              /* ring 'slot' value marking a uim3 control record */
/* r18/r19 — decide the post-REQUEST-SENSE stall. r18's DIRECT File-Mgr log inside uim3 HARD-HUNG the MDD:
 * File Manager is unsafe at the interrupt level uim3 can run at (the classic no-NMI deadlock — it hung
 * during enumeration string-descriptor reads, before ever reaching the bulk phase). So r19 routes BOTH
 * the stub-slot trace (slot_trace) AND the control-xfer trace (ctrl_trace) through this interrupt-safe
 * ring, drained to disk ONLY from uim23 at TASK level. slot_trace records every on-demand slot's full
 * r3..r10 (so slot 19's pipe id is captured IF pef1's stall-recovery fires after RS); ctrl_trace records
 * the enumeration/notification control flow (SET_CONFIGURATION, string reads, GET_MAX_LUN) the RE says
 * seeds the device-state gate. Slot 27's benign per-bus flood (USL:0x570c) is counted, not logged. All
 * stubs still return noErr — behaviour otherwise unchanged; NOTHING below does File Mgr / touches memory
 * beyond the ring + the (guarded) SETUP buffer read. */
static void slot_trace(UInt32 slot, UInt32 a, UInt32 b, UInt32 c, UInt32 d,
                       UInt32 e, UInt32 f, UInt32 g, UInt32 h)
{
    UInt32 i;
    if (slot < 32) gSlotCount[slot]++;
    if (slot == 27) return;                               /* benign periodic per-bus tick — counted, not logged */
    if (slot < 32 && gSlotCount[slot] > 8) return;        /* first 8 of each slot; the rest are counted only */
    i = gSlotHead;
    if (i - gSlotTail >= SLOTLOG_N) return;               /* ring full — drop (drain fell behind) */
    gSlotLog[i % SLOTLOG_N].slot = slot;
    gSlotLog[i % SLOTLOG_N].a = a; gSlotLog[i % SLOTLOG_N].b = b;
    gSlotLog[i % SLOTLOG_N].c = c; gSlotLog[i % SLOTLOG_N].d = d;
    gSlotLog[i % SLOTLOG_N].e = e; gSlotLog[i % SLOTLOG_N].f = f;
    gSlotLog[i % SLOTLOG_N].g = g; gSlotLog[i % SLOTLOG_N].h = h;
    __asm__ __volatile__("eieio");                        /* publish payload before the index (dual-CPU safe) */
    gSlotHead = i + 1;
}
/* r19: interrupt-safe capture of a uim3 control transfer (NO File Mgr — the r18 crash fix). Records into
 * the same ring with a sentinel; uim23 formats it at task level. Rate-limited so enumeration can't flood. */
static void ctrl_trace(UInt32 devAddr, UInt32 pid, UInt32 len, volatile UInt8 *buf)
{
    UInt32 i, s0 = 0, s4 = 0;
    static UInt32 nRoot = 0, nDown = 0;
    /* r36: budget the root-hub enum and the DOWNSTREAM (SanDisk) control flow SEPARATELY. The old flat
     * 44-cap let the root-hub enum consume the whole budget, hiding the SanDisk's enumeration +
     * interface-setup control transfers — exactly the window where the -6999 handoff dies. */
    if (devAddr == ehci_vhub_roothub_addr()) { if (nRoot >= 400) return; nRoot++; }    /* r82: raised for the observe window */
    else                                     { if (nDown >= 2000) return; nDown++; }   /* r82: capture reconnect enumeration */
    gCtrlRec++;                                           /* total tally (kept for continuity) */
    if (pid == 2 && (UInt32)buf >= 0x1000UL) {            /* SETUP: capture the 8-byte setup packet (memory read only) */
        s0 = ((UInt32)buf[0]<<24)|((UInt32)buf[1]<<16)|((UInt32)buf[2]<<8)|buf[3];
        s4 = ((UInt32)buf[4]<<24)|((UInt32)buf[5]<<16)|((UInt32)buf[6]<<8)|buf[7];
    }
    i = gSlotHead;
    if (i - gSlotTail >= SLOTLOG_N) return;
    gSlotLog[i % SLOTLOG_N].slot = CTRL_SENTINEL;
    gSlotLog[i % SLOTLOG_N].a = devAddr; gSlotLog[i % SLOTLOG_N].b = pid; gSlotLog[i % SLOTLOG_N].c = len;
    gSlotLog[i % SLOTLOG_N].d = s0; gSlotLog[i % SLOTLOG_N].e = s4;
    gSlotLog[i % SLOTLOG_N].f = 0; gSlotLog[i % SLOTLOG_N].g = 0; gSlotLog[i % SLOTLOG_N].h = 0;
    __asm__ __volatile__("eieio");
    gSlotHead = i + 1;
}

/* Generic no-op slot. The USL passes args in r3..r10; a stub ignores them and succeeds — but now
 * RECORDS the call (slot + all 8 args) so uim23 can surface it. (CreateControl/Bulk/Interrupt/Isoch
 * endpoint, abort/delete/clear-stall, power, reserved — the virtual hub routes control by device
 * address, so it needs no endpoint bookkeeping; bulk/isoch are later milestones.) */
#define STUB(n) static OSStatus uim##n(UInt32 a,UInt32 b,UInt32 c,UInt32 d,UInt32 e,UInt32 f,UInt32 g,UInt32 h) \
                { slot_trace((n), a, b, c, d, e, f, g, h); return noErr; }
STUB(2) STUB(10) STUB(14) STUB(15) STUB(18) STUB(19) STUB(20) STUB(25) STUB(26) STUB(27) STUB(28)

/* slot 6 — CreateBulkEndpoint: a=devAddr b=endpt c=dir(1=IN/0=OUT) d=maxPacketSize(low byte; 0=>512
 * for HS bulk). Registers the endpoint so slot 7 can route by (addr, endpt) — a stub cannot work. */
static OSStatus uim6(UInt32 a,UInt32 b,UInt32 c,UInt32 d,UInt32 e,UInt32 f,UInt32 g,UInt32 h)
{
    static int n = 0;
    (void)e; (void)f; (void)g; (void)h;
    if (n++ < 200) { ehci_os_log("uim6 CreateBulkEndpoint"); ehci_os_logx("  devAddr", a);   /* r82: raised so reconnect create_bulk logs */
        ehci_os_logx("  endpt", b); ehci_os_logx("  dirIn", c); ehci_os_logx("  maxpkt.lo", d); }
    return (OSStatus)ehci_vhub_create_bulk(a, b, c, d ? d : 512);
}
/* slot 7 — BulkTransfer: a=pipe/cmdBlock b=completionUPP c=buffer e=devAddr f=endpt g=length
 * h=direction(1=IN/0=OUT). Direction is a plain 0/1, NOT a pid. Completion is 3-arg (cmdBlock,status,count). */
static OSStatus uim7(UInt32 a,UInt32 b,UInt32 c,UInt32 d,UInt32 e,UInt32 f,UInt32 g,UInt32 h)
{
    static int n = 0;
    (void)d;
    if (n++ < 20) {
        long st; UInt32 dn, er; UInt8 pd[16]; volatile UInt8 *cbw = (volatile UInt8 *)c;
        (void)ehci_vhub_bulk_stats(&st, &dn, &er, pd);
        ehci_os_log("uim7 BulkTransfer");
        ehci_os_logx("  ep", f); ehci_os_logx("  len", g); ehci_os_logx("  dirIn", h);
        /* PREVIOUS bulk completion: status + first 4 bytes + byte 12 of the received CSW/data (CSW sig
         * 0x53425355='USBS', byte 12 = CSW status 0=pass) or the sent CBW (0x55534243='USBC'). */
        ehci_os_logx("  prevDone", dn); ehci_os_logx("  prevErr", er); ehci_os_logx("  prevStat", (unsigned long)st);
        ehci_os_logx("  prevD0_3", ((UInt32)pd[0]<<24)|((UInt32)pd[1]<<16)|((UInt32)pd[2]<<8)|pd[3]);
        ehci_os_logx("  prevD12", pd[12]);
        if (h == 0 && g >= 16) {   /* OUT = a CBW: SCSI opcode (CDB[0]=byte 15) + expected data length */
            ehci_os_logx("  CBW.op", cbw[15]);
            ehci_os_logx("  CBW.dLen", ((UInt32)cbw[11]<<24)|((UInt32)cbw[10]<<16)|((UInt32)cbw[9]<<8)|cbw[8]);
        }
    }
    return (OSStatus)ehci_vhub_bulk_xfer((void *)a, (void *)b, (volatile UInt8 *)c, e, f, g, h);
}

/* slot 3 — ControlTransfer. a=pipe b=complUPP c=buf e=devAddr g=len h=pid (2=SETUP,1=IN,0=OUT). */
static OSStatus uim3(UInt32 a,UInt32 b,UInt32 c,UInt32 d,UInt32 e,UInt32 f,UInt32 g,UInt32 h)
{
    (void)d; (void)f;
    ctrl_trace(e, h, g, (volatile UInt8 *)c);   /* r19: interrupt-safe ring capture (NO File Mgr — the r18 hard-hang fix) */
    return (OSStatus)ehci_vhub_control_xfer((void *)a, (void *)b, (volatile UInt8 *)c, e, g, h);
}
/* slot 11 — InterruptTransfer. a=devAddr b=endpt c=refcon d=callback e=buffer g=len. */
static OSStatus uim11(UInt32 a,UInt32 b,UInt32 c,UInt32 d,UInt32 e,UInt32 f,UInt32 g,UInt32 h)
{
    (void)f; (void)h;
    return (OSStatus)ehci_vhub_int_xfer(a, b, (void *)c, (void *)d, (volatile UInt8 *)e, g);
}
/* slot 22 — activation: MUST return 0 so the USL promotes this bus (pending-array -> active-array). */
static OSStatus uim22(UInt32 a,UInt32 b,UInt32 c,UInt32 d,UInt32 e,UInt32 f,UInt32 g,UInt32 h)
{ (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h; return 0; }
/* slot 23 — polled-service hook (USLPolledProcessDoneQueue), called continuously by the pump in TASK
 * context. Real servicing runs at interrupt/timer level; here we only DRAIN the bulk-completion
 * snapshot to the disk log (r11 diagnostic — this catches the LAST transfer's completion + full CSW,
 * which the uim7 "prev" logging misses). Edge-triggered on the completion count so it doesn't flood. */
static OSStatus uim23(UInt32 a,UInt32 b,UInt32 c,UInt32 d,UInt32 e,UInt32 f,UInt32 g,UInt32 h)
{
    static UInt32 lastSeq = 0; static int nlog = 0;
    long st; UInt32 dn, er, seq; UInt8 pd[16];
    (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;
    seq = ehci_vhub_bulk_stats(&st, &dn, &er, pd);       /* task-context; fires only AFTER a completion */
    if (seq != lastSeq && nlog < 40) {
        lastSeq = seq; nlog++;
        ehci_os_log("bulk done (uim23)");
        ehci_os_logx("  doneN", dn); ehci_os_logx("  errN", er); ehci_os_logx("  lastStat", (unsigned long)st);
        ehci_os_logx("  d0_3",   ((UInt32)pd[0]<<24)|((UInt32)pd[1]<<16)|((UInt32)pd[2]<<8)|pd[3]);   /* CSW sig / sense */
        ehci_os_logx("  d4_7",   ((UInt32)pd[4]<<24)|((UInt32)pd[5]<<16)|((UInt32)pd[6]<<8)|pd[7]);   /* CSW tag */
        ehci_os_logx("  d8_11",  ((UInt32)pd[8]<<24)|((UInt32)pd[9]<<16)|((UInt32)pd[10]<<8)|pd[11]); /* CSW residue */
        ehci_os_logx("  d12_15", ((UInt32)pd[12]<<24)|((UInt32)pd[13]<<16)|((UInt32)pd[14]<<8)|pd[15]);/* CSW status @12 */
    }
    /* r17: surface any (otherwise-silent) stub-slot call at TASK level. A call appearing AFTER the
     * "bulk done" for REQUEST SENSE is the prime suspect for the stall — its slot + args name exactly
     * what the disk driver waits on. Capped so a flood can't fill the log. */
    {
        static int nsl = 0; static UInt32 last27 = 0;
        while (gSlotTail < gSlotHead && nsl < 5000) {   /* r82: raised so post-mount replug slot 18/19 calls are all visible */
            volatile SlotCall *s = &gSlotLog[gSlotTail % SLOTLOG_N];
            UInt32 sl=s->slot, aa=s->a, bb=s->b, cc=s->c, dd=s->d, ee=s->e, ff=s->f, gg=s->g, hh=s->h;
            gSlotTail++; nsl++;
            if (sl == CTRL_SENTINEL) {          /* r19: a uim3 control transfer (devAddr,pid,len,setup) */
                ehci_os_log("uim3 ControlXfer");
                ehci_os_logx("  devAddr", aa); ehci_os_logx("  pid", bb); ehci_os_logx("  len", cc);
                ehci_os_logx("  setup0_3", dd); ehci_os_logx("  setup4_7", ee);
            } else {
                ehci_os_log("STUB slot called");
                ehci_os_logx("  slot", sl);
                ehci_os_logx("  r3", aa); ehci_os_logx("  r4", bb); ehci_os_logx("  r5", cc); ehci_os_logx("  r6", dd);
                ehci_os_logx("  r7", ee); ehci_os_logx("  r8", ff); ehci_os_logx("  r9", gg); ehci_os_logx("  r10", hh);
            }
        }
        if (gSlotCount[27] - last27 >= 512) { last27 = gSlotCount[27]; ehci_os_logx("slot27 ticks(total)", gSlotCount[27]); }
    }
    /* r36 RELIABILITY: drain the port-event ring + report downstream-engine health. With the un-starved
     * downstream control trace above, these DECIDE the intermittent -6999 handoff death in ONE losing
     * boot: (i) a DISCONN straddling a reset (conn 1->0->1) in PORT EVENT = the composite bounce; (ii)
     * err/timeouts climbing with a lastAddr/lastPid = OUR engine dropped that downstream xfer; (iii)
     * both clean yet slot 6 (uim6) never logged = the Apple handoff dies on its own -> pivot to
     * self-claiming the bulk endpoints. Task level only (File Mgr safe — the r18 hard-hang lesson). */
    {
        UInt32 ms, portsc; UInt8 port, ev; static int npe = 0;
        while (npe < 2000 && ehci_vhub_portevt_pop(&ms, &port, &ev, &portsc)) {   /* r82: raised for the observe window */
            npe++;
            ehci_os_log("PORT EVENT (ev 1=conn 2=disc 3=rstAssert 4=rstDeassert 5=enabled/reset-done)");
            ehci_os_logx("  ms", ms);   ehci_os_logx("  port", port);
            ehci_os_logx("  ev", ev);   ehci_os_logx("  portsc", portsc);
        }
    }
    /* r83 OBSERVE: frame_ms-independent, ring-independent probe. gVhubTick advancing = the heartbeat/service
     * loop is alive (service_ports IS scanning). Live PORTSC = the TRUE hardware state regardless of our
     * detector; gPortConn = what our detector tracked. Edge-triggered on PORTSC so a pull/reinsert logs
     * precisely; a ~5s "alive" line proves scanning even when idle. 60Hz Ticks clock (frame_ms freezes). */
    if (ehci_vhub_obs_armed()) {   /* r85: gate — [obs] runs ONLY in the post-desktop observe loop, NOT during boot */
        static UInt32 obsLast[8]; static UInt32 obsLastTick = 0; static int obsSeeded = 0, obsNChg = 0, obsNAlive = 0;
        UInt32 svc = 0, psc[8], con[8], np2, i2, nowT = *(volatile UInt32 *)0x016AUL;
        np2 = ehci_vhub_obs(&svc, psc, con, 8);
        if (!obsSeeded) {                                    /* one-time baseline: steady-state PORTSC per port */
            for (i2 = 0; i2 < np2; i2++) { ehci_os_log("[obs] baseline");
                ehci_os_logx("  port", i2); ehci_os_logx("  portsc", psc[i2]); ehci_os_logx("  gPortConn", con[i2]); }
            ehci_os_logx("[obs] svc(gVhubTick) baseline", svc);
        }
        for (i2 = 0; i2 < np2; i2++) {
            if (obsSeeded && psc[i2] != obsLast[i2] && obsNChg < 300) {   /* a pull/reinsert toggles CONNECT here */
                obsNChg++;
                ehci_os_log("[obs] PORTSC CHANGE");
                ehci_os_logx("  port", i2);            ehci_os_logx("  from", obsLast[i2]);
                ehci_os_logx("  to", psc[i2]);         ehci_os_logx("  gPortConn(our detector)", con[i2]);
                ehci_os_logx("  svc(gVhubTick)", svc); ehci_os_logx("  ticks", nowT);
            }
            obsLast[i2] = psc[i2];
        }
        obsSeeded = 1;
        if ((nowT - obsLastTick) >= 300 && obsNAlive < 90) {   /* ~5s liveness heartbeat, bounded */
            obsLastTick = nowT; obsNAlive++;
            ehci_os_log("[obs] alive"); ehci_os_logx("  svc(gVhubTick)", svc); ehci_os_logx("  ticks", nowT);
            for (i2 = 0; i2 < np2; i2++) { ehci_os_logx("  port", i2); ehci_os_logx("    portsc(live)", psc[i2]); }   /* r84: steady-state port view */
        }
    }
    {
        static UInt32 lastKey = 0xFFFFFFFFUL; static int ndn = 0;
        UInt32 done = 0, err = 0, tmo = 0, qd = 0, la = 0, lp = 0, key; long ls = 0;
        ehci_vhub_down_stats(&done, &err, &tmo, &qd, &la, &lp, &ls);
        key = done + err * 131u + tmo * 977u + qd * 7919u;
        if (key != lastKey && ndn < 60) {                /* bounded: enough to cover enum, won't flood a self-probe run */
            lastKey = key; ndn++;
            ehci_os_log("DOWN ENGINE");
            ehci_os_logx("  done", done);     ehci_os_logx("  err", err);
            ehci_os_logx("  timeouts", tmo);  ehci_os_logx("  qdrop", qd);
            ehci_os_logx("  lastAddr", la);   ehci_os_logx("  lastPid", lp);
            ehci_os_logx("  lastStat", (unsigned long)ls);
        }
    }
    /* r39: the DECIDER for the r38 wall — controller state at a downstream TIMEOUT. Read the raw values:
     *   USBSTS bit12(0x1000)=HCHalted  -> controller stopped (host error / halt) — we ack HSE but never restart
     *   USBSTS bit15(0x8000)=ASS       -> async schedule actually RUNNING (0 while ASE=1 => schedule never started)
     *   USBSTS bit4(0x10)=HSE          -> host system error latched
     *   USBCMD bit5(0x20)=ASE          -> we asked for the async schedule; bit0=RUN
     *   ASYNCLISTADDR vs 'our QH phys' -> does the controller still point at our QH?
     *   QH.ovlToken bit7(0x80)=Active bit6(0x40)=Halted -> is the queue head stuck active or halted? */
    {
        static UInt32 lastToSeq = 0; static int nto = 0;
        UInt32 cmd = 0, sts = 0, async = 0, qhP = 0, epc = 0, cq = 0, ovl = 0, qtd = 0, seq;
        seq = ehci_vhub_timeout_state(&cmd, &sts, &async, &qhP, &epc, &cq, &ovl, &qtd);
        if (seq != lastToSeq && nto < 6) {
            lastToSeq = seq; nto++;
            ehci_os_log("XFER TIMEOUT — controller state:");
            ehci_os_logx("  USBCMD (b5=ASE b0=RUN)", cmd);
            ehci_os_logx("  USBSTS (b12=HCHalted b15=ASS b4=HSE)", sts);
            ehci_os_logx("  ASYNCLISTADDR", async);
            ehci_os_logx("  our QH phys", qhP);
            ehci_os_logx("  QH.epChar", epc);
            ehci_os_logx("  QH.curQtd", cq);
            ehci_os_logx("  QH.ovlToken (b7=Active b6=Halted)", ovl);
            ehci_os_logx("  QTD.token", qtd);
        }
    }
    /* r41: the failing WRITE finally names itself. On a real ~1GB copy the Finder's "disk error" = a BOT
     * WRITE(10) whose CSW came back nonzero (silent → ioErr). READ:
     *   CSW sig == 55534253('USBS') + status 1 => the DEVICE rejected the write (real SCSI CHECK CONDITION;
     *      next step = REQUEST SENSE for the key/ASC — write-protect? medium error? LBA out of range?)
     *   CSW sig != 'USBS'                       => our CSW READ got garbage (a transport/framing bug)
     *   submit rejections > 0                   => the 16-deep async queue overflowed (Finder out-ran us)
     *   'writes OK before' = how far the copy got; 'lba' = where it died (× the ~62GB-HFS huge-alloc angle) */
    {
        static UInt32 lastFailSeq = 0, lastRej = 0; static int nbf = 0;
        UInt8 isw = 0, stat = 0; UInt16 chunk = 0;
        UInt32 lba = 0, sig = 0, resid = 0, wrok = 0, rdok = 0, rej = 0, seq;
        seq = ehci_vhub_biofail(&isw, &lba, &chunk, &stat, &sig, &resid, &wrok, &rdok, &rej);
        if ((seq != lastFailSeq || rej != lastRej) && nbf < 8) {
            lastFailSeq = seq; lastRej = rej; nbf++;
            ehci_os_log("BIO CSW FAILURE / reject:");
            ehci_os_logx("  isWrite", isw);         ehci_os_logx("  lba", lba);
            ehci_os_logx("  chunk(blocks)", chunk);
            ehci_os_logx("  CSW sig (55534253=USBS)", sig);
            ehci_os_logx("  CSW status (0pass 1fail 2phaseErr)", (unsigned long)stat);
            ehci_os_logx("  CSW residue", resid);
            ehci_os_logx("  writes OK before", wrok);  ehci_os_logx("  reads OK before", rdok);
            ehci_os_logx("  submit rejections", rej);
        }
    }
    ehci_vhub_selfprobe_tick();   /* r21: once the mounter parks, drive INQUIRY/READ CAPACITY/READ ourselves */
    return noErr;
}
/* slot 24 — per-bus frame-time clock: MUST return a 64-bit value; the USL compares the LOW word. */
static unsigned long long uim24(UInt32 a,UInt32 b,UInt32 c,UInt32 d,UInt32 e,UInt32 f,UInt32 g,UInt32 h)
{ (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h; return ehci_vhub_frame_time(); }

/* Version word (6) + 28 slots. Layout mirrors the app leg's PROVEN gDispatch — in particular
 * slot 24 (index 25) = frame-time and slot 23 (index 24) = polled service, correcting the
 * scaffold's swap; NULLs where Apple's UIM leaves an op unimplemented. */
void *ThePluginDispatchTable[] = {
    (void *)6,                        /* dispatch-table version */
    (void *)uimInitialize,            /*  0 Initialize            */
    (void *)uimFinalize,              /*  1 Finalize              */
    (void *)uim2,                     /*  2 CreateControlEndpoint */
    (void *)uim3,                     /*  3 ControlTransfer       */
    (void *)0,                        /*  4 */
    (void *)0,                        /*  5 */
    (void *)uim6,                     /*  6 CreateBulkEndpoint    */
    (void *)uim7,                     /*  7 BulkTransfer          */
    (void *)0,                        /*  8 */
    (void *)0,                        /*  9 */
    (void *)uim10,                    /* 10 CreateInterruptEndpoint */
    (void *)uim11,                    /* 11 InterruptTransfer     */
    (void *)0,                        /* 12 */
    (void *)0,                        /* 13 */
    (void *)uim14,                    /* 14 CreateIsochEndpoint   */
    (void *)uim15,                    /* 15 IsochTransfer         */
    (void *)0,                        /* 16 */
    (void *)0,                        /* 17 */
    (void *)uim18,                    /* 18 */
    (void *)uim19,                    /* 19 */
    (void *)uim20,                    /* 20 */
    (void *)0,                        /* 21 */
    (void *)uim22,                    /* 22 activation (returns 0) */
    (void *)uim23,                    /* 23 polled service         */
    (void *)uim24,                    /* 24 frame-time (64-bit)    */
    (void *)uim25,                    /* 25 */
    (void *)uim26,                    /* 26 */
    (void *)uim27,                    /* 27 */
    (void *)uim28,                    /* 28 */
};
