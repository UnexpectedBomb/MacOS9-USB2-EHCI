/*
 * usb_disk.c — minimal Mac OS 9 native BLOCK DRIVER for a USB mass-storage device,
 * to BYPASS the 1.1-era Apple USB mounter that refuses to proceed for a HIGH-SPEED
 * device (proven: the same device+driver mount at 1.1; the speed is intrinsic to
 * EHCI enumeration and can't be faked away — see project memory r16-r21).
 *
 * Installed via InstallDriverFromMemory by EHCITrigger AFTER our EHCI UIM publishes
 * a synchronous block-read service (Gestalt 'Eusb', a TVector into the UIM's proven
 * BOT engine). This driver owns NO USB knowledge — every read goes through that
 * service. It:
 *   kInitialize -> Gestalt('Eusb'); read the MBR (block 0); find the FAT partition;
 *                  AddDrive it with a valid DrvSts status prefix.
 *   kRead       -> service->read(partStart + block, count, buffer); IOCommandIsComplete.
 *   kWrite      -> declined (read-only v1).
 *   kStatus     -> kDriveStatus fills a DrvSts; everything else declines (statusErr)
 *                  so the mounter uses safe defaults (DriverGestalt crash fix).
 *
 * Adapted from a proven SATA block driver for the same OS (AddDrive + DrvSts prefix +
 * the IOCommandIsComplete completion contract + DriverGestalt->statusErr).
 * NOTE this header comment is historical: the driver mounts HFS, not FAT/PC-Exchange.
 * See scan_volume() for what it actually accepts.
 */
#include <MacTypes.h>
#include <MacMemory.h>
#include <Devices.h>
#include <Disks.h>
#include <Files.h>
#include <Gestalt.h>
#include <DriverServices.h>
#include <DriverGestalt.h>   /* r37: answer 'devt'/'intf' so we aren't misclaimed as an Audio CD */
#include "usb2_icns_blob.h"  /* r98: the USB 2.0 volume icon ('icns'), returned via kdgMediaIconSuite */
#include <Errors.h>
#include <Events.h>       /* n3: PostEvent(diskEvt) — let the OS mount the volume itself */

#define noErr     0L
#define paramErr  (-50L)

/* IOCommandKind bits (Devices.h): immediate cmds complete by returning from DoDriverIO;
 * sync/async cmds (the File-Mgr block reads PBMountVol issues) complete ONLY when we call
 * IOCommandIsComplete(cmdID, result). */
#define kImmediateIOCommandKind  0x00000004UL

/* r34: DoDriverIO returns this to signal "accepted, completing ASYNC" — the I/O system then waits for
 * our IOCommandIsComplete(cmdID,result) instead of treating the return as completion. Classic value 1;
 * absent from Retro68's headers, so defined here. */
#define kIOBusyStatus  1L

/* ---- byte-accurate DriverDescription with one service (InstallDriverFromMemory
 * requires >= 1 service, per DriverFamilyMatching.h). nameInfoStr is NOT matched here
 * (we install explicitly, not by discovery), so its value is cosmetic. ---- */
typedef struct { UInt8 len; char s[31]; } DStr31;
typedef struct { UInt8 a, b, c, d; } DNumVer;
typedef struct { OSType category, type; DNumVer version; } DServiceInfo;
typedef struct {
    OSType     sig;            /* 'mtej' */
    UInt32     descVersion;
    DStr31     nameInfoStr;
    DNumVer    typeVersion;
    UInt32     driverRuntime;
    DStr31     driverName;
    UInt32     reserved[8];
    UInt32     nServices;
    DServiceInfo service0;
} USBDiskDriverDesc;

USBDiskDriverDesc TheDriverDescription = {
    0x6d74656aUL /* 'mtej' */, 0,
    { 11, "usb,massdsk" }, { 1, 0, 0x80, 0 },
    0x00000003UL,                 /* kDriverIsLoadedUponDiscovery|kDriverIsOpenedUponLoad */
    { 7, "USBDisk" }, { 0,0,0,0,0,0,0,0 }, 1,
    { 0x6e647276UL /*'ndrv'*/, 0x626c6f6bUL /*'blok'*/, { 1, 0, 0x80, 0 } }
};

/* ---- debug log to "USB Disk Log" (task-safe: called only from kInitialize, which runs
 * during InstallDriverFromMemory — NOT re-entrant inside a File Manager mount read). ---- */
static short gLogRef = 0;
static short gLogVol = 0;   /* r49: the log's OWN volume (boot), captured at create */
static void dopen(void)
{
    FSSpec sp;
    if (gLogRef) return;
    (void)FSMakeFSSpec(0, 0, "\pUSB Disk Log", &sp);
    (void)FSpDelete(&sp);
    if (FSpCreate(&sp, 'ttxt', 'TEXT', 0) == noErr) { (void)FSpOpenDF(&sp, fsRdWrPerm, &gLogRef); gLogVol = sp.vRefNum; }
}
static void dput(const char *s)
{
    long n = 0, z = 1;
    if (!gLogRef) dopen();
    if (gLogRef) { ParamBlockRec pbf; while (s[n]) n++; (void)FSWrite(gLogRef, &n, (Ptr)s);
        (void)FSWrite(gLogRef, &z, (Ptr)"\r");
        /* r49 COPY-SAFETY: flush the log's OWN volume (not the default, which moves to the USB stick after
         * mount) so the open log's catalog EOF is committed and copies at full size. See ehci_os.c. */
        pbf.ioParam.ioCompletion = 0; pbf.ioParam.ioRefNum = gLogRef; (void)PBFlushFileSync(&pbf);
        (void)FlushVol(0, gLogVol); }
}
static void dputx(const char *label, unsigned long v)
{
    char b[80]; int i = 0, j; static const char hx[] = "0123456789abcdef";
    while (label[i] && i < 60) { b[i] = label[i]; i++; }
    b[i++] = ' '; b[i++] = '0'; b[i++] = 'x';
    for (j = 28; j >= 0; j -= 4) b[i++] = hx[(v >> j) & 0xF];
    b[i] = 0; dput(b);
}

