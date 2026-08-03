/*
 * ehci_activate.c — n4: faceless resident ACTIVATOR + PUMP ("EHCIActivate").
 *
 * WHY IT NOW STAYS RESIDENT (n4, correcting an n2 mis-read): our probe runs from dispatch slot 23
 * (uim23 -> ehci_vhub_selfprobe_tick), and the USL only calls slot 23 out of USLPolledProcessDoneQueue —
 * i.e. from an APPLICATION's pump loop. n2 appeared to show the Finder pumping us, but that evidence was
 * 1.65M calls to slot 27 (an unrelated stub slot the USL ticks by itself); slot 23 stopped the moment the
 * n2 shim quit. Proof in the n3b log: the port map printed its baseline once (during the shim's 3 s settle,
 * which does call USLPolledProcessDoneQueue) and then NEVER AGAIN, so no insert on any of four sockets was
 * ever noticed. So SOMETHING must keep calling the pump, and that is this program.
 *
 * SCOPE OF THE DEPENDENCY (small): the pump is needed for DISCOVERY only. Block I/O to an already-mounted
 * volume runs at interrupt level through the driver's bio engine (bio_kick from the heartbeat SIH), and since
 * n3 the block driver is installed BY THE DRIVER into the system heap — so a mounted drive keeps working even
 * if this program is not running. n5 (async enumeration driven from our own timer) removes the last need.
 *
 * WHY THIS EXISTS: the n0/n1 hardware tests proved the Device Manager NEVER calls our DoDriverIO (0 commands,
 * even with a declared service), so nothing brings the driver up at boot on its own. LoadUIMForEntry is the
 * one activation call that has worked in every session, so this shim does exactly that and nothing else.
 *
 * DELIBERATELY DOES NOT:
 *   - install the block driver  (n2 tests the DRIVER's pumping; a mount is not needed to answer the question,
 *     and installing it from app memory would leave a dangling CFM fragment when we quit — the old
 *     shutdown-crash root cause)
 *   - release the ports on exit (hp6's do_quit clears CONFIGFLAG, which would hand the ports back to the 1.1
 *     companion and sabotage the very observation we are making)
 *   - run a host loop, open windows, or install menus (it must get out of the way immediately)
 *
 * DEPLOYMENT: drop it in System Folder:Startup Items and it activates USB 2.0 at every boot with no user
 * action (that vehicle is already proven on this project). Diagnostics go to "USB2 Activate Log".
 */
#include <stdio.h>
#include <MacTypes.h>
#include <NameRegistry.h>
#include <CodeFragments.h>
#include <OSUtils.h>
#include <Files.h>
#include <Events.h>
#include <Windows.h>     /* n4: hide the console window so the pump is faceless */
#include <Gestalt.h>     /* n4f: reach the driver's 'Eusb' service to unmount before quitting */
#include <Processes.h>   /* n4d: hand the front back to the Finder so IT receives diskEvt */

#define APP_VER "n4f"

typedef OSStatus (*LoadUIMProc)(RegEntryID *node);
extern void ExpertIdleTask(void);
extern void USLPolledProcessDoneQueue(void);
extern void USBExpertSetStatusLevel(UInt32 level);

/* ---- logging: FSWrite + flush the log's OWN volume per line (the default volume switches to a USB stick
 * once one mounts, so FlushVol(0,0) would not commit this file) ---- */
