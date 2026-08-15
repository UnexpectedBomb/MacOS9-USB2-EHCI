/*
 * bootmain.c — R2b-1 RESIDENT-VEHICLE PROOF (PowerPC CFM fragment).
 *
 * Called at BOOT by our thin 68K INIT (ehci_init.c) via the Mixed Mode Manager. The R2b-1 shell
 * (proven on HW) established the vehicle: (a) a 68K INIT runs at boot and can load + call a PowerPC
 * fragment, (b) File Manager I/O works at INIT time (logging), (c) the Name Registry is populated +
 * the EHCI 0c0320 node exists this early, and (d) the USB Family is already up (Gestalt('usb ')).
 *
 * r96 EARLY PORT-OWNERSHIP: it now does one small write set — claim_ports() sets the EHCI CONFIGFLAG
 * (route ports to EHCI) + powers each port, at INIT time, so an attached USB 2.0 drive is grabbed for
 * high-speed BEFORE Apple's companion USB 1.1 controllers can enumerate + mount it (the boot-attach
 * race that made a 1.1 mount appear then "unexpectedly disconnect" when our late driver took the
 * ports). It logs each port's PORTSC BEFORE the write = the evidence for whether the INIT beats the
 * companion. Still defensive: any missing property or absent node just falls through so boot proceeds
 * (recover with Shift-at-startup). ⚠ TEST ON THE EXPENDABLE LAB VOLUME first. Model: kanjitalk755/
 * macos9-usb-tablet. See [[reference_os9_init_resident_driver]] + [[feedback_os9_bootcode_testing_safety]].
 */
#include <MacTypes.h>
#include <Gestalt.h>
#include <NameRegistry.h>
#include <Files.h>
#include <Notification.h>
#include <CodeFragments.h>
#if BOOT_DM_VEHICLE
#include <Devices.h>        /* InstallDriverFromMemory (DriverLoaderLib) */
#include <DriverServices.h> /* IOCommandIsComplete (DriverServicesLib) — usb_disk.c's proven pairing */
#include <Resources.h>     /* Get1Resource — InstallMe re-reads our own 'PPC ' 128 */
#include <MixedMode.h>     /* NewRoutineDescriptor — REAL on PPC (the 68K no-op trap is 68K-only) */
#include <Memory.h>        /* NewPtrSys / BlockMoveData / GetZone / SetZone / SystemZone */
#endif

/* v2 (BOOT_ACTIVATE): the one call that brings the ROM driver up. Same signature the activator uses. */
typedef OSStatus (*LoadUIMProc)(RegEntryID *node);

/* ---- minimal disk logger (standalone — can't share the UIM's ehci_os_log). Writes CR-terminated
 * lines to "EHCIInit Boot Log" on the default (boot) volume, flushed each line so a boot hang still
 * leaves the last line on disk. ---- */
static short gLog = 0;
static void bopen(void)
{
    FSSpec sp;
    if (gLog) return;
    (void)FSMakeFSSpec(0, 0, "\pEHCIInit Boot Log", &sp);
    (void)FSpDelete(&sp);
    if (FSpCreate(&sp, 'ttxt', 'TEXT', 0) == noErr) (void)FSpOpenDF(&sp, fsRdWrPerm, &gLog);
}
static void blog(const char *s)
{
    long n = 0, z = 1;
    if (!gLog) bopen();
    if (gLog) { while (s[n]) n++; (void)FSWrite(gLog, &n, (Ptr)s);
        (void)FSWrite(gLog, &z, (Ptr)"\r"); (void)FlushVol(0, 0); }
}
static void blogx(const char *label, unsigned long v)
{
    char b[80]; int i = 0, j; static const char hx[] = "0123456789abcdef";
    while (label[i] && i < 60) { b[i] = label[i]; i++; }
    b[i++] = ' '; b[i++] = '0'; b[i++] = 'x';
    for (j = 28; j >= 0; j -= 4) b[i++] = hx[(v >> j) & 0xF];
    b[i] = 0; blog(b);
}

/* ---- r96 EARLY PORT-OWNERSHIP: claim the EHCI ports for USB 2.0 at INIT time, BEFORE the companion
 * (USB 1.1) controllers enumerate an attached drive. Constants/accessors copied verbatim from
 * src/ehci_regs.h; the AAPL,address BAR-parse from src/ehci_os.c map_registers(). This fragment is
 * standalone, so they are inlined rather than shared. ---- */
#define EHCI_CAPLENGTH      0x00
#define EHCI_HCSPARAMS      0x04
#define EHCI_HCS_N_PORTS(p) ((p) & 0x0F)
/* ★ v10m — PPC, HCSPARAMS bit 4: "Port Power Control". 1 = the ports have real power switches and
 * PORT_POWER is meaningful. 0 = ports are ALWAYS powered, PORT_POWER reads 1 and writes to it are
 * ignored by the hardware. If this reads 0 the whole power-cycle approach is dead on arrival and no
 * amount of reordering will change it — which is exactly why it is now logged. */
#define EHCI_HCS_PPC(p)     (((p) >> 4) & 0x1)
#define EHCI_CONFIGFLAG     0x40
#define EHCI_PORTSC(n)      (0x44 + (n) * 4)
#define EHCI_CONFIGFLAG_CF  0x00000001UL
#define EHCI_PORT_POWER     0x00001000UL
#define EHCI_PORT_OWNER     0x00002000UL   /* 1 => port owned by companion (1.1) */
#define EHCI_PORTSC_RW1C    0x0000002aUL   /* write-1-to-clear change bits — preserve on RMW */
#define kAddrEntryWords     5
#define AA_SPACE(hi)        (((hi) >> 24) & 0x3)   /* 2=mem32, 3=mem64 */

