/*
 * EHCITrigger — bring up the EHCI UIM the sanctioned (USL-4.2) way.
 *
 * The USB Expert never probes an EHCI (0c0320) node, and a ROM-less PCI node carries no
 * driver,AAPL,MacOS,PowerPC property, so LoadUIMForEntry's FindDriverCandidates finds no driver
 * (fnfErr/-43 — the v0.1 result). This app fixes that: it EMBEDS our ndrv's PEF (ehci_pef_blob.h)
 * and CREATES the node's driver,AAPL,MacOS,PowerPC property from it verbatim — the "property-based
 * driver" a declaration-ROM card would supply (raw PEF container, confirmed vs the PCI DDK).
 * It then calls the Expert's LoadUIMForEntry(node): FindDriverCandidates now returns our driver as
 * the prop-based candidate -> it loads as the UIM plugin (FindSymbol "ThePluginDispatchTable") ->
 * BUILDS the Name Registry parent-deviceRef entry (the reset-routing fix USBResetDevice needs) ->
 * USBAddBus. We then pump the USL and read its status log + a mount check.
 *
 * Self-contained: NO Extensions install needed — the driver rides inside this app. LoadUIMForEntry
 * loads the plugin into system memory, so a resulting mount outlives this app. We do NOT touch the
 * EHCI registers ourselves — the loaded ndrv's Initialize slot does the bring-up. Reboot between
 * runs (a registered bus + installed ISR hold pointers; the Name Registry is rebuilt at boot).
 */
#include <stdio.h>
#include <stdarg.h>
#include <MacTypes.h>
#include <NameRegistry.h>
#include <CodeFragments.h>
#include <OSUtils.h>
#include <Files.h>
#include <Devices.h>         /* FindDriverCandidates, FileBasedDriverRecord, MacDriverType, InstallDriverFromMemory */
#include <Disks.h>           /* DrvQElPtr / dQRefNum / dQDrive — walk the drive queue after AddDrive */
#include <Gestalt.h>         /* Gestalt('Eusb') — the UIM's block-read service handshake */
#include <Events.h>          /* r26: WaitNextEvent — real event loop so the desktop is usable */
#include <Windows.h>         /* r26: FrontWindow/HideWindow — get the fullscreen console out of the way */
#include "ehci_pef_blob.h"   /* gEHCIPef[] / gEHCIPefLen — the raw ndrv PEF to attach to the node */
#include "usb_disk_blob.h"   /* gUsbDiskPef[] / gUsbDiskPefLen — our block driver (BYPASS m2) */

#define kDriverPropertyName "driver,AAPL,MacOS,PowerPC"

/* A DriverDescription byte-identical to the ndrv's exported TheDriverDescription (ehci_uim.c).
 * RE of FindDriverCandidates (Mac OS ROM, DriverLoaderLib) shows the loader sets
 * gotPropBasedDriver=TRUE ONLY after a successful read of the node's "driver-descriptor" property,
 * and copies its driverType (offset 8, 36 bytes) into propBasedDriverType. So we must create it. */
typedef struct { unsigned char len; char s[31]; } TStr31;
typedef struct { unsigned char major, minorBug, stage, nonRel; } TNumVer;
typedef struct {
    OSType  sig; UInt32 descVersion;
    TStr31  nameInfoStr; TNumVer typeVersion;     /* <- driverType (36 bytes) at offset 8 */
    UInt32  driverRuntime;
    TStr31  driverName; UInt32 reserved[8]; UInt32 nServices;
} TDriverDesc;
static const TDriverDesc gDriverDesc = {
    0x6d74656aUL /* 'mtej' */, 0,
    { 15, "pciclass,0c0320" }, { 1, 0, 0x80, 0 },
    0x00000004UL,
    { 7, "EHCIUIM" }, { 0,0,0,0,0,0,0,0 }, 0
};

#define PL(x) ((unsigned long)(x))

/* USL/Expert calls resolved by the linker via the import stubs (usl_import/). */
extern OSStatus USBExpertSetStatusLevel(UInt32 level);
extern OSStatus USBGetVersion(UInt32 *version);
extern void     ExpertIdleTask(void);
extern OSStatus USLPolledProcessDoneQueue(void);
extern void     SystemTask(void);   /* give drivers/deferred continuations task-level time */

/* Resolved at runtime (not in the stubs): the plugin loader + the status-log accessors. */
typedef OSStatus (*LoadUIMProc)(RegEntryID *node);
typedef void (*GetStatusProc)(unsigned long *, unsigned long *, unsigned long *);
typedef void (*ResetStatusProc)(void);
static LoadUIMProc     gLoadUIM = 0;
static GetStatusProc   gGetStatus = 0;
static ResetStatusProc gResetStatus = 0;
static volatile unsigned long *gStatusBuf = 0;

/* Crash-proof progress log: writes to the console AND to a disk file that is FLUSHED to the volume
 * after every line, so if the app dies (r4 = error type 12) the last surviving line pinpoints the
 * step that crashed. The file lands next to the app; open it in SimpleText after a reboot. */
static short gLogRef = 0;
static short gLogVol = 0;   /* r49: the log's OWN volume (boot), captured at create — see LOG() copy-safety */
static int gQuiet = 0;   /* r26: once the console is hidden, log to FILE only — a printf could re-show it */
static void log_open(const char *fname)
{
    FSSpec sp; Str63 pn; int i = 0;
    while (fname[i] && i < 62) { pn[i + 1] = fname[i]; i++; } pn[0] = (unsigned char)i;
    (void)FSMakeFSSpec(0, 0, pn, &sp);      /* default vol/dir = the app's folder */
    (void)FSpDelete(&sp);
    if (FSpCreate(&sp, 'ttxt', 'TEXT', 0) == noErr) { (void)FSpOpenDF(&sp, fsRdWrPerm, &gLogRef); gLogVol = sp.vRefNum; }
}
static void LOG(const char *fmt, ...)
{
    char buf[1024]; long i, n; va_list ap;
    va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);   /* r71: BOUNDED. An over-long banner overflowed the old buf[512] via UNBOUNDED vsprintf and smashed the saved return address -> MacsBug "unmapped memory exception" jumping into string bytes. */
    if (!gQuiet) printf("%s", buf);
    if (gLogRef) {
        ParamBlockRec pbf;
        for (i = 0; buf[i]; i++) if (buf[i] == '\n') buf[i] = '\r';   /* Mac line endings for SimpleText */
        n = i; (void)FSWrite(gLogRef, &n, buf);
        /* r49 COPY-SAFETY: flush the log's OWN volume (not FlushVol(0,0)=default, which moves to the USB
         * stick after mount) so the open log's catalog EOF is committed and it copies at full size. */
        pbf.ioParam.ioCompletion = 0; pbf.ioParam.ioRefNum = gLogRef; (void)PBFlushFileSync(&pbf);
        (void)FlushVol(0, gLogVol);
    }
}

/* r55: a SEPARATE, TINY health/summary log ("USB Health.log") — ONLY the [verify] result + [health] counter
 * lines, NO per-I/O CSLOG spam. Small enough to OPEN in SimpleText (<32KB) and to copy cleanly, so we can
 * finally read the POST-Finder-copy engine state (the big log is unopenable, and the backgrounded app can't
 * capture the copy well). Copy-safe (flush its OWN volume, like LOG). */
static short gHRef = 0, gHVol = 0;
static void hlog_open(const char *fname)
{
    FSSpec sp; Str63 pn; int i = 0;
    while (fname[i] && i < 62) { pn[i + 1] = fname[i]; i++; } pn[0] = (unsigned char)i;
    (void)FSMakeFSSpec(0, 0, pn, &sp);
    (void)FSpDelete(&sp);
    if (FSpCreate(&sp, 'ttxt', 'TEXT', 0) == noErr) { (void)FSpOpenDF(&sp, fsRdWrPerm, &gHRef); gHVol = sp.vRefNum; }
}
static void HLOG(const char *fmt, ...)
{
    char buf[1024]; long i, n; va_list ap; ParamBlockRec pbf;
    va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);   /* r71: BOUNDED (see LOG — this is what crashed r71) */
    if (!gHRef) return;
    for (i = 0; buf[i]; i++) if (buf[i] == '\n') buf[i] = '\r';
    n = i; (void)FSWrite(gHRef, &n, buf);
    pbf.ioParam.ioCompletion = 0; pbf.ioParam.ioRefNum = gHRef; (void)PBFlushFileSync(&pbf);
    (void)FlushVol(0, gHVol);
}

/* Low-mem queues: mounted volumes (VCB @0x0356) + recognized disks (drive @0x0308). */
#define kVCBQ ((QHdrPtr)0x0356L)
#define kDRVQ ((QHdrPtr)0x0308L)
static int qlen(QHdrPtr q){ int n=0; QElemPtr e; if(!q) return 0; for(e=q->qHead; e && n<200; e=e->qLink) n++; return n; }

static void resolve_symbols(void)
{
    CFragConnectionID conn; Ptr mainAddr; Str255 errName; Ptr sym; CFragSymbolClass cls;
    OSErr e = GetSharedLibrary("\pUSBFamilyExpertLib", kPowerPCCFragArch, kReferenceCFrag,
                               &conn, &mainAddr, errName);
    if (e != noErr) { printf("[sym] GetSharedLibrary(USBFamilyExpertLib)=%d\n", (int)e); return; }
    if (FindSymbol(conn, "\pLoadUIMForEntry", &sym, &cls) == noErr) gLoadUIM = (LoadUIMProc)sym;
    if (FindSymbol(conn, "\pgUSBStatusBuffer", &sym, &cls) == noErr) gStatusBuf = (volatile unsigned long *)sym;
    if (FindSymbol(conn, "\pUSBExpertGetStatus", &sym, &cls) == noErr) gGetStatus = (GetStatusProc)sym;
    if (FindSymbol(conn, "\pUSBResetStatus", &sym, &cls) == noErr) gResetStatus = (ResetStatusProc)sym;
    printf("[sym] LoadUIMForEntry=%08lx gUSBStatusBuffer=%08lx USBExpertGetStatus=%08lx\n",
           PL((unsigned long)gLoadUIM), PL((unsigned long)gStatusBuf), PL((unsigned long)gGetStatus));
}

