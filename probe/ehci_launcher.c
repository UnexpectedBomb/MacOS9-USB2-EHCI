/*
 * EHCILauncher — manual launcher for the Mac OS 9 USB 2.0 (EHCI) driver (early beta).
 *
 * WHY MANUAL, and why the insertion timing matters:
 *   A USB 2.0 PCI card shares its physical ports between the EHCI (2.0) controller and companion
 *   USB 1.1 controllers. If a drive is attached at boot, Apple's built-in USB 1.1 driver grabs it
 *   before our driver loads, and Mac OS 9 does not hand the device cleanly from the 1.1 controller
 *   over to EHCI. The reliable path is to keep the companion out of it entirely: boot with NO drive
 *   attached, launch this app so it claims the ports for EHCI, and THEN insert the drive when
 *   prompted — a fresh insert straight onto USB 2.0.
 *
 * Usage:
 *   1. Boot with your USB drive NOT plugged into the card.
 *   2. Double-click this app.
 *   3. When it prints ">>> INSERT USB DRIVE NOW <<<" (and beeps), plug the drive into the card.
 *      You have about 60 seconds before it times out and quits cleanly.
 *   4. It mounts on the desktop at USB 2.0 speed, then hides its window to reveal the Finder.
 *      Leave the app running — it hosts the USB stack; quitting it removes the driver. Eject in the
 *      Finder before unplugging.
 *
 * Diagnostic logs (kept on for the beta): the driver writes "EHCIUIM_init.log", and this app writes
 * a short "USB2 Launcher Log" of its steps + timing next to itself.
 */
#include <stdio.h>
#include <MacTypes.h>
#include <NameRegistry.h>
#include <CodeFragments.h>
#include <OSUtils.h>
#include <Files.h>
#include <Devices.h>         /* InstallDriverFromMemory */
#include <Disks.h>           /* DrvQElPtr */
#include <Gestalt.h>         /* Gestalt('Eusb') */
#include <Events.h>          /* WaitNextEvent, TickCount */
#include <Windows.h>         /* FrontWindow / HideWindow — reveal the desktop after mounting */
#include <Menus.h>           /* File menu with Quit (friendlier than relying on Cmd-Q) */
#include <Sound.h>           /* SysBeep — audible cue for the prompt + the mount */
#include "ehci_pef_blob.h"   /* gEHCIPef[] / gEHCIPefLen — the UIM ndrv PEF */
#include "usb_disk_blob.h"   /* gUsbDiskPef[] / gUsbDiskPefLen — the USB block driver */

#define kDriverPropertyName "driver,AAPL,MacOS,PowerPC"
#define kDRVQ ((QHdrPtr)0x0308L)
#define kInsertTimeoutSec 180L     /* how long to wait for the drive after the prompt. Generous, because on
                                    * some machines the drive re-enumerates several times (Apple's composite
                                    * driver intermittently fails its interface setup -6999 -> the device
                                    * bounces) before CreateBulkEndpoint fires; the mount still lands the
                                    * moment it stabilizes — this is just the worst-case ceiling. */
#define kRevealDelaySec   6L       /* how long the "mounted" message stays up before hiding the window */

/* DriverDescription byte-identical to the UIM's exported TheDriverDescription (see ehci_uim.c). */
typedef struct { unsigned char len; char s[31]; } TStr31;
typedef struct { unsigned char major, minorBug, stage, nonRel; } TNumVer;
typedef struct {
    OSType  sig; UInt32 descVersion;
    TStr31  nameInfoStr; TNumVer typeVersion;
    UInt32  driverRuntime;
    TStr31  driverName; UInt32 reserved[8]; UInt32 nServices;
} TDriverDesc;
static const TDriverDesc gDriverDesc = {
    0x6d74656aUL /* 'mtej' */, 0,
    { 15, "pciclass,0c0320" }, { 1, 0, 0x80, 0 },
    0x00000004UL,
    { 7, "EHCIUIM" }, { 0,0,0,0,0,0,0,0 }, 0
};

extern OSStatus USBExpertSetStatusLevel(UInt32 level);
extern void     ExpertIdleTask(void);
extern OSStatus USLPolledProcessDoneQueue(void);
extern void     SystemTask(void);
typedef OSStatus (*LoadUIMProc)(RegEntryID *node);

