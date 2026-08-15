/* ★ THE ROM BUILD TAG. Bump this on EVERY ROM build; it is logged as the first line of uimInitialize and is
 * the ONLY way a log can say which driver actually booted (the Get-Info route is closed — a `vers` (1) in a
 * Mac OS ROM stops the machine booting). Keep it identical to the tag in the .hqx filename. */
#define EHCI_BUILD_TAG "b16rel" /* h97rel = the 2026-08-15 RELEASE of the b15 driver for Mini + MDD
                                * (LEVEL0 PAINT0, interrupts). b16rel = the SAME SOURCE at the B&W
                                * release config (LEVEL0 PAINT0 FORCE_POLLED, lzss'd parcel). Driver
                                * delta over h96rel/b10rel is exactly the b15 Control-21/22 fix below,
                                * hardware-validated on the B&W (per-slot "USB 2.0, drive N (v1.0)" in
                                * DFA; skip reclassified known-cosmetic, workaround documented).
                                *
                                * b15 = kDriveIcon/kMediaIcon (Control 21/22) FILLED — the actual DFA
                                * garbage-string mechanism. b14 proved by experiment that kdgFlush +
                                * kDriveInfo answers change NOTHING in Disk First Aid; the b14 ring
                                * census then showed DFA never asks kdgVersion, and the Apple-stack
                                * control (HIGHSPEED on the built-in USB 1.1 port) rendered "USB
                                * (v2.1.1)" — a WHERE-STRING the driver itself composes, returned with
                                * a 256-byte ICN# through Control 21. Our blanket noErr acked 21/22
                                * with csParam UNFILLED -> DFA rendered bytes past its own leftover
                                * stack long: deterministic garbage, identical across drives/builds/
                                * machines. Fix in usb_disk.c: per-slot {ICN# + "USB 2.0, drive N
                                * (v1.0)"} blob served for 21/22; per-slot DISTINCT strings also target
                                * the one-of-two-drives-listed skip (identical responses on one refNum
                                * look like one physical drive). VERIFY: DFA lists BOTH volumes, each
                                * with a clean per-drive location line + our icon. b13/b13b/b14 were
                                * the instrument ladder (selector ring; see docs/BW-TEST-CARD-b2.md).
                                *
                                * b2 = the B&W G3 retail-9.2.2-base PROVING build: the h96 driver
                                * (both 2026-08-14 fixes in) at LEVEL1 PAINT1, on the retail ROM base.
                                * Supersedes b1 UNBOOTED — b1 carried the h91 driver, which predates
                                * both the h94 rude-removal UAF fix and the h96 SysZone UPPs; a third
                                * machine never runs known-buggy bytes. Pair with the v9-dm MDD
                                * extension (8.2). See docs/BW-TEST-CARD-b1.md (updated for b2).
                                *
                                * h96rel = the 2026-08-14 RELEASE build of h96 (LEVEL0 PAINT0, one
                                * driver, both machines). Ships beside extension v8.2/v11.1 (the v9-dm
                                * DM-resident vehicle). The two corrupters of the 8/13-8/14 hunt are both
                                * closed and hardware-validated: the rude-removal UAF (h94, confirmed by
                                * efficacy) and the evaporating parade arena (driver-side SysZone UPPs in
                                * h96 + the extension's v9-dm vehicle; both machines cold-boot clean).
                                *
                                * h96 = THE EVAPORATING-DESCRIPTOR FIX (the boot-window crash family).
                                * The h94rel cold-boot crash was SOLVED from one StdLog + the INIT's own
                                * log: the extension's NM response descriptor (printed at 0x01E7C4A0 at
                                * INIT time) sat INSIDE the exact block MacsBug showed recycled as the
                                * Helvetica FOND (01E7B3E0-01E7D6F4) — every heap OK, a stale-pointer
                                * wild jump through SystemTask (KeyboardSystemTaskPatch frame), 28s into
                                * boot, PC executing font bytes. NewRoutineDescriptorTrap/NewNMUPP
                                * allocate in TheZone, and the boot-era zones those calls ran in do not
                                * survive; the freed bytes keep working until the block is reused, which
                                * is why the family (eject-crash session, h92 warm-restart crash, this)
                                * only ever struck at desktop-load time and never under deliberate
                                * hunting. The app era (h53, weeks clean) had NO descriptors — the
                                * corruption era began exactly at the extension transition. h90 already
                                * carried the rule AND the idiom ("the RD must outlive whichever app
                                * hosts boot" — SetZone(SystemZone())); the lesson existed in one place
                                * and was not consulted at the other allocation sites. NOW FIXED AT ALL
                                * THREE: the driver's pump UPP (gNMTUpp — the most-called pointer in the
                                * driver), the shutdown UPP (gShutUPP — called at restart), and the
                                * extension's NM descriptor (ehci_init_dbg.c v9, ships as extension
                                * vers 8.1/11.1). VERIFY: the INIT log's descriptor line must print a
                                * LOW system-heap address (not 0x01Exxxxx), then repeated cold boots +
                                * normal use; the h52/h94 counters and canaries ride along (LEVEL1
                                * PAINT1). Session B's zone damage remains separately attributed to the
                                * h94 UAF (fixed, efficacy-proven); this is the SECOND mechanism.
                                *
                                * h95 = h94 CODE BYTE-IDENTICAL, DIAGNOSTIC CONFIG (LEVEL1 PAINT1).
                                * Exists because the h94rel COLD BOOT crashed at desktop load on the MDD
                                * (Unimplemented Instruction at 01E7D0EA, PC inside the Helvetica FOND —
                                * executing font data; Finder drawing the desktop). Cold boot + fixed
                                * driver + zero removals = NOT the h94 UAF; there is a SECOND corruption
                                * mechanism in the BOOT WINDOW, and the h92-era warm-restart crash
                                * reattributes to it. The release ROM was blind (LEVEL0/PAINT0); this
                                * recut turns every instrument back on with ZERO code delta, so repeated
                                * cold boots can (a) establish the base rate and (b) carry the h93 DM
                                * ring + h94 counters + canaries + h52 swap watch into the crash window.
                                * One variable: the config. Do not add code to this build.
                                *
                                * h94rel = the 2026-08-14 RELEASE build of h94 (LEVEL0 PAINT0, one
                                * driver for both machines). h94's fix CONFIRMED BY EFFICACY on the MDD:
                                * a deliberate settle-window yank caught a live USL client transfer
                                * (gH94Retired 1), the funnel retired it, gH94Orphaned 0, heap clean;
                                * plus 3 mid-wire block-path catches (gQhHalted) all recovered clean
                                * across the batteries. The ASP 15x "residue" was RESOLVED BY ANALYSIS,
                                * not code: it is the USL's task-level idle poll slowing ~8ms -> ~120ms
                                * after ASP; all real-time work is SIH-driven and unaffected; deferred
                                * completions worst-case ~120ms. No behavior change from h94.
                                *
                                * h94 = THE RUDE-REMOVAL USE-AFTER-FREE FIX. The h93 run ANSWERED the h92
                                * question and overturned its correlation: 216 DoDriverIO calls across
                                * three sessions (ASP run deliberately, repeatedly), EVERY
                                * IOCommandIsComplete verdict noErr, and two of those burst-carrying
                                * sessions kept a CLEAN heap — the dispatch path is exonerated by
                                * experiment, and the "72-call burst" is just ASP's normal scan shape
                                * (18 x Open/Control/Status/Close). What both corrupt sessions actually
                                * shared, and every clean one lacked, was a RUDE REMOVAL with USL client
                                * transfers still in flight (the freshly-inserted SanDisk yanked
                                * mid-settle; the boot-era leg pull). MECHANISM (found in code, matches
                                * the MacsBug scribble at 01C20350 exactly): this hardware's USL NEVER
                                * calls AbortPipe/DeletePipe — slots 18/19, zero calls in every banked
                                * log — it just frees its pipe/transfer structures on removal, while our
                                * ENGINE slot/FIFO still hold that client's completion UPP, pipe pointer
                                * and destination buffer; the orphaned transfer times out (-6640) and
                                * down_reap wrote IN-data into the freed buffer and fired the UPP with
                                * the freed pipe. Idle-drive yanks have nothing in flight, hence every
                                * clean hardening pass. THE FIX (ehci_vhub.c, three layers, flag work
                                * only): (1) usl_retire_device at the disconnect handler nulls the client
                                * pointers and queues substitute completions (status -6640) through
                                * gComplQ, and gH94Hold parks the root-hub status-change report until
                                * compl_drain delivers them at task level — clients complete BEFORE the
                                * USL learns and frees; (2) down_pump refuses issues for dead devices
                                * (h81's lesson, USL-side); (3) a reap-side orphan gate suppresses client
                                * writes if the funnel ever misses (gH94Orphaned MUST stay 0). Dead-addr
                                * ring FAILS OPEN (cleared on any connect + create_bulk). VERIFY: insert
                                * a drive, yank it MID-SETTLE (the h93 repro), repeatedly; expect
                                * gH94Retired > 0, alert still posts, hc all CLEAN, gH94Orphaned 0.
                                * Ships as the MDD diagnostic ROM (LEVEL1 PAINT1).
                                *
                                * h93 = h92 + THE DEVICE-MANAGER CALL HISTORY. The h92 soak produced the
                                * tightest correlation of the hunt: sessions where something dispatches
                                * through our USL-made, h90-repaired unit entry CORRUPT the system heap
                                * (ASP afternoon session; the 72-call burst boot, which also saw unit slot
                                * 2's driver pointer swap in the same window); sessions with ZERO such
                                * traffic stay clean (the soak, the cold boot). Canaries stayed 0
                                * throughout and the crash PC sat 2 MB from our blocks, so our DMA is
                                * exonerated -- the suspect is the DISPATCH/COMPLETION path itself, most
                                * concretely IOCommandIsComplete against an entry whose queued-command
                                * bookkeeping the USL never finished (usb_disk's identical tail on a
                                * PROPERLY installed entry never corrupts). h91 threw the DM's own verdict
                                * away; h93 records every call (code, kind, cmdID) and that verdict, plus
                                * a MUST-BE-0 count of nonzero returns. VERIFY by running ASP DELIBERATELY,
                                * then hc all + the log. Ships as the MDD diagnostic ROM (LEVEL1 PAINT1).
                                *
                                * h92 = THE CORRUPTION ATTRIBUTION INSTRUMENT. Pure diagnostics.
                                * The MDD's system heap was found CORRUPT (free list bad, block header
                                * trashed at 01973390) with the eject crash a mere downstream victim, and
                                * attribution UNKNOWN between every system-heap writer. Timeline: h53 ran
                                * for weeks beside the CPU-temp CSM cleanly; the corruption arrived in the
                                * h91 era. This build makes a recurrence SELF-ATTRIBUTING:
                                * (1) CANARIES: the 0x1000 alignment slack around all three transfer-engine
                                *     DMA allocations (dmapage / downbuf / egbuf) is pattern-filled and
                                *     swept every task tick. Dead canary = OUR DMA overran, span named.
                                *     Mask painted RED at the end of row 5 (00 = intact) + '!!'-logged.
                                * (2) MAP: '!! h92 MAP' lines at boot give our blocks' start/end, so a
                                *     MacsBug bad-block address is checkable against our neighbourhood.
                                * (3) KIND COUNTERS: DoDriverIO calls by imm/sync/other — the h91
                                *     completion tail (IOCommandIsComplete) is the one genuinely new
                                *     Device-Manager path of this era, watched explicitly.
                                * Ships as the MDD diagnostic ROM (LEVEL 1, PAINT 1). Soak under normal
                                * use WITH the CPU-temp CSM installed (coexistence is the test config);
                                * on any wedge/crash, photograph row 5 and run MacsBug 'hc all'.
                                *
                                * h91 = THE NATIVE COMPLETION CONTRACT (the m40 infinite wristwatch).
                                * h90 WORKED: ASP's open dispatches into DoDriverIO, no crash. But a native
                                * driver's RETURN VALUE completes only IMMEDIATE commands — synchronous ones
                                * are QUEUED, and finish only via IOCommandIsComplete(cmdID, result). Our
                                * DoDriverIO ignored `kind`, so ASP's synchronous Status PB kept ioResult=1
                                * forever and ASP spun a wristwatch for good. Fix: usb_disk.c:700's proven
                                * completion tail, byte for byte, single exit. n0 proves the Device Manager
                                * never delivered a command here pre-h90, so nothing working can change.
                                * Ships as m41 (Mini, LEVEL 1, PAINT 0). VERIFY: ASP -> Devices and Volumes
                                * populates in the normal second or two.
                                *
                                * h90 = REPLACE THE DISPATCH DESCRIPTOR UNCONDITIONALLY.
                                * The escalation, each check one level too shallow, each proven on hardware:
                                *   h87 assumed EMPTY -> real RD there. h88 checked MAGIC -> fine, left it,
                                *   crashed. h89 checked the TARGET IS A POINTER -> it is (0x001ADxxx), and
                                *   it is the very TVector every crash's R12 held: its CODE WORD is the
                                *   PC=8 payload. DCE -> RD -> TVector; the rot was in the last link.
                                * n0 proves nothing ever dispatched through this entry (total commands 0,
                                * every boot), so there is nothing to preserve: log the corpse's full chain
                                * (including the TVector code word) and plant our own RD, every time.
                                * Ships as m40 (Mini, LEVEL 1, PAINT 0). VERIFY: 'h90 replacing ...
                                * UNCONDITIONALLY' + 'AFTER' in the log, then ASP -> Devices and Volumes.
                                *
                                * h89 = VALIDATE THE DESCRIPTOR'S TARGET, NOT JUST ITS MAGIC.
                                * m37's own '!!' log closed the loop: the entry IS found (slot 0x32, refNum
                                * FFCD), and +0x1E already holds a REAL RoutineDescriptor (0xAAFE magic) —
                                * whose procDescriptor at RD+0x14, the actual jump target, is garbage. h88's
                                * magic-only check called that valid and left it alone; ASP then crashed
                                * through a well-formed descriptor with a junk payload. h89 requires the
                                * TARGET to be plausible too, replaces the shell when it is not (stale RD
                                * leaked, never freed — something unknown allocated it), and logs the bad
                                * target so the diagnosis is in the file. Ships as m38 (Mini, LEVEL 1,
                                * PAINT 0). VERIFY: the h89 lines, then ASP -> Devices and Volumes.
                                *
                                * h88 = h87's DCE fix RETRIED FROM THE PUMP until it lands, and reported.
                                * m36 still crashed IDENTICALLY (one EHCIUIM entry in the whole table, crawl
                                * naming OpenInstalledDriver+B4) => the h87 walk found nothing, leading read:
                                * uimInitialize runs BEFORE the USL registers the unit entry. h88: (1) also
                                * called from the first task-level pump tick, after all registration;
                                * (2) latched only on success, so it retries every tick until it lands;
                                * (3) patches ALL matches; (4) reports on the '!!' channel => a LEVEL 1
                                * build documents the outcome at ~4 lines per boot. Ships as m37 (Mini,
                                * LEVEL 1, PAINT 0). VERIFY: the !!-h88 lines in the log, then ASP.
                                *
                                * h87 = h86 + THE ASP CRASH, FIXED AT ITS ACTUAL CAUSE.
                                *
                                * ASP -> Devices and Volumes crashed on BOTH machines since forever. The
                                * user's MacsBug StdLog decoded it completely: ASP _Opens every unit-table
                                * driver; our parcel-claimed entry ('EHCIUIM', created by the USL's family
                                * machinery, n0-proven NEVER dispatched: total commands 0) lacks the
                                * DoDriverIO UPP that DriverLoaderLib stashes in the DCE's dCtlWindow
                                * (+0x1E). The ROM's open path CallUniversalProc's through that field
                                * (procInfo 0xFFF1) -> garbage vector -> PC=00000008. Byte-for-byte the
                                * "un-openable ROM-claimed ndrv" crash an earlier ndrv project hit (same ROM routine).
                                * FIX (ehci_os_fix_native_dce, called from uimInitialize): find our DCE by
                                * the name MacsBug itself reads (dCtlDriver+0x12), plant
                                * NewRoutineDescriptor(DoDriverIO, 0xFFF1, kPowerPCISA) at +0x1E, allocated
                                * in the SYSTEM zone. Exactly what the ROM installer does for real installs.
                                * AND: DoDriverIO's `default: return noErr` became honest errors (statusErr
                                * etc.) — success-with-untouched-buffer is the h30 disease, and ASP's
                                * DriverGestalt('vers') Status would have parsed garbage one call later.
                                * VERIFY: ASP -> Devices and Volumes on both machines. Ships as m36 (Mini,
                                * LEVEL0 PAINT0) + MDD h87 (LEVEL0 PAINT1). READ docs/H86-TEST-CARD.md for
                                * the rest; fallbacks Mini m29 (h78), MDD h53 + v8. */
                               /* h86 = THE RELEASE CANDIDATE. Two changes on top of the 5/5-validated h85:
                                *
                                * (1) THE RUDE-REMOVAL ALERT ACTUALLY APPEARS. h82's was called and silent —
                                *     m32's level-2 log proves the whole chain ran three times (UnmountVol
                                *     noErr x3, resolver ok, "h82: posting" x3, nothing drawn). The hand-laid
                                *     AlertStdAlertParamRec was PPC-aligned; Dialogs.h wraps the real one in
                                *     #pragma options align=mac68k, so StandardAlert read every field +2 off
                                *     and bailed. Fix: pass NIL alertParam (documented all-defaults form, one
                                *     OK button) — no struct, nothing left to misalign. VERIFY WITH ONE YANK.
                                * (2) EHCI_PAINT beside EHCI_LOG_LEVEL in CMakeLists: PAINT=1 = the m34 test
                                *     behaviour (watchdog + rows 1-5 + storm band); PAINT=0 = the RELEASE
                                *     build, which never draws on the user's screen. Counters stay in both.
                                *
                                * Ships as TWO ROMs from THIS one source:
                                *   m35 (Mini, VBL-fixed base):  LEVEL=0 PAINT=0  — the release candidate
                                *   h86 (MDD, STOCK forkful base): LEVEL=0 PAINT=1 — the universal-door
                                *     retest: does the MDD's gDownTimeouts failure survive with the logging
                                *     load gone? Persists = real NEC-side bug. Gone = logging was implicated.
                                * READ docs/H86-TEST-CARD.md. Fallbacks: Mini m29 (h78), MDD h53 + v8. */
                               /* h85 = THE DISCRIMINATOR: THE FILE MANAGER LOGGING IS COMPILED OUT.
                                *
                                * ★ MEASURED TWICE ON 2026-08-13. Through pump 9 every completed pump body
                                * was <= 274 ms and gBodySlowN was 0 — a genuinely healthy machine. Then
                                * slot 1's AddDrive landed, the Finder began mounting, and THE VERY NEXT LOG
                                * DRAIN TOOK 30715 ms, holding the Finder's own thread for half a minute.
                                * The 09:12 run measured the same section at 31064 ms. 160 lines x ~190 ms
                                * per FSWrite + PBFlushFileSync + FlushVol on a boot volume the mount is
                                * already hammering; the arithmetic closes to within 2%.
                                * ⇒ THE INSTRUMENT IS THE DOMINANT LOAD, precisely when a mount is in flight.
                                *
                                * ⚠ WHY A SWITCH AND NOT A TUNED FLUSH POLICY: h78 cut the DoDriverIO drain
                                * 384 -> 6 AND stood it down 30 s, passed three runs, failed the fourth.
                                * Narrowing a window looks exactly like a fix until it doesn't. At level 0
                                * the code is NOT THERE, so the answer cannot be a coincidence of timing.
                                *   level 0 solid over several boots -> the instrument was the disease,
                                *                                       AND THIS IS THE SHIPPING BUILD
                                *   level 0 still wedges            -> logging was never the cause, and the
                                *                                       screen paint still says where
                                *
                                * SURVIVES AT LEVEL 0: every counter; the whole screen watchdog (it reads
                                * counters and writes the framebuffer — no File Manager); the BANNER, forced
                                * through so run validation still works; and rows 1-5 on a failure. Row 5 now
                                * ends with BUILD ("85") and LEVEL in white, so a photograph identifies the
                                * run with no file at all.
                                * ⚠ The INIT's three logs (~56 lines, separate files/code) are DELIBERATELY
                                * untouched — one variable per run. If startup is still slow, they are next.
                                * Ships as m34. READ docs/H85-TEST-CARD.md. Fallback: m29 (h78). */
                               /* h84 = THE PUMP BODY, TIMED AND LOCATED. Pure instrumentation.
                                *
                                * m32/h83 settled WHAT: at all three paints ARMED == FIRED with POSTED == 1,
                                * which can only mean we were inside the response and it had run past the
                                * watchdog's 5 s trigger. The hang is that picture taken further along — the
                                * h76 dump read gTaskPumpN 0x0a, so we were inside body #10 at the second
                                * AddDrive and it never reached 11. The response borrows the FINDER'S OWN
                                * THREAD, so a body that does not return IS the wedge: one cause, every
                                * symptom. What is missing is WHERE it stops and HOW LONG it takes.
                                *
                                * ⚠ NOTHING WE HAD COULD SEE IT. task_work_arm returns early while a request
                                * is posted, so the next arm can only happen AFTER the body ends: gNmLat and
                                * gTickGap BOTH measure the gap. Two instruments, one blind spot.
                                * ⚠ AND A HIGH-WATER MARK ALONE WOULD BE USELESS — it is written at body
                                * EXIT, and the failure is a body that never exits. So the load-bearing
                                * fields are LIVE: gBodyPhase and gBodyStartMs, read by the paint at
                                * interrupt level as "stuck in section N for M ms".
                                *
                                * SCREEN ROW 5 (orange): PH NOWMS MAXMS MAXPH PHMS.
                                * LOG: three lines — body LAST/MAX, gBodySlowN (bodies over 1000 ms), and the
                                * worst SECTION. The per-phase table stays on screen, not in the log, because
                                * this dump runs INSIDE the body it measures (I10).
                                *
                                * ZERO behaviour change: eleven bphase() checkpoints, each one subtract and
                                * two stores. No logging, no allocation, nothing on the hot path.
                                * Ships as m33. READ docs/H84-TEST-CARD.md. Fallback: m29 (h78). */
                               /* h83 = WHY THE PUMP STOPS. One diagnostic + one proven defect.
                                *
                                * The 2026-08-12 Mini hang froze gTaskPumpN at 0x0a with the engine idle and
                                * every error counter zero — the m27 signature exactly, and h81's bio_kick
                                * guard was provably uninvolved (bio ring empty, both slots free). The last
                                * dump read gNmArmed == gNmFired == 10: balanced, so the log could not say
                                * whether we STOPPED ASKING or asked and were NEVER ANSWERED.
                                *
                                * (1) SCREEN WATCHDOG ROW 4 (magenta): ARMED, FIRED, STUCK, POSTED, KEEP.
                                *     Reads the answer off a wedged machine with no working task level.
                                *     ARMED > FIRED = nobody answered; POSTED stuck at 1 = the latch that
                                *     makes task_work_arm return on its first line forever.
                                * (2) h63's STUCK-REQUEST WATCHDOG HAS BEEN DEAD SINCE h65, by a unit
                                *     mismatch: it compares gVhubTick (SIH passes) against gNmArmTick
                                *     (frame_ms MILLISECONDS). frame_ms outruns gVhubTick ~8x from the first
                                *     second, so the difference is permanently negative and the watchdog can
                                *     never reach its own body. gNmStuckRearms is 0 in EVERY run ever
                                *     logged — that is the fingerprint, not a quiet watchdog. Fixed with a
                                *     second stamp in the check's own clock; the latency instrument is
                                *     untouched. Row 4's STUCK field makes the fix self-verifying.
                                * (3) The gNmLat log label said "ticks"; both ends are frame_ms. It is
                                *     MILLISECONDS, and the wrong word cost a 16x misreading of the stall.
                                * No transfer-engine change. Ships as m32. READ docs/H83-TEST-CARD.md.
                                * Fallback: m29 (h78). */
                               /* h78 = THE FILE-MANAGER PRESSURE FIXES, named by the m27 hub hang. The
                                * screen watchdog measured driver-alive / task-level-DEAD with OUR ENGINE
                                * COMPLETELY IDLE, which rules out a stuck transfer and points at what we
                                * DO TO the File Manager. Three cuts:
                                *  (1) the DoDriverIO drain is capped at 6 lines (was 384) — it runs from
                                *      INSIDE a File Manager call chain, on the boot volume;
                                *  (2) that drain stands down entirely for 30 s from AddDrive, wall-clock
                                *      and self-clearing, because the mount is the dangerous window;
                                *  (3) the periodic dump is paced by WALL CLOCK (2 s enumerating, 15 s
                                *      once mounted) instead of pump passes — it was emitting 5365 lines
                                *      after the mount, measured.
                                * Carries h74's split (validated: gSplitSaved 5, zero clobbers), h75's
                                * recovery routing, and the h76 screen watchdog. Ships as m29.
                                * READ docs/H78-TEST-CARD.md. Fallback m25 (h73). */

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
typedef struct { OSType serviceCategory; OSType serviceType; NumVersion serviceVersion; } EHCIServiceInfo;
typedef struct {
    OSType     sig;             /* 'mtej' */
    UInt32     descVersion;     /* 0 */
    Str31      nameInfoStr;     /* "pciclass,0c0320" <- EHCI class match */
    NumVersion typeVersion;
    UInt32     driverRuntime;   /* 0x05 = LoadedUponDiscovery|UnderExpertControl */
    Str31      driverName;      /* "EHCIUIM" */
    UInt32     reserved[8];
    UInt32     nServices;       /* n1: MUST be >= 1 — see the note on the initializer below */
    EHCIServiceInfo service0;   /* n1: the one declared service (generic native driver) */
} EHCIDriverDescription;