/* Render the Expert/USL status log as readable lines (nulls between messages -> newline). */
static void dump_status_log(const char *tag)
{
    volatile unsigned long *w = gStatusBuf;
    unsigned long a=0,b=0,c=0, dataPtr, size, wr, k; int col=0, atnl=1;
    if (!w) { printf("[status] gUSBStatusBuffer unresolved [%s]\n", tag); return; }
    if (gGetStatus) gGetStatus(&a, &b, &c);
    dataPtr = w[0]; size = w[1]; wr = w[2];
    LOG("\n==== USB STATUS LOG [%s]  hdr=%08lx/%08lx/%08lx ====\n", tag, PL(dataPtr), PL(size), PL(wr));
    if (dataPtr < 0x20000UL || dataPtr >= 0x20000000UL) { LOG("  (hdr[0] not a plausible ptr)\n"); return; }
    if (size < 0x40UL || size > 0x4000UL) size = 0x2000UL;
    { volatile unsigned char *p = (volatile unsigned char *)dataPtr; char line[128];
      unsigned long n = (wr > 0 && wr <= size) ? (wr + 16 > size ? size : wr + 16) : size;
      (void)atnl;
      for (k = 0; k < n; k++) { unsigned char ch = p[k];
          if (ch >= 32 && ch < 127) { line[col++] = (char)ch; if (col >= 120) { line[col] = 0; LOG("%s\n", line); col = 0; } }
          else if (col > 0) { line[col] = 0; LOG("%s\n", line); col = 0; } }
      if (col > 0) { line[col] = 0; LOG("%s\n", line); } }
    LOG("==== end log [%s] ====\n", tag);
}

/* r24: dump the block driver's Status/Control probe ring (published via Gestalt 'Ucsl'). Prints
 * only records newer than the last call, so the monitor loop shows a LIVE timeline of the Foreign
 * File Access / Audio CD Access probes that hit our drive — the calls the r23 mount-time log missed.
 * The struct layout mirrors usb_disk.c's CsLog byte-for-byte (same PPC ABI, same toolchain). */
typedef struct { short kind, csCode, ioVRefNum, pad; long p0, p1; } CsRec;    /* 16 bytes */
typedef struct { unsigned long magic, count, cap, nReads, nWrites; CsRec recs[1]; } CsLog;
static unsigned long gCsDumped = 0, gLastReads = 0, gLastWrites = 0;
static void dump_cslog(const char *tag)
{
    long gv; CsLog *cl; unsigned long total, cap, i, from;
    if (Gestalt('Ucsl', &gv) != noErr || gv == 0) return;
    cl = (CsLog *)gv;
    if (cl->magic != 0x5563736cUL) return;                          /* 'Ucsl' */
    cap = cl->cap; total = cl->count;
    if (cap == 0 || (cap & (cap - 1)) != 0 || cap > 4096) return;   /* index math needs power of 2 */
    /* report if there are new probe records OR the read/write counters moved (a Finder copy shows
     * up as writes climbing even though writes aren't recorded as probe records) */
    if (total == gCsDumped && cl->nReads == gLastReads && cl->nWrites == gLastWrites) return;
    from = gCsDumped;
    if (total > cap && total - cap > from) from = total - cap;      /* ring wrapped past unseen */
    LOG("\n---- CSLOG [%s] reads=%lu writes=%lu; new %lu..%lu (total=%lu%s)"
        "  [RD/WR: a=nblk b=startblk k=iokind(1sync/2async/4imm); ST/CT: a=csCode b=selector] ----\n",
        tag, cl->nReads, cl->nWrites, from, total ? total - 1 : 0, total, (total > cap ? " WRAPPED" : ""));
    for (i = from; i < total; i++) {
        CsRec *r = &cl->recs[i & (cap - 1)];
        const char *kn = (r->kind == 1 ? "STATUS" : r->kind == 2 ? "CONTROL" :
                          r->kind == 3 ? "READ"   : r->kind == 4 ? "WRITE"   :
                          r->kind == 5 ? "REENTR" : "?");
        LOG("  [%lu] %-7s a=%-5d vRef=%-4d b=%08lx k=%08lx\n",
            i, kn, (int)r->csCode, (int)r->ioVRefNum, (unsigned long)r->p0, (unsigned long)r->p1);
    }
    gCsDumped = total; gLastReads = cl->nReads; gLastWrites = cl->nWrites;
}

/* ---- 'Eusb' block-read service published by the UIM (r49: extended to reach healthFn). The layout is a
 * prefix of the UIM's real gSvc (magic, readFn, writeFn, blkSize, blkCnt, submitFn, healthFn) so appended
 * fields are ABI-safe; existing readFn/writeFn users are unaffected. ---- */
typedef long (*UsbRwFn)(unsigned long lba, unsigned long count, void *buf);
typedef long (*UsbSubmitFn)(long cmdID, unsigned long lba, unsigned long count, void *buf, int isWrite, long *actCount);
typedef void (*UsbHealthFn)(unsigned long *reject, unsigned long *hiwater, unsigned long *downTimeouts, unsigned long *downErr, unsigned long *downDone,
                            unsigned long *failSeq, unsigned long *failStat, unsigned long *failSig, unsigned long *failLba, unsigned long *isrHits, unsigned long *maxStall,
                            unsigned long *downRecov, unsigned long *downRelink, unsigned long *lastAnchorLink,
                            unsigned long *dataBytes, unsigned long *dataFrames);   /* r60: QH re-splices; r67: pure data-phase rate */
typedef unsigned long (*UsbToStateFn)(unsigned long *cmd, unsigned long *sts, unsigned long *async, unsigned long *qhP, unsigned long *epChar, unsigned long *curQtd, unsigned long *ovlTok, unsigned long *qtdTok);
typedef unsigned long (*UsbSimFn)(unsigned long n);   /* r81: hot-replug async-schedule isolation test */
typedef void          (*UsbArmFn)(void);              /* r85: arm the [obs] probe */
typedef void          (*UsbCrumbFn)(unsigned long tag); /* r94: app idle-loop breadcrumb (diagnostic) */
typedef struct { unsigned long magic; UsbRwFn readFn; UsbRwFn writeFn; unsigned long blkSize, blkCnt; UsbSubmitFn submitFn; UsbHealthFn healthFn; UsbToStateFn toStateFn; UsbSimFn simReplugFn; UsbArmFn obsArmFn; UsbArmFn tickFn; UsbCrumbFn loopFn; } UsbSvc;

/* r49 THREAD-B DECIDER: read the block engine's health via 'Eusb' healthFn and log it when a decisive
 * counter moves. Runs from the idle loop (task level, File-Mgr-safe) so it captures the AFTERMATH of a
 * Finder large copy even though the copy starved the UI — reject/hiwater/timeouts are cumulative, so the
 * first idle pass after recovery logs the peak values. READ THE LAST [health] LINE:
 *   reject > 0                    => the 16-deep async ring overflowed (Finder out-ran us) = the "disk
 *                                    error" is mechanism (a); cheap fix = deeper ring + soft back-pressure.
 *   downTimeouts > 0 (reject==0)  => slow single-in-flight engine watchdog-timed-out = mechanism (b); R4.
 *   hiwater near 16               => ring pressure (how close it came to overflow). */
static void dump_health(void)
{
    long gv; UsbSvc *sv;
    unsigned long rej = 0, hw = 0, tmo = 0, derr = 0, ddone = 0;
    unsigned long fseq = 0, fstat = 0, fsig = 0, flba = 0;   /* r50: CSW-failure detail */
    unsigned long isrh = 0;                                  /* R4-P1: real EHCI IRQ count */
    unsigned long mstall = 0;                                /* r54: worst device stall (60Hz ticks) */
    unsigned long drecov = 0;                                /* r57: BOT reset-recoveries */
    unsigned long drelink = 0, lanchor = 0;                  /* r60: async-ring re-splices + current anchor target */
    unsigned long dbytes = 0, dframes = 0;                   /* r67: pure data-phase rate (MB/s = bytes/(frames*125)) */
    static unsigned long lRej = 0xFFFFFFFFUL, lHw = 0xFFFFFFFFUL, lTmo = 0xFFFFFFFFUL, lErr = 0xFFFFFFFFUL, lFseq = 0xFFFFFFFFUL, lRelink = 0xFFFFFFFFUL;
    if (Gestalt('Eusb', &gv) != noErr || gv == 0) return;
    sv = (UsbSvc *)gv;
    if (sv->magic != 0x45555342UL || !sv->healthFn) return;
    sv->healthFn(&rej, &hw, &tmo, &derr, &ddone, &fseq, &fstat, &fsig, &flba, &isrh, &mstall, &drecov, &drelink, &lanchor, &dbytes, &dframes);
    /* r67: the DECISIVE throughput datum — the PURE data-phase rate (isolated from per-command overhead).
     * MB/s*100 = dbytes*8 / (dframes*125) *100 ... = dbytes*800/(dframes*125). >~10 MB/s => per-command overhead
     * is the wall (do Phase 2 phase-prequeue + ITC); ~2-3 MB/s => the data phase itself is slow (park mode / RL). */
    if (dframes) {
        /* rate = dbytes / (dframes * 125us). MB/s*100 = dbytes*0.8/dframes = (dbytes*4)/(dframes*5). */
        unsigned long mbx100 = (dbytes * 4UL) / (dframes * 5UL);
        HLOG("[rcmd] %lu KB in %lu uframes (125us) = %lu.%02lu MB/s on-the-wire per-READ-command (issue->reap: device latency + xfer + 1 IRQ). Compare to [speed]: rcmd >> speed => File-Mgr/above-us overhead is the wall (read-ahead lever); rcmd ~= speed => on-the-wire bound.\n",
             dbytes >> 10, dframes, mbx100 / 100, mbx100 % 100);
    }
    /* r50: also log whenever failSeq moves — each CSW-level write failure (the Finder "disk error").
     * r60: also whenever downRelink moves — a QH re-splice (the freeze fix firing). */
    if (rej == lRej && hw == lHw && tmo == lTmo && derr == lErr && fseq == lFseq && drelink == lRelink) return;
    lRej = rej; lHw = hw; lTmo = tmo; lErr = derr; lFseq = fseq; lRelink = drelink;
    LOG("\n[health] reject=%lu hiwater=%lu (ring=16) downTimeouts=%lu downErr=%lu downDone=%lu isrHits=%lu\n",
        rej, hw, tmo, derr, ddone, isrh);
    LOG("[health]   r54 maxStall=%lu ticks (~%lu.%01lu s @60Hz) = the device's worst-case flash-GC pause; watchdog now 60s\n",
        mstall, mstall / 60, ((mstall % 60) * 10) / 60);
    /* r55: the DECISIVE line in the small openable log. downTimeouts/downErr/failSeq all 0 after a Finder
     * copy => the "disk error" is ABOVE our block I/O (Status/File-Mgr/HFS); any nonzero => it's our block path. */
    HLOG("[health] downTimeouts=%lu downRecov=%lu downErr=%lu failSeq=%lu reject=%lu maxStall=%lu(~%lu.%01lus) downDone=%lu\n",
        tmo, drecov, derr, fseq, rej, mstall, mstall / 60, ((mstall % 60) * 10) / 60, ddone);
    /* r60 QH-UNLINK FREEZE FIX — the decisive line. downRelink>0 => our downstream QH fell out of the
     * async ring under heavy wedging and the driver RE-SPLICED it (a would-be FREEZE turned into slow-but-
     * alive). lastAnchorLink = phys the async anchor points at right now; == the ourQH in [timeout-state]
     * means the ring is intact. WIN = the copy survives with downRelink climbing instead of a hard freeze. */
    HLOG("[ring] downRelink=%lu lastAnchorLink=%08lx (relink>0 = QH re-spliced = freeze fix fired)\n",
        drelink, lanchor);
    /* r56: when a watchdog timeout has occurred, dump the controller state captured AT it — the decisive clue
     * for WHY a Finder-copy transfer hangs 60s: schedule stopped (ASS=0)? our QH UNLINKED from the async ring?
     * qTD Halted (b6)? or just Active+NAKing (device)? UNLINKED => a shared-engine race clobbered the ring. */
    if (tmo > 0 && sv->toStateFn) {
        unsigned long cmd = 0, sts = 0, async = 0, qhP = 0, epc = 0, cq = 0, ovl = 0, qtd = 0;
        (void)sv->toStateFn(&cmd, &sts, &async, &qhP, &epc, &cq, &ovl, &qtd);
        HLOG("[timeout-state] USBSTS=%08lx(b12halt b15schedRun b4hse) USBCMD=%08lx async=%08lx ourQH=%08lx=%s ovlTok=%08lx(b7active b6halt)\n",
            sts, cmd, async, qhP, (async == qhP ? "LINKED" : "UNLINKED"), ovl);
    }
    LOG("[health] CSW-FAIL failSeq=%lu failStat=%lu failSig=%08lx (55534253='USBS'=real device reject; else our CSW read=garbage) failLba=%lu\n",
        fseq, fstat, fsig, flba);
}