static UInt8 er8(volatile void *b, UInt32 o) { return *((volatile UInt8 *)b + o); }
static UInt32 er32(volatile void *b, UInt32 o)
{
    volatile UInt32 *p = (volatile UInt32 *)((volatile UInt8 *)b + o); UInt32 v;
    __asm__ __volatile__("lwbrx %0,0,%1" : "=r"(v) : "r"(p) : "memory"); return v;
}
static void ew32(volatile void *b, UInt32 o, UInt32 v)
{
    volatile UInt32 *p = (volatile UInt32 *)((volatile UInt8 *)b + o);
    __asm__ __volatile__("stwbrx %0,0,%1" : : "r"(v), "r"(p) : "memory");
    __asm__ __volatile__("eieio");
}

/* Read the controller's OS-mapped register base from the node (AAPL,address BAR), LOG each port's live
 * PORTSC (the timing evidence: CONNECT+OWNER set => the companion already has a device here, i.e. we
 * were too late and this write will yank it; CONNECT clear => no device yet, we won the race), then
 * set CONFIGFLAG=1 + clear OWNER + power each port (the same writes the UIM's ehci_hc_start does, just
 * early). Defensive: bails on any missing/oversized property so a bad card cannot hang boot. */
/* ★ Guarded at the DEFINITION, not just the call site, so "is the port claim really gone?" is answerable by
 * looking for its log strings in the shipped binary. With it merely uncalled, the strings stayed linked and
 * the check was ambiguous — a verification that cannot fail is not a verification. */