/* ---- short app-level log next to the app (persists after the window hides; for beta bug reports) ---- */
static short gLog = 0;
static void slog(const char *s)
{
    long n = 0, one = 1;
    if (!gLog) return;
    while (s[n]) n++;
    (void)FSWrite(gLog, &n, (Ptr)s);
    (void)FSWrite(gLog, &one, (Ptr)"\r");
}
static void slog_open(void)
{
    FSSpec sp; Str63 pn; const char *fname = "USB2 Launcher Log"; int i = 0;
    while (fname[i] && i < 62) { pn[i + 1] = fname[i]; i++; } pn[0] = (unsigned char)i;
    (void)FSMakeFSSpec(0, 0, pn, &sp);
    (void)FSpDelete(&sp);
    if (FSpCreate(&sp, 'ttxt', 'TEXT', 0) == noErr) (void)FSpOpenDF(&sp, fsRdWrPerm, &gLog);
}

static OSStatus set_prop(RegEntryID *n, const char *name, const void *val, RegPropertyValueSize sz)
{
    OSStatus e = RegistryPropertyCreate(n, name, (RegPropertyValue)val, sz);
    if (e != noErr) { (void)RegistryPropertyDelete(n, name); e = RegistryPropertyCreate(n, name, (RegPropertyValue)val, sz); }
    return e;
}
static LoadUIMProc resolve_loaduim(void)
{
    CFragConnectionID conn; Ptr mainAddr; Str255 errName; Ptr sym; CFragSymbolClass cls;
    if (GetSharedLibrary("\pUSBFamilyExpertLib", kPowerPCCFragArch, kReferenceCFrag,
                         &conn, &mainAddr, errName) != noErr) return 0;
    if (FindSymbol(conn, "\pLoadUIMForEntry", &sym, &cls) != noErr) return 0;
    return (LoadUIMProc)sym;
}

/* Path A: on quit, hand the EHCI root ports back to the companion 1.1 controllers so a
 * drive plugged in afterward still mounts at 1.1. The UIM stays RESIDENT when we quit, so
 * it can't do this for us (its kFinalize won't fire) — we do it directly. Derive the
 * operational register base the same way the driver does (assigned-addresses picks the
 * memory BAR index; AAPL,address[index] is the CPU-logical base; opBase = capBase +
 * CAPLENGTH) and write CONFIGFLAG=0 (EHCI 4.2: routes ALL ports to the companion).
 * Best-effort + silent: if anything is missing we simply leave the ports as they are
 * (a reboot always resets CONFIGFLAG). Value 0 is endian-neutral, so no byte-swap. */
static void release_ports(RegEntryID *n)
{
    UInt32 aa[48], la[16]; RegPropertyValueSize aaSz = sizeof(aa), laSz = sizeof(la);
    UInt32 nEnt, i, memIdx = 0xFFFFFFFFUL;
    volatile UInt8 *capBase, *opBase;
    if (RegistryPropertyGet(n, (RegPropertyName *)"assigned-addresses", aa, &aaSz) != noErr) return;
    if (RegistryPropertyGet(n, (RegPropertyName *)"AAPL,address", la, &laSz) != noErr) return;
    nEnt = aaSz / (5u * sizeof(UInt32));                 /* 5 UInt32 per assigned-address entry */
    for (i = 0; i < nEnt; i++) if (((aa[i * 5u] >> 24) & 0x3u) >= 2u) { memIdx = i; break; }  /* mem32/mem64 */
    if (memIdx == 0xFFFFFFFFUL || (memIdx * sizeof(UInt32)) >= laSz) return;
    capBase = (volatile UInt8 *)la[memIdx];
    if (capBase == 0) return;
    opBase = capBase + capBase[0];                        /* opBase = capBase + CAPLENGTH */
    *(volatile UInt32 *)(opBase + 0x40) = 0;              /* CONFIGFLAG = 0 -> all ports to the companion */
    slog("ports handed back to the 1.1 companion on quit (CONFIGFLAG cleared)");
}

/* ---- File menu (Apple + File>Quit) so the user can quit from the menu, not just Cmd-Q ----
 * gNodeP/gNodeReady mirror main's `node` so the menu handler can release the ports on quit. */