/* Create a property, or replace it if it already exists (a prior run this boot). */
static OSStatus set_prop(RegEntryID *n, const char *name, const void *val, RegPropertyValueSize sz)
{
    OSStatus e = RegistryPropertyCreate(n, name, (RegPropertyValue)val, sz);
    if (e != noErr) { (void)RegistryPropertyDelete(n, name); e = RegistryPropertyCreate(n, name, (RegPropertyValue)val, sz); }
    return e;
}

/* Read + print one Name Registry property: null-separated strings if printable, else hex. */
static void dump_prop(RegEntryID *n, const char *name)
{
    RegPropertyValueSize sz = 0; unsigned char buf[300]; OSStatus e; unsigned long i; int printable;
    e = RegistryPropertyGetSize(n, name, &sz);
    if (e != noErr) { printf("    %-26s absent (%ld)\n", name, (long)e); return; }
    if (sz > sizeof(buf)) { printf("    %-26s [%lu bytes — present, too big to show]\n", name, (unsigned long)sz); return; }
    e = RegistryPropertyGet(n, name, buf, &sz);
    if (e != noErr) { printf("    %-26s get failed (%ld)\n", name, (long)e); return; }
    printable = (sz > 0);
    for (i = 0; i < sz; i++) { unsigned char c = buf[i]; if (c != 0 && (c < 32 || c > 126)) { printable = 0; break; } }
    printf("    %-26s [%lu] ", name, (unsigned long)sz);
    if (printable) { for (i = 0; i < sz; i++) { if (buf[i] == 0) { if (i + 1 < sz) printf(" | "); } else putchar(buf[i]); } }
    else { for (i = 0; i < sz && i < 24; i++) printf("%02x ", buf[i]); }
    putchar('\n');
}

/* Call the Driver Loader's candidate search directly and report every output — the empirical
 * answer to "does the loader see a driver for this node, and via which path (prop vs file)?". */
static void probe_candidates(RegEntryID *n, const char *tag)
{
    Ptr propDrv = 0; RegPropertyValueSize propSz = 0; Str255 devName;
    MacDriverType propType; Boolean gotProp = false;
    FileBasedDriverRecord files[8]; ItemCount nFiles = 8; OSErr e; ItemCount i;
    devName[0] = 0; propType.nameInfoStr[0] = 0;
    LOG("  >> FindDriverCandidates(%s) calling...\n", tag);
    e = FindDriverCandidates(n, &propDrv, &propSz, devName, &propType, &gotProp, files, &nFiles);
    LOG("  [FindDriverCandidates %s] -> %d  gotPropDriver=%d propSize=%lu nFileDrivers=%lu\n",
        tag, (int)e, (int)gotProp, (unsigned long)propSz, (unsigned long)nFiles);
    printf("      deviceName='%.*s'\n", devName[0], (char *)devName + 1);
    if (gotProp) printf("      propDriver.nameInfoStr='%.*s'\n",
                        propType.nameInfoStr[0], (char *)propType.nameInfoStr + 1);
    for (i = 0; i < nFiles && i < 8; i++)
        printf("      file[%lu] nameInfoStr='%.*s' compatMatch=%d\n", (unsigned long)i,
               files[i].theType.nameInfoStr[0], (char *)files[i].theType.nameInfoStr + 1,
               (int)files[i].compatibleProp);
}

/* r25: AUTOMATED WRITE TEST — run BY THE APP while it is alive and pumping, so the write path
 * (WRITE(10) engine + PC Exchange FAT writes) is exercised WITHOUT desktop interaction and WITHOUT
 * the force-quit that invalidated the r24 manual copy. Quitting the app frees the memory the USL /
 * bulk engine / interrupt heartbeat point into, so ANY USB I/O afterward crashes — that is what the
 * r24 "Items remaining: 1" freeze was, NOT a write-path verdict (the mount itself was healthy: correct
 * files/name/icon, all read while the app was alive + Finder-cached). Two levels, SAFE FIRST:
 *   L1 raw WRITE(10) via the UIM's 'Eusb' writeFn to RESERVED LBA 1 (the MBR->partition gap, before
 *      the FAT partition @ 0x20): read-save -> write pattern -> read-verify -> RESTORE original.
 *      Net-zero to the disk; proves the engine end to end. WRITE(10) shares READ(10)'s CDB LBA fields,
 *      and READ addressed correctly at r22 self-probe, so a mis-addressed write is very unlikely.
 *   L2 create a real file on the mounted FAT volume + read it back = the full Finder-copy path. */
/* (UsbSvc / UsbRwFn / UsbSubmitFn / UsbHealthFn typedefs moved up to precede dump_health — r49) */

static void write_test_L1(void)
{
    long gv; UsbSvc *sv; static unsigned char sav[512], pat[512], rb[512];
    long wr, rd; int i, mism;
    if (Gestalt('Eusb', &gv) != noErr || gv == 0) { LOG("[wtest L1] no 'Eusb' service\n"); return; }
    sv = (UsbSvc *)gv;
    if (sv->magic != 0x45555342UL || !sv->writeFn || !sv->readFn) { LOG("[wtest L1] service incomplete\n"); return; }
    LOG("\n[wtest L1] raw WRITE(10) to reserved LBA 1 (read-save -> write -> verify -> restore)...\n");
    rd = sv->readFn(1, 1, sav);  if (rd <= 0) { LOG("[wtest L1] pre-read FAILED (%ld) — aborting L1\n", rd); return; }
    for (i = 0; i < 512; i++) pat[i] = (unsigned char)(0xA5 ^ (i & 0xFF));
    wr = sv->writeFn(1, 1, pat);                 LOG("[wtest L1] writeFn -> %ld\n", wr);
    for (i = 0; i < 512; i++) rb[i] = 0;
    rd = sv->readFn(1, 1, rb);                   LOG("[wtest L1] readback -> %ld\n", rd);
    for (i = 0, mism = 0; i < 512; i++) if (rb[i] != pat[i]) mism++;
    LOG("[wtest L1] %s (mismatches=%d/512; rb[0..3]=%02x %02x %02x %02x)\n",
        (wr > 0 && rd > 0 && mism == 0) ? "*** WRITE(10) ENGINE OK ***" : "*** WRITE FAILED ***",
        mism, rb[0], rb[1], rb[2], rb[3]);
    wr = sv->writeFn(1, 1, sav);                 LOG("[wtest L1] restore original -> %ld (net-zero)\n", wr);
}