static short gLog = 0, gLogVol = 0;
static void slog(const char *s)
{
    long n = 0, one = 1; ParamBlockRec pbf;
    if (!gLog) return;
    while (s[n]) n++;
    (void)FSWrite(gLog, &n, (Ptr)s);
    (void)FSWrite(gLog, &one, (Ptr)"\r");
    pbf.ioParam.ioCompletion = 0; pbf.ioParam.ioRefNum = gLog; (void)PBFlushFileSync(&pbf);
    (void)FlushVol(0, gLogVol);
}
static void slog_open(void)
{
    FSSpec sp; Str63 pn; const char *fname = "USB2 Activate Log"; int i = 0;
    while (fname[i] && i < 62) { pn[i + 1] = fname[i]; i++; } pn[0] = (unsigned char)i;
    (void)FSMakeFSSpec(0, 0, pn, &sp);
    (void)FSpDelete(&sp);
    if (FSpCreate(&sp, 'ttxt', 'TEXT', 0) == noErr) {
        if (FSpOpenDF(&sp, fsRdWrPerm, &gLog) == noErr) gLogVol = sp.vRefNum;
    }
}
/* ★ n4d: put the FINDER back in front. A launched app is frontmost, and PostEvent(diskEvt) is routed to the
 * FRONT process — so while we were frontmost we were the one being handed the "disk inserted" event that is
 * supposed to reach the Finder. That is exactly what the n4c run showed: the driver did everything right
 * (AddDrive drive#10 + posted diskEvt, both in the block-driver log) and the volume only appeared once the
 * user QUIT us and the Finder came forward. Telling the user to "click the desktop first" was a workaround
 * for a design flaw — and it fails outright as a Startup Item, which launches frontmost at boot. */
static void front_to_finder(void)
{
    ProcessSerialNumber psn; ProcessInfoRec info; Str31 nm;
    psn.highLongOfPSN = 0; psn.lowLongOfPSN = kNoProcess;
    while (GetNextProcess(&psn) == noErr) {
        info.processInfoLength = sizeof(info);
        info.processName       = nm;
        info.processAppSpec    = 0;
        if (GetProcessInformation(&psn, &info) == noErr &&
            info.processSignature == 'MACS' && info.processType == 'FNDR') {
            (void)SetFrontProcess(&psn);
            slog("handed the front back to the Finder (so IT receives diskEvt, not us)");
            return;
        }
    }
    slog("WARNING: Finder not found — staying frontmost; the diskEvt mask still protects the mount");
}
/* n4f: the driver publishes its block service via Gestalt('Eusb'). We only need the LAST field, quitFn,
 * so the struct below must mirror the driver's gSvc EXACTLY up to that point (ehci_vhub.c ~L1428).
 * Everything before quitFn is opaque to us and declared only to get the offset right. */
typedef struct {
    UInt32 magic;                                   /* 'EUSB' */
    void  *readFn, *writeFn;
    UInt32 blkSize, blkCnt;
    void  *submitFn, *healthFn, *toStateFn, *simReplugFn, *obsArmFn, *tickFn, *loopFn;
    long (*quitFn)(void);                           /* n8: unmount our volumes; returns the BUSY count */
} UsbSvcView;
static long ask_driver_to_prepare_quit(void)
{
    long v;
    UsbSvcView *s;
    if (Gestalt('Eusb', &v) != noErr || v == 0) return 0;   /* nothing mounted by us -> safe */
    s = (UsbSvcView *)v;
    if (s->magic != 0x45555342UL || s->quitFn == 0) return 0;
    return s->quitFn();
}
static LoadUIMProc resolve_loaduim(void)
{
    CFragConnectionID conn; Ptr mainAddr; Str255 errName; Ptr sym; CFragSymbolClass cls;
    if (GetSharedLibrary("\pUSBFamilyExpertLib", kPowerPCCFragArch, kReferenceCFrag,
                         &conn, &mainAddr, errName) != noErr) return 0;
    if (FindSymbol(conn, "\pLoadUIMForEntry", &sym, &cls) != noErr) return 0;
    return (LoadUIMProc)sym;
}