/* ---- USB block-read service published by the UIM via Gestalt('Eusb') ---- */
/* ★★★★ n19 STEP 4 ABI, MIRRORED FROM ehci_vhub.c. These two declarations MUST STAY IN SYNC.
 * magic2 catches a layout drift, but it CANNOT catch a changed function signature: that is a silent
 * break where the call goes through with the wrong argument list. So `magic` moved to 'EUS2' at the same
 * time, and fetch_svc below refuses to bind on a mismatch. In practice this means the ROM and this block
 * driver (which the ROM embeds) and the activator are one release, installed together.
 *
 * EUSB_MAX_DEV is pinned at 4 and is INDEPENDENT of the driver's USB_MAX_DEV, so raising that later does
 * not move these offsets and costs no further ABI break. */
#define EUSB_MAX_DEV 4
#define EUSB_MAGIC   0x45555332UL      /* 'EUS2' */
typedef long (*usb_rw_fn)(int dev, UInt32 lba, UInt32 count, void *buf);   /* >=0 = blocks done; <0 = error */
typedef long (*usb_submit_fn)(int dev, IOCommandID cmdID, UInt32 lba, UInt32 count, void *buf,
                              int isWrite, long *actCount);
typedef struct {
    UInt32 magic; usb_rw_fn readFn; usb_rw_fn writeFn;
    UInt32 blkSize[EUSB_MAX_DEV], blkCnt[EUSB_MAX_DEV];   /* per-device geometry; blkCnt 0 = no device */
    UInt8  present[EUSB_MAX_DEV];
    UInt32 devCount;
    usb_submit_fn submitFn;
    void *healthFn, *toStateFn, *simReplugFn, *obsArmFn, *tickFn, *loopFn, *quitFn;
    void (*drainFn)(void);          /* n9: flush the UIM's interrupt-level log ring, from TASK level */
    void (*ejectFn)(int dev);       /* n24: post Apple's "You may now remove the cartridge" alert, naming
                                     * the drive at THIS slot. Signature-only change; the field stays at
                                     * offset 88 so no offsets move and the n4g activator (whose view of
                                     * gSvc ends at quitFn, offset 80) is unaffected and never calls it.
                                     * This block driver is embedded in the same ROM as the UIM, so the two
                                     * ends of this call can never come from different builds. */
    UInt32 magic2;                  /* 'EUS2' again — layout guard */
} UsbSvc;
static UsbSvc *gSvc = 0;

static int fetch_svc(void)
{
    long v;
    if (gSvc) return 1;
    if (Gestalt('Eusb', &v) != noErr || v == 0) return 0;
    gSvc = (UsbSvc *)v;
    /* ★ n19: verify BOTH ends of the struct. magic alone would accept a driver whose layout changed
     * beneath us; magic2 sits after the last field, so matching both means the block driver and the
     * UIM agree on the whole shape. A mismatch means a ROM and block driver from different builds:
     * refuse to bind rather than call through with the wrong argument list. */
    if (gSvc->magic != EUSB_MAGIC || gSvc->magic2 != EUSB_MAGIC) {
        dput("!! n19: 'Eusb' ABI mismatch - ROM and block driver are from different builds; not binding");
        gSvc = 0; return 0;
    }
    return 1;
}
/* Read `count` 512-byte blocks at absolute LBA `lba` into buf; loops the service's
 * per-call cap. Returns 1 on success, 0 on failure. */
static int svc_read(int dv, UInt32 lba, UInt32 count, void *buf)
{
    UInt8 *p = (UInt8 *)buf;
    if (!fetch_svc()) return 0;
    while (count > 0) {
        UInt32 n = (count > 7) ? 7 : count;
        long r = gSvc->readFn(dv, lba, n, p);     /* n19 step 3: the caller's device */
        if (r <= 0) return 0;
        lba += (UInt32)r; p += (UInt32)r * 512UL; count -= (UInt32)r;
    }
    return 1;
}
/* r34: svc_write removed — kWrite is now ASYNC via the 'Eusb' submitFn (see disk_submit). svc_read
 * stays: kInitialize's MBR scan reads synchronously at install time (task-level, one-shot, safe). */

/* ---- drive state ---- */
/* ★★★ n19 STEP 2: PER-SLOT DRIVE STATE. One driver instance serves N drives, which is the native OS 9
 * idiom for a multi-drive controller: AddDrive once per device, then route each call by the drive number
 * the Device Manager already puts in ioVRefNum. gRefNum stays single, there is one driver.
 *
 * ⚠ gPartStart is the dangerous one. It is the base of EVERY LBA this driver computes, so using the wrong
 * slot's base reads or writes the wrong region of the wrong disk, silently. Same class of harm as the n17
 * address collision. Every use must be reached through the slot the call is FOR. */
static short  gRefNum = 0;
static short  gDriveNumS[EUSB_MAX_DEV];
#define gDriveNum gDriveNumS[0]
static UInt32 gPartStartS[EUSB_MAX_DEV], gPartCountS[EUSB_MAX_DEV];
/* n19: gPartStart/gPartCount are now slot-indexed above. These macros keep slot 0's meaning for the
 * paths that are still single-device, and every one of them is a marker for what step 3 revisits. */
#define gPartStart gPartStartS[0]
#define gPartCount gPartCountS[0]
/* n4c HOT RE-INSERT: keep the drive-queue element and its DrvSts status prefix so a re-insert can update
 * the geometry and the media-present flag IN PLACE. The queue entry must survive a pull — AddDrive is a
 * once-per-driver act, exactly as for a floppy or Zip drive. */
static DrvQElPtr gDrvQElS[EUSB_MAX_DEV];
#define gDrvQEl gDrvQElS[0]
static DrvSts   *gDrvStsS[EUSB_MAX_DEV];
#define gDrvSts gDrvStsS[0]
/* n4c: private Control csCode our EHCI driver uses to report media state. Far outside Apple's range (the
 * CD-ROM codes this driver already sees top out at 125), and guarded by a magic in csParam so a stray
 * control can never fake a disk-inserted event. csParam[2] != 0 = arrived, 0 = gone. */