#define kAppleMenuID 128
#define kFileMenuID  129
static RegEntryID *gNodeP = 0;
static int gNodeReady = 0;
static void setup_menus(void)
{
    MenuHandle m;
    m = NewMenu(kAppleMenuID, "\p\024");                       /* Apple menu (\024 = apple glyph) */
    if (m) { AppendMenu(m, "\pAbout the USB 2.0 Mount Helper"); InsertMenu(m, 0); }
    m = NewMenu(kFileMenuID, "\pFile");
    if (m) { AppendMenu(m, "\pQuit/Q"); InsertMenu(m, 0); }     /* Quit item, Cmd-Q equivalent */
    DrawMenuBar();
}
static void do_quit(void)
{
    if (gNodeReady && gNodeP) release_ports(gNodeP);           /* hand the ports back to 1.1 before we go */
    printf("\nQuitting.\n");
    slog("quit (menu/Cmd-Q); ports handed back");
    ExitToShell();
}
/* Process one event for the menu bar: File>Quit (mouse or Cmd-Q). Other events fall through
 * untouched, exactly as before. Call this from every wait/host loop. */
static void handle_menu_event(EventRecord *evt)
{
    long r = 0; WindowPtr w; short menuID, item;
    if (evt->what == mouseDown) {
        if (FindWindow(evt->where, &w) == inMenuBar) r = MenuSelect(evt->where);
    } else if (evt->what == keyDown && (evt->modifiers & cmdKey)) {
        char ch = (char)(evt->message & charCodeMask);
        r = MenuKey((short)ch);
        /* Safety net: quit on Cmd-Q even if the menu bar didn't map it (RetroConsole edge cases),
         * so we never regress the previously-working Cmd-Q behaviour. */
        if (((r >> 16) & 0xFFFF) == 0 && ch == 'q') do_quit();
    }
    menuID = (short)((r >> 16) & 0xFFFF); item = (short)(r & 0xFFFF);  /* hi word = menuID, lo = item */
    if (menuID == kFileMenuID && item == 1) do_quit();          /* File > Quit (mouse or Cmd-Q) */
    if (menuID) HiliteMenu(0);
}

/* ---- Path A node-probe: dump the EHCI node's OF identity so we can (1) lock the
 * ROM-parcel match string and (2) tell whether the boot USB Expert (device_type
 * == "usb") or the Device Manager loads our driver — which decides the parcel
 * flags + TheDriverDescription.driverRuntime. Always node-probe the target and lock
 * the match string before burning a ROM. ---- */
#define PATHA_NODEPROBE 0
/* PATH A mount vehicle: 1 = skip the driver-property injection and use the driver the ROM PARCEL already
 * bound to the node (driver,AAPL,MacOS,PowerPC + driver-ptr, prepared by the boot loader). We only call
 * LoadUIMForEntry to activate the ROM-shipped UIM, then self-probe + mount. 0 = the old app behaviour
 * (embed + inject our own driver copy). Requires the SELF-PROBE ROM (USB2_PathA_ROM_selfprobe.bin) burned. */