static void write_test_L2(short vref)
{
    FSSpec sp; short rf = 0; long n, want; OSErr e; static char rbk[64]; int i, ok;
    static const char msg[] = "USB2-EHCI r25 write test payload 0123456789";
    if (vref == 0) { LOG("[wtest L2] no mounted-volume vRefNum — skipping\n"); return; }
    LOG("\n[wtest L2] create a real file on the mounted volume (vRef=%d)...\n", (int)vref);
    e = FSMakeFSSpec(vref, 2 /*fsRtDirID*/, "\pUSBWTEST.TXT", &sp);
    LOG("[wtest L2] FSMakeFSSpec -> %d (fnfErr/-43 is fine; spec still valid)\n", (int)e);
    (void)FSpDelete(&sp);
    e = FSpCreate(&sp, 'ttxt', 'TEXT', 0);               LOG("[wtest L2] FSpCreate -> %d\n", (int)e);
    if (e != noErr) return;
    e = FSpOpenDF(&sp, fsRdWrPerm, &rf);                 LOG("[wtest L2] FSpOpenDF(rw) -> %d\n", (int)e);
    if (e != noErr) return;
    for (want = 0; msg[want]; want++) ;                  /* strlen */
    n = want; e = FSWrite(rf, &n, (Ptr)msg);             LOG("[wtest L2] FSWrite -> %d (wrote %ld/%ld)\n", (int)e, n, want);
    (void)FSClose(rf);
    (void)FlushVol(0, vref);                             LOG("[wtest L2] FlushVol done\n");
    rf = 0;
    e = FSpOpenDF(&sp, fsRdPerm, &rf);                   LOG("[wtest L2] reopen(ro) -> %d\n", (int)e);
    if (e != noErr) return;
    n = want; for (i = 0; i < 64; i++) rbk[i] = 0;
    e = FSRead(rf, &n, (Ptr)rbk);                        LOG("[wtest L2] FSRead -> %d (read %ld)\n", (int)e, n);
    (void)FSClose(rf);
    ok = (n == want); for (i = 0; ok && i < (int)want; i++) if (rbk[i] != msg[i]) ok = 0;
    LOG("[wtest L2] %s (readback: \"%.44s\")\n", ok ? "*** FAT FILE WRITE OK ***" : "*** MISMATCH/FAIL ***", rbk);
}

/* r27: raw MULTI-BLOCK WRITE(10) via the 'Eusb' engine to the reserved gap (LBAs 1..nblk, before the
 * FAT partition @ 0x20), read-save->write->verify->RESTORE. Isolates whether the multi-block obig
 * large-OUT path (never exercised — every proven write was 1 block) is the crash. nblk 1..7 (7*512 =
 * the per-WRITE(10) cap). The "trying" line is flushed BEFORE the write, so a freeze pinpoints nblk. */
static void write_test_mb(int nblk)
{
    long gv; UsbSvc *sv; static unsigned char sav[3584], pat[3584], rb[3584];
    long wr, rd; int i, mism, nbytes;
    if (nblk < 1 || nblk > 7) return;
    nbytes = nblk * 512;
    if (Gestalt('Eusb', &gv) != noErr || gv == 0) return;
    sv = (UsbSvc *)gv;
    if (sv->magic != 0x45555342UL || !sv->writeFn || !sv->readFn) return;
    LOG("[mbtest] %d-block (%dB) raw WRITE(10) to gap LBA 1 ...\n", nblk, nbytes);   /* flush BEFORE = pinpoints a freeze */
    rd = sv->readFn(1, nblk, sav);  if (rd < nblk) { LOG("[mbtest] %d-blk pre-read ret %ld — skip\n", nblk, rd); return; }
    for (i = 0; i < nbytes; i++) pat[i] = (unsigned char)((0x30 + nblk) ^ (i & 0xFF));
    wr = sv->writeFn(1, nblk, pat);
    for (i = 0; i < nbytes; i++) rb[i] = 0;
    rd = sv->readFn(1, nblk, rb);
    for (i = 0, mism = 0; i < nbytes; i++) if (rb[i] != pat[i]) mism++;
    LOG("[mbtest] %d-block: wr=%ld rd=%ld %s (mism=%d/%d)\n", nblk, wr, rd,
        (wr == nblk && rd == nblk && mism == 0) ? "*** OK ***" : "*** FAIL ***", mism, nbytes);
    (void)sv->writeFn(1, nblk, sav);   /* restore the gap (net-zero) */
}

/* r27: write a real multi-block FILE on the mounted FAT volume in 4KB chunks — the SAME
 * FileMgr -> PC Exchange -> svc_write -> multi-block WRITE(10) path a Finder copy uses, but IN-APP
 * (frontmost, no background/contention confound). Logs BEFORE each chunk so a freeze pinpoints the
 * cumulative size. Then reads it all back and verifies the position-dependent pattern. */
static void write_test_big(short vref)
{
    FSSpec sp; short rf = 0; long n; OSErr e; static unsigned char buf[4096];
    int c, i, ok, bad; const int kChunks = 16;   /* 16 * 4KB = 64 KB */
    if (vref == 0) { LOG("[bigtest] no mounted-volume vRefNum — skipping\n"); return; }
    LOG("\n[bigtest] write a %dKB file on the mounted volume in 4KB chunks (the Finder's multi-block path)...\n", kChunks * 4);
    e = FSMakeFSSpec(vref, 2, "\pUSBBIG.DAT", &sp);
    (void)FSpDelete(&sp);
    e = FSpCreate(&sp, 'ttxt', 'BINA', 0);   LOG("[bigtest] FSpCreate -> %d\n", (int)e);   if (e != noErr) return;
    e = FSpOpenDF(&sp, fsRdWrPerm, &rf);     LOG("[bigtest] open -> %d\n", (int)e);          if (e != noErr) return;
    for (c = 0; c < kChunks; c++) {
        for (i = 0; i < 4096; i++) buf[i] = (unsigned char)((c * 7 + i) & 0xFF);   /* position-dependent */
        n = 4096;
        LOG("[bigtest] writing chunk %d/%d (through %dKB) ...\n", c + 1, kChunks, (c + 1) * 4);  /* flush BEFORE */
        e = FSWrite(rf, &n, (Ptr)buf);
        if (e != noErr) { LOG("[bigtest] chunk %d FSWrite -> %d (wrote %ld) — STOP\n", c + 1, (int)e, n); break; }
    }
    (void)FSClose(rf);
    (void)FlushVol(0, vref);
    LOG("[bigtest] wrote+flushed; verifying by read-back...\n");
    rf = 0; e = FSpOpenDF(&sp, fsRdPerm, &rf);   if (e != noErr) { LOG("[bigtest] reopen -> %d\n", (int)e); return; }
    ok = 1; bad = -1;
    for (c = 0; c < kChunks && ok; c++) {
        n = 4096; e = FSRead(rf, &n, (Ptr)buf);
        if (e != noErr || n != 4096) { LOG("[bigtest] read chunk %d -> %d (n=%ld)\n", c + 1, (int)e, n); ok = 0; break; }
        for (i = 0; i < 4096; i++) if (buf[i] != (unsigned char)((c * 7 + i) & 0xFF)) { ok = 0; bad = c + 1; break; }
    }
    (void)FSClose(rf);
    LOG("[bigtest] %s%s\n", ok ? "*** BIG MULTI-BLOCK FILE WRITE OK ***" : "*** VERIFY FAILED ***",
        (bad > 0) ? " (data mismatch)" : "");
}

/* r44: TIMED multi-MB write+read to the mounted volume = a QUANTITATIVE throughput number
 * AND a crash probe. A large FRONTMOST sequential write is the proven-safe pattern (r28: in-app writes
 * always worked, Finder copies froze), so if 4MB survives here the copy crash is Finder-specific
 * (re-entrancy/interleaving); if it dies ~1MB in (where the Finder copy died) it's a sustained-write
 * engine bug. 32KB FSWrite chunks; TickCount() timing (60.15 Hz); progress flushed every 512KB so a
 * freeze pins the failure point. KB/s = KB*60/ticks; MB/s = KB/s / 1024. */
