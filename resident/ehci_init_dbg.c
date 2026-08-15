/*
 * ehci_init_dbg.c — R2b-2 RESIDENT install (evolved from the r2b1 crash-diagnosis build).
 *
 * r2b2-DBG proved the 68K INIT can load the PPC fragment and call it via Mixed Mode
 * (NewRoutineDescriptorTrap — see [[reference_os9_init_resident_driver]]) and that the EHCI node
 * is in the Name Registry at boot. This build proves the two remaining vehicle unknowns:
 *   (1) RESIDENCY — GetMemFragment(kPrivateCFragCopy) copies the fragment into CFM-owned memory;
 *       we HOLD the connection open (never CloseConnection) so the code survives after this INIT
 *       returns and its temp heap is freed.
 *   (2) DEFERRED TASK-LEVEL EXECUTION — FindSymbol the PPC "ResidentNMResponse", build a
 *       PERSISTENT routine descriptor for it (NewRoutineDescriptorTrap; kept in the system heap,
 *       never disposed), and NMInstall a Notification pointing at it. The Notification Manager
 *       delivers it at TASK level once the Finder's event loop runs — post-boot, USB up, File
 *       Manager safe. (A Time Manager task fires at INTERRUPT level, where FSWrite/Gestalt/CFM are
 *       unsafe, so NM is the correct primitive to marshal deferred work to task level.)
 * All persistent state lives in the system heap (NMRec via NewPtrSysClear; descriptor via the Trap
 * allocator; fragment via the held connection). Defensive: any failure closes the connection and
 * returns so boot proceeds. SUCCESS = the INIT log ends with "resident install complete" AND, a
 * moment after the desktop appears, "EHCIInit NM Log" shows ResidentNMResponse fired at task level.
 */
#include <OSUtils.h>
#include <MacMemory.h>
#include <Resources.h>
#include <Gestalt.h>
#include <CodeFragments.h>
#include <MixedMode.h>
#include <Sound.h>
#include <Files.h>
#include <Notification.h>
#include "Retro68Runtime.h"

/* ---- 68K disk logger, CR-terminated + flushed per line (mintest pattern), own file. ---- */
static short gLog = 0;
static void lopen(void)
{
    FSSpec sp;
    if (gLog) return;
    (void)FSMakeFSSpec(0, 0, "\pEHCIInit INIT Log", &sp);
    (void)FSpDelete(&sp);
    if (FSpCreate(&sp, 'ttxt', 'TEXT', 0) == noErr)
        (void)FSpOpenDF(&sp, fsRdWrPerm, &gLog);
}
static void L(const char *s)
{
    long n = 0, z = 1;
    if (!gLog) lopen();
    if (gLog) { while (s[n]) n++; (void)FSWrite(gLog, &n, (Ptr)s);
        (void)FSWrite(gLog, &z, (Ptr)"\r"); (void)FlushVol(0, 0); }
}
static void Lx(const char *label, unsigned long v)
{
    char b[96]; int i = 0, j; static const char hx[] = "0123456789abcdef";
    while (label[i] && i < 72) { b[i] = label[i]; i++; }
    b[i++] = ' '; b[i++] = '0'; b[i++] = 'x';
    for (j = 28; j >= 0; j -= 4) b[i++] = hx[(v >> j) & 0xF];
    b[i] = 0; L(b);
}

void _start(void)
{
    long cfmAttr = 0;
    Handle h = NULL;
    CFragConnectionID connID = (CFragConnectionID)0;
    Ptr mainAddr; Str255 errName;
    Ptr symAddr; CFragSymbolClass symClass;
    OSErr err;
    Boolean haveConn = false;

    RETRO68_RELOCATE();
    Retro68CallConstructors();

    /* v9: the SysBeep(30) load-proof is GONE for shipping builds (user request, 2026-08-14): a beep
     * during the extension parade reads as an error on a machine that boots otherwise silently. The
     * Boot Log line below is the load-proof now — "no log file at all" = the INIT never ran. */
    L("=== EHCIInit v9-dm (DM-resident vehicle): 68K INIT _start ran ===");

    if (Gestalt(gestaltCFMAttr, &cfmAttr) != noErr) { L("Gestalt(CFM) FAILED"); goto fail; }
    if (!(cfmAttr & (1L << gestaltCFMPresent)))      { L("CFM NOT present"); goto fail; }

    h = Get1Resource('PPC ', 128);
    if (h == NULL) { L("Get1Resource('PPC ',128) == NULL"); goto fail; }
    HLock(h);

    /* kPrivateCFragCopy => CFM copies the fragment's code+data into its own (system) memory, so the
     * resource handle is disposable and residency is achieved simply by never closing the connection. */
    err = GetMemFragment(*h, GetHandleSize(h), "\pEHCIResident", kPrivateCFragCopy,
                         &connID, &mainAddr, errName);
    Lx("GetMemFragment err (0=ok)", (unsigned long)(long)err);
    if (err != noErr) goto fail;
    haveConn = true;

    /* ★★★★★★★ v9-dm (2026-08-14) — THE DM-RESIDENT VEHICLE. The r2b2R residency this file shipped
     * ("hold the connection, never CloseConnection") is the boot-window crash family: an INIT's
     * context does not survive boot, so the "private copy" (code + data + import TVectors) and every
     * TheZone allocation sat in an evaporating arena — first the NM descriptor (recycled into the
     * Helvetica FOND, PC executing font bytes), then, with the descriptor SysZone-forced, the fragment
     * itself (LR 01E7A00C / R12 TVector 01E7A4C4 -> garbage 80410014). Our own reference had proven
     * this failure AND its fix on hardware a month earlier ([[reference_os9_init_resident_driver]]):
     * "a held connID is reaped -> dangles -> crash on first post-boot use"; the PROVEN pattern is
     * InstallDriverFromMemory — the Device Manager owns the copy. So THIS load is now deliberately
     * TRANSIENT: call InstallMe once (it runs BootMain's early port claim synchronously, then
     * NewPtrSys-copies the PEF and installs it under the DM; the INSTALLED copy's kInitialize arms
     * the activation NM from DM-owned memory), then CLOSE the connection and let this copy die. */
    err = FindSymbol(connID, "\pInstallMe", &symAddr, &symClass);
    Lx("FindSymbol(InstallMe) err (0=ok)", (unsigned long)(long)err);
    if (err != noErr) goto fail;

    {   UniversalProcPtr upp;
        upp = NewRoutineDescriptorTrap((ProcPtr)symAddr, kCStackBased, kPowerPCISA);
        Lx("InstallMe descriptor (0 = FAILED; must be != symAddr — the r2b1 lesson)", (unsigned long)upp);
        if (upp == NULL) goto fail;
        L(">>> calling InstallMe: claim + DM-resident install (see EHCIInit Boot Log for both) <<<");
        (*(void (*)(void))upp)();
        DisposeRoutineDescriptorTrap(upp);
        L("<<< InstallMe returned >>>");
    }

    /* The transient loader connection is DROPPED — residency now belongs to the Device Manager's
     * copy, which is the entire point of v9-dm. Nothing post-boot points into this one. */
    (void)CloseConnection(&connID);
    haveConn = false;
    HUnlock(h);
    (void)ReleaseResource(h);
    L("=== v9-dm install complete: transient copy dropped, DM-resident copy armed; boot continues ===");
    Retro68FreeGlobals();
    return;

fail:
    L("!!! resident install FAILED — cleaning up; boot continues !!!");
    if (haveConn) (void)CloseConnection(&connID);
    if (h) HUnlock(h);
    Retro68FreeGlobals();
}