int main(void)
{
    RegEntryIter iter; RegEntryID node; Boolean done = false;
    UInt32 wantClass = 0x000c0320UL;      /* PCI class 0C/03/20 = EHCI */
    LoadUIMProc loadUIM;
    EventRecord evt;
    unsigned long t0;

    /* One printf first: RetroConsole initialises the Toolbox lazily on the FIRST console write, so anything
     * Toolbox-ish before this would run against an uninitialised Menu/Window Manager (that ordering bug once
     * scribbled the heap and crashed us two builds in a row). */
    printf("USB 2.0 for Mac OS 9 (%s) - activating the ROM driver...\n", APP_VER);
    slog_open();
    slog("EHCIActivate (" APP_VER ") start");

    if (RegistryEntryIDInit(&node) != noErr) { slog("ERR RegistryEntryIDInit"); return 1; }
    if (RegistryEntryIterateCreate(&iter) != noErr) { slog("ERR iterate create"); return 1; }
    if (RegistryEntrySearch(&iter, kRegIterDescendants, &node, &done,
                            "class-code", &wantClass, sizeof(wantClass)) != noErr || done) {
        slog("ERR: no EHCI controller (class 0c0320) found — is the USB 2.0 card installed?");
        (void)RegistryEntryIterateDispose(&iter);
        printf("No EHCI controller found.\n");
        return 1;
    }
    (void)RegistryEntryIterateDispose(&iter);
    slog("EHCI node found (class-code 0c0320)");

    loadUIM = resolve_loaduim();
    if (!loadUIM) { slog("ERR: USBFamilyExpertLib/LoadUIMForEntry unavailable"); printf("USB loader unavailable.\n"); return 1; }
    USBExpertSetStatusLevel(0);

    /* THE activation call. Proven every session: it runs uimInitialize synchronously, which brings the
     * controller fully up (registers mapped, HCReset, schedules started, real IRQ + heartbeat installed). */
    slog("calling LoadUIMForEntry (the one proven activation path)");
    (void)loadUIM(&node);
    slog("LoadUIMForEntry returned — controller should now be up and self-servicing");

    /* n4: HIDE the console window, then pump FOREVER. Hiding (rather than never printing) keeps the
     * RetroConsole Toolbox init we depend on — it runs lazily on the first console write — while leaving no
     * visible window; this is the same pattern hp6 used successfully. No menus are installed: building menus
     * is what scribbled the heap in hp1/safe1, so Cmd-Q is detected directly from the event record instead. */
    printf("Active. USB 2.0 drives will mount by themselves. (Cmd-Q to stop.)\n");
    { WindowPtr w = FrontWindow(); if (w) HideWindow(w); }
    front_to_finder();   /* n4d: drop to the background BEFORE we start pumping */
    slog("window hidden; pumping the driver's polled service (slot 23) — a drive inserted now should mount by itself");

    for (;;) {
        /* ★ n4d: NEVER take diskEvt out of the queue. `everyEvent` made us swallow the driver's own
         * disk-inserted event whenever we happened to be frontmost, so the volume never mounted. Masking it
         * out leaves it for the Finder no matter who is in front — the belt to front_to_finder's braces, and
         * what keeps this correct if the user brings us forward from the Application menu to quit. */
        (void)WaitNextEvent(everyEvent & ~diskMask, &evt, 6L, NULL);
        /* Cmd-Q escape, without installing a menu bar. */
        if (evt.what == keyDown && (evt.modifiers & cmdKey)) {
            char ch = (char)(evt.message & charCodeMask);
            if (ch == 'q' || ch == 'Q') {
                /* ★★ n4f: DO NOT just walk away from a mounted volume. Quitting with a drive still
                 * mounted shadowed the volume, produced "please insert the disk", and FROZE THE FINDER —
                 * the driver survives (no Finalize, no stop_service in the log), but slot 23 stops with us
                 * and the mounted volume evidently still needs it. Ask the driver to unmount our volumes
                 * first; if any is BUSY it returns non-zero and we REFUSE to quit, because leaving a live
                 * volume with no pump is the crash. */
                long busy = ask_driver_to_prepare_quit();
                if (busy > 0) { slog("Cmd-Q REFUSED: a USB 2.0 volume is still in use — close its files first"); continue; }
                break;
            }
        }
        ExpertIdleTask();
        USLPolledProcessDoneQueue();   /* ← the ONE call the driver's probe (slot 23) depends on */
    }

    /* Quit WITHOUT releasing the ports (clearing CONFIGFLAG would hand them back to the 1.1 companion) and
     * without removing the block driver (since n3 the DRIVER installs it into the system heap, so it is not
     * ours to tear down and an already-mounted volume keeps working through the interrupt-level bio engine). */
    slog("Cmd-Q: stopping the pump; ports left with EHCI and the block driver left installed");
    t0 = TickCount(); (void)t0;
    return 0;
}