static void write_test_speed(short vref)
{
    FSSpec sp; short rf = 0; long n; OSErr e;
    const int kChunks = 128;                 /* 128 * 32KB = 4 MB (write still uses 32KB chunks) */
    Handle bufH = 0; Ptr buf; long bufSz = 4L * 1024 * 1024;   /* r76: up to a 4MB read buffer */
    OSErr terr = -1;
    UInt32 ticks, kb, kbps; int vbad = 0;
    if (vref == 0) { LOG("[speed] no mounted-volume vRefNum — skipping\n"); return; }
    /* r76: the app's memory partition is only 1MB (Retro68 default SIZE), so NewPtr(1MB) failed in r75. Draw the
     * big read buffer from TEMPORARY MEMORY (free system RAM, OUTSIDE the app partition) instead — the classic
     * idiom for large transient buffers. Fall back by halving if the biggest size isn't available. */
    for (;;) {
        bufH = TempNewHandle(bufSz, &terr);
        if (bufH && terr == noErr) break;
        if (bufH) DisposeHandle(bufH);
        bufSz /= 2;
        if (bufSz < 131072L) { LOG("[speed] TempNewHandle failed even at 128KB (%d) — skipping\n", (int)terr); return; }
    }
    HLock(bufH); buf = *bufH;
    LOG("\n[speed] TIMED %d-MB write (contiguous alloc); read buffer = %ld KB (temp mem) ...\n", (kChunks * 32) / 1024, bufSz / 1024);
    (void)FSMakeFSSpec(vref, 2, "\pUSBSPEED.DAT", &sp);
    (void)FSpDelete(&sp);
    e = FSpCreate(&sp, 'ttxt', 'BINA', 0);   if (e != noErr) { LOG("[speed] FSpCreate -> %d\n", (int)e); HUnlock(bufH); DisposeHandle(bufH); return; }
    e = FSpOpenDF(&sp, fsRdWrPerm, &rf);     if (e != noErr) { LOG("[speed] open -> %d\n", (int)e); HUnlock(bufH); DisposeHandle(bufH); return; }
    /* r75: pre-allocate the whole 4MB CONTIGUOUSLY. Per the FM-internals research there is NO ~64KB File-Mgr
     * per-request cap — the driver gets min(app buffer, contiguous-extent run) in one _Read — so the r73/r74
     * ~64KB plateau was file FRAGMENTATION (or the small app buffer). A contiguous file + big FSReads should let
     * bio chunk each read into back-to-back 128KB commands with NO File-Mgr gap, amortizing the ~6.3ms/FSRead
     * overhead → throughput should climb toward the ~20 MB/s wire rate as the read size grows. */
    {
        ParamBlockRec pb; pb.ioParam.ioCompletion = 0; pb.ioParam.ioRefNum = rf;
        pb.ioParam.ioReqCount = (long)kChunks * 32768L; pb.ioParam.ioPosMode = fsFromStart; pb.ioParam.ioPosOffset = 0;
        e = PBAllocContigSync(&pb);
        LOG("[speed] PBAllocContig(4MB) -> %d, got %ld KB %s\n", (int)e, pb.ioParam.ioActCount / 1024,
            (e == noErr) ? "CONTIGUOUS" : "(not contiguous — volume may be fragmented)");
    }
    { long j; for (j = 0; j < bufSz; j++) ((unsigned char *)buf)[j] = (unsigned char)(j & 0xFF); }   /* r79: fill the WHOLE buffer with the position pattern (so the untimed read-back verify holds for any write size) */
    /* r79: SWEEP the WRITE chunk size (32KB → 4MB), same as the read sweep, to expose the WRITE ceiling — a big
     * FSWrite lets bio chain it into back-to-back 128KB write commands (no File-Mgr gap), amortizing the
     * ~6.3ms/FSWrite the same way big reads hit 20 MB/s. FlushVol is inside the timing = real sustained write to
     * the device (not just into the FM cache). 4MB overwritten from pos 0 each pass onto the contiguous file. */
    {
        static const long wsizes[5] = { 32768L, 131072L, 524288L, 1048576L, 4194304L };
        const long total = (long)kChunks * 32768L;   /* 4 MB per pass */
        int s; long prev = 0;
        for (s = 0; s < 5; s++) {
            long wsz = wsizes[s], wrote = 0; UInt32 wt0;
            if (wsz > bufSz) wsz = bufSz;
            if (wsz == prev) continue;                /* skip a duplicate if capping collapsed two sizes */
            prev = wsz;
            e = SetFPos(rf, fsFromStart, 0);
            if (e != noErr) { LOG("[speed] SetFPos(w) -> %d\n", (int)e); break; }
            wt0 = TickCount();
            while (wrote < total) {
                long want = total - wrote; if (want > wsz) want = wsz;
                n = want; e = FSWrite(rf, &n, buf);
                if (e != noErr) { LOG("[speed] *** WRITE FAILED at %ld KB (%ldKB chunks) -> %d ***\n", wrote / 1024, wsz / 1024, (int)e); break; }
                wrote += n;
            }
            (void)FlushVol(0, vref);                  /* commit to the device — inside the timing */
            ticks = TickCount() - wt0; kb = (UInt32)(wrote / 1024); kbps = ticks ? (kb * 60UL) / ticks : 0;
            LOG("[speed] WROTE (%ldKB) %lu KB in %lu ticks (~%lu.%01lu s) = %lu KB/s = %lu.%02lu MB/s\n",
                wsz / 1024, kb, ticks, ticks / 60, ((ticks % 60) * 10) / 60, kbps, kbps / 1024, ((kbps % 1024) * 100) / 1024);
            if (e != noErr) break;
        }
    }
    (void)FSClose(rf);
    /* r75: SWEEP big READ sizes (64KB → 1MB). NO inline verify here (it would pollute the timing); integrity is
     * checked untimed below + by the separate 64MB [verify]. Rising MB/s with size = the 6.3ms/FSRead amortizing. */
    {
        static const long rsizes[4] = { 131072L, 524288L, 1048576L, 4194304L };   /* r76: 128KB → 4MB (one whole-file read) */
        int s; long prev = 0;
        for (s = 0; s < 4; s++) {
            long rsz = rsizes[s], total = 0; UInt32 rt0;
            if (rsz > bufSz) rsz = bufSz;               /* cap to the temp buffer we actually got */
            if (rsz == prev) continue;                  /* skip a duplicate if capping collapsed two sizes */
            prev = rsz;
            rf = 0; e = FSpOpenDF(&sp, fsRdPerm, &rf);
            if (e != noErr) { LOG("[speed] reopen(ro) -> %d\n", (int)e); break; }
            rt0 = TickCount();
            for (;;) { n = rsz; e = FSRead(rf, &n, buf); if (n > 0) total += n; if (e != noErr || n < rsz) break; }
            ticks = TickCount() - rt0; (void)FSClose(rf);
            kb = (UInt32)(total / 1024); kbps = ticks ? (kb * 60UL) / ticks : 0;
            LOG("[speed] READ (%ldKB) %lu KB in %lu ticks (~%lu.%01lu s) = %lu KB/s = %lu.%02lu MB/s\n",
                rsz / 1024, kb, ticks, ticks / 60, ((ticks % 60) * 10) / 60, kbps, kbps / 1024, ((kbps % 1024) * 100) / 1024);
        }
        /* untimed integrity pass over the whole file in 128KB reads (exercises the big-read chunking) */
        rf = 0; e = FSpOpenDF(&sp, fsRdPerm, &rf);
        if (e == noErr) {
            long base = 0;
            for (;;) { long k; n = 131072; e = FSRead(rf, &n, buf);
                for (k = 0; k < n; k++) if (((unsigned char *)buf)[k] != (unsigned char)((base + k) & 0xFF)) { vbad++; break; }
                base += n; if (e != noErr || n < 131072) break; }
            (void)FSClose(rf);
        }
        LOG("[speed] read-back verify: %s\n", vbad ? "*** DATA MISMATCH ***" : "OK");
    }
    (void)FSpDelete(&sp);   /* remove the 4MB test file */
    HUnlock(bufH); DisposeHandle(bufH);
}

/* r51: LARGE characterizing write-verify. r48-r50 proved the USB engine returns noErr for EVERY I/O
 * (failSeq=0, downErr=0) yet the Finder fails a big copy with "cannot be written, disk error" — so either
 * the DATA lands wrong despite a clean CSW (corruption our engine can't see) or the error is HFS/FM-level.
 * This writes a big file (proven-safe sequential pattern; FSClose+reopen defeats FM caching so the read-back
 * really hits the DEVICE) with a 4-byte big-endian GLOBAL-BLOCK-INDEX marker at each 512-block head, then
 * verifies. On the FIRST mismatch it decodes the index that actually landed:
 *   landed index == expected, bytes wrong  => bit/DMA corruption
 *   landed index != expected               => WRONG-BLOCK data (LBA/addressing/ordering bug)
 *   marker all-zero                        => dropped/incomplete transfer
 * ALL OK at 64MB => our sequential data path is solid; the Finder error is above it (interleaved pattern/HFS). */
static void write_test_verify_big(short vref)
{
    FSSpec sp; short rf = 0; long n; OSErr e; static unsigned char buf[32768];
    const long kChunks = 2048;            /* 2048 * 32KB = 64 MB (the Finder fails well under this) */
    long c, i, blk; int failed = 0;
    if (vref == 0) { LOG("[verify] no vRefNum — skipping\n"); return; }
    LOG("\n[verify] LARGE %ld-MB write+readback verify (per-512-block index marker). Our engine reports every\n", (kChunks * 32) / 1024);
    LOG("[verify] I/O OK — this asks whether the DATA is actually correct. ~a few min; fails fast if corrupt.\n");
    (void)FSMakeFSSpec(vref, 2, "\pUSBVERIFY.DAT", &sp);
    (void)FSpDelete(&sp);
    e = FSpCreate(&sp, 'ttxt', 'BINA', 0); if (e != noErr) { LOG("[verify] create -> %d\n", (int)e); return; }
    e = FSpOpenDF(&sp, fsRdWrPerm, &rf);   if (e != noErr) { LOG("[verify] open -> %d\n", (int)e); return; }
    for (c = 0; c < kChunks; c++) {
        for (i = 0; i < 32768; i++) buf[i] = (unsigned char)((c * 251 + i) & 0xFF);
        for (blk = 0; blk < 64; blk++) {                       /* 4-byte BE global block index at each 512-blk head */
            long g = c * 64 + blk; unsigned char *p = &buf[blk * 512];
            p[0] = (unsigned char)(g >> 24); p[1] = (unsigned char)(g >> 16); p[2] = (unsigned char)(g >> 8); p[3] = (unsigned char)g;
        }
        n = 32768; e = FSWrite(rf, &n, (Ptr)buf);
        if (e != noErr || n != 32768) { LOG("[verify] WRITE FAIL chunk %ld (@%ld MB) -> %d n=%ld\n", c, (c * 32) / 1024, (int)e, n); failed = 1; break; }
        if ((c & 127) == 0) LOG("[verify] ... wrote %ld MB\n", (c * 32) / 1024);   /* progress every 4MB */
    }
    (void)FSClose(rf);
    (void)FlushVol(0, vref);
    if (failed) { (void)FSpDelete(&sp); return; }
    LOG("[verify] wrote %ld MB + flushed + closed; reopening read-only to verify from the DEVICE...\n", (kChunks * 32) / 1024);
    rf = 0; e = FSpOpenDF(&sp, fsRdPerm, &rf); if (e != noErr) { LOG("[verify] reopen -> %d\n", (int)e); return; }
    for (c = 0; c < kChunks && !failed; c++) {
        for (i = 0; i < 32768; i++) buf[i] = 0;
        n = 32768; e = FSRead(rf, &n, (Ptr)buf);
        if (e != noErr || n != 32768) { LOG("[verify] READ FAIL chunk %ld (@%ld MB) -> %d n=%ld\n", c, (c * 32) / 1024, (int)e, n); failed = 1; break; }
        for (i = 0; i < 32768; i++) {
            unsigned char exp; long bb = i & ~511L, off = i & 511L, g = c * 64 + (i >> 9);
            if (off == 0) exp = (unsigned char)(g >> 24); else if (off == 1) exp = (unsigned char)(g >> 16);
            else if (off == 2) exp = (unsigned char)(g >> 8); else if (off == 3) exp = (unsigned char)g;
            else exp = (unsigned char)((c * 251 + i) & 0xFF);
            if (buf[i] != exp) {
                unsigned long landed = ((unsigned long)buf[bb] << 24) | ((unsigned long)buf[bb + 1] << 16) | ((unsigned long)buf[bb + 2] << 8) | buf[bb + 3];
                LOG("[verify] *** MISMATCH @%ld MB: chunk %ld byte %ld (blk-off %ld): expected %02x got %02x ***\n", (c * 32) / 1024, c, i, off, exp, buf[i]);
                LOG("[verify]   landed block index=%lu expected=%ld -> %s\n", landed, g,
                    (landed == (unsigned long)g) ? "SAME block => bit/DMA corruption" :
                    (buf[bb] == 0 && buf[bb + 1] == 0 && buf[bb + 2] == 0 && buf[bb + 3] == 0) ? "ZERO marker => dropped/incomplete transfer" :
                    "DIFFERENT block => wrong-block/LBA-addressing bug");
                failed = 1; break;
            }
        }
    }
    (void)FSClose(rf);
    (void)FSpDelete(&sp);
    if (!failed) { LOG("[verify] *** %ld MB write+readback ALL VERIFIED CLEAN — data path solid at this size; the Finder error is above it (interleave/HFS) ***\n", (kChunks * 32) / 1024);
                   HLOG("[verify] %ld MB ALL VERIFIED CLEAN\n", (kChunks * 32) / 1024); }
    else { LOG("[verify] *** CORRUPTION REPRODUCED IN-APP (see MISMATCH) — it's our DATA PATH, not HFS/Finder-specific ***\n");
           HLOG("[verify] FAILED in-app (see big log)\n"); }
}