#if BOOT_CLAIM_PORTS
static void claim_ports(RegEntryID *node)
{
    UInt32 aa[48]; RegPropertyValueSize aasz = sizeof(aa);
    UInt32 la[16]; RegPropertyValueSize lasz = sizeof(la);
    volatile void *capBase, *opBase;
    UInt32 i, nEntries, memIdx = 0xFFFFFFFFUL, nPorts;

    if (RegistryPropertyGet(node, "assigned-addresses", aa, &aasz) != noErr) { blog("  claim: no assigned-addresses (skip)"); return; }
    nEntries = aasz / (kAddrEntryWords * sizeof(UInt32));
    for (i = 0; i < nEntries; i++) if (AA_SPACE(aa[i * kAddrEntryWords]) >= 2) { memIdx = i; break; }
    if (memIdx == 0xFFFFFFFFUL) { blog("  claim: no memory BAR (skip)"); return; }

    if (RegistryPropertyGet(node, "AAPL,address", la, &lasz) != noErr) { blog("  claim: no AAPL,address (skip)"); return; }
    if (memIdx * sizeof(UInt32) >= lasz) { blog("  claim: AAPL,address too short (skip)"); return; }
    capBase = (volatile void *)la[memIdx];
    blogx("  claim: capBase (logical)", (unsigned long)la[memIdx]);

    opBase = (volatile void *)((volatile UInt8 *)capBase + er8(capBase, EHCI_CAPLENGTH));
    nPorts = EHCI_HCS_N_PORTS(er32(capBase, EHCI_HCSPARAMS));
    blogx("  claim: nPorts", (unsigned long)nPorts);
    blogx("  claim: HCSPARAMS (raw)", (unsigned long)er32(capBase, EHCI_HCSPARAMS));
    blogx("  ★ claim: PPC (HCSPARAMS bit 4) — 1 = per-port power switches exist and PORT_POWER is REAL; "
          "0 = ports always powered, every PORT_POWER write is ignored and no power-cycle can work",
          (unsigned long)EHCI_HCS_PPC(er32(capBase, EHCI_HCSPARAMS)));

    for (i = 0; i < nPorts && i < 15; i++)               /* timing evidence: state BEFORE we touch it */
        blogx("  PORTSC[before] (b0=connect b13=companion-owner)", (unsigned long)er32(opBase, EHCI_PORTSC(i)));

    /* ★★★★★★ v8 — POWER OCCUPIED PORTS DOWN *BEFORE* THE ROUTING CHANGE, so Apple sees a clean DISCONNECT
     * instead of a device that stops answering mid-read.
     *
     * ⚠ THE MECHANISM, now that the evidence is in. With our extension REMOVED, Apple mounts this drive fine at
     * 1.1 - so the partitionless-HFS layout is readable and Apple is not confused by the medium. With our
     * extension PRESENT, the Disk Init Package reports "unreadable ... initialize?" AND the correct capacity
     * (3.7 GB = our READ CAPACITY of 0x780000 x 512), which means something completed a capacity read and then
     * failed a volume read. The Boot Log shows why that is possible: at INIT time every port reads Owner=1
     * (0x2000) - the power-on default, CONFIGFLAG=0 - so the ROM's own USB support has ALREADY enumerated this
     * drive before any extension runs. Our INIT then claims the port. Apple's mass-storage extension loads
     * LATER, during desktop startup - exactly when the dialog appears - tries to mount the device its stack
     * still believes in, finds we own it, and the read fails.
     *
     * ★ SO KILL THE STALE STATE CLEANLY. Powering the port down makes the device genuinely disappear, which is
     * an event Apple's stack handles silently for an UNMOUNTED device (its own removal alert is scoped to the
     * in-use case - see the n25 work). Nothing is left for its extension to try to mount. Then we take the
     * port and power it back up, and the device re-appears as ours.
     * ★ Occupancy is readable even while Apple owns the port: CCS reads 0 under Owner=1, but LINE STATE does
     * not - bits 11:10 give SE0 (00, empty), K (01, low speed) or J (10, FS/HS). The Boot Log confirms it:
     * 0x2800 = J on the drive and mouse, 0x2400 = K on the keyboard, 0x2000 = SE0 on the empty ports.
     * ⚠ HONEST UNCERTAINTY: the EHCI spec may ignore PORTSC writes on a port whose Owner bit is set, in which
     * case the power-down is a no-op and this changes nothing - that is the whole point of running it. It is
     * NOT the n11 hazard: n11 cleared Port ENABLE to force an ownership handoff on a live high-speed port.
     * This clears Port POWER on a device we are about to re-enumerate anyway, and every device here gets
     * power-cycled by the HCReset at bring-up regardless. */
#if BOOT_POWERCYCLE
    /* ★★★★★★ v10m — THE PROBE THAT DECIDES WHETHER THE CEDE-TIME FIX CAN EXIST AT ALL.
     *
     * ⚠⚠ v8's power-down above is a PROVEN NO-OP. Three boot logs (MDD, and both mini runs on 2026-08-11)
     * read back `PORTSC after power-down` = 0x2800 — BIT 12 STILL SET — because the write is issued while
     * Owner = 1 and CONFIGFLAG = 0, and the spec lets a controller ignore PORTSC writes on a companion-owned
     * port. So the "clean disconnect" it was written to manufacture has never once happened.
     *
     * ★ WHY THAT MATTERS, from the Phase C root cause: our cede DOES fire (PORTSC -> 0x3800) and the keyboard
     * stays dead anyway, because Apple's OHCI is handed a port whose device was already attached and already
     * reset — it never sees a CONNECT TRANSITION, so it never enumerates. Manufacturing a real connect is the
     * only known way out, and that means PORT_POWER has to actually work.
     *
     * ★★ THIS BUILD DOES NOT CLAIM TO BE THE FIX, AND MUST NOT BE READ AS ONE. The real fix belongs at CEDE
     * time, inside the driver (ehci_vhub.c), which means a new ROM — and the project's rule is that two
     * speculative ROMs were shipped on untested theories and both were wrong. So this INIT answers the one
     * load-bearing question for the cost of a single boot, BEFORE an m6 is built:
     *
     *      CAN WE POWER-CYCLE A ROOT PORT AT ALL ONCE WE GENUINELY OWN IT?
     *
     * It issues exactly the same write v8 does, but AFTER CONFIGFLAG = 1 and with OWNER CLEARED in the same
     * write, i.e. at the one moment ownership is unambiguous — and it logs the readback either side.
     * The readback IS the result:
     *      bit 12 CLEAR -> the write lands when we own the port. v8's placement was the whole problem, and
     *                      the cede-time power-cycle in m6 is viable. BUILD IT.
     *      bit 12 SET   -> PORT_POWER is not writable on this controller at all (check the PPC line logged
     *                      above: PPC = 0 means the ports have no power switches). The power-cycle route is
     *                      DEAD and m6 must manufacture the connect some other way. Do not build it.
     * Either answer is worth the boot, and neither can be obtained from the desk. */
    {
        UInt32 occ = 0UL;
        for (i = 0; i < nPorts && i < 15; i++)
            if (((er32(opBase, EHCI_PORTSC(i)) >> 10) & 3UL) != 0UL) occ |= (1UL << i);
        blogx("  v10m: occupied-port mask (line state != SE0) before anything is touched", (unsigned long)occ);

        ew32(opBase, EHCI_CONFIGFLAG, EHCI_CONFIGFLAG_CF);  /* route to EHCI FIRST, so the port is ours */
        blog("  v10m: CONFIGFLAG=1 written BEFORE the power write (this is the whole difference from v8)");

        for (i = 0; i < nPorts && i < 15; i++) {
            UInt32 pv;
            if (!(occ & (1UL << i))) continue;
            pv = er32(opBase, EHCI_PORTSC(i)) & ~EHCI_PORTSC_RW1C;
            pv &= ~EHCI_PORT_OWNER;                          /* ours, unambiguously */
            pv &= ~EHCI_PORT_POWER;                          /* ...and OFF */
            ew32(opBase, EHCI_PORTSC(i), pv);
            blogx("  v10m: power OFF written with OWNER CLEARED; port", (unsigned long)i);
            blogx("  ★★ v10m:   PORTSC readback — BIT 12 CLEAR = THE WRITE LANDED (v8's did not). THIS IS "
                  "THE RESULT OF THE RUN", (unsigned long)er32(opBase, EHCI_PORTSC(i)));
        }
        /* Same ~6-tick (100 ms) settle as v8, same hard iteration backstop: an INIT that spins forever is
         * unrecoverable without a Shift-boot. */
        {   UInt32 t0 = *(volatile UInt32 *)0x016AUL; long guard = 20000000L;
            while ((*(volatile UInt32 *)0x016AUL - t0) < 6UL && --guard > 0) { }
            blogx("  v10m: unpowered settle done; ticks elapsed (6 = full wait, 0 = tick counter not running)",
                  (unsigned long)(*(volatile UInt32 *)0x016AUL - t0));
        }
        for (i = 0; i < nPorts && i < 15; i++) {
            if (!(occ & (1UL << i))) continue;
            blogx("  v10m: PORTSC while unpowered (bit 0 CLEAR = the device really went away; bit 0 still "
                  "SET = it never dropped); port<<28|PORTSC",
                  ((unsigned long)i << 28) | (er32(opBase, EHCI_PORTSC(i)) & 0x0FFFFFFFUL));
        }
        /* Power back on happens in the standard claim loop below, which already sets OWNER clear + POWER. */
    }
#else
    for (i = 0; i < nPorts && i < 15; i++) {
        UInt32 pv = er32(opBase, EHCI_PORTSC(i));
        UInt32 ls = (pv >> 10) & 3UL;                 /* 00 SE0 (empty) / 01 K (LS) / 10 J (FS or HS) */
        if (ls != 0UL) {
            ew32(opBase, EHCI_PORTSC(i), (pv & ~EHCI_PORTSC_RW1C) & ~EHCI_PORT_POWER);
            blogx("  v8: powered DOWN an occupied port so Apple sees a clean disconnect; port<<8|lineState",
                  ((unsigned long)i << 8) | (unsigned long)ls);
            blogx("  v8:   PORTSC after power-down (0x1000 clear = really off)",
                  (unsigned long)er32(opBase, EHCI_PORTSC(i)));
        }
    }
    /* Let the companion notice the removal: ~6 ticks (100 ms). Spin on lowmem Ticks, which advances at task
     * level during the INIT parade, with a hard iteration backstop so a stopped tick counter can never hang
     * the boot - an INIT that spins forever is unrecoverable without a Shift-boot. */
    {   UInt32 t0 = *(volatile UInt32 *)0x016AUL; long guard = 20000000L;
        while ((*(volatile UInt32 *)0x016AUL - t0) < 6UL && --guard > 0) { }
        blogx("  v8: settle wait done; ticks elapsed (6 = full wait, 0 = tick counter not running)",
              (unsigned long)(*(volatile UInt32 *)0x016AUL - t0));
    }
    ew32(opBase, EHCI_CONFIGFLAG, EHCI_CONFIGFLAG_CF);   /* route all root ports to EHCI (2.0) */
#endif
    for (i = 0; i < nPorts && i < 15; i++) {
        UInt32 pv = er32(opBase, EHCI_PORTSC(i)) & ~EHCI_PORTSC_RW1C;
        /* ★★★★ v5 (h33 family) — NEVER CLAIM A LOW-SPEED DEVICE'S PORT. K-state on the line (PORTSC bits
         * 11:10 = 01) = a low-speed device is attached, and EHCI can never drive one on a root port. Claiming
         * it here is what froze the keyboard mid-parade (T1's 3-4 s) and is the prime suspect for the v3-T4
         * Finder crash. The line state reads through Owner=1, but Apple may be actively DRIVING the device
         * and an LS transfer can hold the line off-K longer than consecutive µs-reads — so sample across a
         * window wider than any LS transfer: 16 samples, ~100 µs apart. ANY K ⇒ low-speed ⇒ skip (an FS/HS
         * idle line never shows K). A skipped port stays Apple's; the keyboard never notices any of this. */
        /* ★★★★★★ v7 — CLAIM AND POWER *EVERY* OCCUPIED PORT. THE K-FILTER IS GONE FROM THE INIT.
         *
         * ⚠ v5/v6 skipped a low-speed port here, meaning to protect the keyboard. It did the opposite, and the
         * Boot Log proved it: CONFIGFLAG=1 (the line above) is GLOBAL — it routes every port to EHCI and
         * strips the companion's ownership regardless of intent — so "skip" only skipped the per-port write,
         * which is where PORT_POWER is set. The keyboard's port came out 0x0400: routed to us, UNPOWERED, and
         * deliberately undriven by us. Dead keyboard, no hardware fault.
         *
         * ★★ AND THE THREE-RUN COMPARISON SAYS CLAIMING IS RIGHT, not merely harmless:
         *      v3 (claim everything, powered)  -> keyboard WORKED (Apple met it after our bring-up)
         *      v6 (claim, K-filter skip)       -> DEAD (unpowered limbo)
         *      v4 (no claim at all)            -> DEAD (Apple enumerated it during the parade, then HCReset
         *                                         power-cycled the device out from under Apple's address)
         * The keyboard works when Apple enumerates it AFTER our bring-up and dies when Apple enumerates it
         * BEFORE — because HCReset power-cycles every port on this card, the device forgets its USB address,
         * and Apple never observes a clean disconnect/reconnect across that outage. Claiming the port here
         * keeps Apple away until we hand it over deliberately, which is a FIRST-TIME connect for Apple.
         *
         * ★ SO THE DIVISION OF LABOUR IS: the INIT claims everything occupied (Apple enumerates nothing on
         * this card during the parade); ehci_hc_start (h38) then cedes ONLY the ports we cannot drive, using
         * the K-test where that decision actually belongs. The speed test never belonged at INIT time — at
         * INIT we have no reason to treat any occupied port differently, and every reason not to. */
        {   int k = 0, s, d;
            for (s = 0; s < 16; s++) {
                if (((er32(opBase, EHCI_PORTSC(i)) >> 10) & 3) == 1) k++;
                for (d = 0; d < 100; d++) (void)er32(opBase, EHCI_PORTSC(i));   /* ~100 µs spacer */
            }
            /* Logged only — it is genuinely useful evidence (it is how we identified the keyboard's port and
             * the real K distributions) and it now decides NOTHING here. h38 makes the cede decision. */
            blogx("  v7: K-samples of 16 (LOGGED ONLY now; h38 decides the cede at bring-up); port<<8|K",
                  ((unsigned long)i << 8) | (unsigned long)k);
        }
        pv &= ~EHCI_PORT_OWNER; pv |= EHCI_PORT_POWER;   /* claim for EHCI + power on */
        ew32(opBase, EHCI_PORTSC(i), pv);
    }
    blog("  claim: CONFIGFLAG=1 + ports claimed for EHCI");
    for (i = 0; i < nPorts && i < 15; i++)
        blogx("  PORTSC[after]", (unsigned long)er32(opBase, EHCI_PORTSC(i)));
}
#endif /* BOOT_CLAIM_PORTS */

