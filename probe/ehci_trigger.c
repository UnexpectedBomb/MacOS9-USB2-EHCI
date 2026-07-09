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
static int gQuiet = 0;   /* r26: once the console is hidden, log to FILE only — a printf could re-show it */
static void log_open(const char *fname)
{
    FSSpec sp; Str63 pn; int i = 0;
    while (fname[i] && i < 62) { pn[i + 1] = fname[i]; i++; } pn[0] = (unsigned char)i;
    (void)FSMakeFSSpec(0, 0, pn, &sp);      /* default vol/dir = the app's folder */
    (void)FSpDelete(&sp);
    if (FSpCreate(&sp, 'ttxt', 'TEXT', 0) == noErr) (void)FSpOpenDF(&sp, fsRdWrPerm, &gLogRef);
}
static void LOG(const char *fmt, ...)
{
    char buf[512]; long i, n; va_list ap;
    va_start(ap, fmt); vsprintf(buf, fmt, ap); va_end(ap);
    if (!gQuiet) printf("%s", buf);
    if (gLogRef) {
        for (i = 0; buf[i]; i++) if (buf[i] == '\n') buf[i] = '\r';   /* Mac line endings for SimpleText */
        n = i; (void)FSWrite(gLogRef, &n, buf); (void)FlushVol(0, 0);  /* force to disk each line */
    }
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
typedef long (*UsbRwFn)(unsigned long lba, unsigned long count, void *buf);
typedef struct { unsigned long magic; UsbRwFn readFn; UsbRwFn writeFn; unsigned long blkSize, blkCnt; } UsbSvc;

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

/* r44: TIMED multi-MB write+read to the mounted volume = a QUANTITATIVE throughput number (the user asked)
 * AND a crash probe. A large FRONTMOST sequential write is the proven-safe pattern (r28: in-app writes
 * always worked, Finder copies froze), so if 4MB survives here the copy crash is Finder-specific
 * (re-entrancy/interleaving); if it dies ~1MB in (where the Finder copy died) it's a sustained-write
 * engine bug. 32KB FSWrite chunks; TickCount() timing (60.15 Hz); progress flushed every 512KB so a
 * freeze pins the failure point. KB/s = KB*60/ticks; MB/s = KB/s / 1024. */
static void write_test_speed(short vref)
{
    FSSpec sp; short rf = 0; long n; OSErr e; static unsigned char buf[32768];
    const int kChunks = 128;                 /* 128 * 32KB = 4 MB */
    UInt32 t0, ticks, kb, kbps; int c, i, done, vbad = 0;
    if (vref == 0) { LOG("[speed] no mounted-volume vRefNum — skipping\n"); return; }
    LOG("\n[speed] TIMED %d-MB write to the mounted volume (throughput + sustained-write probe)...\n", (kChunks * 32) / 1024);
    (void)FSMakeFSSpec(vref, 2, "\pUSBSPEED.DAT", &sp);
    (void)FSpDelete(&sp);
    e = FSpCreate(&sp, 'ttxt', 'BINA', 0);   if (e != noErr) { LOG("[speed] FSpCreate -> %d\n", (int)e); return; }
    e = FSpOpenDF(&sp, fsRdWrPerm, &rf);     if (e != noErr) { LOG("[speed] open -> %d\n", (int)e); return; }
    for (i = 0; i < 32768; i++) buf[i] = (unsigned char)(i & 0xFF);
    t0 = TickCount();
    for (c = 0; c < kChunks; c++) {
        if ((c & 15) == 0) LOG("[speed] writing... %ld KB\n", (long)c * 32);   /* flushed each 512KB -> pins a freeze */
        n = 32768; e = FSWrite(rf, &n, (Ptr)buf);
        if (e != noErr) { LOG("[speed] *** WRITE FAILED at %ld KB (chunk %d) -> %d ***\n", (long)c * 32, c, (int)e); break; }
    }
    done = c;
    (void)FSClose(rf);
    (void)FlushVol(0, vref);
    ticks = TickCount() - t0; kb = (UInt32)((long)done * 32);
    kbps = ticks ? (kb * 60UL) / ticks : 0;
    LOG("[speed] WROTE %lu KB in %lu ticks (~%lu.%01lu s) = %lu KB/s = %lu.%02lu MB/s\n",
        kb, ticks, ticks / 60, ((ticks % 60) * 10) / 60, kbps, kbps / 1024, ((kbps % 1024) * 100) / 1024);
    if (done < kChunks) { LOG("[speed] (write stopped early = SUSTAINED-WRITE failure at ~%lu KB, in-app)\n", kb); return; }
    rf = 0; e = FSpOpenDF(&sp, fsRdPerm, &rf); if (e != noErr) { LOG("[speed] reopen(ro) -> %d\n", (int)e); return; }
    t0 = TickCount();
    for (c = 0; c < kChunks; c++) { n = 32768; e = FSRead(rf, &n, (Ptr)buf);
        if (e != noErr || n != 32768) { LOG("[speed] read failed chunk %d -> %d (n=%ld)\n", c, (int)e, n); break; }
        for (i = 0; i < 32768; i++) if (buf[i] != (unsigned char)(i & 0xFF)) { vbad++; break; } }   /* r46: verify the 40-block-chunk DMA */
    ticks = TickCount() - t0; (void)FSClose(rf);
    kb = (UInt32)((long)c * 32); kbps = ticks ? (kb * 60UL) / ticks : 0;
    LOG("[speed] READ  %lu KB in %lu ticks (~%lu.%01lu s) = %lu KB/s = %lu.%02lu MB/s\n",
        kb, ticks, ticks / 60, ((ticks % 60) * 10) / 60, kbps, kbps / 1024, ((kbps % 1024) * 100) / 1024);
    LOG("[speed] read-back verify: %s\n", vbad ? "*** DATA MISMATCH — r46 big-chunk DMA bug ***" : "OK");
    (void)FSpDelete(&sp);   /* remove the 4MB test file */
}

int main(void)
{
    RegEntryIter iter; RegEntryID node; Boolean done = false; OSStatus err;
    UInt32 wantClass = 0x000c0320UL, ver = 0;
    int vcbBase, drvBase, vcbNow, drvNow, mounted = 0; long lr = 999;
    unsigned long ticks; int t; int diskDone = 0; short mountedVRef = 0;   /* r25: volume for the write test */

    setvbuf(stdout, NULL, _IONBF, 0);
    log_open("EHCITrigger_r47.log");
    LOG("==== EHCITrigger r47 — REVERT r46's big-chunk speed attempt (it BACKFIRED: 0.14 MB/s, failed mid-\n");
    LOG("     write, choppy Finder). Root cause: a 20KB data copy AT INTERRUPT LEVEL starves the UI, and the\n");
    LOG("     5-page scatter path was unreliable. Back to the PROVEN r45 engine: 7-block (3584B) chunks,\n");
    LOG("     single-page transfers, small interrupt-level copies = reliable ~0.76 MB/s (identical to r45).\n");
    LOG("     Real speedup needs a deeper rework that first moves the copy OFF the interrupt path. SUCCESS =\n");
    LOG("     mounts every boot, [speed] ~0.76 MB/s + 'read-back verify: OK', Finder stays responsive.\n\n");

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
    /* r26: quick write self-test first (proven path — guarantees a verdict even if Finder use has
     * issues), THEN hand the desktop to the user. DO NOT close the log or block on getchar. */
    LOG("\n==== r44: FS-level write tests + TIMED throughput/sustained-write probe on the mounted volume ====\n");
    dump_cslog("pre-write");
    write_test_L2(mountedVRef);                        /* FS: tiny file */
    write_test_big(mountedVRef);                       /* FS: 64KB multi-block file */
    write_test_speed(mountedVRef);                     /* r44: TIMED 4MB write+read = KB/s + sustained-write crash probe */
    dump_cslog("post-write");

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
    {
        EventRecord evt;
        for (;;) {
            (void)WaitNextEvent(everyEvent, &evt, 3L, NULL);   /* short sleep: yield but loop often to capture */
            /* r29: dump EVERY iteration. dump_cslog is a no-op unless the ring advanced, so this is cheap
             * when idle and flushes the Finder's read/write records to disk right up to a freeze. (No
             * ExpertIdleTask/USLPolledProcessDoneQueue — proven unnecessary post-mount, r22/r28.) */
            dump_cslog("live");
        }
    }
    return 0;
}
