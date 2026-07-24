# Technical notes

How a USB 2.0 disk stack was built for an OS that never had one, and the two hard bugs that
took the longest to crack.

## The problem

Classic Mac OS ships **OHCI** (USB 1.1) host-controller drivers but **no EHCI** (USB 2.0)
driver. A USB 2.0 PCI card has one EHCI controller (high-speed) plus OHCI "companion"
controllers (1.1). On OS 9, the EHCI function is never claimed — so a ROM-less card's
high-speed controller does nothing, and devices only work (if at all) via the 1.1 companions.

There is, however, a documented extension point: Apple's USB Manager can load a third-party
**UIM** (USB Interface Module — a host-controller plugin) for a controller it doesn't itself
support. That's the hook this project drives.

## Loading as a UIM

The USB Expert finds a UIM for a controller node via `FindDriverCandidates`. A ROM-less PCI
card exposes no `driver,AAPL,MacOS,PowerPC` property, so nothing matches. We fix that from an
app:

1. Find the EHCI node in the Name Registry (`class-code` `0x0C0320` = serial-bus / USB /
   EHCI).
2. Create the properties a declaration-ROM card would have supplied — most importantly a
   `driver,AAPL,MacOS,PowerPC` (our ndrv's raw PEF) and a `driver-descriptor`. RE of
   `FindDriverCandidates` showed it sets its "found a prop-based driver" flag only after a
   successful read of `driver-descriptor`.
3. Call the Expert's `LoadUIMForEntry(node)`. It now finds our driver, loads it as the UIM
   plugin (via `ThePluginDispatchTable`), builds the Name Registry parent-deviceRef entry,
   and calls `USBAddBus`.

Our ndrv exports only the two data symbols a UIM needs — `TheDriverDescription` (sig `mtej`,
match `pciclass,0c0320`) and `ThePluginDispatchTable` (version word + 28 host-controller entry
points). The dispatch slots are thin wrappers over the controller/virtual-hub engine.

## Loading from the Mac OS ROM (this release)

The method above worked, but the app had to recreate the controller's driver properties on every
launch. This release moves that binding into the **Mac OS ROM**, so the OS itself loads the driver at
boot, exactly as it would for a card that shipped with a declaration ROM.

`rom/usb_rom_inject.py` (built on Elliot Nunn's Mac OS ROM toolchain) injects a **parcel** into the
ROM: a `driver,AAPL,MacOS,PowerPC` property carrying our ndrv's PEF, plus the driver descriptor,
attached to the EHCI controller node (`pciclass,0c0320`). At boot the OS finds the parcel, prepares
the driver, and records it against the node. This is the same binding the app used to create at
runtime, now supplied by the ROM.

Two things had to be true for a ROM-loaded driver to be safe here:

- **Runtime flags `0x05`** (`kDriverIsLoadedUponDiscovery` + `kDriverIsUnderExpertControl`). The OS
  prepares the driver at boot but does not open it; opening it, and therefore the real bring-up,
  happens later through the USB Expert path.
- **A stash-only `kInitialize`.** `DoDriverIO`'s `kInitialize` runs at the early PCI-claim phase,
  before the File Manager and much of the Toolbox are safe to call. It must do nothing but record the
  node and return `noErr`. All of the actual controller bring-up (PCI enable, register map, EECP
  hand-off, schedules, run) is deferred to `kOpen`. This mirrors what the sibling SATA project learned
  the hard way: bring-up work at claim time hangs the boot.

The helper then activates the ROM-loaded driver exactly as before, by calling `LoadUIMForEntry(node)`,
which opens the driver (triggering `kOpen` and the bring-up) and installs it as the live UIM. So the
helper no longer carries or installs the driver; it only *activates* the one the ROM already loaded,
runs the self-probe, and mounts.

**Port hand-back on quit.** Because the driver now persists across a session and the helper can be
quit at will, quitting cleanly returns the root ports to the 1.1 companion: the helper clears the EHCI
`CONFIGFLAG` and sets each port's Owner bit, so plain USB 1.1 resumes with no reboot. The same
teardown runs from the driver's `kFinalize` / `uimFinalize`.

**Honest limit.** This does not make the mount seamless. OS 9's own mass-storage mounter still will
not advance a Hi-Speed device (it parks at `TEST UNIT READY` / `REQUEST SENSE` and never issues
`INQUIRY`), which is why the self-probe inside the helper is still what performs the mount. The ROM
integration is the foundation a future seamless path needs, not that path itself.

## The stack, layer by layer

- **Controller bring-up** (`ehci_os.c`, `ehci_hw.c`): PCI enable, map the operational
  registers from `AAPL,address`, EECP legacy hand-off, HCReset, allocate the periodic frame
  list + an async-schedule **anchor** queue head, program the registers, route the ports to
  EHCI, run.
- **Virtual root hub** (`ehci_vhub.c`): the UIM presents a fabricated 5-port hub to the USB
  Manager (device/config/hub descriptors, port status, `SET_FEATURE(PORT_POWER/RESET)`), so
  Apple's standard hub driver drives it and enumerates whatever is plugged into a physical
  EHCI port. Port connect/reset/enable are serviced from the real EHCI interrupt plus a
  periodic timer.
- **Downstream transfer engine** (`ehci_vhub.c`): **one persistent async queue head per
  endpoint** (control ep0, bulk-IN, bulk-OUT), each spliced into the reclamation ring for the
  life of the device, with the controller maintaining each bulk endpoint's **data toggle in
  hardware** (`DTC=0`). Transfers are queued by a race-free *dummy-qTD append* (a permanently
  inactive tail qTD is filled in place, its Active bit written last). A driver-owned **bounce
  buffer** stages all DMA (never directly to/from a File-Manager buffer — the latter freezes
  the mount). One transfer in flight at a time today. *(This replaced an earlier single shared
  queue head with a software-managed toggle — see Bug hunt #3 for why that mattered.)*
- **Self-driven SCSI probe** (`ehci_vhub.c`): once the device's bulk endpoints exist, the UIM
  drives **Bulk-Only Transport** itself — INQUIRY, READ CAPACITY, READ block 0 — proving the
  data path, then publishes a block read/write service via `Gestalt('Eusb')` (a TVector into
  the BOT engine + the geometry).
- **Block driver + mount** (`usb_disk.c`): a separate native ndrv, installed via
  `InstallDriverFromMemory`, consumes the `'Eusb'` service. Its `kInitialize` scans the Apple
  Partition Map (or a partitionless HFS volume — see below), `AddDrive`s the volume with a
  valid `DrvSts` status prefix, and the app `PBMountVol`s it. Reads/writes are **async**:
  `kRead`/`kWrite` enqueue a request and return `kIOBusyStatus`; the UIM runs the BOT on the
  interrupt/heartbeat and calls `IOCommandIsComplete`.

## Bug hunt #1 — intermittent enumeration (the EHCI async schedule)

For a long time the device mounted only **~1 boot in 3**. On the other boots the port came up
electrically enabled, the device was assigned an address, control transfers were issued — and
**every one timed out** (`done=0`), as if the device were unreachable.

Instrumenting the exact moment of timeout with the controller's own registers cracked it. The
controller was healthy every time (`USBCMD.RUN`+`ASE` set, `USBSTS.ASS` = async schedule
*running*, not halted, no host error) — but:

```
ASYNCLISTADDR = 0x01fa3000   (the async anchor QH)
our transfer QH = 0x01fa5000  (NOT what the controller was executing)
```

The transfer engine was installing its queue head by **overwriting `ASYNCLISTADDR`** — which
per the EHCI spec is unreliable while the async schedule is already running. Sometimes the
write landed (mount worked); usually it didn't, and the controller kept looping the empty
anchor, so our transfers never executed.

The fix was to do what EHCI intends (and what the code's own unused helper already
implemented): **splice the transfer QH into the anchor's reclamation ring**
(`anchor → ourQH → anchor`) and leave `ASYNCLISTADDR` pointing at the anchor for the life of
the controller. Deterministic — the controller reaches our QH every pass. Result: **4/4 clean
mounts**, and reliable on every boot since. (Also removed the stray Head-of-Reclamation bit
from the member QH — only the anchor heads the ring.)

Lesson: never move `ASYNCLISTADDR` on a live schedule; link/unlink queue heads into the ring
instead.

## Bug hunt #2 — the "Audio CD" mis-mount → pivot to HFS

An early version formatted the stick FAT and mounted it via **PC Exchange** (Foreign File
Access). Intermittently the volume came up **read-only as "Audio CD 1"** (files shown as
"Track 1/2"). The data path was provably clean (INQUIRY = direct-access disk, valid MBR,
block-0 read correct); the OS's Audio CD Access foreign-file plugin was simply winning the
claim arbitration for our driver-presented FAT volume, and File-Manager writes then failed
`wPrErr`. Answering `DriverGestalt('devt')` as a hard disk and declining the CD-ROM control
calls both helped inconsistently — the arbitration is a genuine, deep OS 9 Foreign-File-Access
problem (the sibling SATA project hit the same wall).

The clean escape was to **stop using Foreign File Access entirely**: format the drive as
**HFS/HFS+** and let OS 9's **built-in mounter** mount it. The built-in mounter never invokes
the foreign-file plugins, so there is no CD plugin to lose to — the mis-ID is structurally
impossible. The block driver's scanner handles both an Apple Partition Map (`'ER'` DDR →
`'PM'` entries → `Apple_HFS`) and a **partitionless HFS volume** (block 0 = zeroed boot
blocks, volume header `'BD'`/`'H+'` at block 2 → mount the whole device).

Trade-off: the stick is Mac-only. FAT cross-platform support is deferred to a future version
pending a real solution to the Foreign-File-Access arbitration.

## Bug hunt #3 — the large-copy wedge (a hand-rolled data toggle)

The stubbornest bug: normal file work was flawless, but a **large Finder copy** (hundreds of
MB, many files) would eventually stall — a file would fail with a "disk error," and pushing on
could wedge the machine and leave the volume needing repair. A self-contained 64 MB sequential
write verified byte-perfect every time; only the Finder's *interleaved* read/write/metadata
workload tripped it. The exact same flash drive was, and is, **completely reliable on the OHCI
(USB 1.1) stack** — used daily for years. So the hardware was innocent; the bug was ours.

Instrumentation (a tiny always-flushed health log + a controller-state snapshot captured at
the moment of a stall) walked it down by elimination:

- The async schedule and our queue head were **healthy** at every stall — controller running,
  schedule running, our QH linked. (An earlier theory that the QH was falling out of the ring
  was a **misread**: the `ASYNCLISTADDR` register reports the controller's *live position* in
  the ring, so catching it parked on the anchor looked like an "unlink" but wasn't.)
- A **destructive recovery we had added** — a USB Bulk-Only Reset fired on a stalled
  transfer — turned out to be what actually *froze* the machine; with it disabled, a stall
  degraded to a survivable error instead of a hang.
- The device was NAKing a **fresh command** (the 31-byte CBW) indefinitely — for the full
  watchdog, whatever we set it to. That is not flash back-pressure (which resolves); it is a
  **Bulk-Only-Transport phase desync**: the device still considered the previous command
  unfinished and refused the next one.

Root cause: the transfer engine used **one shared queue head, reprogrammed per transfer**
(control ↔ bulk-in ↔ bulk-out), with the USB **data toggle tracked in software**. That toggle
is correct on a tidy sequential stream but drifts under the Finder's rapid interleaving — and
once the host and device toggles disagree, the device silently wedges. OHCI (USB 1.1) never
hits this because its controller keeps a **per-endpoint** data toggle in hardware; our shortcut
had thrown that away.

The fix is to build the shape EHCI actually intends (and that OHCI's robustness comes from):
**one persistent queue head per endpoint**, with the controller maintaining the data toggle in
**hardware** (`DTC=0` on the bulk queues), and transfers appended via the standard **dummy-qTD**
technique instead of poking the queue-head overlay. Software never touches the toggle again.

Result: the 64 MB verify still passes byte-perfect, the foreground many-files stress test
completes **all 800 files** with **zero** timeouts (worst transfer pause 50 ms), and a real
Finder copy of a full **~800 MB / 1000+ file** folder completes with **zero errors**. The
multi-day wedge saga was one root cause: a hand-rolled toggle on a shared queue.

Lesson: don't reinvent what the host controller will do for you. A per-endpoint queue with a
hardware-maintained data toggle is not an optimization — it's the correct design, and the
reason the 1.1 stack was bulletproof all along.

## Bug hunt #4 — coexisting with the keyboard on an on-board controller

Moving from a PCI card to an **on-board** USB 2.0 controller (a Mac Mini G4) changed the game in
one way: there, the EHCI is one function of a multi-function chip whose **OHCI companion
controllers drive the machine's keyboard and mouse**, sharing both the physical ports *and* a
single PCI interrupt line with the EHCI. On a card the EHCI's ports were empty and its interrupt
was its own, so the driver simply seized the whole controller. Do that here and you take the
keyboard down with it.

The intended fix is a **per-port claim**: after routing the ports to EHCI, hand every port that
already holds a device back to the 1.1 companion (set its Port Owner bit) and claim only the empty
ports — so a drive inserted afterward comes up on EHCI at Hi-Speed while the keyboard and mouse
stay on the companion. A standalone register-poke test proved the idea (keyboard released, drive
links at high speed, input survives), but folding it into the real driver failed twice, and the
driver's own trace log walked down why:

- **Port Power Control (the keyboard "wouldn't release").** The log showed the keyboard's port
  coming up **EHCI-owned** — claimed, not released — every time. The chip reports *Port Power
  Control* (`HCSPARAMS` PPC=1), and per the EHCI spec **a port's Connect-Status reads 0 while the
  port is unpowered.** The driver's `HCReset` (run just before the claim) leaves every port powered
  off, so reading connect-status to decide "occupied vs. empty" saw the keyboard's port as *empty*
  and claimed it. (The standalone test dodged this by never doing an `HCReset` — the ports kept
  their power from the running OS.) Worse, once the keyboard sat on an EHCI port the USB Expert
  spent ~40 s trying to enumerate it as a device — it's full-speed, so it never enables — which
  starved the actual drive until the launcher timed out. **Fix:** power the ports first, wait out
  the USB connect debounce (timed off the running controller's `FRINDEX`, no OS call), *then* read
  connect-status and decide.

- **A displaced interrupt handler (the shared line).** With the keyboard correctly released, a
  second problem surfaced: our interrupt handler had been installed on the interrupt member the
  OHCI companions were already using, **replacing** their handler. When an interrupt wasn't ours we
  returned "not complete" but never called the handler we'd displaced — so the companion's
  interrupts (keyboard and mouse) were never serviced. **Fix:** chain to the saved handler — but
  *conditionally*, via a runtime **`sharedCompanion` discriminator** set at port-claim time. On an
  on-board controller (a claim actually released an occupied port to a companion) the ISR invokes
  the saved handler so the keyboard/mouse keep running; on a dedicated PCI-card line it never does
  (chaining on our own completions there stalled the card's completion path). This is what lets the
  **one** universal driver serve both machine types.

With both in place the Mac Mini G4 *has* mounted a USB 2.0 drive on its **built-in** ports at
Hi-Speed with the keyboard and mouse still working, and copied a full folder in both directions.
But it is **not yet reliable** (see Roadmap): intermittently, during the first bulk probe of a
freshly inserted drive, servicing a completion on that shared line wedges the processor and the
machine locks up before the mount lands. The conditional chaining above is necessary but not
*sufficient* — taming that shared-line path under load is the open on-board problem. On a
dedicated PCI-card line (no chaining) this never happens and mounts are reliable.

Lesson: on shared silicon, claim **surgically**. Read a port's state only once it's powered,
never orphan a handler you replace — and expect the shared *interrupt* line to be as delicate as
the shared ports.

## Bug hunt #5 — silent corruption on large writes (the LBA overflow)

The nastiest bug, and the last to fall. On a large volume, copying enough data would eventually
corrupt a file — or, when the wrong write landed, the whole volume. It was intermittent and
data-dependent (which is why it looked for a long time like a File Manager timing race), but the
cause was entirely ours: the block driver turns the File Manager's **byte** position into a
512-byte **block** (LBA), and that conversion was wrong at two size boundaries.

**At 2 GB — a signed divide.** `ioPosOffset` is a *signed* 32-bit `long`. The first write whose
volume offset reaches 2 GB has the sign bit set (`0x80000000`), so `(UInt32)(ioPosOffset / 512)`
did a **signed** divide → a negative quotient → a garbage LBA (`0x80000000 / 512` → `0xffc00000`).
The device rejected the out-of-range write and the copy failed with a disk error. Fix: cast to
`UInt32` *before* dividing, so the divide is unsigned. (The logging path already did it that way —
the two disagreeing was exactly why the traces looked sane while the wire got garbage.)

**At 4 GB — a 32-bit offset that can't reach.** A byte offset past 4 GB doesn't fit in the 32-bit
`ioPosOffset` at all; it *wraps*, so a write meant for 5 GB lands near block 0 — on top of the boot
blocks, the catalog, or another file. This one is **silent**: the wrong-block write "succeeds" on
the device, so nothing errors until you open the mangled file (or the volume won't mount). OS 9 has
a wide-positioning driver ABI for exactly this — a driver answers the `kdgWide` DriverGestalt query
"true", and the File Manager then passes the real **64-bit** offset in `XIOParam.ioWPosOffset`
(flagged by `kUseWidePositioning` in `ioPosMode`). We weren't opting in. Fix: advertise `kdgWide`
and read the 64-bit offset when the flag is set. The block *number* always fit in 32 bits (a 62 GB
volume is ~121 M blocks) — only the byte offset overflowed.

How it was cornered: a probe that recorded the *original* submitted LBA at the moment of failure.
When it equalled the garbage LBA, with zero transfer retries, it proved the bad address was born at
submit — in our code — not corrupted downstream and not handed to us by the File Manager. That
killed the FM-race theory and pointed straight at the conversion.

Lesson: a byte offset is unsigned and can exceed 32 bits. Convert it to a block number with an
**unsigned** divide, and support the wide-positioning ABI (`kdgWide` + `ioWPosOffset`) before
trusting a driver on any volume larger than 2 GB.

## Also worth knowing

- **Reset timing:** after a port reset the UIM waits for the port to actually report *Enabled*
  (the high-speed chirp handshake) before telling the hub driver the reset is done — reporting
  too early sent `SET_ADDRESS`/`GET_DESCRIPTOR` before the device was on the bus.
- **File Manager at interrupt level is fatal:** logging or `FSWrite` from a completion (which
  runs at secondary-interrupt level) deadlocks with no NMI. All disk logging is task-level
  only; interrupt-level diagnostics write to a pure-memory ring drained later.

## Roadmap / known limitations

- **Throughput — solved.** Reads ~20 MB/s and writes ~13 MB/s (the flash device's own ceiling),
  by pre-queuing whole commands (one interrupt per command) with multi-qTD 128 KB transfer chains
  and per-endpoint hardware toggles (Bug hunt #3). Real Finder copies land lower (~8 read / ~5
  write) — the Finder's own I/O sizing, not the driver, is the ceiling there.
- **On-board (shared-port) controllers — experimental (intermittent).** Machines like the Mac
  Mini G4, whose EHCI shares its ports *and interrupt line* with the OHCI companions that drive the
  keyboard/mouse, are handled by the per-port claim + the `sharedCompanion` interrupt discriminator
  (Bug hunt #4). It has genuinely worked — the drive mounts at Hi-Speed on the built-in ports and
  copies files while input stays live — but not dependably: the shared-line interrupt path
  intermittently locks the machine up mid-mount (kbd/mouse dead). Same drive, next cold boot, often
  mounts fine. A robust fix for chaining into Apple's OHCI handler under load is **open** — this is
  the main on-board blocker. Dedicated PCI-card lines are unaffected and reliable.
- **Large Finder copies: fixed.** Previously wedged on the Finder's interleaved access; the
  cause was a software data toggle on a shared queue head, resolved by per-endpoint hardware
  toggle (Bug hunt #3). A ~800 MB / 1000+ file copy now completes cleanly.
- **Large writes / volumes past 2 GB & 4 GB: fixed.** The byte-offset→LBA conversion used a
  signed divide (garbage LBA at the 2 GB boundary) and ignored the wide-positioning ABI (a 32-bit
  offset wraps past 4 GB → wrong-block writes). Now an unsigned divide plus `kdgWide` + the 64-bit
  `ioWPosOffset` give correct addressing across the whole volume (Bug hunt #5). Earlier builds
  silently corrupted data past those boundaries; verified fixed on the MDD with repeated >4 GB
  copies that launch. The completion-pacing workaround that used to mask this is retired — writes
  run at full speed.
- **BOT error recovery is minimal.** With the wedge gone, a *correct* one-shot Bulk-Only Reset
  (for genuine device errors, not the false-timeout churn that was removed) is a small planned
  hardening.
- **Manual mount, by design (the big one).** The driver now loads from the ROM at boot, but the
  mount still goes through a small prompt-driven helper: boot with the drive *unplugged*, let the
  helper run (it activates the ROM driver and claims the ports for EHCI), and insert when prompted, so
  the drive enumerates fresh on EHCI. Auto-mount-at-boot and hot re-insertion are
  **not** supported, and for the same underlying reason: both require handing a mass-storage
  device between Apple's USB 1.1 *companion* controller and EHCI, and Mac OS 9's USB stack does
  not do that gracefully. A drive attached at boot is claimed by the 1.1 companion before our
  driver loads, and taking the port over to EHCI stalls or hangs; a hot re-insert makes the USB
  Mass Storage class driver monopolize the USB Expert's task-level idle loop (`ExpertIdleTask`)
  and never yield it back, so the task-level re-mount never runs. Preventing the companion from
  claiming the port early (from an INIT) fails too — the card's registers aren't CPU-mappable
  that early in boot. This controller hand-off is the main open problem; see the README.
- Only validated on one card (NEC µPD720100A) and a couple of drives.