/* Entry point called from the 68K INIT via Mixed Mode. void(void) so the Mixed Mode ProcInfo is
 * trivial (kCStackBased). Exported via bootmain.exp. */
void BootMain(void)
{
    long usbAttr = 0; OSErr ge; OSStatus err;
    RegEntryIter iter; RegEntryID node; Boolean done = false;
    UInt32 wantClass = 0x000c0320UL;

    blog("=== EHCIInit R2b-1 BootMain: PowerPC fragment RUNNING AT BOOT (68K INIT -> Mixed Mode) ===");

    /* (d) USB Family readiness — research says the USB Support 'expt' loads before the INIT parade. */
    ge = Gestalt('usb ', &usbAttr);
    blogx("Gestalt('usb ') err (0=ok)", (unsigned long)(long)ge);
    blogx("  usb attr (bit0 = USB present)", (unsigned long)usbAttr);

    /* (c) Name Registry populated this early? Find the EHCI controller node (class 0c0320). */
    err = RegistryEntryIterateCreate(&iter);
    blogx("RegistryEntryIterateCreate err", (unsigned long)(long)err);
    if (err == noErr) {
        err = RegistryEntrySearch(&iter, kRegIterDescendants, &node, &done,
                                  "class-code", &wantClass, sizeof(wantClass));
        RegistryEntryIterateDispose(&iter);
        blogx("RegistryEntrySearch EHCI(0c0320) err", (unsigned long)(long)err);
        /* FOUND iff err==noErr — matching the proven app leg (ehci_os.c / ehci_probe.c). 'done' is
         * the iteration-EXHAUSTED flag, NOT a found flag (it is false when a match is returned), so
         * the old `err==noErr && done` mislabeled a real hit as "NOT found" (r2b2 run: err was 0). */
        if (err == noErr) {
            blog("  *** EHCI node FOUND at boot ***");
#if BOOT_CLAIM_PORTS
            claim_ports(&node);   /* r96: grab the ports for USB 2.0 before the 1.1 companion enumerates */
#else
            /* ★★★★★ DISABLED BY DEFAULT (2026-08-08). r96 added an early claim to win the race against
             * Apple's 1.1 companion, and at INIT time that means writing CONFIGFLAG and every port's PORTSC
             * **before any driver exists to service those ports**.
             *
             * ⚠ THAT IS THE HAZARD WE JUST DIAGNOSED, one stage earlier. n4j quitting left ports claimed by a
             * driver that no longer existed; the device then went to the 1.1 companion and the same medium
             * ended up mounted twice, 2.0 and 1.1, with the File Manager reporting a damaged disk. An early
             * claim with no driver behind it is the same state, reached from the other direction — and for
             * the VEHICLE-PROOF stage there is no activation at all, so the ports would stay claimed and
             * unserviced for the whole session.
             * ⚠ Second reason: a PORTSC write is the one thing this controller has punished before — n11
             * cleared Port Enable to force a handoff and killed USB until reboot.
             * ★ It is also unnecessary: the driver's own bring-up (ehci_hc_start) performs exactly these
             * writes when it comes up, which is where they belong. The early claim optimises a race we have
             * never measured, and in a test whose entire purpose is to prove the VEHICLE it is a second
             * variable. Turn it on deliberately, with -DBOOT_CLAIM_PORTS=1, once the vehicle is proven and
             * the race is worth measuring. */
            blog("  (early port claim DISABLED: no driver is up yet to service claimed ports)");
#endif
        }
        else              blog("  EHCI node NOT found at boot");
    }

    blog("=== BootMain done — if you can read this, the RESIDENT VEHICLE WORKS at boot ===");
}