#define kCsUsbDiskMedia 20481
#define kUsbDiskMagic   0x45484349UL   /* 'EHCI' */
/* r80: media-present / ejectability state. 1 = ejectable disk in drive (removable, like USB 1.1); 0 = ejected
 * (no media). Reported via DrvSts.diskInPlace in AddDrive + kStatus(8). The Eject control (csCode 7) sets it 0
 * so the eject STICKS (else kStatus still says "disk present" and the Finder immediately remounts). Was a
 * hardwired 8 (= NONEJECTABLE fixed disk, inherited from the eSATA driver) — wrong for a removable USB stick. */
static char   gDiskInPlaceS[EUSB_MAX_DEV];
#define gDiskInPlace gDiskInPlaceS[0]

/* ★ n19 STEP 2: map a Device Manager call to a device slot.
 * Every Read/Write/Status/Control arrives with the drive it targets in ioVRefNum (this driver already
 * logged it as 'which drive/vol the call targets'), so one driver instance can serve N drives. Returns
 * the slot, or -1 if the drive number is not ours.
 *
 * Step 3 routes through this. It exists now, unused on the data path, so the mapping is in place and
 * reviewable before anything depends on it, rather than being written in the same change that starts
 * computing LBAs from it. */
static int slot_for_drive(short dnum)
{
    int i;
    if (dnum == 0) return -1;
    for (i = 0; i < EUSB_MAX_DEV; i++) if (gDriveNumS[i] == dnum) return i;
    return -1;
}
/* n19: slot 0 keeps the historical default of 'media present' so a single-drive session behaves exactly
 * as before; slots 1.. start empty and are filled by AddDrive when step 3 exposes them. */
static void slots_init_once(void)
{
    static int done = 0; int i;
    if (done) return; done = 1;
    for (i = 0; i < EUSB_MAX_DEV; i++) {
        gDriveNumS[i] = 0; gDrvQElS[i] = 0; gDrvStsS[i] = 0;
        gPartStartS[i] = 0; gPartCountS[i] = 0;
        gDiskInPlaceS[i] = (char)(i == 0 ? 1 : 0);
    }
}

/* ---- r24 INSTRUMENTATION: wrap-around ring of every Status/Control call the OS issues to this
 * drive. Pure memory writes only — safe at File-Mgr / interrupt time (unlike the dput() file I/O,
 * which is why that stays confined to kInitialize). The point: Foreign File Access / Audio CD
 * Access probes the drive AFTER PBMountVol returns — exactly the calls the r23 mount-time logs
 * never captured. Published via Gestalt('Ucsl') so the trigger app can dump it LIVE while the
 * volume is browsed. Behavior is otherwise byte-for-byte r23 (writes still enabled) so the
 * 'Audio CD 1' misID still reproduces. ---- */
#define kCsCap 512                                    /* power of 2: index = count & (kCsCap-1) */
typedef struct { short kind; short csCode; short ioVRefNum; short pad; long p0, p1; } CsRec; /* 16B */
/* r26: nReads/nWrites count File-Mgr I/O to this drive so the log proves a Finder copy actually
 * reaches us (write climbing) vs failing upstream in PC Exchange. Header is now 5 longs (20B). */
typedef struct { UInt32 magic; UInt32 count; UInt32 cap; UInt32 nReads; UInt32 nWrites; CsRec recs[kCsCap]; } CsLog;
static CsLog gCsLog;
/* v25: control-plane trace — EVERY DoDriverIO code + our return err (aux=csCode for ctl/status), published
 * via Gestalt('Ucs2') so the UIM can dump it. Reveals kKillIO/Open/Close + any declined Status/Control near
 * the Finder's abort — the last driver-side signal, since the abort reason isn't a data Prime we receive. */
#define kDioCap 128
typedef struct { short code; short err; long aux; } DioRec;   /* 8B */
typedef struct { UInt32 magic; UInt32 count; DioRec recs[kDioCap]; } DioLog;
static DioLog gDioLog;
static void diolog(short code, short err, long aux)
{
    DioRec *r = &gDioLog.recs[gDioLog.count & (kDioCap - 1)];
    r->code = code; r->err = err; r->aux = aux;
    gDioLog.count++;
}
static void cslog(short kind, ParmBlkPtr pb)          /* kind: 1 = Status, 2 = Control */
{
    CsRec *r = &gCsLog.recs[gCsLog.count & (kCsCap - 1)];
    r->kind      = kind;
    r->csCode    = pb->cntrlParam.csCode;
    r->ioVRefNum = pb->cntrlParam.ioVRefNum;          /* which drive/vol the call targets */
    r->pad       = 0;
    /* csParam[0..1] as one big-endian long = DriverGestalt/CD-ROM sub-selector (e.g. 'devt') */
    r->p0 = ((long)(unsigned short)pb->cntrlParam.csParam[0] << 16) | (unsigned short)pb->cntrlParam.csParam[1];
    r->p1 = ((long)(unsigned short)pb->cntrlParam.csParam[2] << 16) | (unsigned short)pb->cntrlParam.csParam[3];
    gCsLog.count++;                                   /* free-running; ring index masks it */
}
/* r29: record each File-Mgr READ/WRITE — its IOCommandKind (1=sync/2=async/4=immediate), the
 * partition-relative start block, and the block count. This reveals whether the Finder copy that
 * freezes is issuing ASYNC writes (our driver blocks synchronously — illegal at async/interrupt
 * time -> hard freeze) and whether reads are interleaved with writes (re-entrancy into our engine). */
/* v42: partition-relative start BLOCK from the File Mgr's position, WIDE-AWARE. For volumes >4GB the FM sets
 * kUseWidePositioning in ioPosMode and puts the true 64-bit byte offset in XIOParam.ioWPosOffset (it only does
 * this because v42's disk_status now answers kdgWide=true). Narrow requests keep the 32-bit ioPosOffset with
 * the cast-UInt32-FIRST unsigned divide (the v41 2GB fix). block = byteOffset/512, computed as (hi<<23)|(lo>>9)
 * so it needs NO 64-bit-divide intrinsic and is exact for volumes <2TB (block fits UInt32). */