#define PATHA_ROM_DRIVER 1
static void probe_prop(RegEntryID *n, const char *nm)
{
    RegPropertyValueSize sz = 0; unsigned char buf[260]; long i; int printable = 1;
    if (RegistryPropertyGetSize(n, (RegPropertyName *)nm, &sz) != noErr) {
        printf("  %-26s : (absent)\n", nm); slog(nm); slog("   (absent)"); return;
    }
    if (sz > 256) sz = 256;
    if (RegistryPropertyGet(n, (RegPropertyName *)nm, buf, &sz) != noErr) {
        printf("  %-26s : (read failed)\n", nm); return;
    }
    for (i = 0; i < (long)sz; i++) { unsigned char c = buf[i]; if (c != 0 && (c < 0x20 || c > 0x7e)) { printable = 0; break; } }
    if (printable) {                                   /* string / NUL-separated string-list */
        for (i = 0; i < (long)sz; i++) if (buf[i] == 0 && i < (long)sz - 1) buf[i] = '/';
        buf[sz ? (sz < 256 ? sz : 256) : 0] = 0; buf[259] = 0;
        printf("  %-26s : \"%s\"  (%ld bytes)\n", nm, (char *)buf, (long)sz);
        slog(nm); slog((char *)buf);
    } else {
        char hb[80]; long n2 = sz < 8 ? sz : 8, k = 0; static const char hx[] = "0123456789abcdef";
        for (i = 0; i < n2; i++) { hb[k++] = hx[(buf[i] >> 4) & 0xF]; hb[k++] = hx[buf[i] & 0xF]; hb[k++] = ' '; }
        hb[k] = 0;
        printf("  %-26s : <%ld bytes> %s\n", nm, (long)sz, hb);
        slog(nm); slog("   <binary>");
    }
}
static void probe_node(RegEntryID *n)
{
    printf("\n==== EHCI NODE PROBE (Path A: lock the ROM-parcel match) ====\n");
    slog("==== EHCI NODE PROBE ====");
    probe_prop(n, "name");
    probe_prop(n, "compatible");
    probe_prop(n, "device_type");
    probe_prop(n, "class-code");
    probe_prop(n, "vendor-id");
    probe_prop(n, "device-id");
    probe_prop(n, "driver,AAPL,MacOS,PowerPC");   /* the parcel's injected PEF (present => parcel matched) */
    probe_prop(n, "driver-ref");                  /* Unit Table refnum => driver LOADED + registered (the formal claim) */
    probe_prop(n, "driver-ptr");                  /* driver code pointer => loaded */
    probe_prop(n, "AAPL,driver-name");            /* driver name the DM registered us under */
    printf("=============================================================\n");
    printf("Decides: device_type==\"usb\" -> USB Expert loads us (plugin path);\n");
    printf("         else -> Device Manager loads us via DoDriverIO.\n");
    printf("Match the parcel on the 'compatible' entry containing pciclass,0c0320\n");
    printf("(or the exact 'name' if compatible lacks it).\n\n");
}
static void banner(const char *s)
{
    printf("\n****************************************************\n");
    printf("*   %s\n", s);
    printf("****************************************************\n\n");
}
/* pump the USB stack once (call in every wait loop so enumeration + the self-probe make progress) */
static void pump_once(EventRecord *evt)
{
    (void)WaitNextEvent(everyEvent, evt, 3L, NULL);
    handle_menu_event(evt);   /* File > Quit works during the settle/insert wait too */
    SystemTask();
    ExpertIdleTask();
    USLPolledProcessDoneQueue();
}