/* r59 FOREGROUND REPRODUCTION of the Finder's per-file "cannot be written". The Finder copies MANY files
 * (UT is hundreds); our single-file verify passes, so the trigger is likely the many-files catalog churn,
 * NOT the data path. This creates many files ITSELF (same File Mgr calls the Finder uses) in the FOREGROUND
 * (app not starved, so nothing is hidden), and on the FIRST failure logs the EXACT File Mgr error code — the
 * one datum we've never captured (we only ever saw the Finder's paraphrase). ALL OK => the trigger is buried
 * in the Finder's own copy engine (effectively unfixable from our side). */
static void write_test_manyfiles(short vref)
{
    FSSpec sp; short rf = 0; long n; OSErr e; static unsigned char buf[32768];
    const int kFiles = 800; int f, c, chunks, bad = 0; long fsize; Str63 nm;
    if (vref == 0) { LOG("[manyfiles] no vRefNum — skipping\n"); return; }
    LOG("\n[manyfiles] FOREGROUND repro: create up to %d files (create+write+close each, varying sizes) to churn\n", kFiles);
    LOG("[manyfiles] the catalog like a UT-folder copy. The FIRST failure's ERROR CODE is what we're after.\n");
    HLOG("[r80] REMOVABLE/EJECTABLE: the USB volume now registers as an EJECTABLE removable disk (block driver diskInPlace 8->1 + Eject csCode 7 handled) instead of a fixed internal disk — Finder should offer EJECT + safe removal like USB 1.1. Device type kept 'disk'/writable (no Audio-CD misID). EXPECT: mounts WRITABLE + copies CLEAN + the volume shows Eject + ejecting removes the icon. WATCH: if it comes up read-only/'Audio CD' or won't mount, revert. Post-eject remount = reboot (v2). Throughput/reliability unchanged (20x R / ~13x W ceiling).\n");
    HLOG("[manyfiles] running (foreground repro of the per-file 'cannot be written')...\n");
    for (n = 0; n < 32768; n++) buf[n] = (unsigned char)n;
    for (f = 0; f < kFiles; f++) {
        int L = 0;
        nm[++L] = 'M'; nm[++L] = 'F';
        nm[++L] = (unsigned char)('0' + (f / 100) % 10); nm[++L] = (unsigned char)('0' + (f / 10) % 10); nm[++L] = (unsigned char)('0' + f % 10);
        nm[++L] = '.'; nm[++L] = 'D'; nm[++L] = 'A'; nm[++L] = 'T'; nm[0] = (unsigned char)L;
        (void)FSMakeFSSpec(vref, 2, nm, &sp);
        (void)FSpDelete(&sp);
        e = FSpCreate(&sp, 'ttxt', 'BINA', 0);
        if (e != noErr) { LOG("[manyfiles] *** FILE %d: FSpCreate -> %d ***\n", f, (int)e); HLOG("[manyfiles] FAIL @file %d FSpCreate err=%d\n", f, (int)e); bad = 1; break; }
        e = FSpOpenDF(&sp, fsRdWrPerm, &rf);
        if (e != noErr) { LOG("[manyfiles] *** FILE %d: FSpOpenDF -> %d ***\n", f, (int)e); HLOG("[manyfiles] FAIL @file %d open err=%d\n", f, (int)e); bad = 1; break; }
        fsize = (long)(((f % 8) + 1) * 32768);            /* 32KB..256KB, varying like a real folder */
        chunks = (int)(fsize / 32768);
        for (c = 0; c < chunks; c++) {
            n = 32768; e = FSWrite(rf, &n, (Ptr)buf);
            if (e != noErr || n != 32768) { LOG("[manyfiles] *** FILE %d chunk %d: FSWrite -> %d (n=%ld) ***\n", f, c, (int)e, n); HLOG("[manyfiles] FAIL @file %d write err=%d n=%ld\n", f, (int)e, n); bad = 1; break; }
        }
        if (bad) { (void)FSClose(rf); break; }
        e = FSClose(rf);
        if (e != noErr) { LOG("[manyfiles] *** FILE %d: FSClose -> %d ***\n", f, (int)e); HLOG("[manyfiles] FAIL @file %d close err=%d\n", f, (int)e); bad = 1; break; }
        if ((f % 50) == 0) { LOG("[manyfiles] ... %d files ok\n", f); (void)FlushVol(0, vref); }
    }
    if (!bad) { LOG("[manyfiles] *** ALL %d FILES OK — many-files pattern did NOT reproduce it (trigger is Finder-copy-specific) ***\n", kFiles);
                HLOG("[manyfiles] ALL %d OK — NOT reproduced (trigger is Finder-copy-specific)\n", kFiles); }
    else LOG("[manyfiles] *** REPRODUCED at file %d — the err code above IS the File Mgr error the Finder shows as 'cannot be written' ***\n", f);
    for (f = 0; f < kFiles; f++) {   /* cleanup (best-effort) */
        int L = 0;
        nm[++L] = 'M'; nm[++L] = 'F';
        nm[++L] = (unsigned char)('0' + (f / 100) % 10); nm[++L] = (unsigned char)('0' + (f / 10) % 10); nm[++L] = (unsigned char)('0' + f % 10);
        nm[++L] = '.'; nm[++L] = 'D'; nm[++L] = 'A'; nm[++L] = 'T'; nm[0] = (unsigned char)L;
        (void)FSMakeFSSpec(vref, 2, nm, &sp); (void)FSpDelete(&sp);
    }
    (void)FlushVol(0, vref);
}