/* ============================================================================================
 * R2b-2 residency + deferred-task proof. The 68K INIT holds our CFM connection open (this
 * fragment stays resident) and NMInstall's a Notification whose response proc is THIS function.
 * The Notification Manager delivers it at TASK level during SystemTask/WaitNextEvent — i.e. once
 * the Finder's event loop runs, post-boot, when the USB Family is up and the File Manager is safe
 * to call. (A Time Manager task fires at INTERRUPT level, where FSWrite/Gestalt/CFM are all
 * unsafe — hence NM, not a raw timer, for anything that touches those.) If "EHCIInit NM Log"
 * appears with these lines, residency + deferred task-level execution BOTH work — the backbone
 * for R2b-3, where LoadUIMForEntry + the mount orchestration will go right here. NMRemove's self.
 * ============================================================================================ */
static short gNM = 0;
static void nmopen(void)
{
    FSSpec sp;
    if (gNM) return;
    (void)FSMakeFSSpec(0, 0, "\pEHCIInit NM Log", &sp);
    (void)FSpDelete(&sp);
    if (FSpCreate(&sp, 'ttxt', 'TEXT', 0) == noErr) (void)FSpOpenDF(&sp, fsRdWrPerm, &gNM);
}
static void nmlog(const char *s)
{
    long n = 0, z = 1;
    if (!gNM) nmopen();
    if (gNM) { while (s[n]) n++; (void)FSWrite(gNM, &n, (Ptr)s);
        (void)FSWrite(gNM, &z, (Ptr)"\r"); (void)FlushVol(0, 0); }
}
static void nmlogx(const char *label, unsigned long v)
{
    char b[80]; int i = 0, j; static const char hx[] = "0123456789abcdef";
    while (label[i] && i < 60) { b[i] = label[i]; i++; }
    b[i++] = ' '; b[i++] = '0'; b[i++] = 'x';
    for (j = 28; j >= 0; j -= 4) b[i++] = hx[(v >> j) & 0xF];
    b[i] = 0; nmlog(b);
}