int main(void)
{
    RegEntryIter iter; RegEntryID node; Boolean done = false;
    UInt32 wantClass = 0x000c0320UL;
    LoadUIMProc loadUIM;
    long gv; int mounted = 0;
    EventRecord evt;
    unsigned long startTicks;

    setvbuf(stdout, NULL, _IONBF, 0);
    slog_open();
    setup_menus();   /* Apple + File(Quit) menu bar */
    printf("========================================================\n");
    printf("  USB 2.0 for Mac OS 9  --  Path A: ROM driver + headless mount\n");
    printf("========================================================\n\n");
    printf("BEFORE YOU CONTINUE: your USB drive must NOT be plugged in yet.\n");
    printf("If it is, quit now (Cmd-Q), unplug it, REBOOT, and run this again\n");
    printf("with the drive unplugged. (A drive attached at boot is claimed by\n");
    printf("USB 1.1 and will not hand over cleanly.) Your keyboard and mouse\n");
    printf("can stay plugged in -- and you need at least ONE free USB port for\n");
    printf("the drive.\n\n");
    printf("NOTE: your keyboard and mouse may pause for a second or two while\n");
    printf("the ports are claimed -- this is normal; they come right back.\n\n");
    printf("Bringing up the USB 2.0 controller (claiming the ports)...\n");
    slog("EHCILauncher (beta) start; bringing up controller");

    if (RegistryEntryIterateCreate(&iter) != noErr) { printf("ERROR: Name Registry unavailable.\n"); slog("ERR registry"); goto hold; }
    if (RegistryEntrySearch(&iter, kRegIterDescendants, &node, &done,
                            "class-code", &wantClass, sizeof(wantClass)) != noErr) {
        RegistryEntryIterateDispose(&iter);
        printf("ERROR: no USB 2.0 (EHCI) controller found. Check that your USB 2.0\n");
        printf("PCI card is seated, or that this Mac has built-in USB 2.0.\n");
        slog("ERR no EHCI card"); goto hold;
    }
    RegistryEntryIterateDispose(&iter);
    gNodeReady = 1; gNodeP = &node;   /* node valid from here -> release_ports() safe on menu Quit */

#if PATHA_NODEPROBE
    /* Path A: dump the node identity and STOP — no UIM load, no bring-up. Grab the
     * "USB2 Launcher Log" (or read the console) to lock the parcel match string. */
    probe_node(&node);
    printf("Node probe complete. This build does NOT bring up USB — quit (Cmd-Q).\n");
    printf("Send the 'USB2 Launcher Log' so we can finalize usb_rom_inject.py.\n");
    slog("node-probe done; holding (no UIM load)");
    goto hold;
#endif

    loadUIM = resolve_loaduim();
    if (!loadUIM) { printf("ERROR: the USB Family loader is not available on this system.\n"); slog("ERR no loader"); goto hold; }
    USBExpertSetStatusLevel(0);

#if PATHA_ROM_DRIVER
    /* Path A: the ROM parcel already bound driver,AAPL,MacOS,PowerPC (+ driver-ptr) to this node at boot.
     * Do NOT inject our own copy — just activate the ROM-shipped UIM. (Requires the self-probe ROM burned;
     * the node-probe confirmed driver,AAPL,MacOS,PowerPC + driver-ptr are present post-boot.) */
    printf("Using the USB 2.0 driver shipped in the Mac OS ROM (no injection)...\n");
    slog("Path A: ROM-shipped driver; LoadUIMForEntry to activate");
#else
    {
        RegPropertyValueSize nsz = 0; void *codePtr = (void *)gEHCIPef;
        Boolean hadName = (RegistryPropertyGetSize(&node, "name", &nsz) == noErr);
        if (!hadName) (void)set_prop(&node, "name", "pciclass,0c0320", 16);
        (void)set_prop(&node, kDriverPropertyName, gEHCIPef, (RegPropertyValueSize)gEHCIPefLen);
        if (set_prop(&node, "driver-descriptor", &gDriverDesc, (RegPropertyValueSize)sizeof(gDriverDesc)) != noErr) {
            printf("ERROR: could not attach the driver.\n"); slog("ERR attach"); goto hold;
        }
        (void)set_prop(&node, "driver-ptr", &codePtr, (RegPropertyValueSize)sizeof(codePtr));
    }
#endif

    (void)loadUIM(&node);          /* LoadUIMForEntry: activate the UIM on this node (ROM-shipped driver) */
    printf("USB 2.0 controller is ready.\n");
    slog("controller ready; settling input devices");

    /* Settle window: claiming the ports briefly disconnects any input devices that share this
     * controller (on an on-board machine the keyboard/mouse do), forcing them to re-enumerate. If the
     * drive is inserted while that's still happening, the two enumerations collide on the same
     * controller and the mount can stall intermittently. So pump the stack for a few seconds FIRST —
     * long enough for the keyboard/mouse to come back and settle — before asking for the drive. (On a
     * dedicated-line PCI card there's nothing to re-enumerate, so this is just a harmless short wait.) */
    printf("Settling input devices (a few seconds)...\n");
    { unsigned long t0 = TickCount(); while ((TickCount() - t0) < 8UL * 60UL) pump_once(&evt); }
    slog("settle done; prompting for insert");

    /* THE moment that matters: the ports now belong to EHCI, so a freshly inserted drive comes up at
     * high speed. Prompt loudly + audibly, and count down the insert window. */
    SysBeep(30);
    banner(">>>  INSERT USB DRIVE NOW  <<<");
    printf("Plug your USB drive into any FREE USB port -- one that is empty right\n");
    printf("now (NOT the port your keyboard or mouse uses). Any free port works;\n");
    printf("the driver already left the keyboard/mouse ports on USB 1.1.\n");
    printf("Then wait. The drive may re-try a few times before it appears -- this can\n");
    printf("take up to a couple of minutes; it mounts as soon as it's ready. (If nothing\n");
    printf("shows up yet, this keeps waiting -- it mounts the moment the drive is ready.)\n\n");

    /* The wait loop is intentionally SILENT: printing to the console during the insert/enumeration
     * window perturbs the timing-sensitive USB enumeration (a live countdown froze it). We only pump
     * + watch here; the timeout below is checked without any per-iteration output. */
    startTicks = TickCount();
    for (;;) {
        pump_once(&evt);
        if (Gestalt('Eusb', &gv) == noErr && gv != 0) {           /* drive detected — mount it */
            short drefNum = 0; OSErr ie; DrvQElPtr el;
            slog("drive detected ('Eusb'); mounting");
            ie = InstallDriverFromMemory((Ptr)gUsbDiskPef, (ByteCount)gUsbDiskPefLen,
                                         "\pUSBDisk", &node, 48, 127, &drefNum);
            if (ie == noErr && drefNum != 0) {
                for (el = (DrvQElPtr)kDRVQ->qHead; el; el = (DrvQElPtr)el->qLink) {
                    if (el->dQRefNum == drefNum) {
                        ParamBlockRec pb; unsigned char *pp = (unsigned char *)&pb; long j; OSErr me;
                        for (j = 0; j < (long)sizeof(pb); j++) pp[j] = 0;
                        pb.ioParam.ioVRefNum = el->dQDrive;
                        me = PBMountVol((ParmBlkPtr)&pb);
                        if (me == 0) mounted = 1;
                    }
                }
            }
            break;
        }
#if !PATHA_ROM_DRIVER
        if ((long)((TickCount() - startTicks) / 60UL) >= kInsertTimeoutSec) {   /* silent timeout */
            printf("\nTimed out -- no drive detected. Quit (Cmd-Q), reboot with the drive\n");
            printf("unplugged, and run this again (insert only when prompted).\n");
            slog("insert timed out"); goto hold;
        }
#else
        (void)startTicks;   /* Startup-Item / headless: NO timeout — poll forever, mount whenever a drive
                             * appears (boot with the drive out, insert any time after; it just mounts). */
#endif
    }

    SysBeep(30);
    if (!mounted) {
        banner("A drive was detected, but it did not mount.");
        printf("Make sure it is Mac OS Standard/Extended (HFS) formatted. Then quit, reboot unplugged, and try again.\n");
        slog("mount FAILED"); goto hold;
    }

    banner("MOUNTED -- your USB 2.0 drive is ready in the Finder.");
    printf("Leave this app running while you use the drive.\n");
    printf("Eject it in the Finder (drag to Trash / Special > Eject) BEFORE unplugging.\n");
    printf("To use a drive again afterward: quit, reboot with it unplugged, and run this again.\n\n");
    slog("MOUNTED ok");

    /* brief pause so the "MOUNTED" message can be read, then hide the window to reveal the desktop.
     * One line up front + a SILENT wait (no per-second redraw) — same reason the wait loop is silent. */
    {
        unsigned long t0 = TickCount();
        printf("This window will hide and reveal your desktop in a few seconds...\n");
        while ((TickCount() - t0) < (unsigned long)(kRevealDelaySec * 60L)) pump_once(&evt);
        { WindowPtr w = FrontWindow(); if (w) HideWindow(w); }
        slog("window hidden; hosting USB stack in background");
    }

    /* v44: resident + silent — keep the USB stack serviced so the drive stays alive and eject works.
     * Cmd-Q quits cleanly; the driver is hosted here, so quit only after ejecting the drive. */
    for (;;) {
        (void)WaitNextEvent(everyEvent, &evt, 6L, NULL);
        handle_menu_event(&evt);   /* File > Quit (mouse or Cmd-Q) -> hand ports back + quit */
        ExpertIdleTask();
        USLPolledProcessDoneQueue();
    }

hold:
    printf("\nStopped -- resolve the above, then quit (Cmd-Q) and try again.\n");
    for (;;) {
        (void)WaitNextEvent(everyEvent, &evt, 30L, NULL);
        handle_menu_event(&evt);   /* File > Quit (mouse or Cmd-Q) -> hand ports back + quit */
        SystemTask();
    }
    return 0;
}