static UInt32 pb_block(ParmBlkPtr pb)
{
    if (pb->ioParam.ioPosMode & kUseWidePositioning) {
        XIOParam *x = (XIOParam *)pb;
        return ((UInt32)x->ioWPosOffset.lo >> 9) | ((UInt32)x->ioWPosOffset.hi << 23);
    }
    return ((UInt32)pb->ioParam.ioPosOffset) / 512UL;
}
static void iolog(short kind, unsigned long iokind, ParmBlkPtr pb)  /* kind: 3=read, 4=write */
{
    CsRec *r = &gCsLog.recs[gCsLog.count & (kCsCap - 1)];
    r->kind      = kind;
    r->csCode    = (short)((UInt32)pb->ioParam.ioReqCount / 512UL);   /* block count */
    r->ioVRefNum = pb->ioParam.ioVRefNum;
    r->pad       = 0;
    r->p0 = (long)pb_block(pb);                                       /* v42: partition-relative start block (wide-aware) */
    r->p1 = (long)iokind;                                             /* IOCommandKind bits */
    gCsLog.count++;
    if (kind == 3) gCsLog.nReads++; else gCsLog.nWrites++;
}

static short pick_drive_num(void)
{
    QHdrPtr q = GetDrvQHdr();
    DrvQElPtr el; short mx = 4;                 /* drive #s <=4 are historically reserved */
    for (el = (DrvQElPtr)(q ? q->qHead : 0); el; el = (DrvQElPtr)el->qLink)
        if (el->dQDrive > mx) mx = el->dQDrive;
    return (short)(mx + 1);
}

/* kInitialize: scan the Apple Partition Map, find the Apple_HFS partition, AddDrive it. r38: PIVOTED
 * from MBR/FAT to APM/HFS. HFS uses OS 9's BUILT-IN mounter (NOT Foreign File Access), which sidesteps
 * the intermittent audio-CD misID entirely — the sibling eSATA project proved native HFS = clean stable
 * R/W (v50) where FAT/PC-Exchange was a dead end. Tradeoff: the stick is Mac-only (format via Drive
 * Setup: Mac OS Extended + Apple Partition Map). Reads go through the SAME 'Eusb' BOT service. APM layout:
 * blk0='ER' Driver Descriptor Record; blk1..N='PM' partition entries (pmMapBlkCnt @+4, pmPyPartStart @+8,
 * pmPartBlkCnt @+12, type string @+48). All fields big-endian. */
/* n4c: the volume scan, factored out of scan_and_add so a HOT RE-INSERT can re-run it. Sets
 * gPartStart/gPartCount from whatever is in the drive NOW — so swapping in a different stick with
 * different geometry mounts correctly, which is most of the point of hot-plug. */
static OSErr scan_volume(int dv)
{
    static UInt8 blk[512];
    UInt32 e, mapCnt, bs = 0, bc = 0;
    int isAPM;

    if (!fetch_svc())         { dput("  service 'Eusb' NOT present"); return ioErr; }
    if (!svc_read(dv, 0, 1, blk)) { dput("  block0 read FAILED"); return ioErr; }
    dputx("  blk0[0..3]", ((UInt32)blk[0]<<24)|((UInt32)blk[1]<<16)|((UInt32)blk[2]<<8)|blk[3]);   /* r40: see the layout */
    isAPM = (blk[0] == 0x45 && blk[1] == 0x52);                      /* 'ER' Driver Descriptor Record */

    if (isAPM) {
        /* --- Apple Partition Map: block1 'PM' pmMapBlkCnt; scan entries for the Apple_HFS partition --- */
        if (!svc_read(dv, 1, 1, blk) || !(blk[0] == 0x50 && blk[1] == 0x4D)) {   /* 'PM' first map entry */
            dput("  APM: no 'PM' partition map at block 1"); return ioErr;
        }
        mapCnt = ((UInt32)blk[4] << 24) | ((UInt32)blk[5] << 16) | ((UInt32)blk[6] << 8) | blk[7];  /* pmMapBlkCnt */
        if (mapCnt > 63) mapCnt = 63;
        dputx("  APM map entries", mapCnt);
        for (e = 1; e <= mapCnt; e++) {
            if (!svc_read(dv, e, 1, blk) || blk[0] != 0x50 || blk[1] != 0x4D) break;
            /* partition type string @ +48; match "Apple_HFS" (covers HFS + HFS+) */
            if (!(blk[48]=='A'&&blk[49]=='p'&&blk[50]=='p'&&blk[51]=='l'&&blk[52]=='e'&&
                  blk[53]=='_'&&blk[54]=='H'&&blk[55]=='F'&&blk[56]=='S')) continue;
            bs = ((UInt32)blk[8]  << 24) | ((UInt32)blk[9]  << 16) | ((UInt32)blk[10] << 8) | blk[11];  /* pmPyPartStart */
            bc = ((UInt32)blk[12] << 24) | ((UInt32)blk[13] << 16) | ((UInt32)blk[14] << 8) | blk[15];  /* pmPartBlkCnt */
            dputx("  -> Apple_HFS entry", e); dputx("    start", bs); dputx("    count", bc);
            break;   /* first HFS partition = the volume */
        }
        if (bc == 0) { dput("  APM: no Apple_HFS partition found"); return ioErr; }
    } else {
        /* --- r40: partitionless (superfloppy) HFS — no partition map; the HFS volume IS the whole device
         * (block 0 = zeroed boot blocks, which is why the APM 'ER' check missed). Confirm via the volume
         * header at block 2 ('BD'=0x4244 HFS, 'H+'=0x482B HFS+) and mount the whole device (partStart=0).
         * This is what a USB stick formatted as a single HFS volume (no APM) looks like — the r39 case. --- */
        if (!svc_read(dv, 2, 1, blk)) { dput("  non-APM + block2 read FAILED"); return ioErr; }
        dputx("  blk2[0..1] (4244=HFS 482B=HFS+)", ((UInt32)blk[0]<<8)|blk[1]);
        if ((blk[0]==0x42 && blk[1]==0x44) || (blk[0]==0x48 && blk[1]==0x2B)) {
            bs = 0; bc = gSvc->blkCnt[dv];                            /* whole device is the volume */
            dput("  -> partitionless HFS (volume header @ block 2): whole-device volume");
            dputx("    count(blkCnt)", bc);
        } else {
            dput("  no APM 'ER' at blk0 and no HFS header at blk2 - unrecognized (reformat as Mac OS Extended)");
            return ioErr;
        }
    }
    if (bc == 0) { dput("  volume block count is 0 - cannot AddDrive"); return ioErr; }
    /* ★ n19 step 3: THE dangerous assignment. gPartStart is the base of every LBA this driver computes,
     * so it must land on the slot this scan was FOR. A wrong index here writes to the wrong region of
     * the wrong disk, silently, which is the failure mode the audit called out. */
    gPartStartS[dv] = bs; gPartCountS[dv] = bc;

    /* r38 diagnostic (crash-free, pre-mount): dump the HFS Master Directory Block (partition block 2)
     * to confirm a real HFS/HFS+ volume reads clean through 'Eusb' before the built-in mounter runs.
     * 'BD'(0x4244)=HFS, 'H+'(0x482B)=HFS+. */
    if (svc_read(dv, gPartStartS[dv] + 2, 1, blk))   /* n19: THIS slot's partition base */
        dputx("  MDB sig @part+2 (4244=HFS 482B=HFS+)", ((UInt32)blk[0] << 8) | blk[1]);
    return noErr;
}