int main(void)
{
    RegEntryIter iter; RegEntryID node; Boolean done = false; OSStatus err;
    UInt32 wantClass = 0x000c0320UL, ver = 0;
    int vcbBase, drvBase, vcbNow, drvNow, mounted = 0; long lr = 999;
    unsigned long ticks; int t; int diskDone = 0; short mountedVRef = 0;   /* r25: volume for the write test */

    setvbuf(stdout, NULL, _IONBF, 0);
    log_open("USB Trigger.log");   /* version-neutral name (was the stale "EHCITrigger_r59.log"); pairs with "USB Health.log" */
    hlog_open("USB Health.log");   /* read THIS in SimpleText — NO Finder copy needed this run */
    HLOG("==== USB Health (r59 = FOREGROUND repro of the per-file 'cannot be written'). The [manyfiles] test\n");
    HLOG("     creates ~800 files itself (foreground, fully observable) to churn the catalog like a UT copy.\n");
    HLOG("     READ the [manyfiles] line: 'FAIL @file N ... err=CODE' = REPRODUCED (that CODE is the answer!);\n");
    HLOG("     'ALL 800 OK' = NOT reproduced (trigger is buried in the Finder's copy engine). NO Finder copy needed.\n");
    LOG("==== EHCITrigger r59 — FOREGROUND REPRODUCTION. We keep failing to SEE the per-file 'cannot be written'\n");
    LOG("     because it's above our engine + the app is starved during a bg Finder copy. NEW APPROACH: reproduce\n");
    LOG("     it in the FOREGROUND. Our single-file verify passes, but the Finder copies MANY files, so the trigger\n");
    LOG("     is likely many-files catalog churn. [manyfiles] creates ~800 files itself (same FM calls) and on the\n");
    LOG("     FIRST failure logs the EXACT File Mgr error code (never captured before). NO Finder copy needed — just\n");
    LOG("     run r59 and read the [manyfiles] result in 'USB Health.log'. (64MB verify skipped this run to save time.)\n");
    LOG("==== (r80) REMOVABLE/EJECTABLE (block-driver change, usb_disk.c). The USB volume now registers as EJECTABLE removable media: DrvSts.diskInPlace 8->1 in AddDrive + kStatus(8), and a new Eject control (csCode 7) handler sets diskInPlace=0 so the eject STICKS (FM already unmounted+flushed, so no in-flight I/O to drain). Was a fixed NON-ejectable disk. Device type kept 'disk'/writable (kdgDiskType) to preserve the r37 anti-Audio-CD-misID fix. EXPECT: still mounts WRITABLE + copies CLEAN + the Finder now offers EJECT (menu or drag-to-Trash) + ejecting removes the icon (safe removal, like USB 1.1). WATCH FOR REGRESSIONS: (a) volume read-only or 'Audio CD' => removability reopened the CD-misID => revert; (b) won't mount => diskInPlace value wrong. Post-eject remount = reboot (v2). Throughput (20x R / ~13x W device ceiling) + residue-check reliability UNCHANGED. r79 write sweep also present.\n");
    LOG("     and got to 52MB: the fix DIRECTION is right, the device just GC-stalls >33s sometimes. r54:\n");
    LOG("     (1) watchdog now on the RELIABLE 60Hz Ticks clock @ 60s (was a variable service-counter);\n");
    LOG("     (2) [health] now reports maxStall = the device's worst-case stall in seconds. EXPECT: 64MB\n");
    LOG("     [verify] PASSES + downTimeouts=0; maxStall tells us the real worst pause (tune watchdog to it).\n");
    LOG("     If downTimeouts>0 even at 60s => the device truly hangs (needs BOT reset, not just patience).\n");
    LOG("     ⚠ Use the freshly-reformatted stick. NO Finder needed for the verify.\n");
    LOG("==== (r53) watchdog root-cause fix: flash-GC CSW-NAK false-failure.\n");
    LOG("     CSW status for SECONDS (flash garbage-collection); our 1.6s watchdog fired -> falsely failed the\n");
    LOG("     write (-36) -> Finder 'disk error' + volume corruption ('problem with the disk'). FIX: watchdog\n");
    LOG("     200->4096 ticks (~1.6s -> ~33s); real faults still fail fast (HALTED/XACTERR). ⚠ REFORMAT the\n");
    LOG("     stick CLEAN first (prior runs damaged it) so this is a fair test. EXPECT: the 64MB [verify]\n");
    LOG("     reliably PASSES + [health] downTimeouts=0 downErr=0; then a Finder copy should complete.\n");
    LOG("==== (r52) R4 THROUGHPUT PHASE 1: isrHits vs downDone (interrupt-driven confirmed).\n");
    LOG("     byte-perfect (64MB verified) but throughput is ~0.76 MB/s = SLOWER than USB 1.1, not USB 2.0.\n");
    LOG("     Latency-bound: ~5ms per 3.5KB BOT while the HS transfer needs ~60us => the bus idles ~98%%. This\n");
    LOG("     build adds isrHits to [health]. After the 64MB verify (122k transfers), READ isrHits vs downDone:\n");
    LOG("        isrHits ~= downDone (or x2-3)  => completions ARE interrupt-driven; the stall is chunk-size/\n");
    LOG("           serialization => P2 = bigger chunks off the interrupt path + pipelining.\n");
    LOG("        isrHits << downDone            => completions fall back to the 8ms heartbeat => fix the IRQ\n");
    LOG("           delivery FIRST (huge win, no rework). Just run r52 + copy the log; NO Finder needed.\n");
    LOG("==== (r51) DATA-CORRUPTION DECIDER. r50 proved the engine returns noErr for EVERY I/O\n");
    LOG("     (failSeq=0 downErr=0) yet the Finder fails a large copy 'cannot be written, disk error'; a single\n");
    LOG("     173MB file fails too (~random 30s-2min) => it's the DATA PATH, not metadata-churn. r51 adds a\n");
    LOG("     self-contained [verify] test: write 64MB with per-512-block index markers, close+reopen (defeats\n");
    LOG("     FM cache), read back + verify from the DEVICE. NO Finder needed. READ THE [verify] RESULT:\n");
    LOG("        MISMATCH (landed index=expected)   => bit/DMA corruption in our data path.\n");
    LOG("        MISMATCH (landed index != expected) => wrong-block/LBA-addressing bug.\n");
    LOG("        ALL VERIFIED CLEAN                  => our path is solid; the error is HFS/interleave-level.\n");
    LOG("     Also still logs [health]/CSW-FAIL (engine stays clean). r50 recap follows:\n");
    LOG("==== (r50) CSW-FAILURE DECIDER. r49 ruled out ring(a)+timeout(b): reject=0 hiwater=0\n");
    LOG("     downTimeouts=0 downErr=0, yet the Finder shows 'item X cannot be written, because a disk error'\n");
    LOG("     per-file. Mechanism (code): a nonzero CSW status byte -> gBioResult=-36 -> ioErr to the File Mgr,\n");
    LOG("     which downErr does NOT count and uim23 doesn't log during a copy. r50 surfaces it: [health] now\n");
    LOG("     also prints CSW-FAIL failSeq/failStat/failSig/failLba and logs on EACH new failure. READ:\n");
    LOG("        failSeq>0 + failSig=55534253('USBS') + failStat=1 => the DEVICE rejected the write (real\n");
    LOG("           CHECK CONDITION; we issue no REQUEST SENSE) -> fix = REQUEST SENSE + retry.\n");
    LOG("        failSeq>0 + failSig != 'USBS'                     => our CSW READ is garbage (transport bug).\n");
    LOG("        failSeq==0 (with the dialog seen)                 => the error is ABOVE us (File Mgr/HFS).\n");
    LOG("     TEST: reboot -> run r50 -> mount -> Finder-copy the folder until the disk-error dialog -> hit\n");
    LOG("     Continue/Stop, let it settle -> copy EHCITrigger_r50.log off; READ THE LAST [health] CSW-FAIL LINE.\n\n");

    /* [1] locate the EHCI controller's Name Registry node */
    LOG("CKPT: searching for EHCI node...\n");
    err = RegistryEntryIterateCreate(&iter);
    if (err != noErr) { LOG("iter %ld\n", (long)err); goto park; }
    err = RegistryEntrySearch(&iter, kRegIterDescendants, &node, &done, "class-code", &wantClass, sizeof(wantClass));
    RegistryEntryIterateDispose(&iter);
    if (err != noErr) { LOG("[1] EHCI node NOT FOUND %ld\n", (long)err); goto park; }
    LOG("[1] EHCI node found (class 0c0320)\n");

    /* [1b] what does the node actually look like? (the strings a driver must match, + whether a
     * driver is already attached) and what does the loader see for it BEFORE we attach anything. */
    LOG("[1b] node properties:\n");
    dump_prop(&node, "name");
    dump_prop(&node, "compatible");
    dump_prop(&node, "class-code");
    dump_prop(&node, "device_type");
    dump_prop(&node, "vendor-id");
    dump_prop(&node, "device-id");
    dump_prop(&node, "driver-ptr");
    dump_prop(&node, "driver-descriptor");
    dump_prop(&node, kDriverPropertyName);
    LOG("CKPT: node-property dump done\n");
    probe_candidates(&node, "before-attach");

    /* [2] resolve the loader + status-log accessors */
    LOG("CKPT: resolving Expert symbols...\n");
    resolve_symbols();
    if (!gLoadUIM) { printf("[2] LoadUIMForEntry NOT exported — cannot proceed\n"); goto park; }
    if (USBGetVersion(&ver) == noErr) printf("[2] USBGetVersion=%08lx\n", PL(ver));

    /* [3] verbose status + clear the boot flood so the LoadUIMForEntry narration stands alone */
    USBExpertSetStatusLevel(5);
    if (gResetStatus) { gResetStatus(); USBExpertSetStatusLevel(5); printf("[3] status log cleared\n"); }
    vcbBase = qlen(kVCBQ); drvBase = qlen(kDRVQ);
    printf("[3] baseline: volumes=%d drives=%d\n", vcbBase, drvBase);

    /* [3.5] THE association fix (from RE of FindDriverCandidates in the Mac OS ROM): a prop-based
     * driver needs FOUR node properties, not one. The reader early-outs if "name" is absent, and
     * sets gotPropBasedDriver=TRUE ONLY on a successful read of "driver-descriptor". It reports
     * propBasedDriver from "driver-ptr" (4-byte code pointer) and propBasedDriverSize from the
     * size of "driver,AAPL,MacOS,PowerPC" (the raw PEF). No name/compatible match is needed. */
    {
        RegPropertyValueSize nsz = 0; void *codePtr = (void *)gEHCIPef;
        Boolean hadName = (RegistryPropertyGetSize(&node, "name", &nsz) == noErr);
        OSStatus eN = hadName ? noErr : set_prop(&node, "name", "pciclass,0c0320", 16); /* label only; don't clobber a real name */
        OSStatus eC = set_prop(&node, kDriverPropertyName, gEHCIPef, (RegPropertyValueSize)gEHCIPefLen);  /* size -> propBasedDriverSize */
        OSStatus eD = set_prop(&node, "driver-descriptor", &gDriverDesc, (RegPropertyValueSize)sizeof(gDriverDesc)); /* DECISIVE: sets the flag */
        OSStatus eP = set_prop(&node, "driver-ptr", &codePtr, (RegPropertyValueSize)sizeof(codePtr));      /* 4-byte ptr -> propBasedDriver */
        LOG("[3.5] attach 4 props: name %s | driver,AAPL,MacOS,PowerPC=%ld | driver-descriptor[%lu]=%ld | driver-ptr=%ld\n",
            (hadName ? "(node already had one)" : (eN == noErr ? "(created)" : "(create FAILED)")),
            (long)eC, (unsigned long)sizeof(gDriverDesc), (long)eD, (long)eP);
        if (eD != noErr) { LOG("      driver-descriptor is the decisive property and it FAILED — aborting\n"); goto park; }
    }

    /* [3.6] does the loader NOW see our driver? (the decisive diagnostic — if gotPropDriver is
     * still false, attaching driver,AAPL,MacOS,PowerPC is not how FindDriverCandidates finds one) */
    dump_prop(&node, kDriverPropertyName);
    probe_candidates(&node, "after-attach");

    /* [4] THE trigger: load our ndrv as the UIM plugin + build the Name Registry entry + USBAddBus */
    LOG("CKPT: LoadUIMForEntry ENTER — if the log ENDS on this line, our ndrv loaded and its\n");
    LOG("      Initialize slot (EHCI bring-up + ISR/timer install) crashed. That = fix WORKED.\n");
    lr = gLoadUIM(&node);
    LOG("[4] LoadUIMForEntry -> %ld %s\n", lr, lr == 0 ? "*** OK ***" : "(err/pending)");

    /* [5] pump the USL so the root hub enumerates + any device is processed; watch for a mount */
    LOG("CKPT: pumping ~60s (route=EHCI: connect -> enumerate -> reset -> config -> MOUNT)...\n");
    for (t = 0; t < 900; t++) {
        Delay(4, &ticks);
        SystemTask();                /* task-level time so the disk driver's deferred continuation runs */
        ExpertIdleTask();
        USLPolledProcessDoneQueue();
        /* r22 (BYPASS m2 — THE MOUNT): the moment the UIM publishes its 'Eusb' block-read service
         * (i.e. the self-probe has proven the data path), install our OWN block driver. Its
         * kInitialize scans the MBR + AddDrive's the FAT partition through that service; then we
         * PBMountVol it. Runs exactly once. This is the desktop. */
        if (!diskDone) {
            long gv;
            if (Gestalt('Eusb', &gv) == noErr && gv != 0) {
                short drefNum = 0; OSErr ie; DrvQElPtr el;
                diskDone = 1;
                LOG("\n[disk] 'Eusb' service present @ t=%d — installing USB block driver...\n", t);
                ie = InstallDriverFromMemory((Ptr)gUsbDiskPef, (ByteCount)gUsbDiskPefLen,
                                             "\pUSBDisk", &node, 48, 127, &drefNum);
                LOG("[disk] InstallDriverFromMemory -> %d  refNum=%d\n", (int)ie, (int)drefNum);
                if (ie == noErr && drefNum != 0) {
                    for (el = (DrvQElPtr)kDRVQ->qHead; el; el = (DrvQElPtr)el->qLink) {
                        if (el->dQRefNum == drefNum) {
                            ParamBlockRec pb; unsigned char *pp = (unsigned char *)&pb; long j; OSErr me;
                            for (j = 0; j < (long)sizeof(pb); j++) pp[j] = 0;
                            pb.ioParam.ioVRefNum = el->dQDrive;
                            me = PBMountVol((ParmBlkPtr)&pb);
                            if (me == 0) mountedVRef = pb.ioParam.ioVRefNum;   /* r25: capture for write test */
                            LOG("[disk] drive#=%d PBMountVol -> %d %s (vRef=%d)\n", (int)el->dQDrive, (int)me,
                                (me == 0) ? "*** MOUNTED ***" : "", (int)mountedVRef);
                        }
                    }
                }
            }
        }
        if (!mounted) { vcbNow = qlen(kVCBQ); drvNow = qlen(kDRVQ);
            if (vcbNow > vcbBase || drvNow > drvBase) { mounted = 1;
                LOG("    *** NEW VOLUME/DRIVE (vol %d->%d drv %d->%d) at t=%d — MOUNT! ***\n",
                    vcbBase, vcbNow, drvBase, drvNow, t); } }
        if (t == 250 || t == 500) {   /* periodic snapshot: survives a later hang, shows the enum timeline */
            LOG("CKPT: pump t=%d (vol=%d drv=%d)\n", t, qlen(kVCBQ), qlen(kDRVQ));
            dump_status_log("mid-pump");
            dump_cslog("mid-pump");   /* r24: flush probes seen during the pump before the 256-ring can wrap */
        }
    }

    /* [6] verdict */
    LOG("CKPT: reached verdict (survived LoadUIMForEntry + the pump loop without crashing)\n");
    dump_status_log("after LoadUIMForEntry");
    vcbNow = qlen(kVCBQ); drvNow = qlen(kDRVQ);
    LOG("\n===================== EHCITrigger SUMMARY =====================\n");
    LOG(" LoadUIMForEntry -> %ld  (0=OK; -6xxx=err)\n", lr);
    LOG(" MOUNT CHECK: volumes %d->%d  drives %d->%d\n", vcbBase, vcbNow, drvBase, drvNow);
park:
    /* r84: SKIP the write self-tests + r81 replug test. They run pre-observe with the pump OFF, and the
     * device was dropping off the port during that window (before we could watch). Go straight from mount
     * to the observe loop so the device is freshly connected AND the pump is on when the user pulls. The
     * disabled block is kept verbatim for reference. */
#if 0
    /* r26: quick write self-test first (proven path — guarantees a verdict even if Finder use has
     * issues), THEN hand the desktop to the user. DO NOT close the log or block on getchar. */
    LOG("\n==== r44: FS-level write tests + TIMED throughput/sustained-write probe on the mounted volume ====\n");
    dump_cslog("pre-write");
    write_test_L2(mountedVRef);                        /* FS: tiny file */
    write_test_big(mountedVRef);                       /* FS: 64KB multi-block file */
    write_test_speed(mountedVRef);                     /* r44: TIMED 4MB write+read = KB/s + sustained-write crash probe */
    write_test_verify_big(mountedVRef);                /* r63: RE-ENABLED — byte-perfect proof the NEW dummy-qTD/HW-toggle engine moves 64MB correctly (the rework's data-integrity safety net) */
    write_test_manyfiles(mountedVRef);                 /* r59: FOREGROUND repro of the Finder's per-file "cannot be written" */
    dump_cslog("post-write");

    /* r81: HOT-REPLUG ISOLATION TEST. After the proven mount+read+write path (engine known-good and
     * idle), exercise the async-schedule teardown/rebuild surgery — the reliability-critical core of
     * hot re-mount — 20x WITHOUT a physical pull. A clean pass (20/20, read OK + stable block-0 sig,
     * downErr/downTimeouts unmoved) means the stop/start-the-schedule mechanics are safe; the next
     * step wires that same teardown/rebuild to a real port connect/disconnect event. */
    {
        long gv;
        if (Gestalt('Eusb', &gv) == noErr && gv != 0) {
            UsbSvc *sv = (UsbSvc *)gv;
            if (sv->magic == 0x45555342UL && sv->simReplugFn) {
                unsigned long passed = sv->simReplugFn(20);
                LOG("\n[replug] simReplugFn returned %lu/20 (see the r81 [replug] block for sig/err detail)\n", passed);
            } else {
                LOG("\n[replug] 'Eusb' present but simReplugFn missing — rebuild the ndrv + regen the PEF blob\n");
            }
        } else {
            LOG("\n[replug] no 'Eusb' service — skipping the hot-replug isolation test\n");
        }
    }
#endif  /* r84: end skipped pre-observe self-tests (write tests + r81 replug) */

    /* Make the desktop usable: get the fullscreen console out of the way and run a REAL event loop.
     * The app STAYS ALIVE (memory + interrupt heartbeat valid), so Finder copies to the USB volume
     * work while it sits in the background. My r24/r25 tight Delay/SystemTask loop had NO event
     * pump — that's what killed the windowshade and trapped the desktop; WaitNextEvent fixes it. */
    LOG("\n==== USB DRIVE READY FOR THE FINDER ====\n");
    LOG(">>> This console hides in ~6s. Then CLICK THE DESKTOP to bring the Finder forward and DRAG\n");
    LOG(">>> FILES onto the USB drive. This log's reads=/writes= will climb as the Finder touches it.\n");
    LOG(">>> Done? Finder's Special menu > Restart. Do NOT quit this app (it holds the USL alive).\n");
    printf("\n==== USB DRIVE READY. Console hides in ~6s; then use the Finder. Special > Restart when done. ====\n");
    { unsigned long dtk; short s; for (s = 0; s < 6; s++) { Delay(60, &dtk); SystemTask(); } }  /* ~6s to read */
    gQuiet = 1;                                        /* file-only from here (a printf could re-show the console) */
    { WindowPtr w = FrontWindow(); if (w) HideWindow(w); }   /* uncover the desktop */
    /* r85: ARM the [obs] probe now that we've reached the post-desktop observe loop. Until this call,
     * uim23's [obs] block is gated OFF, so boot enumeration is not logged + the mid-pump insertion runs clean. */
    { long agv; if (Gestalt('Eusb', &agv) == noErr && agv != 0) { UsbSvc *asv = (UsbSvc *)agv;
        if (asv->magic == 0x45555342UL && asv->obsArmFn) asv->obsArmFn(); } }
    /* r82 OBSERVE (hot re-mount Phase 0): re-enable the post-mount USL pump so the hub driver + Family
     * Expert can PROCESS port changes that happen NOW (a real unplug/re-insert), and so uim23 — which
     * already logs PORT EVENTs + every dispatch-slot call (18/19 teardown, 6 create_bulk) — runs again.
     * (r22/r28 dropped these as unnecessary for the DATA path; they ARE needed to service hot-plug. The
     * data path is interrupt-driven and unaffected.) DIAGNOSTIC build: we only OBSERVE what the USL does
     * on a real replug; no teardown/re-mount handlers yet — that is Phase 1+. */
    LOG("\n==== r95 PHASE 1 FIX: interrupt-level takeover-arm (heartbeat SIH fences Apple the moment it parks) ====\n");
    LOG(">>> WAIT ~15s, THEN: Finder-eject -> wait 10s -> PULL -> wait 10s -> REINSERT -> then WAIT 60-90s before snapshot.\n");
    LOG(">>> r94 proved Apple's driver MONOPOLIZES the task loop post-reinsert. r95 arms the takeover from the 8ms\n");
    LOG(">>> heartbeat interrupt (which survives the monopoly): when Apple's bulk probing goes QUIET it fences + arms,\n");
    LOG(">>> and the self-probe takes over the instant the task loop is released. SUCCESS in \"EHCIUIM_init.log\":\n");
    LOG(">>> 'SIH-armed reconnect takeover' -> SELFPROBE COMPLETE (#2) -> 'Eusb' re-published. Watch the 'r95 bulk_xfer' trace.\n");
    {
        EventRecord evt;
        UsbSvc *svc = NULL;
        { long agv; if (Gestalt('Eusb', &agv) == noErr && agv != 0) {   /* resolve ONCE: gSvc is static, addr stable across re-publish */
            UsbSvc *s = (UsbSvc *)agv; if (s->magic == 0x45555342UL) svc = s; } }
        for (;;) {
            (void)WaitNextEvent(everyEvent, &evt, 3L, NULL);   /* short sleep: yield but loop often to capture */
            if (svc && svc->loopFn) svc->loopFn(1);  /* r94: crumb — top of pass, BEFORE ExpertIdleTask */
            ExpertIdleTask();                  /* r82: drive the USB Family Expert (device add/remove decisions) */
            USLPolledProcessDoneQueue();       /* r82: drive the USL done-queue -> calls uim23 (port-event + slot tracing) */
            if (svc && svc->loopFn) svc->loopFn(2);  /* r94: crumb — after USL, JUST BEFORE the self-probe tick */
            if (svc && svc->tickFn) svc->tickFn();   /* r92: drive the self-probe DIRECTLY. USLPolledProcessDoneQueue
                                                      * stops reaching uim23/slot 23 after a reinsert (r91 logs), so the
                                                      * reconnect re-probe (gReprobe path) must be ticked from here — task
                                                      * level (File-Mgr-safe); its bulk xfers run on our own SIH down-engine,
                                                      * not the USL done-queue, so they don't depend on the dropped poll. */
            if (svc && svc->loopFn) svc->loopFn(3);  /* r94: crumb — end of pass, AFTER tickFn (decrements the trace counter) */
            /* dump_cslog is a no-op unless the ring advanced, so this is cheap when idle and flushes the
             * Finder's read/write records to disk right up to a freeze. */
            dump_cslog("live");
            dump_health();       /* r49: THREAD-B decider — logs reject / hiwater / downTimeouts on change */
        }
    }
    return 0;
}