/* Called by the Notification Manager at TASK level, post-boot. Signature matches NMProcPtr; it is
 * invoked through a real routine descriptor the 68K INIT built with NewRoutineDescriptorTrap, so
 * PPC globals (TOC) are valid — and this only runs at all if the fragment is still resident. */
void ResidentNMResponse(NMRecPtr nmReqPtr)
{
    long usbAttr = 0; OSErr ge;
    CFragConnectionID conn = (CFragConnectionID)0; Ptr addr; Str255 errStr; OSErr le;
    RegEntryIter iter; RegEntryID node; Boolean done = false; OSStatus rs;
    UInt32 wantClass = 0x000c0320UL;

    nmlog("=== ResidentNMResponse: FIRED at TASK level post-boot — RESIDENT FRAGMENT IS ALIVE ===");
    nmlogx("  nmReqPtr", (unsigned long)nmReqPtr);
    if (nmReqPtr) nmlogx("  nmRefCon (expect 0x45727362 'Ersb')", (unsigned long)nmReqPtr->nmRefCon);

    /* USB Family readiness — safe at task level; would be unsafe from a Time Manager interrupt. */
    ge = Gestalt('usb ', &usbAttr);
    nmlogx("Gestalt('usb ') err", (unsigned long)(long)ge);
    nmlogx("  usb attr", (unsigned long)usbAttr);
    le = GetSharedLibrary("\pUSBServicesLib", kPowerPCCFragArch, kReferenceCFrag, &conn, &addr, errStr);
    nmlogx("GetSharedLibrary(USBServicesLib) err (0=USB Family up)", (unsigned long)(long)le);
    if (le == noErr) (void)CloseConnection(&conn);   /* probing presence only */

    /* Is our EHCI controller node still there at task level? (fixed found-logic: found iff err==0) */
    rs = RegistryEntryIterateCreate(&iter);
    if (rs == noErr) {
        rs = RegistryEntrySearch(&iter, kRegIterDescendants, &node, &done,
                                 "class-code", &wantClass, sizeof(wantClass));
        RegistryEntryIterateDispose(&iter);
        nmlogx("RegistryEntrySearch EHCI err", (unsigned long)(long)rs);
        if (rs == noErr) nmlog("  *** EHCI node FOUND at task level ***");
        else             nmlog("  EHCI node not found at task level");
    }

#if BOOT_ACTIVATE
    /* ★★★★★★ R2b-3 / v2 — THE ACTIVATION, finally in the place this comment block reserved for it in July.
     *
     * This is the last piece of app-less: h26 moved the driver's task-level work onto its own
     * NMInstall(nmStr=0) pump, and n4j reduced the app to nothing but this one call. Making it from here
     * removes the app entirely.
     *
     * ★ WHY HERE AND NOT IN BootMain (i.e. not during the INIT parade): LoadUIMForEntry needs Apple's USB
     * Family to be LOADED — we resolve it with kReferenceCFrag, which references an already-present library
     * and fails cleanly if it is not. At INIT time that is a coin toss on Extensions-folder load order. The
     * Notification Manager response runs post-boot, at task level, in the Finder's event loop, which is
     * exactly when the USL is guaranteed up and the File Manager is safe — the same reasoning that made this
     * function an nmResp in the first place.
     *
     * ⚠ USBFamilyExpertLib, NOT USBServicesLib. The probe above tests USBServicesLib for presence; the symbol
     * lives in USBFamilyExpertLib, which is where the proven activator resolves it from. Getting this wrong
     * would look exactly like "the USL is not up".
     *
     * ★ IF THE USL IS NOT UP YET, WE RETRY rather than give up: re-arm the same NM request and try again on a
     * later pass, bounded. Unbounded would spin at event-loop rate forever on a machine with no USB Family. */
    {
        static int  sTries = 0;
        static int  sDone  = 0;
        CFragConnectionID xc; Ptr xa; Str255 xe; Ptr sym; CFragSymbolClass cls;
        LoadUIMProc loadUIM = 0;

        if (sDone) { nmlog("v2: already activated — nothing to do"); if (nmReqPtr) (void)NMRemove(nmReqPtr); return; }

        if (GetSharedLibrary("\pUSBFamilyExpertLib", kPowerPCCFragArch, kReferenceCFrag, &xc, &xa, xe) == noErr) {
            if (FindSymbol(xc, "\pLoadUIMForEntry", &sym, &cls) == noErr) loadUIM = (LoadUIMProc)sym;
            else nmlog("v2: USBFamilyExpertLib is up but LoadUIMForEntry did NOT resolve");
        }

        if (loadUIM && rs == noErr) {
            OSStatus lst;
            nmlogx("v2: resolved LoadUIMForEntry at", (unsigned long)loadUIM);
            nmlog("v2: calling LoadUIMForEntry — THE ROM DRIVER COMES UP NOW, WITH NO APPLICATION");
            lst = loadUIM(&node);
            /* ★ v6: LOG THE RETURN STATUS. `(void)loadUIM(...)` made a USL rejection indistinguishable from
             * success — "calling … returned" read identically either way, and a zero-log run could not say
             * whether activation failed or never happened. 0 = the driver came up (its own banner follows). */
            nmlogx("v2: LoadUIMForEntry STATUS (0 = driver up; nonzero = USL REJECTED it)", (unsigned long)lst);
            nmlog("=== v2: LoadUIMForEntry returned. From here the driver is self-servicing and h26's own NM");
            nmlog("=== pump carries its task-level work. If a drive now mounts, THIS STACK IS APP-LESS. ===");
            sDone = 1;
            if (nmReqPtr) (void)NMRemove(nmReqPtr);
            return;
        }

        /* Not ready (or no node). Re-arm and come back, bounded. */
        sTries++;
        nmlogx("v2: not ready yet (USL up? node found?) — re-arming; attempt", (unsigned long)sTries);
        if (nmReqPtr) {
            (void)NMRemove(nmReqPtr);
            if (sTries < 120) {                      /* ~120 event-loop passes, then stop trying */
                if (NMInstall(nmReqPtr) != noErr)
                    nmlog("v2: re-arm FAILED — activation will not happen this boot");
            } else {
                nmlog("v2: giving up after 120 attempts — launch the activator by hand this boot");
            }
        }
        return;
    }
#endif

    nmlog("=== ResidentNMResponse done; removing our NM request ===");
    if (nmReqPtr) (void)NMRemove(nmReqPtr);
}