/* kInitialize (ONCE per driver load): scan the volume, then AddDrive it and announce it. r38: PIVOTED
 * from MBR/FAT to APM/HFS. HFS uses OS 9's BUILT-IN mounter (NOT Foreign File Access), which sidesteps
 * the intermittent audio-CD misID entirely. n4c: the AddDrive here happens exactly once — every LATER
 * insertion goes through media_arrived() below, which re-scans and re-announces the SAME drive number. */
/* ★ n19 step 3: add ONE drive, for device slot dv. Called once per device. The driver itself is
 * installed only once (gRefNum), which is the native OS 9 shape for a multi-drive controller. */
static OSErr scan_and_add(short refNum, int dv)
{
    OSErr e;

    slots_init_once();          /* n19: per-slot drive state, before anything touches it */
    gRefNum = refNum;
    /* r24: arm + publish the Status/Control ring before we AddDrive (probes captured live). */
    gCsLog.magic = 0x5563736cUL;   /* 'Ucsl' */
    gCsLog.count = 0;
    gCsLog.cap   = kCsCap;
    gCsLog.nReads = 0;
    gCsLog.nWrites = 0;
    (void)NewGestaltValue('Ucsl', (long)&gCsLog);
    gDioLog.magic = 0x44696f4cUL;   /* 'DioL' — v25 control-plane trace */
    gDioLog.count = 0;
    (void)NewGestaltValue('Ucs2', (long)&gDioLog);
    dput("=== USB disk driver v48 (n10: posts Apple's eject alert via the UIM; n9: drains the UIM log ring from DoDriverIO so we can still see logs after the activator quits; n6e: reads/writes now fail with offLinErr once the media is gone, so an un-ejected pull reports REMOVED not DAMAGED; n4c hot re-insert — AddDrive once, then re-scan + re-announce per insertion; v41 2GB + v42 >4GB wide fixes): scanning for a mountable HFS volume (APM or partitionless) via 'Eusb' ===");
    e = scan_volume(dv);
    if (e != noErr) return e;

    /* AddDrive with the DrvSts STATUS PREFIX valid — the anti-hang fix: the mounter reads
     * diskInPlace/installed from the 6 bytes BEFORE qLink; zeros there => "offline" => hang. */
    {
        Ptr raw = NewPtrSysClear(sizeof(DrvSts) + 8);
        DrvSts *ds; DrvQElPtr dq; short dnum;
        if (!raw) { dput("  NewPtrSysClear failed"); return memFullErr; }
        ds = (DrvSts *)raw;
        ds->track       = 0;
        ds->writeProt   = 0;             /* write-enabled */
        ds->diskInPlace = gDiskInPlaceS[dv];  /* r80: 1 = EJECTABLE disk present (was 8 = nonejectable/fixed) */
        ds->installed   = 1;             /* drive installed */
        ds->sides       = 0;
        dq = (DrvQElPtr)&ds->qLink;
        dq->qType    = 1;                /* dQDrvSz/dQDrvSz2 valid */
        dq->dQDrvSz  = (unsigned short)(gPartCountS[dv] & 0xFFFF);
        dq->dQDrvSz2 = (unsigned short)(gPartCountS[dv] >> 16);
        dnum = pick_drive_num();
        AddDrive(refNum, dnum, dq);
        gDriveNumS[dv] = dnum;                      /* n19 step 3: this device's own drive number */
        gDrvQElS[dv] = dq; gDrvStsS[dv] = ds;       /* n4c: keep them — a re-insert updates these IN PLACE */
        gDiskInPlaceS[dv] = 1;
        dputx("  AddDrive Apple_HFS drive#", (unsigned long)dnum);
        dputx("    slot", (unsigned long)dv);
        dputx("    partStart", gPartStartS[dv]); dputx("    partCount", gPartCountS[dv]);
        /* ★ n3 NATIVE MOUNT: tell the OS a disk arrived and let IT mount the volume, exactly as a floppy or
         * Zip driver does — instead of an application calling PBMountVol for us (which is why a launcher was
         * needed at all). diskEvt's message is: high word = result code (0 = no error), low word = drive
         * number. The Finder/File Manager picks this up and mounts the volume itself, so a drive inserted
         * with NO application running still appears on the desktop. */
        PostEvent(diskEvt, (long)(unsigned short)dnum);
        dput("  posted diskEvt — the OS should now mount the volume itself (no app needed)");
    }

    return noErr;
}