EHCIDriverDescription TheDriverDescription = {
    FOURCC('m','t','e','j'),
    0,
    { 15, "pciclass,0c0320" },
    { 1, 0, 0x80 /*final*/, 0 },
    0x00000005UL,   /* 0x05 = kDriverIsLoadedUponDiscovery(0x01) | kDriverIsUnderExpertControl(0x04).
                     * Same value as the working ROM ATA controller (cmd646-ata) and an earlier disk ndrv (its v65).
                     * ★ DELIBERATELY *NOT* kDriverIsOpenedUponLoad(0x02): an earlier disk ndrv proved on HARDWARE (its v64)
                     * that auto-opening at boot crashes near the desktop (the OS brings a not-yet-
                     * functional driver fully online into the SystemTask machinery, then calls a garbage
                     * UPP) — and our own kOpen does the FULL bring-up (HCReset/DMA/IRQ), which the
                     * same driver's v63/v64 lesson says FREEZES during the early PCI-claim phase. So we accept
                     * load-without-open here; a later, task-context trigger performs the Open (see
                     * docs/NATIVE_INTEGRATION_DESIGN.md — likely a tiny boot INIT calling OpenDriver,
                     * which is how Apple itself shipped USB support on PCI-card machines). */
    { 7, "EHCIUIM" },
    { 0,0,0,0,0,0,0,0 },
    /* ★★ n1 ROOT-CAUSE FIX (2026-07-31). This was `0` — NO declared service — and the n0 hardware test
     * proved DoDriverIO was NEVER CALLED (trace: 0 commands; "DoDriverIO" absent from the whole log;
     * EHCIUIM_init.log did not even exist before an app ran). Reason, documented in an earlier disk ndrv by the same author:
     * a native driver's description MUST declare at least one service (DriverFamilyMatching.h, "The List
     * of Services (at least one)") — **omitting it makes VerifyFragmentAsDriver REJECT the fragment**, so
     * the Device Manager never loads it as a driver at all. The USL could still call our exported dispatch
     * table directly (LoadUIMForEntry), which is exactly the behaviour we had been relying on unknowingly.
     * NB this also means lc1's "boot quiesce at kInitialize" has never actually run.
     * Category/type = 'ndrv'/'genr' = a GENERIC native driver: honest for a host controller and neutral —
     * deliberately NOT 'usb ' (kServiceCategoryUSB), which could invite Apple's USB Expert to adopt us and
     * undo the Apple-independence we built. (that earlier ndrv declares 'ndrv'/'blok'; if 'genr' turns out not to
     * satisfy the loader, 'blok' is the HW-proven fallback to try next.) */
    1,
    { FOURCC('n','d','r','v'), FOURCC('g','e','n','r'), { 1, 0, 0x80 /*final*/, 0 } }
};