#if BOOT_DM_VEHICLE
/* ============================================================================================
 * v9-dm — THE DM-RESIDENT VEHICLE (2026-08-14). The r2b2R "hold the CFM connection" residency
 * shipped, and it is the boot-window crash family: an INIT's context does not survive boot, so
 * the fragment copy (code + data + import TVectors) sits in an evaporating arena. Three boots
 * paid for the proof — the NM descriptor recycled into the Helvetica FOND (01E7C4A0 inside
 * 01E7B3E0..01E7D6F4, PC executing font bytes), then, with the descriptor SysZone-forced, the
 * SAME arena ate the fragment itself (LR 01E7A00C, R12 TVector 01E7A4C4 -> garbage 80410014).
 * Our own reference had already proven this failure AND its fix on hardware (2026-07-08,
 * [[reference_os9_init_resident_driver]]): "a held connID is reaped -> dangles -> crash on
 * first post-boot use"; the PROVEN pattern is InstallDriverFromMemory — the Device Manager owns
 * the copy, resident independent of the dying INIT. This section is that pattern, ported around
 * the validated v8-era claim/activation logic above, from the R2b-3 vehicle (ehci_resident.c).
 *
 * Flow: 68K INIT loads a TRANSIENT copy of this fragment (any zone — it is allowed to die) and
 * Mixed-Mode-calls InstallMe once. InstallMe runs BootMain() synchronously (the proven early
 * port claim), then NewPtrSys-copies our own PEF and InstallDriverFromMemory's it. The DM
 * initializes the INSTALLED copy — DoDriverIO(kInitialize) below, running in DM-owned memory —
 * which arms the activation NM with a SysZone descriptor pointing at the INSTALLED instance's
 * ResidentNMResponse. Everything the Notification Manager will ever call outlives boot.
 * ============================================================================================ */
typedef struct { UInt8 len; char s[31]; } DStr31;
typedef struct { UInt8 a, b, c, d; } DNumVer;
typedef struct { OSType category, type; DNumVer version; } DServiceInfo;
typedef struct {
    OSType sig; UInt32 descVersion; DStr31 nameInfoStr; DNumVer typeVersion;
    UInt32 driverRuntime; DStr31 driverName; UInt32 reserved[8];
    UInt32 nServices; DServiceInfo service0;
} ResidentDriverDesc;
ResidentDriverDesc TheDriverDescription = {
    0x6d74656aUL /* 'mtej' */, 0,
    { 9, "ehci,actv" }, { 1, 0, 0x80, 0 },
    0x00000003UL,
    { 12, "EHCIActivate" }, { 0,0,0,0,0,0,0,0 }, 1,
    { 0x6e647276UL /*'ndrv'*/, 0x6d697363UL /*'misc'*/, { 1, 0, 0x80, 0 } }
};

static OSErr find_ehci_node(RegEntryID *node)
{
    RegEntryIter it; Boolean done = false; OSStatus e;
    UInt32 wantClass = 0x000c0320UL;
    e = RegistryEntryIterateCreate(&it);
    if (e != noErr) return (OSErr)e;
    e = RegistryEntrySearch(&it, kRegIterDescendants, node, &done,
                            "class-code", &wantClass, sizeof(wantClass));
    RegistryEntryIterateDispose(&it);
    return (OSErr)e;
}

/* kInitialize runs in the INSTALLED (DM-owned) copy: `ResidentNMResponse` here resolves through
 * THIS instance's TOC/TVector — memory the Device Manager keeps for the life of the boot. The
 * descriptor and NMRec are forced into the system zone (the h90 idiom: "the RD must outlive
 * whichever app hosts boot"). */