/* ★ n4c HOT RE-INSERT — the half that was missing. The installed driver AND its drive-queue entry both
 * PERSIST across a pull, so a re-insert must NOT re-install anything and must NOT AddDrive a second time
 * (that would leak a drive number per insertion). The standard removable-media contract is: AddDrive once,
 * then per insertion re-scan the media, refresh the geometry in place, set diskInPlace, and post a fresh
 * diskEvt — the mirror image of the Eject control (csCode 7) that already clears diskInPlace.
 * Called at TASK level only (the EHCI driver issues it from selfprobe_tick/uim23), which is what makes the
 * dput() file I/O and the synchronous svc_read here safe — the same context kInitialize itself runs in. */
/* ★ n19 step 3: media arrival for ONE device slot. If that slot has no drive yet this is its FIRST
 * arrival, so add one; otherwise re-scan and re-announce the same drive number, which is the removable
 * contract (AddDrive once, announce per insertion, never leak a drive number). */
static OSErr media_arrived(int dv)
{
    OSErr e;
    if (dv < 0 || dv >= EUSB_MAX_DEV) return paramErr;
    if (gRefNum == 0) { dput("!! n19 media-arrived before the driver was installed - ignoring"); return notOpenErr; }
    if (gDriveNumS[dv] == 0 || gDrvQElS[dv] == 0) {
        dputx("=== n19: FIRST arrival for slot - adding a drive for it; slot", (unsigned long)dv);
        return scan_and_add(gRefNum, dv);
    }
    dputx("=== n4c: media re-arrived - re-scan and re-announce, no AddDrive; slot", (unsigned long)dv);
    e = scan_volume(dv);                     /* re-read: the stick may have been SWAPPED for a different one */
    if (e != noErr) { dput("  re-scan FAILED - not announcing (drive left empty)"); gDiskInPlaceS[dv] = 0;
                      if (gDrvStsS[dv]) gDrvStsS[dv]->diskInPlace = 0; return e; }
    gDrvQElS[dv]->dQDrvSz  = (unsigned short)(gPartCountS[dv] & 0xFFFF);   /* refresh geometry in place */
    gDrvQElS[dv]->dQDrvSz2 = (unsigned short)(gPartCountS[dv] >> 16);
    gDiskInPlaceS[dv] = 1;
    if (gDrvStsS[dv]) gDrvStsS[dv]->diskInPlace = 1;   /* the mounter reads this prefix, not just kStatus */
    PostEvent(diskEvt, (long)(unsigned short)gDriveNumS[dv]);
    dputx("  posted diskEvt for drive#", (unsigned long)gDriveNumS[dv]);
    dputx("    partStart", gPartStartS[dv]); dputx("    partCount", gPartCountS[dv]);
    return noErr;
}
/* n4c: the device was physically pulled. Mark the media gone so kStatus reports diskInPlace = 0 instead of
 * leaving a stale "disk present" answer behind a volume that is no longer there. Pure flag writes — safe to
 * reach from anywhere the Control can be issued. */
static void media_gone(int dv)
{
    if (dv < 0 || dv >= EUSB_MAX_DEV) return;
    gDiskInPlaceS[dv] = 0;
    if (gDrvStsS[dv]) gDrvStsS[dv]->diskInPlace = 0;
    dputx("=== n4c: media gone (device pulled) - drive marked empty; slot", (unsigned long)dv);
}

/* r34: kRead/kWrite are now ASYNC. disk_submit hands the request to the UIM's 'Eusb' submitFn (which
 * runs the BOT on the heartbeat and completes cmdID via IOCommandIsComplete) and returns immediately —
 * NO blocking, so the File Manager is never held and the shared engine is never re-entered (that was
 * the Finder freeze). Partition-relative LBA. Returns: 0=submitted (caller returns kIOBusyStatus),
 * 1=zero-length (caller completes noErr), -1=no service/queue full (caller completes ioErr). */
static long disk_submit(IOCommandID cmdID, ParmBlkPtr pb, int isWrite)
{
    UInt32 lba, nblk;
    if (!fetch_svc() || !gSvc->submitFn) return -1;
    /* v41 fixed the 32-bit signed->unsigned divide (the 2GB wrap → 0xffc00000 garbage LBA the v40 probe caught).
     * v42 removes the 4GB ceiling: pb_block() reads the 64-bit XIOParam.ioWPosOffset when the FM sets
     * kUseWidePositioning (which it now does because disk_status answers kdgWide=true). Correct across the
     * whole volume for USB sticks up to 2TB. */
    /* ★★★ n19 step 3: ROUTE BY DRIVE. This is the assignment the audit flagged as the one that can
     * silently read or write the wrong region of the wrong disk, so it is deliberate and explicit:
     * the LBA base comes from the slot the Device Manager named in ioVRefNum, and if that drive is
     * not ours we refuse rather than defaulting to slot 0 and corrupting somebody. */
    {
        int dv = slot_for_drive(pb->ioParam.ioVRefNum);
        if (dv < 0) { dput("!! n19 submit for a drive that is not ours - refusing"); return -1; }
        if (!gDiskInPlaceS[dv]) return -1;         /* media gone: fail fast rather than read absent media */
        lba  = gPartStartS[dv] + pb_block(pb);
        nblk = (UInt32)(pb->ioParam.ioReqCount / 512);
        pb->ioParam.ioActCount = 0;                /* UIM sets the real count on completion */
        return gSvc->submitFn(dv, cmdID, lba, nblk, pb->ioParam.ioBuffer, isWrite, &pb->ioParam.ioActCount);
    }
}

/* kStatus: answer kDriveStatus(8) with a valid DrvSts and DriverGestalt device-type/interface;
 * decline everything else with statusErr so the File Manager uses safe defaults. */
