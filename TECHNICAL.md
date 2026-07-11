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

## Also worth knowing

- **Reset timing:** after a port reset the UIM waits for the port to actually report *Enabled*
  (the high-speed chirp handshake) before telling the hub driver the reset is done — reporting
  too early sent `SET_ADDRESS`/`GET_DESCRIPTOR` before the device was on the bus.
- **File Manager at interrupt level is fatal:** logging or `FSWrite` from a completion (which
  runs at secondary-interrupt level) deadlocks with no NMI. All disk logging is task-level
  only; interrupt-level diagnostics write to a pure-memory ring drained later.

## Roadmap / known limitations

- **Throughput (~0.8 MB/s) — the main open task.** The engine still runs one BOT command in
  flight with a data copy on the interrupt path. Bigger chunks alone were tried and
  **backfired** — a 20 KB copy at interrupt level starves the UI (choppy Finder) and the
  multi-page scatter was unreliable. Real speed needs the data copy moved **off** the interrupt
  path, then multiple in-flight commands — which the **per-endpoint queue heads (Bug hunt #3)
  now make possible**. That rework is next.
- **Large Finder copies: fixed.** Previously wedged on the Finder's interleaved access; the
  cause was a software data toggle on a shared queue head, resolved by per-endpoint hardware
  toggle (Bug hunt #3). A ~800 MB / 1000+ file copy now completes cleanly.
- **BOT error recovery is minimal.** With the wedge gone, a *correct* one-shot Bulk-Only Reset
  (for genuine device errors, not the false-timeout churn that was removed) is a small planned
  hardening.
- **No resident auto-load.** Runs as an app today. A resident 68K INIT can load the PPC UIM at
  boot (proven separately), but the mount needs a top-level process context, so the shippable
  vehicle is a faceless background app — not yet built.
- Only validated on one card (NEC µPD720100A) and a couple of drives.