static void arm_activation_nm(void)
{
    NMRecPtr nm; NMUPP upp;
    THz oldZone = GetZone();
    SetZone(SystemZone());
    upp = NewNMUPP((NMProcPtr)ResidentNMResponse);
    SetZone(oldZone);
    blogx("  v9-dm: NM response descriptor (SysZone; points into the DM-owned copy)", (unsigned long)upp);
    if (upp == NULL) { blog("  v9-dm: NewNMUPP FAILED — activation will not happen this boot"); return; }
    nm = (NMRecPtr)NewPtrSysClear((Size)sizeof(NMRec));
    blogx("  v9-dm: NMRec (system heap)", (unsigned long)nm);
    if (nm == NULL) return;
    nm->qType    = nmType;
    nm->nmResp   = upp;
    nm->nmRefCon = 0x45727362UL;   /* 'Ersb' */
    blogx("  v9-dm: NMInstall err (0=ok; response fires post-boot at task level)",
          (unsigned long)(long)NMInstall(nm));
}

/* Resident driver entry (main == DoDriverIO via patch-pef-main.py in the PEF rule). */
OSErr DoDriverIO(AddressSpaceID spaceID, IOCommandID cmdID,
                 IOCommandContents contents, IOCommandCode code, IOCommandKind kind)
{
    OSErr err = noErr;
    (void)spaceID; (void)contents;
    switch (code) {
        case kInitializeCommand:
            blog("=== v9-dm DoDriverIO kInitialize: DM-RESIDENT — arming the activation NM ===");
            arm_activation_nm();
            err = noErr; break;
        case kOpenCommand: case kCloseCommand:
        case kFinalizeCommand: case kSupersededCommand:
        case kReplaceCommand: case kKillIOCommand:  err = noErr; break;
        case kReadCommand: case kWriteCommand:      err = ioErr; break;
        case kControlCommand: err = noErr; break;
        case kStatusCommand:  err = statusErr; break;
        default:              err = paramErr; break;
    }
    if (kind & kImmediateIOCommandKind) return err;             /* the h91 native completion contract */
    return (OSErr)IOCommandIsComplete(cmdID, (short)err);
}

/* ★★★★ v10 — THE PARCEL-PRESENCE GATE. On 2026-08-14 this extension, installed against a STOCK ROM
 * (no EHCI parcel), froze the B&W's boot: with no driver bound to the node, `AAPL,address` is not a
 * live parade-time mapping, and claim_ports' first MMIO read through it killed the machine. The
 * community WILL install the extension without the ROM (wrong machine, missed step), so before
 * touching ANY hardware we require the node to carry the `driver,AAPL,MacOS,PowerPC` property — which
 * exists precisely when our ROM's parcel has bound the driver. Absent => one log line, do NOTHING:
 * no claim, no install, no NM (kInitialize never runs, so nothing is ever armed). Fail-open for boot,
 * fail-closed for hardware. */
static int rom_driver_bound(RegEntryID *node)
{
    RegPropertyValueSize sz = 0;
    OSErr e = RegistryPropertyGetSize(node, "driver,AAPL,MacOS,PowerPC", &sz);
    blogx("  v10 gate: driver,AAPL,MacOS,PowerPC on the EHCI node — err (0 = bound)", (unsigned long)(long)e);
    if (e == noErr) blogx("  v10 gate: bound driver property size", (unsigned long)sz);
    return (e == noErr && sz > 0);
}

/* Synchronous, called ONCE by the 68K INIT via Mixed Mode from the transient copy. Claims the
 * ports (the validated v8/v10m logic in BootMain), then installs the RESIDENT copy of this same
 * PEF under the Device Manager. After this returns, the transient copy and its whole zone may
 * die — nothing post-boot points into them. */
void InstallMe(void)
{
    Handle h; Ptr img; Size sz; RegEntryID node; DriverRefNum ref = 0; OSErr err;

    /* v10: the gate runs BEFORE BootMain's claim — the node search is duplicated deliberately so the
     * gate cannot be bypassed by a BootMain code path change. */
    {   RegEntryID gnode;
        if (find_ehci_node(&gnode) != noErr) {
            blog("=== v10 gate: no EHCI controller in the Name Registry — extension doing nothing ===");
            return;
        }
        if (!rom_driver_bound(&gnode)) {
            blog("=== v10 gate: EHCI node has NO bound ROM driver (the USB2 Mac OS ROM is not installed).");
            blog("=== This extension pairs with that ROM; doing nothing so the machine boots normally. ===");
            return;
        }
    }

    BootMain();                                     /* early port claim + boot-time evidence log */

    blog("=== v9-dm InstallMe: installing the DM-resident copy (InstallDriverFromMemory) ===");
    h = Get1Resource('PPC ', 128);
    if (h == NULL) { blog("  v9-dm: Get1Resource('PPC ',128)==NULL — cannot install"); return; }
    sz = GetHandleSize(h);
    blogx("  v9-dm: PEF size", (unsigned long)sz);
    img = NewPtrSys(sz);
    if (img == NULL) { blog("  v9-dm: NewPtrSys FAILED — cannot install"); return; }
    HLock(h); BlockMoveData(*h, img, sz); HUnlock(h);
    if (find_ehci_node(&node) != noErr) { blog("  v9-dm: no EHCI node — not installing"); return; }
    err = (OSErr)InstallDriverFromMemory(img, (long)sz, "\pEHCIActivate", &node, 48, 127, &ref);
    blogx("  v9-dm: InstallDriverFromMemory err (0 = resident; kInitialize has armed the NM)",
          (unsigned long)(long)err);
    blogx("  v9-dm: driver refNum", (unsigned long)(long)ref);
}
#endif /* BOOT_DM_VEHICLE */