static OSErr disk_status(ParmBlkPtr pb)
{
    cslog(1, pb);                        /* r24: record every Status csCode (before we decide) */
    if (pb->cntrlParam.csCode == 8) {
        DrvSts *ds = (DrvSts *)&pb->cntrlParam.csParam[0];
        int k; for (k = 0; k < 11; k++) pb->cntrlParam.csParam[k] = 0;
        ds->track       = 0;
        ds->writeProt   = 0;             /* r23: write-enabled */
        /* ★ n19 step 3: answer for the drive the caller named. Getting this wrong tells the File
         * Manager that a device with no media is present, or the reverse. */
        { int dv = slot_for_drive(pb->cntrlParam.ioVRefNum); if (dv < 0) dv = 0;
          ds->diskInPlace = gDiskInPlaceS[dv]; }  /* r80: ejectable/media-present (1 present, 0 after Eject) */
        ds->installed   = 1;
        ds->sides       = 0;
        return noErr;
    }
    /* r37 AUDIO-CD-MISID FIX: r36 proved a clean HIGH-SPEED mount (self-probe read block-0 0x55AA,
     * raw WRITE(10) verified) still came up READ-ONLY as "Audio CD 1" — File-Mgr writes to it failed
     * with wPrErr (-44). Mechanism (CSLOG): the OS probed DriverGestalt('devt') = kdgDeviceType on our
     * drive and we DECLINED (statusErr), so the CD Foreign File Access plugin claimed our FAT volume.
     * Answering 'devt'=hard disk (+ 'intf'=USB) asserts a writable fixed disk so PC Exchange claims it.
     * This is the authoritative device-type gate — earlier + stronger than r30's Control 104/125
     * rejection (kept below as defence in depth). SAFETY: only answer selectors we can FULLY fill;
     * decline the rest with statusErr. Returning noErr for a selector whose response is a pointer/
     * handle (icons, media info) and leaving it unset was the original "DriverGestalt crash". */
    if (pb->cntrlParam.csCode == kDriverGestaltCode) {          /* 43 */
        DriverGestaltParam *dg = (DriverGestaltParam *)pb;
        if (dg->driverGestaltSelector == kdgDeviceType) { dg->driverGestaltResponse = kdgDiskType; return noErr; }
        if (dg->driverGestaltSelector == kdgInterface)  { dg->driverGestaltResponse = kdgUSBIntf;  return noErr; }
        /* v42: advertise WIDE positioning so the File Mgr addresses >4GB via the 64-bit XIOParam.ioWPosOffset
         * (a boolean response, fully filled — safe, no half-filled-pointer trap). Without this the FM uses the
         * 32-bit ioPosOffset, which wraps past 4GB -> writes to wrong blocks -> the silent corruption (-199). */
        if (dg->driverGestaltSelector == kdgWide)       { dg->driverGestaltResponse = 1;           return noErr; }
        /* r98: our custom USB 2.0 volume icon. Return a pointer to the embedded 'icns' IconFamily for the
         * media icon (kdgMediaIconSuite, formerly csCode 22) AND the physical-drive icon (kdgPhysDriveIconSuite,
         * formerly csCode 21). This response IS fully filled (a valid pointer), so it does NOT hit the r37
         * "half-filled pointer response = crash" trap. Only OUR (2.0) volumes go through this driver, so USB
         * 1.1 volumes keep Apple's stock icon — the icon itself signals "mounted at Hi-Speed". */
        if (dg->driverGestaltSelector == kdgMediaIconSuite ||
            dg->driverGestaltSelector == kdgPhysDriveIconSuite) {
            dg->driverGestaltResponse = (UInt32)(unsigned long)gUsb2Icns;
            return noErr;
        }
        return statusErr;                /* every other selector: safe default (never a half-filled response) */
    }
    return statusErr;
}

/* kControl: r23 accepted every control query with noErr. r24 keeps that EXACT behavior (so the
 * Audio-CD misID still reproduces) but records the csCode first — this is where a Foreign File
 * System's "is this a CD? / prepare medium" probe would land and be answered too permissively. */
static OSErr disk_control(ParmBlkPtr pb)
{
    cslog(2, pb);
    /* r30: reject the CD-ROM-specific Control csCodes (104 & 125) that the r29 capture showed appear
     * ONLY when the volume is misclaimed as "Audio CD 1". Our old blanket noErr falsely told the CD
     * Foreign-File-Access plugin "yes, that op worked" -> it claimed our FAT volume as a read-only
     * audio CD (writes then failed with wPrErr -44). controlErr makes it back off so PC Exchange
     * claims the FAT volume (writable). Other csCodes (e.g. 70/100 used by the normal mount) still
     * get noErr, so this shouldn't disturb the FAT mount path. */
    /* r80: EJECT (control csCode 7). Now reachable because we register as ejectable (diskInPlace=1). The File
     * Manager has ALREADY unmounted + flushed the volume before it sends Eject, so there is no in-flight I/O to
     * drain here — we just mark the media gone (so kStatus reports diskInPlace=0 and the Finder does NOT
     * immediately remount) and ack. This gives clean, safe removal of the USB stick, like the USB-1.1 path.
     * (A USB stick has no physical eject; re-mounting after eject re-runs at boot — hot re-mount is a v2 item.) */
    if (pb->cntrlParam.csCode == 7) {
        /* ★ n10: EJECT. Apple posts a Notification Manager alert here — "You may now remove the
         * cartridge from the USB device X because your Macintosh is finished with it." The user
         * confirmed OHCI shows it and we showed nothing at all, so ask the UIM to post it (it owns
         * the NM record and the INQUIRY-derived device name). Task level: the Finder's eject. */
        {   /* ★ n19 step 3: eject the drive this call names, not always slot 0. */
            int dv = slot_for_drive(pb->cntrlParam.ioVRefNum);
            if (dv < 0) dv = 0;
            gDiskInPlaceS[dv] = 0; if (gDrvStsS[dv]) gDrvStsS[dv]->diskInPlace = 0;
            /* ★ n24: tell the UIM WHICH slot, so Apple's alert names the drive the user actually ejected.
             * The UIM's device name used to be one global holding the most-recently-enumerated device, so
             * with two drives mounted this alert named the wrong one. */
            if (fetch_svc() && gSvc->magic2 == EUSB_MAGIC && gSvc->ejectFn) gSvc->ejectFn(dv);
        }
        return noErr;
    }
    /* ★ n4c: private media-state control from our EHCI driver (see kCsUsbDiskMedia). Magic-guarded so a
     * stray control can never fabricate a disk-inserted event. csParam[2] != 0 = arrived, 0 = gone. */
    if (pb->cntrlParam.csCode == kCsUsbDiskMedia) {
        UInt32 magic = ((UInt32)(unsigned short)pb->cntrlParam.csParam[0] << 16)
                     |  (UInt32)(unsigned short)pb->cntrlParam.csParam[1];
        if (magic != kUsbDiskMagic) return controlErr;
        {   /* ★ n19 step 3: csParam[3] carries the device slot. Older senders leave it 0, which is
             * slot 0, so the meaning is unchanged for a single-device build. */
            int dv = (int)(unsigned short)pb->cntrlParam.csParam[3];
            if (dv < 0 || dv >= EUSB_MAX_DEV) dv = 0;
            if (pb->cntrlParam.csParam[2]) return media_arrived(dv);
            media_gone(dv);
        }
        return noErr;
    }
    if (pb->cntrlParam.csCode == 104 || pb->cntrlParam.csCode == 125) return controlErr;
    return noErr;
}