#include "ehci.h"
#include "ehci_vhub.h"

#define noErr 0L

/* Shared controller soft state (DoDriverIO in ehci_os.c + the vhub also reference it). */
ehci_softc gSoftc;
extern int gBroughtUp;   /* ehci_os.c: shared one-time-bring-up guard (see uimInitialize below) */

/* slot 0 — Initialize: the USL/Expert hands us the controller's Name Registry node. Bring the
 * controller up (ehci_os.c: PCI enable, register map, reset, schedules, run) then initialize the
 * virtual-hub transfer engine (DMA page + control QH). */
static OSStatus uimInitialize(UInt32 a0, UInt32 a1, UInt32 a2, UInt32 a3,
                              UInt32 a4, UInt32 a5, UInt32 a6, UInt32 a7)
{
    long e;
    (void)a4; (void)a5; (void)a6; (void)a7;
    /* ★★★★★★ THE BUILD TAG, FIRST LINE, ALWAYS. Bump EHCI_BUILD_TAG on every ROM.
     *
     * ⚠⚠ WHY THIS EXISTS: on 2026-08-08 an app-less run could not be validated at all, because the only
     * build identity in this log was the narrative banner below — which still said **h21** while h22 through
     * h28 had shipped. Asked "which ROM actually booted?", the log could not answer, and neither could I: the
     * load addresses differ every boot, so nothing in the trace distinguishes one driver build from another.
     * The project's own rule is "validate the run before analysing it: expected banner" — and for the ROM that
     * rule was unmeetable. Combined with the Get-Info stamp being impossible (a `vers` (1) breaks boot,
     * see reference_os9_rom_vers1_breaks_boot) the FILENAME was the only identity, and a filename does not
     * travel into the log.
     * ★ One short line, bumped per build, costs nothing and makes every future run self-identifying. */
    /* h85: _always, not _log. With the File Manager logging compiled out this is the ONLY line the driver
     * writes, and the run-validation rule depends on it. The line after it says which mode is running, so a
     * log with two lines is a level-0 run and a log with thousands is a level-2 one — unmistakable. */
    ehci_os_log_always("=== EHCIUIM BUILD " EHCI_BUILD_TAG " === (if this tag is not the ROM you installed, the run is void)");
    ehci_os_log_always(ehci_os_log_level() == 0
        ? "=== h85 LOG LEVEL 0 — the File Manager logging is COMPILED OUT. This banner and this line are the"
          " only two lines the driver will write. Every counter and the whole screen watchdog still work;"
          " read the run off the SCREEN (rows 1-5), not off this file. ==="
        : (ehci_os_log_level() == 1
            ? "=== h85 LOG LEVEL 1 — only '!!' lines are written. A quiet log is a clean run, not a dead one. ==="
            : "=== h85 LOG LEVEL 2 — full trace. ⚠ THIS IS THE DIAGNOSTIC MODE AND IT IS THE DOMINANT LOAD:"
              " measured at 30715 ms in one log drain while a mount was in flight. Do not read timing from a"
              " level-2 run and call it the driver's. ==="));
    ehci_os_log("=== EHCIUIM h21: the CTL/BULK split is REVERTED - it caused the hub-connect freeze. ★ ACTIVATOR UNCHANGED - n4g still pairs. ★★ h20 WAS THE DISCRIMINATOR AND IT ANSWERED CLEANLY, in the direction I did not expect: h20 was h19's split STRUCTURE with h15's SERIALISED timing, and it FROZE ON THE HUB CONNECT exactly like h19. So removing the concurrency did not help, and the concurrency was never the trigger - the split's PLUMBING is. The chain: h17 (h15 + diagnostics + un-park + an inert promotion) connects the leg FINE; h19 (same, plus the split, concurrent) FROZE; h20 (same, plus the split, serialised) FROZE. The only meaningful delta from h17 to h20 is the split plumbing, because the promotion was measured inert (gCtlPromoted 0). ★★ SO THE SPLIT IS OUT. This build is h15's engine plus exactly two things that earned their place: the h16 DIAGNOSTICS, which change no USB behaviour and paid for themselves immediately (the gated gEnumDeferBusy and a token-bucket log cap that stops a driver loop from wedging the File Manager), and h17's UN-PARK fix, which moves n24's un-park above the 'apple_hidden_port && !port_ceded' gate - port_ceded() reports a PARKED port as ceded, so that un-park had been DEAD CODE since the day it was written and a parked port could never come back. ★★ WHAT IS NOW KNOWN AND SHOULD NOT BE RE-DERIVED: the hot-plug-during-a-copy failure is REAL and its cause is understood - down_reap calls bio_advance BEFORE down_pump, and bio_advance re-arms the next chunk itself, so during a multi-chunk copy gDpBusy is always 1 by the time down_pump runs and a queued control request is never issued at all. That diagnosis is in docs/CTL-BULK-SPLIT.md and it still stands. What does NOT work is fixing it by splitting the in-flight state: four attempts, two of which regressed a working driver into a freeze. The user has scoped hot-plug-during-a-copy as a narrow nice-to-have, so it is parked deliberately, not forgotten. ★ THE VALIDATED FOUR-DRIVE BUILD IS h15 and it remains the shipping artifact until this one is validated in its own right. ★ WATCH: connect the leg with a drive already mounted - the sequence that froze h18, h19 and h20 - then the full four-drive sequence. A drive plugged in DURING a copy is still expected to fail. ===");
    ehci_os_log("=== (h20, the discriminator that ruled the split OUT) ===");
    ehci_os_log("=== (h19) THE CTL/BULK SPLIT - control tran - h19's structure with h15's timing. ★ ACTIVATOR UNCHANGED - n4g still pairs. ★★ THIS BUILD IS AN EXPERIMENT AND ITS ONLY JOB IS TO ANSWER ONE QUESTION. h19's CTL/BULK split DID work at what it was for - gCiIssued climbed 16 to 96 with zero control timeouts or errors, so control transfers really do issue and complete on their own slot - and it ALSO froze the machine on connecting the hub, which h15 does not. h18 froze exactly the same way from a completely different arbitration change, and h17 (which carries the un-park hoist and nothing else) connects the hub fine. So the freeze tracks WHEN control and bulk get issued relative to each other; it is not the un-park, and not obviously the plumbing. ★★ TWO CANDIDATES REMAIN AND THEY NEED OPPOSITE RESPONSES, so guessing is worth nothing: (a) the split's PLUMBING is unsound - separate slots, separate bounce, ctl_reap - in which case revert to h15 entirely; or (b) the CONCURRENCY is the trigger, the hub claim being unable to tolerate control and bulk overlapping while a drive is mounted and doing I/O, in which case the plumbing is worth keeping and simply stays serialised. ★★ h20 = CTL_BULK_CONCURRENT 0. Every structural change from h19 is still here (the control slot, its own DMA bounce at 0xF90, its own watchdog, and ctl_reap deliberately not driving bio_advance) but down_pump now issues NOTHING while any transfer is in flight, which is h15's rule expressed over two slots instead of one. Concurrency is the single isolated variable. ★★ READ THE RESULT LIKE THIS: if the hub connects and four drives work, the answer is (b) - the plumbing is sound and worth keeping on its own merits, because it also fixes control completions driving the BOT state machine and the one-watchdog-for-two-classes blind spot that made an orphaned transfer invisible. Hot-plug-during-a-copy then needs concurrency, which is a separate and much narrower problem and the user has called it a nice-to-have. If it still freezes, the answer is (a) - the plumbing is at fault, revert to h15 and stop. ★ WATCH: connect the leg with a drive already mounted (the exact h18/h19 freeze), then the full four-drive sequence. gCiIssued should still climb (control still flows, just never concurrently), gCiTimeouts and gCtlTooBig must be 0. A drive plugged in DURING a copy is EXPECTED TO FAIL in this build - that is not the question being asked. ===");
    ehci_os_log("=== (h19) THE CTL/BULK SPLIT - control transfers get their own in-flight slot, their own DMA bounce and their own watchdog. ★ ACTIVATOR UNCHANGED - n4g still pairs. ★★ h17 AND h18 ARE REVERTED; h15 remains the validated fallback. Both were fixes to a mechanism I had not verified, and both were refuted by counters I shipped to test them - h17's gCtlPromoted read 0, h18's gBioDeferBusy read 0, and h18 turned a silent enumeration failure into a machine freeze. ★★ THE ROOT CAUSE, FOUND IN THE CODE AND NOT ON HARDWARE: down_reap calls bio_advance BEFORE down_pump, and bio_advance -> bio_start_chunk -> bio_issue_read/write set gDpBusy = 1 themselves. So during a multi-chunk copy gDpBusy is ALREADY 1 again by the time down_pump runs, and its opening 'if (gDpBusy) return;' fires every single time. A queued control request was therefore NEVER ISSUED AT ALL for the whole duration of a copy, and ctl_step gave up on its 800 ms cap. That accounts for everything: gDownErr and gDownTimeouts both stayed 0 because a transfer that was never issued cannot error or trip the watchdog, and gCtlPromoted read 0 because h17's scan sits AFTER that early return - my own fix was in unreachable code. ★★ AND THE SPLIT IS NOT OPTIONAL, because two more defects fall out of the same shared slot and no amount of arbitration touches them: (1) gDownBuf is ONE 128KB bounce that down_issue stages into with no control/bulk distinction, so whenever bio armed over an in-flight control transfer BOTH were live in hardware on different QHs pointing at the SAME buffer - a latent DMA corruption risk that has existed all along; (2) down_reap could not tell whose completion it was, so 'if (gBioPhase && !gDpUPP) bio_advance(status)' fired for CONTROL completions too (ours pass upp = 0) and advanced the BOT state machine with an unrelated status; (3) one watchdog for two classes meant an orphaned transfer was never timed out, which is exactly why gDownTimeouts read 0. ★★ h19: gDp* is now the BULK slot only. Control gets gCi* (busy/q/td/len/isIn/dest/upp/pipe/armTick), its own 112-byte DMA bounce in the spare tail of the already-wired page at 0xF90 (no second allocation), its own watchdog, and its own counters. epq_issue now takes the bounce's PHYSICAL pages instead of hardcoding gDownBufPhys and RETURNS the activated qTD instead of storing it in gDp*, so the caller records it in whichever slot owns the transfer. down_pump issues up to ONE CONTROL AND ONE BULK per pass, in queue order within each class, marking an out-of-order pick 'taken' so the tail can be reclaimed. ctl_reap services the control slot and DELIBERATELY does not call bio_advance. There is no hardware reason for the old serialisation: control runs on gCtrlQ, bulk on gDev[d].bulkQ[], different queue heads both already in the async ring, and the controller executes qTDs on both at once. ★ WATCH: a drive plugged in DURING a large copy must MOUNT, with gCiIssued CLIMBING and no 'control transfer FAILED at step 0x02'. If it fails and gCiIssued is NOT climbing, control transfers still are not reaching the hardware and the split did not take. gCtlTooBig must be 0. Then re-run the full four-drive sequence - this is the transfer engine, the part that produced r84/r85, r48 and the n17 corruption. ===");
    ehci_os_log("=== (h18, REVERTED) block I/O must not overwrite an enumeration that is already in flight. ★ ACTIVATOR UNCHANGED - n4g still pairs. ★★ h17's FIX WAS WRONG AND ITS OWN COUNTER SAID SO: gCtlPromoted read 0, meaning a control transfer was NEVER queued behind bulk, so promoting control could not possibly have been the answer. Setting that discriminator is what stopped a second wrong build. ★★ THE REAL CAUSE: bio_issue_read/bio_issue_write arm the hardware DIRECTLY - they set gDpBusy, gDpQ, gDpTd and gDpBulkEp themselves instead of going through down_submit/down_pump, because the r70/r77 pre-queued chain is one whole command in one qTD chain - and they did it UNCONDITIONALLY. bio_kick only checked that the BOT state machine was idle and that the ring had work. So an enumeration control transfer already in flight was simply OVERWRITTEN. And it is not even a race: ehci_vhub_service calls as_tick() and then bio_kick() on the SAME pass, so every heartbeat that found a queued copy clobbered whatever control transfer as_tick had just issued. The control qTD may well have completed on the wire, but gDpTd no longer pointed at it, so nothing reaped it, gCtlSeq never advanced, ctl_done() never became true, and ctl_step failed on its 800 ms cap - with gDownErr and gDownTimeouts BOTH STAYING ZERO, because the watchdog only ever watches gDpTd. That is exactly the h16 and h17 signature: three attempts, 'control transfer FAILED at step 0x02/0x03', and not one error counted anywhere. ★★ THE PROTECTION WAS ONE-DIRECTIONAL, which is the durable lesson: n18 fix 4 stops an ENUMERATION starting while block I/O is in flight, and NOTHING stopped block I/O starting while an ENUMERATION was in flight. down_pump has carried 'if (gDpBusy) return;' all along - this path bypasses down_pump, so it needed its own copy and never had one. ⇒ A FAST PATH THAT BYPASSES THE PLACE A GUARD LIVES MUST CARRY THE GUARD WITH IT. ★★ h18: bio_kick defers while gDpBusy is set. Free and deadlock-proof - bio_kick runs from every completion AND from the 8 ms heartbeat, and a control phase retires in microseconds, so worst case is one heartbeat of latency. gBioDeferBusy counts the deferrals: non-zero is the proof that these starts were previously being clobbered. ★ h17's control promotion is KEPT but is INERT so far (gCtlPromoted 0) - it is correct and cheap, and its counter will say if it ever matters. ★ WATCH: plug a fourth drive in during a large copy and it must MOUNT, with gBioDeferBusy non-zero and NO 'control transfer FAILED at step 0x02'. Then re-run the full four-drive sequence as a regression check. ===");
    ehci_os_log("=== (h17) a drive plugged in DURING a copy must still enumerate; and a parked port must be able to come back. ★ ACTIVATOR UNCHANGED - n4g still pairs. ★★ h16's DIAGNOSTIC FIXES BOTH WORKED and immediately earned their keep: gEnumDeferBusy read 0 during ordinary copying and 686 when a drive was genuinely waiting, and the token bucket dropped nothing, which is the only reason the full enumeration trace survived to be read. ★★ BUG 1 - CONTROL TRANSFERS WERE STARVED BEHIND BULK I/O. The fourth drive, plugged in during a large copy, reset and ENABLED AT HIGH SPEED three times, and all three enumeration attempts died at 'control transfer FAILED at step 0x02' - the first control transfer to the device at address 0. The timestamps name the mechanism exactly: attempts 1007 ms and 1024 ms apart against ctl_step's 800 ms per-phase cap, so each one sat waiting for its 8-byte SETUP to be issued and timed out. Control and bulk share one FIFO and the engine runs a single transfer at a time, so the enumeration was queued behind a continuous stream of the copy's bulk requests. n18 fix 4 could not help: 'do not start while the bio ring is busy' only guards the START, and the ring is MOMENTARILY empty between File Manager requests, so the arm slips through a gap and is then starved mid-sequence. ★★ h17: down_pump now promotes a CONTROL request ahead of queued bulk. An enumeration needs about ten transfers of 8-64 bytes, microseconds of bus time, so this cannot reproduce the n17 problem of enumeration starving a MOUNTED volume - that risk lives in the BOT probe's bulk transfers, and those are deliberately LEFT IN FIFO ORDER. Only ep0 is promoted. The mechanism is a SWAP, which is safe because down_issue copies everything it needs out of the ring slot and keeps no pointer into it; and order within each class is preserved, because a BOT sequence and a control sequence are each issued one phase at a time driven by the previous completion, so at most one request per stream is ever queued. ★★ BUG 2 - A PARKED PORT COULD NEVER BE UN-PARKED, so the drive stayed dead for the whole session, through a physical re-plug. n24 sweep finding 2 added an un-park for exactly this and it has been DEAD CODE since the day it was written: it sat inside 'apple_hidden_port(p) && !port_ceded(p)', and port_ceded() reports a PARKED port as ceded - so the moment a port was parked the gate went false and every later event on it was skipped, including the un-park meant to revive it. The n24 comment states the very reasoning it violates: 'unlike a CEDED port, a PARKED port is still OURS and its disconnect IS visible right here - that asymmetry is what makes this fixable.' port_ceded() erases the asymmetry by conflating the two states. The un-park now sits ABOVE that gate and the unreachable copy is deleted rather than left in place. ⇒ THIRD time in this project a fix turned out to be unreachable code, after DoDriverIO and ehci_os_boot_quiesce. A park and a cede are DIFFERENT STATES and must not share a test. ★ WATCH: plug a fourth drive in during a large copy and it should MOUNT, with gCtlPromoted (new) non-zero; 'control transfer FAILED at step 0x02' should not appear; and if a port ever does get parked, unplugging that device should now log the un-park and a re-plug should get a clean set of attempts. ===");
    ehci_os_log("=== (h16) diagnostics that tell the truth (h15 validated: FOUR DRIVES, ANY PORT, ZERO ERRORS). ★ ACTIVATOR UNCHANGED - n4g still pairs. ★★ NO BEHAVIOUR CHANGE IS INTENDED: h15 is the validated four-drive build and this changes only the diagnostics layer. (1) gEnumDeferBusy counted every heartbeat during ANY file copy instead of every occasion a device was actually held back - it reached 8390 on the h15 run with NOTHING waiting, making it worthless for the one question it exists to answer. Now gated on a pending arrival (a root port mid-enumeration, or a hub port the hub has flagged). (2) The h15 log rate cap protected the machine but was too tight for a LEGITIMATE burst: it fired twice on the validated run and dropped 43 lines while the four drives were enumerating, losing detail exactly where it is most interesting. Now a TOKEN BUCKET - 256 lines of burst depth, 60/second sustained - so a real enumeration burst passes intact while a livelock still drains the bucket in under a second and is then held to the sustained rate, which is what keeps the File Manager responsive. (3) gBioReject and gBioHiWater existed but were printed NOWHERE, so the bio ring (BIOQ_N=16 slots shared by ALL devices) could not be checked under four-drive load at all. Both now appear in the stall dump. ⇒ ALL THREE are the same lesson as n24 sweep finding 3: a diagnostic that lies, or that cannot be read, costs hardware cycles. ★ WATCH: nothing user-visible should change from h15. gEnumDeferBusy should now be SMALL or zero during copies, the log should be COMPLETE through a four-drive enumeration with no RATE-CAPPED line, and the bio hiWater tells us how close 16 slots come to full with four drives copying. ===");
    ehci_os_log("=== (h15) a ROOT-port enumeration must not be aimed at the HUB. ★ ACTIVATOR UNCHANGED - n4g still pairs. ★★ h14's TWO REPORTED SYMPTOMS WERE ONE BUG: drive 4 on the card never mounted, AND dragging the other three to the Trash hung the machine with a wristwatch cursor. Root cause: gAs.hubAddr is the switch that sends every reset and port query through the hub, and it was cleared in exactly ONE place - as_fail. A hub-downstream enumeration that SUCCEEDED left it set, and the root-port arm never cleared it, so drive 4's enumeration was aimed at the HUB's downstream port instead of its own root port. That port was already owned by a live slot, so the run took 'goto h3_next_port' - which clears gAs.running but touches NEITHER gSelfEnumPort, gSelfEnumDone NOR gSelfEnumTries - and as_tick armed the identical run again. Unbounded, at heartbeat rate. ★★ WHY IT NEEDED h14 TO SURFACE: that arm only runs when a slot is FREE. At USB_MAX_DEV 2 a root-port connect after two hub drives always hit 'no free slot' and never armed, so the stale switch was never acted on. Raising the limit to 4 made a PRE-EXISTING LATENT bug reachable - h14 did not introduce it. ★★ AND WHY IT LOOKED LIKE A CRASH: every log line is a synchronous FSWrite + FlushVol, so ~3 lines per pass at heartbeat rate saturated the File Manager. The DRIVER bug was the cause; the LOG escalated it from 'a drive did not mount' to 'the machine is hung'. h15 fixes all three layers: (1) the root-port arm now sets gAs.hubAddr = 0 explicitly, so a run always states its own mode instead of inheriting one; (2) an arm that achieves nothing 64 times consecutively STOPS re-arming and says so, because the SHAPE - a no-op that changes no state and trips no retry policy - will repeat as fast as the heartbeat allows no matter which field goes stale next; (3) the log drain is RATE-CAPPED at 30 lines/second with the excess counted, an order of magnitude above what a healthy run needs, so no future bug of any shape can take the machine down through the File Manager again. ★★ ALSO NEW, and the user approved the deviation: reaching the four-drive limit now posts a NOTIFICATION instead of only writing one line to a log nobody sees - 'An additional USB 2.0 drive cannot be used because 4 USB 2.0 drives are already connected.' It is generic BY NECESSITY: naming the drive needs INQUIRY, which needs endpoints, which needs a slot, and having no slot is the whole reason we are here. Apple has NO equivalent string because their stack has no device-count limit (addresses 1-127, allocated on demand), so there is nothing to match word for word - second deliberate exception after the n27 eject wording. ★ WATCH: four drives across root AND hub ports in any order, drive 4 mounting on the CARD after two hub drives (the exact h14 failure), a FIFTH drive producing the notification and nothing else, Trash/eject staying responsive throughout, and 'h15: an enumeration arm achieved nothing repeatedly' NEVER appearing - if it does, another field is stale and the backstop caught it. ===");
    ehci_os_log("=== (h14) FOUR concurrent USB 2.0 drives, anywhere - root ports and hub ports alike. ★ ACTIVATOR UNCHANGED - n4g still pairs. ★★ WHY: on the h13 run the third drive was refused with 'no free device slot' and that was NOT a bug - USB_MAX_DEV was 2 and both slots were held by the hub drives. h13 otherwise PASSED: both hub drives mounted at once, test 8 passed (the refusal's unplug touched nothing), the engine stayed healthy all run, and the confirm-twice low-speed rule worked exactly as designed on the display's real HID. ★★ h14 RAISES USB_MAX_DEV 2 -> 4, which is the most this single wired DMA page holds: devices 1-3 sit at 0x500/0x800/0xB00, 0x300 each, ending 0xDFF, and h10 deliberately put the hub's status-change QH at 0xE00 'above the device area even at its maximum' so this could not scribble it. Going past 4 needs a SECOND DMA PAGE, not a bigger constant - four devices plus the hub QH and its buffer fill this page to 0xF80 of 0x1000. ★★ THE SWEEP RE-ASK (n24's two questions, re-asked at FOUR devices instead of two) FOUND ONE REAL LANDMINE: NBULK was a bare literal 6. Each device registers two bulk endpoints, so four devices need 8 - the table was silently one short, and the failure would NOT have looked like an overflow: create_bulk returns -1, both live call sites discarded it with (void), pb_find_eps would have found no endpoints for the fourth slot, and the drive would simply never have appeared with nothing in the log pointing at the cause. NBULK is now DERIVED as (USB_MAX_DEV * 2), the table-full case LOGS, and both registrations are checked. Same defect shape as every other bug in this driver's history: a fact recorded in one place and not consulted when a related one changed. ★★ ALSO: the DMA-page backstop compared against 0x1000 when the thing it protects is the hub QH at 0xE00 (safe at 4 only by arithmetic accident) - it now states the real constraint; the slot-refusal line names WHICH slots are held, because with four slots a bare 'all in use' cannot distinguish a full house from a leaked slot; and a stale n14 comment claiming this function 'stays single-slot' is corrected, since that expired at n19. ★★ EVERYTHING ELSE PASSED THE RE-ASK: all 42 device loops are bounds-gated on USB_MAX_DEV with no hardcoded bounds, every per-device array is [USB_MAX_DEV], DEV_ADDR = 1 + slot gives addresses 1-4 clear of HUB_ADDR 15, bulkQ[2] is direction not device count, and gSecondDevMask is a PORT mask so four devices do not widen it. ★ WATCH: four drives mounted at once across root and hub ports, each with its own contents; gEnumDeferBusy (new) says whether a newcomer's enumeration is being made to wait behind mounted-volume I/O, which is a global guard whose cost scales with drive count; and 'h14 BULK ENDPOINT TABLE FULL' must NEVER appear - if it does, the NBULK derivation broke. ===");
    ehci_os_log("=== (h13) a control transfer that delivers NO DATA must not read as success. ★ ACTIVATOR UNCHANGED - n4g still pairs. ★★ h12's OWN FIX WORKED: 'that disconnect freed no slot of ours' appeared exactly once and no drive was wrongly torn down. But test 8 is NOT a pass, because the controller was already dead when the third drive went in. ★★ SYMPTOM 1, only one of two drives behind the hub was recognised: gAsPortSt is ONE 4-byte static shared by every port, and the sweep trusted it the instant AS_CTL returned. ctl_done() only reports that a COUNTER MOVED - and gDownErr and gDownTimeouts move the SAME counter as gDownDone - so a control IN that completed without delivering its payload was indistinguishable from a full one, and the caller read the PREVIOUS port's status. On the h12 run that was port 3, the display's genuine LOW-SPEED HID (0x0301). The proof it was stale and not the hub's opinion: those reads carried change bits 0x0000 on a port whose connect change had never been acknowledged, which section 11.24.2.7.2 makes impossible, and the value was bit-identical to port 3's. ★★ AND IT WAS PERMANENT: the low-speed verdict latches into skipMask and is cleared only when the device physically leaves, so ONE bad sample spent that drive's entire attached lifetime. h13: (1) down_reap's byte count is now visible to the control path as gDpActual, every control phase checks gDpLastStat so an errored phase fails instead of succeeding, and all three GET_PORT_STATUS sites zero the buffer first and refuse to decide on a reply shorter than 4 bytes; (2) a port must read LOW-SPEED on TWO CONSECUTIVE sweeps before it is skipped - a real low-speed device still latches on its second look, a drive that glitches once during power-up keeps its chance. ★★ SYMPTOM 2, the third drive on the card, was NOT a separate bug: gDownDone was frozen at 0xc6 and gIsrHits flat at 0x1c4 across every stall, where h11's 133 stalls all had both climbing. The engine was dead before that drive was plugged in. CAUSE NOT YET KNOWN - deliberately instrumented rather than guessed at: the stall dump now prints FRINDEX, ASYNCLISTADDR, the live anchor link, r60's gLastAnchorLink and gDownRelink, every QH address, and the stalled QH's overlay plus the in-flight qTD token, which separates 'qTD never activated' from 'active but unreachable' from 'controller stopped'. ★★ ALSO FIXED: is_our_qh never knew about the hub's status-change QH that h10 added to the same async ring, so if r60's backstop ever fired it would re-splice everything EXCEPT that QH and leave us permanently deaf to port changes. ★★ PORT AGNOSTICISM: the driver's LOGIC is port-independent - every port loop is bounds-gated on nPorts and nothing compares against a port literal - but the heartbeat dump hardcoded ports 0 and 4, so it was blind to a hub on any other port. It now dumps every port the controller reports. ★ WATCH: both drives behind the hub mounting regardless of which downstream port each is in, 'h13: reads LOW-SPEED - NOT latching on one sample' at most once per genuinely low-speed port, gHubShortSt telling us whether short replies are real on this hardware, and if a stall recurs, the h13 block naming which of the three engine-death cases it is. ===");
    ehci_os_log("=== (h12) a disconnect that frees NO slot must not tear down a drive. ★ ACTIVATOR UNCHANGED - n4g still pairs. ★★ h11 PASSED tests 1-7 AND 9: both drives mounted behind the hub as USB 2.0, file copies including drive-to-drive, idle, Trash, eject, un-ejected pull, re-insert, and pulling the whole leg unmounted its drives cleanly with no freeze. The status-change design held: across the entire session 8 change reports, 8 acknowledged, 15 port visits TOTAL - against h9's 205 and climbing. ★★ THE ONE FAILURE, and it is a bug I identified while writing n23 and did not act on: with both slots full, a third drive plugged into a ROOT port was correctly refused for want of a slot - and pulling it out again unmounted one of the two drives behind the HUB. Cause: the disconnect loop only assigns gRearmDev when it finds a slot whose probedPort matches the port. A disconnect that frees NO slot left gRearmDev holding whatever the PREVIOUS removal put there, and the task-level rearm then ran blk_notify_media(stale, 0), marking another drive's media gone, after which the correct selective unmount dutifully unmounted it. ★★ h12: gRearmDev is now set EXPLICITLY to the freed slot, or -1 when nothing of ours was on that port, and the per-device teardown (alert retract, media-gone, unmount, reconnect_reset) runs only when it is a real slot. The engine reset and the enumeration re-arm still run either way - those concern the PORT, not any one device. ★ WATCH: with both slots full, plug a third drive into the CARD, then unplug it - expect 'h12: that disconnect freed no slot of ours' and BOTH hub drives untouched. ===");
    ehci_os_log("=== (h11) acknowledge the port change bits - the omission that made h10 livelock. ★ ACTIVATOR UNCHANGED - n4g still pairs. ★★ h10's DESIGN WORKED: the hub's status-change endpoint was found (endpoint 1), the qTD parked at task level, and a drive mounted. Then it crashed - and the log shows why: port 3, the display's low-speed HID, was being visited over and over at full speed. ★★ THE BUG, mine, and present since h1: USB 2.0 section 11.24.2.7.2 - a port's change bits LATCH, and until they are cleared with CLEAR_PORT_FEATURE the hub keeps reporting that port in its status-change bitmap forever. I cleared them only on the successful high-speed RESET path, never on the low-speed skip or the empty path, and port 3 has had C_PORT_CONNECTION latched since h2. Under h7 to h9's 500 ms timer that merely wasted one transfer per sweep, invisibly. Under h10's change-driven design it is a LIVELOCK: the hub says port 3 changed, we look, we do not acknowledge, we re-park the qTD, it completes INSTANTLY, we look again - as fast as the engine allows. Faster and more damaging than the polling h10 was built to remove. The timer had been hiding a protocol error, and removing the timer exposed it. ★★ h11: the port's wPortChange is now read alongside its status and ACKNOWLEDGED (C_PORT_CONNECTION and C_PORT_ENABLE) before any decision is taken, on EVERY path - empty, low-speed, already-ours and high-speed alike. Two transfers, and only on a port that actually reported a change. ★ WATCH: 'h11 wPortChange (must be acknowledged)' followed by 'h11 port change acknowledged', and then SILENCE - no repeated visits to port 3. Then mount, copy, drag to Trash, eject. ===");
    ehci_os_log("=== (h10) the hub now TELLS US when a port changes - no bus polling at all. ★ ACTIVATOR UNCHANGED - n4g still pairs. ★★ WHY: h7 through h9 asked every port for its status every 500 ms forever. Measured against h6 that is 205 control transfers and climbing AFTER everything had mounted, versus ZERO - and they are issued from the SIH, concurrent with File Manager I/O on the mounted volumes. That is the documented r48 hard-crash shape and it took the machine down twice, both during a WRITE (dragging to Trash, and an eject flush). h6 was quiet in steady state only by accident, because it skipped classified ports without querying them; h7 removed that accident without replacing it with anything. ★★ h10: a hub has exactly one interrupt IN endpoint whose entire purpose is to report WHICH port changed. We park a single IN qTD on it, on its own resident QH in the async ring, and then do nothing: the hub NAKs it IN HARDWARE until something actually changes. Noticing it is a read of the qTD token out of the DMA page - a memory load, no bus traffic, no engine involvement, nothing issued from the SIH. Steady state therefore costs exactly nothing, by design rather than by luck. The port walk is no longer on a timer: it runs only for ports the hub has flagged, seeded with every port at claim so devices already attached are still found. ★★ THE AUDIT EARNED ITS KEEP: the first cut programmed the new QH from as_advance and idled it from service_ports, both at interrupt level, and scripts/irq-audit.py flagged both as the r84/r85 live-QH-reprogram freeze. QH programming now happens at TASK level behind a park (same contract as create_bulk, quiesce first), and the teardown clears software state only. ★ WATCH: 'h10 hub status-change endpoint', then 'status-change qTD PARKED at task level'. Then mount, COPY FILES, drag to Trash and eject - the operations that crashed h9. Then pull a drive from the display and expect it to be noticed WITHOUT any polling. ===");
    ehci_os_log("=== (h9) the sweep was outracing the task-level handoff. ★ ACTIVATOR UNCHANGED - n4g still pairs. ★★ h8 FIXED ITS TARGET: both drives enumerated behind the hub with CORRECT DISTINCT addresses 1 and 2, both HIGH-SPEED confirmed, both recorded geometry, no ADDRESS COLLISION. And neither was ever EXPOSED to the OS, so neither mounted. ★★ THE REAL BUG, which h7 introduced and h8 did not touch: gAsProbeOK and gAsNeedBulk are parks that selfprobe_tick completes AT TASK LEVEL, and both read gEnumDev to know WHICH slot they are finishing. Arming the next sweep port overwrites gEnumDev. The engine runs on the 8 ms heartbeat while selfprobe_tick only runs when the app polls (about 10 times a second), so the sweep won that race every time: the h8 log shows SELFPROBE COMPLETE followed immediately by the next enumeration, and the publish plus AddDrive for the device that had just probed simply never happened. ★★ WHY h6 WORKED AND h7 DID NOT: h6 SKIPPED ports that already held a device, so it armed rarely and the handoff always got its turn. h7 made the sweep visit every port on every pass - which is correct, and necessary for disconnect detection - and in doing so it closed a window the handoff had been quietly relying on. The dependency was never written down anywhere, which is exactly why it broke. ★★ h9 FIX: the hub sweep does not arm while gAsProbeOK or gAsNeedBulk is pending, and it no longer clears gEnumDev merely to ask a port a question. ★ WATCH: after 'SELFPROBE COMPLETE' you should now see 'n5 handoff at task level', then the Eusb publish, then 'device EXPOSED to the OS as its own drive' - for BOTH slots. ===");
    ehci_os_log("=== (h8) fixes an ADDRESS bug h7 introduced. ★ ACTIVATOR UNCHANGED - n4g still pairs. ★★ h7 REGRESSED what h6 had working, and it was mine. h7 arms the hub sweep with no slot chosen (gAs.dev = -1) so that merely ASKING a port a question does not churn the slot table. But case 0 computes gAs.addr = DEV_ADDR(dev) on entry, and with dev = -1 that resolves to DEV_ADDR(0) = 1 for EVERY downstream device. The h7 log shows it plainly: both drives came up with outEp.addr 0x0001 - address 1, twice. Worse, create_bulk matches an existing endpoint entry on address+endpoint and RE-TAGS its owner (the n18 fix, correct for a device re-enumerating), so the SECOND device stole the FIRST's endpoint registrations; slot 0 was left with none, its probe stalled, and neither drive ever reached AddDrive. Both enumerated and recorded geometry, and neither was ever EXPOSED. ★★ THE LESSON: h7 moved WHEN the slot is chosen without moving what DEPENDS on that choice. The compiler cannot help - DEV_ADDR(0) is a perfectly valid expression - which is the same shape as n19 declaring a dev argument and then ignoring it. ★★ h8 FIXES: gAs.addr is recomputed at the moment the slot is allocated, and the assigned address is LOGGED. Plus a new invariant in create_bulk: if an address+endpoint entry already belongs to a DIFFERENT slot that is still in use, that is two devices on one address and it now says so loudly. That check would have caught this in one line of log instead of a hardware cycle - it is the third time an address or index has been wrong here (n17, h4 unused HUB_ADDR, h7 DEV_ADDR(0)). ★ WATCH: 'h8 slot allocated for this downstream device; USB address' should read 1 for the first drive and 2 for the second, and 'ADDRESS COLLISION' must never appear. ===");
    ehci_os_log("=== (h7) TIER 1 COMPLETE - downstream connect AND disconnect tracking. ★ ACTIVATOR UNCHANGED - n4g still pairs. ★★ h6 WORKED: both drives behind the Apple Cinema Display hub mounted at high speed, first time a USB 2.0 device has been driven through an external hub on Mac OS 9. Port 3 (the display's own low-speed brightness/power HID) correctly skipped, ports 1 and 2 reset to wPortStatus 0x0503 = CONNECTION|ENABLE|POWER|HIGH_SPEED, SanDisk Ultra 62GB to slot 0 drive 10 and Generic Flash 4GB to slot 1 drive 11, each with its own geometry. ★★ WHAT h6 STILL COULD NOT DO: a port holding a device was SKIPPED on later sweeps, so a downstream DISCONNECT was never noticed - the slot stayed in use and the volume stayed mounted over a device that had physically left - and a drive could not be re-inserted after an eject because the port stayed taken forever. Both are the same missing piece. ★★ h7: the sweep now VISITS EVERY PORT every 500 ms and decides from its status against what we already know. Order matters - is-it-already-ours is asked BEFORE is-it-connected, because the interesting case is a port that WAS ours and is now empty. On a downstream removal the slot is freed, its endpoints released, and the removal handed to the SAME task-level rearm a root-port pull uses, so the volume unmounts properly instead of lingering. A non-high-speed port is still remembered so it is not reset every sweep, but its REMOVAL is now watched too, so the next device plugged in there gets a fresh look instead of inheriting the old verdict. No slot is allocated merely to ask a port a question any more - that happens only once a high-speed device actually needs enumerating, which also ends h6's churn of allocating and releasing a slot on every sweep of an empty port. ★ WATCH: 'h7: a device behind the hub was UNPLUGGED' when you pull a drive out of the display, with that volume - and ONLY that volume - unmounting. Then re-insert it and expect it to come back. ===");
    ehci_os_log("=== (h6) TIER 1, round 4 - the speed gate was reading the spec wrong. ★ ACTIVATOR UNCHANGED - n4g still pairs. ★★ h5 WORKED almost all the way: hub claimed, re-addressed to 15 and CONFIRMED there, configured, ports powered, sweep repeating and advancing correctly. And the drives DID appear - the h5 log shows port 1 and port 2 going from 0x0100 to 0x0101, CONNECTION|POWER, once they finished powering up. Then h5 SKIPPED them. ★★ THE BUG: h5 required PORT_HIGH_SPEED before reset. USB 2.0 section 11.24.2.7.1 - the high-speed determination happens DURING the reset handshake (the chirp), so a hub CANNOT report HS on a port it has not reset yet. LOW speed IS knowable at connect from the D-plus/D-minus idle state, which is why the display's genuine low-speed HID on port 3 reads 0x0301 correctly. So 0x0101 means connected, not low-speed, speed NOT YET DETERMINED - a high-speed CANDIDATE - and h5 was skipping precisely the devices it wanted. ★★ h6 FIX: pre-reset we skip only a port whose LOW_SPEED bit is set; anything else connected gets RESET, and the speed is read AFTER, where it is real. A port that turns out to be FULL-speed after reset is recorded in a skip mask so it is not reset again on every 500 ms sweep - the answer cannot change while the same device is plugged in - and the mask is cleared when the hub goes. Also added: a second hub is now parked with a clear message instead of overwriting the first hub's bookkeeping (nested hubs are not supported). ★ WATCH: 'connected and NOT low-speed: a high-speed CANDIDATE', then 'HIGH-SPEED confirmed by the reset handshake', then a normal mount. ===");
    ehci_os_log("=== (h5) TIER 1, round 3 - re-address the hub in the state the SPEC allows, and never let a sweep spin. ★ ACTIVATOR UNCHANGED - n4g still pairs. ★★ WHAT h4 GOT WRONG, both mine: (1) h4 sent SET_ADDRESS to a hub that was ALREADY CONFIGURED. USB 2.0 section 9.4.6 defines SET_ADDRESS only for the Default and Address states, so a configured device may reject it - and this one did. We then talked to address 15 forever and every request failed: 178 identical failures at step 0x46, the first GET_PORT_STATUS of every sweep. We never SAW the rejection because ctl_done() cannot tell a STALL from success, the same blindness pb_ready() has had since p1a. (2) a sweep failure never advanced gHub.scanPort, so port 1 was retried those 178 times and the engine never idled long enough for a ROOT-port device to enumerate. THAT is the reported 'the stack stopped looking for USB 2.0 devices' - it never stopped, it was stuck on one hub port. ★★ h5 FIXES: the re-address now happens while the hub is still in the ADDRESS state, before SET_CONFIGURATION, which is exactly where the spec permits it; and it is CONFIRMED by re-reading the device descriptor at the new address rather than trusting a transfer that cannot report a STALL - if the hub does not answer there we abandon it and park, instead of spinning. A sweep failure now ADVANCES past the offending port, hub failures get their own bounded budget of 6 and do NOT consume the root-port retry policy, and when that budget is spent the hub is UNCLAIMED and its root port parked so the stack falls back to proven n27 behaviour. The sweep also refuses to run on a parked or unclaimed root port. ★ TEST: drives in the display before or after the leg. WATCH for 'h5 hub CONFIRMED at its new address' - if that line is absent the re-address still is not taking and nothing downstream will work. Then a sweep line per port, then a mount. ===");
    ehci_os_log("=== (h4) TIER 1, round 2 - the hub port walk now REPEATS, and the hub moves off the device address range. ★ ACTIVATOR UNCHANGED - n4g still pairs. ★★ WHY h3 FOUND NOTHING, and it was NOT a design failure: the walk worked end to end - hub claimed on root port 3, configured, ports powered, all three ports walked, port 3 correctly skipped as low-speed. But ports 1 and 2 read wPortStatus 0x0100 with wPortChange 0x0000: powered, NO connection, and no attach ever recorded. A hub reports CURRENT connect status, so at the instant we looked the hub genuinely saw nothing. The drives had been inserted into UNPOWERED ports and only began booting when our SET_PORT_FEATURE(PORT_POWER) arrived; a flash drive commonly needs several hundred ms to assert its pull-up, and h3 looked exactly once, 120 ms after power-on, and never again. ★★ h4 FIX 1 - THE SWEEP REPEATS. A claimed hub's ports are re-swept every 500 ms, skipping any port that already has a device so a mounted drive is never re-enumerated under the File Manager. This is also the fix for h3's hot-plug limitation: a drive plugged in later is simply seen on a later pass. The 500 ms gap is armed in ONE place at wrap, because several skip paths advance the port counter and none should have to remember; without it the sweep would restart on the next 8 ms heartbeat and flood the hub with control traffic, starving a mounted drive's I/O. ★★ h4 FIX 2 - AN ADDRESS COLLISION THE h3 LOG EXPOSED. h3 left the hub on the address it got during its own enumeration, DEV_ADDR(0) = 1, and then released slot 0 - so the FIRST downstream device would have been given address 1 as well. Two devices on one address is the n17 defect that corrupted a mounted volume. It did not fire only because nothing downstream enumerated. The hub is now re-addressed to 15, clear of DEV_ADDR for every slot, leaving both gDev slots free for drives. ★ TEST AS BEFORE, and this time insertion order does not matter: plug drives into the display's ports before OR after the leg. Expect a sweep line per port every ~500 ms until a drive appears, then normal enumeration and mount at high speed. ===");
    ehci_os_log("=== (h3) TIER 1 HUB SUPPORT - a high-speed drive behind a high-speed hub. ★ ACTIVATOR UNCHANGED - n4g still pairs. Built on validated n27. ★★ WHAT CHANGED: the hub's root port is now CLAIMED instead of parked. We configure the hub, power its ports, then walk them one at a time: read GET_PORT_STATUS, and for a HIGH-SPEED device SET_PORT_FEATURE(PORT_RESET), poll until the hub clears PORT_RESET and reports ENABLED, acknowledge C_PORT_RESET and C_PORT_CONNECTION, wait TRSTRCY, then join the PROVEN enumeration path at the device descriptor. From that point on it is ordinary address-based control traffic which the hub forwards transparently, so there is ONE enumeration path and no second copy to keep in sync. FULL/LOW-SPEED ports are logged and SKIPPED - they need split transactions (tier 2) - and left powered and untouched. ★ USB_MAX_DEV STAYS AT 2: a hub has no bulk endpoints, so it gets an address (15) and a small state struct, never a gDev slot, and the DMA page layout is untouched. The slot the hub borrowed during its own enumeration is released. ★ ONE PORT AT A TIME IS REQUIRED, not just simpler: a freshly reset downstream port is enabled and its device answers at address 0, so two enabled at once would both hear address-0 traffic. ★★ EXPECT ON THIS DISPLAY, from the h2 survey: ports 1 and 2 EMPTY, port 3 LOW-SPEED (the display's own brightness/power HID) which will be SKIPPED. So with nothing plugged into the display you should see the hub claimed, three ports walked, and nothing mounted - that is SUCCESS. ★ THE REAL TEST: plug a USB 2.0 drive into either of the display's external ports; it should land on port 1 or 2 and MOUNT AT HIGH SPEED. Then pull the display's USB leg and confirm 'the claimed HUB was unplugged' with the drive unmounting cleanly. ===");
    ehci_os_log("=== (h2) HUB PROBE, round 2 - CONFIGURE the hub first. ★ ACTIVATOR UNCHANGED - n4g still pairs. ★★ h1 RESULT: the Apple Cinema Display hub ENUMERATES CLEANLY on our stack. Device descriptor and hub class descriptor both read fine - class 0x09, bDeviceProtocol 0x02 = high-speed hub with MULTIPLE transaction translators, VID 0x05ac PID 0x911d, bNbrPorts 3, wHubCharacteristics 0x0080 (GANGED power switching, global overcurrent, port indicators supported, TT think time 8 FS bit times), bPwrOn2PwrGood 50 = 100 ms, bHubContrCurrent 100 mA. ★★ WHAT h1 GOT WRONG - OUR BUG, NOT THE HUB'S: GET_PORT_STATUS failed at step 0x3f because this branch runs BEFORE the SET_CONFIGURATION at the bottom of enumeration, so the hub was still in the ADDRESS state. USB 2.0 section 9.4: a device in Address state need only answer standard DEVICE requests, and GET_PORT_STATUS is a class request with recipient OTHER (a port), which needs the CONFIGURED state. GET_DESCRIPTOR(0x29) is recipient DEVICE - which is precisely why that one worked and the port query did not. ★★ h2 ADDS: SET_CONFIGURATION, then SET_PORT_FEATURE(PORT_POWER) on each downstream port, then a 120 ms wait for bPwrOn2PwrGood, then GET_PORT_STATUS per port with the same decoding. ⚠ NOTE h2 is NO LONGER strictly read-only - SET_CONFIGURATION and PORT_POWER are writes to the DEVICE. It still performs NO PORTSC WRITE, which is the one this controller punishes (n11 killed USB until reboot that way). Configuring a device we enumerated and powering a hub's ports are the ordinary things any hub driver does. The port is still PARKED afterwards, so steady-state behaviour is unchanged. ⚠ A probe failure retries enumeration 3x and then parks; the later tries failing at step 0x02 is an artifact of aborting mid-enumeration, not a second bug. n24's un-park means pulling the hub recovers the port. ★ WATCH: 'h2 hub CONFIGURED', then one decoded line per port 1-3. ===");
    ehci_os_log("=== (h1) HUB RECONNAISSANCE PROBE. Everything in the VALIDATED n27 plus a READ-ONLY probe of a downstream hub. ★ ACTIVATOR UNCHANGED - n4g still pairs. ★★ WHY: a high-speed hub CANNOT be handed to Apple's 1.1 companion on this NEC controller - Port Owner does not stick for a device that enabled at high speed (n11's run refuted it), which is why n12 PARKS such a port instead. So nothing works behind a hub on our ports today, and hub support is the only route to one working at all, not merely a faster one. ★★ WHAT THIS BUILD DOES: in the exact branch where a high-speed non-storage device is parked, it first asks the device GET_DESCRIPTOR(DEVICE); if bDeviceClass is 0x09 it reads the hub class descriptor (type 0x29) and then GET_PORT_STATUS for every downstream port, decoding each as EMPTY/UNPOWERED, HIGH-SPEED (tier 1, normal transfers, no splits), or FULL/LOW-SPEED (tier 2, needs split transactions). Then it PARKS the port exactly as n27 did. ★ STRICTLY READ-ONLY: only GET requests. No SET_PORT_FEATURE, no PORT_POWER, no PORT_RESET, no PORTSC write of any kind. The device is already enumerated and idle here, so asking it questions changes nothing, and post-probe behaviour is identical to n27. ⚠ If ports read !POWER with no connection that is the ANSWER, not an absence of devices - the next probe would add SET_PORT_FEATURE(PORT_POWER). ★ TARGET: the Apple 20in Cinema Display hub, VID 0x05ac PID 0x911d. WATCH for 'h1 HUB DETECTED', bNbrPorts, and one decoded line per downstream port. ===");
    ehci_os_log("=== (n27) the eject alert names the device and its SPEED, and drops Apple's 'cartridge'. SUPERSEDES n25 AND n26 - install this one, it contains both (n25's offLinErr fix and n26's self-retracting alert). ★ ACTIVATOR UNCHANGED - n4g still pairs. ★★ NEW WORDING, at the user's request: 'You may now remove the USB 2.0 device \"NAME\" because your Macintosh is finished with it.' + 'The device will not be remounted until it is physically removed.' Apple's original says 'remove the CARTRIDGE from the USB device' - Zip/SyQuest-era vocabulary for removable media inside a fixed drive, which describes nothing that exists on a USB stick. ⚠ This DELIBERATELY relaxes the standing 'match Apple's OHCI strings word for word' rule for THIS message only; do not correct it back, the deviation IS the requirement. ★ The speed is MEASURED, not assumed: gDev[].hiSpeed is recorded at enumeration from the port's ENABLE bit after reset, which is the EHCI speed determination. Today it is always 2.0, because a port that does not enable is surrendered to Apple's 1.1 companion and THEIR stack mounts it and posts THEIR alert - we never own a 1.1 device, so the 1.1 string will not appear from us yet. It becomes reachable when real hub support lands (split transactions for FS/LS devices behind a high-speed hub), which is why the branch exists rather than a hardcoded '2.0'. ===");
    ehci_os_log("=== (n26) the eject alert now RETRACTS ITSELF when its drive is unplugged. SUPERSEDES n25 - INSTALL THIS INSTEAD, it contains n25's offLinErr fix too, so n25 never needs to be run. ★ ACTIVATOR UNCHANGED - n4g still pairs. ★★ Apple's own alert vanishes on its own once the referenced drive is physically removed, with no OK click; ours lingered until dismissed. The message IS 'you may now remove the cartridge', so the moment the cartridge is removed it has served its purpose. The posted notification now records WHICH SLOT it belongs to (gNMDev) and the task-level rearm retracts exactly that one - with two drives mounted, unplugging one must not clear an alert that is still about the other. ★ NOTE the 'Please reconnect the USB device' alert deliberately does NOT auto-retract: it asks the user to plug the device BACK IN, so the device being absent is the entire reason it is showing. ★★ STILL OUTSTANDING - the alert's LOOK. Apple draws theirs with the Appearance Manager's StandardAlert (two separate strings, bold headline + smaller explanation, auto-sized window, icon); we pass one flat string to NMInstall, which the Notification Manager renders in its own older uniform style. Matching that needs the alert drawn from a real task-level APPLICATION context, and our only one is the activator's idle loop via tickFn - which raises a genuine unknown: a faceless background app putting up a MODAL alert may not come to the front properly, and that is very likely why Apple pairs NMInstall (to get attention) WITH StandardAlert (to draw). Not attempted here rather than risk wedging a working stack on a cosmetic change. ===");
    ehci_os_log("=== (n25) an IN-FLIGHT transfer on a yanked drive now completes offLinErr, not ioErr. ★ ACTIVATOR UNCHANGED - n4g still pairs. ★★ THE n24 REPORT: hard-removing an IDLE drive was silent and clean and the survivor kept working, but hard-removing one that had just been copied to/from produced the OS's generic 'there may be a problem with the disk' - which is NOT one of Apple's strings, so it came from the File Manager, not us. CAUSE: n6e made a NEWLY SUBMITTED read/write on absent media return offLinErr (-65), 'R/W requested for an off-line drive', exactly so the FM treats the medium as ABSENT rather than BAD. But a transfer already accepted (we returned kIOBusyStatus) completes through bio_finish instead, and a failed one carried gBioResult = -36 = ioErr - and ioErr on a mounted volume is the FM's cue that the DISK IS DAMAGED, the very alert n6e existed to prevent. So it was the same defect on the path n6e did not cover, which is why only the drive with I/O in flight showed it. FIX: bio_finish completes with offLinErr when the request's device is no longer attached (gDev[dev].inUse cleared by the disconnect handler - the signal available at interrupt level, and it means precisely what offLinErr means). NOTE this is an ERROR-CODE fix, not a string fix: Apple's own removal alert is 'Please reconnect the USB device X / It is IN USE and must be reconnected now', which we already post via notify_reconnect when an unmount fails as busy - a clean removal of an idle drive is silent in Apple's stack too, so n24's silent first removal was correct. ★ WATCH: copy files to a drive, then hard-remove it while the other stays mounted - expect NO 'problem with the disk', the volume just going away, and the survivor unaffected. ===");
    ehci_os_log("=== (n24 history) the per-device SWEEP - three findings from auditing all 142 file-scope globals against 'with two devices live, and with a slot reused by a DIFFERENT device, is this the right one, and who resets it?' ★ ACTIVATOR UNCHANGED - n4g still pairs (no gSvc field offsets move, magic still 'EUS2'; ejectFn's signature gained a slot but it sits at offset 88, past the activator's view which ends at quitFn/80, and the activator never calls it - its only caller is the block driver embedded in this same ROM). ★★ (1) gDevName was ONE Str63 holding the most-recently-enumerated device, so with two drives mounted Apple's eject alert named the WRONG drive. Now per-slot, and ejectFn takes the slot the Finder ejected. (2) gPortParked was PERMANENT - park_port marks a port we cannot drive so we stop re-arming, and nothing ever cleared it, so the port stayed dead until the driver reloaded. But the reason for a park is the DEVICE, and a parked port is still OURS so its disconnect IS visible (unlike a CEDED port, where CCS reads 0 either way and removal genuinely cannot be seen). Un-parked on disconnect and LOGGED; the n21 run parked two ports over a bug n22 fixed and both stayed unusable for the session. (3) the v36 write instrumentation bound-checked every device's LBA against gPBlkCnt = slot 0's capacity: false 'LBA off the device' flags for a bigger drive, missed ones for a smaller. Now uses gDevBlkCnt[dev]. ★ CLASSIFIED AS CORRECT-AS-IS: gPB/gCtl/gBot/gAs*/gSense are shared but strictly SERIAL by deliberate invariant (one enumeration and probe at a time); the aggregate counters are global by design. ★ WATCH: with two drives mounted, eject each in turn and confirm the alert names the RIGHT device; and confirm 'n24: parked port UN-PARKED' appears if a port ever parks. ===");
    ehci_os_log("=== (n23 history) a removal now unmounts ONLY the drive that went away, and a non-slot-0 removal is reported at all. ★ ACTIVATOR UNCHANGED - n4g still pairs ('Eusb' layout/signatures identical since n19, magic 'EUS2'). ★★ TWO FIXES for the n22 report 'Finder-eject one drive, remove only that drive, and the OTHER drive disappears'. (A) blk_unmount_removed_volumes swept EVERY volume whose vcbDRefNum matched our block driver - but since n19 ONE block driver serves N drives, so that is ALL of them. Written when one drive meant 'all ours' == 'the removed one'; n19 made it wrong and nothing noticed until two volumes could coexist. It now skips any drive still reporting media present, asked of the block driver via kDriveStatus(8) since that fragment owns the slot->drive-number map - no ABI change - and it fails SAFE (any error reads as present, so a missed answer can never cause an extra unmount). (B) a disconnect on a slot other than 0 reported NOTHING: the rearm hinged on gSelfEnumDone, which the PREVIOUS rearm had already cleared. The n22 log is unambiguous - every disconnect on port 4 (slot 0) produced a rearm and media-GONE; all four on port 2 (slot 1) produced silence, so that volume was never unmounted and gRearmDev kept pointing at the old slot. Now any slot WE OWNED losing its device reports it. ★ WATCH: two drives mounted, Finder-eject and pull ONE - the OTHER must stay mounted and usable, and only the pulled one's volume should vanish. Test it BOTH WAYS ROUND, since slot 0 and slot 1 took different paths before this build. ===");
    ehci_os_log("=== (n22 history) a reused device slot now resets its HARDWARE DATA TOGGLE. ★ ACTIVATOR UNCHANGED - n4g still pairs ('Eusb' layout/signatures identical since n19, magic 'EUS2'). ★★ THE n21 FAILURE: after one drive was pulled and re-inserted, a SECOND concurrently-connected drive was ignored - it enumerated fine but its INQUIRY stalled at BOT step 9 with gDownErr 0 and gDownTimeouts 0, three times, then the port was parked. CAUSE: since r63 the bulk data toggle lives in HARDWARE in the QH overlay (DTC=0), and epq_arm_idle is the ONLY code that clears it - epq_program writes epChar and never touches the overlay. reconnect_reset arms ONE slot's QHs, the slot being reset, and every pull tested happened to be slot 0's, so slot 0 got re-armed for free and slot 1 never did. A slot freed by a pull and later handed to a DIFFERENT device therefore kept the PREVIOUS device's toggle; the newcomer leaves USB reset at DATA0, the QH expects DATA1, and a toggle mismatch is not an error, it is silence - the qTD never retires. FIX: create_bulk epq_arm_idles the QH it is about to program, so any slot being (re)assigned starts at DATA0. Safe because create_bulk has already ase_quiesce()d and each direction owns its own QH. ★ WATCH: mount both drives, eject+pull one, re-insert, then connect the second again - BOTH must mount, repeatedly, and 'BOT transport failure at step 0x00000009' must NOT appear. Also re-run the n21 eject-both-then-pull-one check (must still not auto-mount) and try pulling the OTHER drive first. ===");
    ehci_os_log("=== (n21 history) fixes the n20 eject-both-then-pull-one spurious remount. ★ ACTIVATOR UNCHANGED - n4g still pairs with this ROM ('Eusb' layout and signatures identical since n19, magic still 'EUS2'). ★★ TWO FIXES, both 'consult the fact we already recorded': (1) gSelfEnumPort is a single which-port-next variable cleared ONLY when that same port's device is pulled, so pulling a device on a DIFFERENT port left it aimed at the SURVIVOR's port while the rearm cleared gSelfEnumDone - enumeration then re-ran on a port another slot already owned and dev_alloc handed it the slot the pull had just freed, so the surviving drive was re-announced under the PULLED drive's drive number. Now any port a live slot owns (gDev[].probedPort, recorded since n13) is refused, and the refusal is LOGGED. (2) the task-level rearm called reconnect_reset(0) instead of reconnect_reset(gRearmDev) - the third time in this driver a device index was plumbed in and then discarded at one call site. Latent: every pull tested so far has been slot 0's; pulling the SECOND drive first would have reset the state of the drive still plugged in. ★ WATCH: mount both, Finder-eject BOTH, then pull ONE - the other must STAY unmounted, and 'n21: enumeration re-armed on a port a LIVE slot already owns - refused' should appear. Then repeat pulling the OTHER drive first. ===");
    ehci_os_log("=== (n20 history) TWO VOLUMES, each reading its OWN medium. ★ ACTIVATOR IS UNCHANGED - n4g still pairs with this ROM: the 'Eusb' layout and signatures are identical to n19, so the magic stays 'EUS2' and only the driver changed. ★★ WHAT n19 GOT WRONG: n19 added the device argument to readFn/writeFn/submitFn (the change that forced the ABI bump) but three call sites then threw it away - ehci_usb_read/ehci_usb_write ignored it and talked to gDev[0], and ehci_usb_submit stamped r->dev = 0 on every request. So the block driver's per-slot routing was correct, AddDrive gave each device its own drive number and its own geometry, and then EVERY read and write went to device 0 regardless. On hardware that showed as the second stick mounting as a SECOND COPY of the first (same name, same contents), and mounting correctly only once the first was pulled and it re-enumerated into slot 0. Fixed: submit stamps the real slot, the sync read/write paths route on dev, and bio_advance's legacy phases route on r->dev. An out-of-range slot is now REFUSED, never clamped to 0. ★ WATCH: insert drive A (mounts), then drive B while A is mounted - EXPECT two DIFFERENT volume names/sizes/contents, 'device EXPOSED to the OS as its own drive' for slot 0 AND slot 1, and two different AddDrive drive numbers. ★★ VERIFY CONTENTS AND SIZES, and copy to BOTH: the write path carried the same defect and a cross-device write is silent. Scratch drives only. ===");
    ehci_os_log("uimInitialize: entered (dispatch slot 0)");
    /* n0: dump the DoDriverIO trace HERE — deliberately BEFORE the gBroughtUp early-return below, because if
     * the Device Manager already opened + brought us up at boot then gBroughtUp is 1 and that return would
     * fire, hiding the very evidence we came for. */
    { extern void ehci_os_n0_dump(void); ehci_os_n0_dump(); }
    /* IDEMPOTENT: a SECOND app launch (LoadUIMForEntry) re-enters here while the driver is already up.
     * Re-running the bring-up armed a 2nd self-re-arming heartbeat timer, corrupted gSavedHandler (it
     * saved OUR vhub_isr instead of the companion's), re-HCReset the live controller, and leaked the DMA
     * pool -- and that duplicate interrupt state is what crashed at interrupt level during the next warm
     * reboot's teardown. So if we are already up, do NOTHING and return; the relaunch uses the running
     * driver (its reconnect self-probe handles a fresh insert). */
    if (gBroughtUp) { ehci_os_log("uimInitialize: driver already up -> SKIP re-init (idempotent)"); return noErr; }
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
        gBroughtUp = 1;                          /* mark up so a 2nd launch skips the re-init (see above) */
        ehci_os_log("  vhub_start_service done — INIT COMPLETE");
        /* n10: hook the Shutdown Manager NOW, at task level, while the File Manager is up. Without this a
         * warm reboot inherits a running controller with live DMA and interrupts — the frozen-cursor
         * desktop the user hit, which a cold boot cures. ehci_os_boot_quiesce() was meant to cover this but
         * lives on the kInitialize path, which n0 proved is never called. */
        { extern void ehci_vhub_install_shutdown_hook(void); ehci_vhub_install_shutdown_hook(); }
        /* ★ h24: publish the 'Eusb' service HERE, while we are still at task level (LoadUIMForEntry), so an
         * external pump can find tickFn before anything has mounted. Until h24 the selector appeared only
         * after a probe succeeded, which made the n4h USL-pump experiment unable to tick at all — see the
         * long note on ehci_vhub_publish_service_early(). Publishing is inert before a mount. */
        { extern void ehci_vhub_publish_service_early(void); ehci_vhub_publish_service_early(); }
        /* ★★★ h87: make the parcel-claimed unit-table entry dispatchable (the ASP Devices-and-Volumes
         * crash — see the long note at ehci_os_fix_native_dce). HERE because this is proven task level
         * (we are logging), the unit table exists, and it must run before any user can reach ASP. */
        ehci_os_fix_native_dce();
        /* ★ h31: our HCReset above cleared CONFIGFLAG and handed every port to the companions for the
         * bring-up window; on this chip an active companion connection then survives hc_start's
         * CONFIGFLAG=1. Evict such ports now, while nothing can be mounted yet. */
        { extern void ehci_vhub_bringup_owner_sweep(void); ehci_vhub_bringup_owner_sweep(); }
        /* ★ h43: and one dump at BRING-UP, the earliest task-level moment we exist. Together with the
         * defer-window dumps this brackets the prompt: bring-up (before any exposure of ours), every ~2 s
         * through the defer, then before/after/+15 s around the install. If a foreign entry for this medium
         * ever exists, one of those frames must contain it. */
        { extern void ehci_vhub_queue_dump_public(const char *when);
          ehci_vhub_queue_dump_public("=== h43 QUEUES **AT BRING-UP** (earliest we can look; nothing of ours "
                                      "is exposed yet) ==="); }
        /* ★★★★★ h26 APP-LESS: build the NMUPP for the task-level pump. Here because NewNMUPP allocates, and
         * this is the one place we are guaranteed task level (LoadUIMForEntry). After this, a parked
         * task-level job reaches task level with no application of ours running. */
        { extern void ehci_vhub_appless_init(void); ehci_vhub_appless_init(); }
    } else {
        ehci_os_logx("  ehci_os_init FAILED e=", (unsigned long)e);
    }
    return (OSStatus)e;
}
static OSStatus uimFinalize(void)
{
    /* On a clean UIM unload: FIRST stop the driver cold (heartbeat timer, ISR, interrupts, schedules) so
     * nothing keeps running / dangles after us, THEN hand the root ports back to the companion 1.1
     * controllers so a drive plugged in afterward is seen by the OS's own OHCI driver. */
    ehci_vhub_stop_service();
    ehci_hc_release_ports(&gSoftc);
    gBroughtUp = 0;   /* allow a fresh bring-up after a clean finalize */
    return noErr;
}

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
 * address, so it needs no endpoint bookkeeping; bulk/isoch are later milestones.)
 * ★★★★ h94: slots 18/19 (AbortPipe/DeletePipe) have NEVER been called on this hardware — zero captures
 * in every banked log, including the whole rude-removal era. This G4's USL frees its pipe/transfer
 * structures on removal WITHOUT telling the UIM, which is why stubbing them was never the bug and
 * implementing them would fix nothing: the removal safety lives in ehci_vhub.c's usl_retire_device
 * (the disconnect funnel + issue refusal + reap orphan gate). If a slot 18/19 call EVER shows up in
 * slot_trace, capture its args — that is the day these two earn real bodies. */
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
        if (gSlotCount[27] - last27 >= 8192) { last27 = gSlotCount[27]; ehci_os_logx("slot27 ticks(total)", gSlotCount[27]); }   /* v47: throttled 512->8192 (16x less FSWrite/FlushVol) = truly lean timing + a coarse liveness heartbeat; the v47 STALL dump carries the fine-grained state during a freeze */
        { static int p0logged = 0; UInt32 sb = 0, sm = 0; ehci_vhub_spoof_stats(&sb, &sm);   /* p0i3: confirm the descriptor spoof engaged (log once, task level) */
          if (!p0logged && (sb | sm)) { p0logged = 1; ehci_os_log("p0i3 DESCRIPTOR SPOOF fired:"); ehci_os_logx("  bcdUSB patches", sb); ehci_os_logx("  bulk-maxpkt patches", sm); } }
        /* p0i3b: one-shot config-descriptor diagnostic (why did the maxpkt spoof never fire?). Fire as soon
         * as a FULL config fetch (actual>9) is captured — the snapshot is complete by then; else after 2 cfg
         * fetches; else a ~5s (300-tick) settle after the first cfg fetch (so a header-only/no-full run still
         * reports). down_reap captures pre-spoof, so the raw bytes are what the DRIVE sent, not our lie. */
        { static int cfgDone = 0; static UInt32 cfgT0 = 0;
          UInt32 gdDev=0,gdCfg=0,gdStr=0,gdOther=0,cfgFull=0,maxAct=0,slen=0,epN=0,ep4[4]; UInt8 snap[48];
          UInt32 nowT = *(volatile UInt32 *)0x016AUL;
          ehci_vhub_cfgcap(&gdDev,&gdCfg,&gdStr,&gdOther,&cfgFull,&maxAct,&slen,&epN,ep4,snap);
          if (!cfgDone && gdCfg > 0 && cfgT0 == 0) cfgT0 = nowT;
          if (!cfgDone && (cfgFull > 0 || gdCfg >= 2 || (cfgT0 && (nowT - cfgT0) >= 300u))) {
              int q, w; cfgDone = 1;
              ehci_os_log("p0i3b CONFIG-DESC DIAGNOSTIC (why maxpkt spoof didn't fire):");
              ehci_os_logx("  GET_DESC dev   count", gdDev);
              ehci_os_logx("  GET_DESC cfg   count", gdCfg);
              ehci_os_logx("  GET_DESC str   count", gdStr);
              ehci_os_logx("  GET_DESC other count", gdOther);
              ehci_os_logx("  cfg FULL(actual>9) count", cfgFull);
              ehci_os_logx("  cfg max actual", maxAct);
              ehci_os_logx("  cfg max setup.wLength", slen);
              ehci_os_logx("  cfg endpoints found", epN);
              for (q = 0; q < 4; q++) ehci_os_logx("  ep (off<<24|attr<<16|maxpkt)", ep4[q]);
              for (w = 0; w < 12; w++)   /* 48 raw bytes, 4/word: b0_3, b4_7, ... b44_47 */
                  ehci_os_logx("  cfg snap b",
                      ((UInt32)snap[w*4]<<24)|((UInt32)snap[w*4+1]<<16)|((UInt32)snap[w*4+2]<<8)|snap[w*4+3]);
          } }
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
            { extern UInt32 ehci_vhub_failsubmit(UInt32 *); UInt32 rtr = 0, sub = ehci_vhub_failsubmit(&rtr);
              ehci_os_logx("  v40 submitLba (untouched orig)", sub);   /* == 'lba' above => FM handed us garbage; differs => corrupted after submit */
              ehci_os_logx("  v40 gBioRetry (timeouts b4 fail)", rtr); }
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