/* Native-driver entry point (main == DoDriverIO, set by patch-pef-main.py). */
OSErr DoDriverIO(AddressSpaceID spaceID, IOCommandID cmdID,
                 IOCommandContents contents, IOCommandCode code, IOCommandKind kind)
{
    OSErr err;
    (void)spaceID;
    /* ★★ n9: DRAIN THE UIM's LOG RING FROM HERE. Once the activator quits, dispatch slot 23 stops and the
     * UIM has no task-level context left — so anything the interrupt-level engine logs is stranded in the
     * ring and the log simply stops. That is exactly why the "quit with a volume mounted freezes the
     * Finder" failure is currently un-diagnosable: we cannot see past the quit.
     * The File Manager keeps calling THIS driver at task level for as long as a volume is mounted, so this
     * is the one place that can still write. Guarded by magic2, the UIM's end-of-struct marker, so a layout
     * drift between the two fragments fails safe rather than calling a garbage pointer.
     * ⚠ NOT on kRead/kWrite. Those are issued by the File Manager from INSIDE a volume operation, and the
     * drain writes to a log file — re-entering the File Manager on the data path is precisely what this
     * file's own dput() comment warns against. Status/Control/Open/Close are direct Device Manager calls,
     * and the Finder polls status often enough to flush the ring promptly. */
    if (code != kReadCommand && code != kWriteCommand &&
        fetch_svc() && gSvc->magic2 == EUSB_MAGIC && gSvc->drainFn) gSvc->drainFn();
    switch (code) {
        case kInitializeCommand: err = scan_and_add(contents.initialInfo->refNum, 0); break;
        case kOpenCommand:
        case kCloseCommand:      err = noErr; break;
        case kReadCommand:
        case kWriteCommand: {
            /* r34: ASYNC. Submit to the UIM and return kIOBusyStatus; the UIM completes cmdID from the
             * heartbeat. No blocking -> the File Manager isn't held and the engine isn't re-entered
             * (that was the Finder freeze). Async reads from the Finder now just queue. */
            long sr;
            /* ★★ n6e: REFUSE I/O WHEN THE MEDIA IS GONE. This path had no media-present check at all, so
             * after an UN-EJECTED pull the volume stayed mounted, the File Manager kept issuing reads, and
             * we kept submitting them to a device that is no longer there. The transfers fail or return
             * stale bytes, and the File Manager reads that as a CORRUPT volume — which is why yanking the
             * stick produced "the disk is damaged" instead of "the disk was removed".
             * offLinErr (-65) is the classic Mac OS "R/W requested for an off-line drive" code: it tells the
             * File Manager the medium is ABSENT rather than bad, so the volume goes offline the way a yanked
             * floppy or Zip disk does. media_gone() already sets gDiskInPlace = 0 on the pull; this is what
             * makes that flag actually mean something on the I/O path. */
            /* ★ n19 step 3: the media gate is per drive. Checking slot 0 here would take a second
             * device offline whenever the first one was pulled, and vice versa. */
            { int dv = slot_for_drive(contents.pb->ioParam.ioVRefNum); if (dv < 0) dv = 0;
              if (!gDiskInPlaceS[dv]) { diolog((short)code, (short)offLinErr, 0); err = offLinErr; break; } }
            iolog((short)(code == kReadCommand ? 3 : 4), (unsigned long)kind, contents.pb);
            /* v36: the v35 DebugStr breaks are REMOVED. On a USB-keyboard-only Mac (MDD) MacsBug cannot take
             * keyboard input when it is entered from our driver's (sub-task-level) calling context, so the break
             * wedged the machine. The FM-race signal is now captured to EHCIUIM_init.log with NO halt: every
             * write's partition-relative LBA is in the 'Ucsl' ring (iolog above); the re-entrancy count is
             * gSubmitReentry; and the UIM flags SUSPICIOUS writes (issued from inside our completion, or an
             * off-device LBA) in the v36 tick dump. Run: boot unplugged -> launch -> insert -> copy the UT
             * folder -> quit -> send EHCIUIM_init.log. */
            sr = disk_submit(cmdID, contents.pb, (code == kWriteCommand));
            if (sr == 0) { diolog((short)code, (short)1 /*kIOBusyStatus*/, 0); return kIOBusyStatus; }  /* v25: trace accepted-async */
            err = (sr == 1) ? noErr : ioErr;       /* 1 = zero-length (noErr); -1 = no service/queue full (ioErr) */
            break;
        }
        case kControlCommand:    err = disk_control(contents.pb); break;  /* r24: log then accept */
        case kStatusCommand:     err = disk_status(contents.pb); break;
        case kFinalizeCommand:
        case kSupersededCommand:
        case kReplaceCommand:
        case kKillIOCommand:     err = noErr; break;
        default:                 err = paramErr; break;
    }
    diolog((short)code, (short)err, (code == kControlCommand || code == kStatusCommand) ? (long)contents.pb->cntrlParam.csCode : 0);  /* v25 control-plane trace */
    if (kind & kImmediateIOCommandKind) return err;                /* immediate: return completes it */
    return (OSErr)IOCommandIsComplete(cmdID, (short)err);          /* sync/async: signal the File Mgr */
}
