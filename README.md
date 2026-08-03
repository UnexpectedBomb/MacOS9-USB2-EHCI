# USB 2.0 for Mac OS 9: ROM-Integrated Beta

**The first USB 2.0 (Hi-Speed) mass-storage driver for classic Mac OS 9.** Mac OS 9 shipped with USB 1.1 only (a 12 Mbit/s ceiling, roughly 1 MB/s in practice). This is a from-scratch EHCI (USB 2.0) stack that mounts a USB flash drive and reads and writes it at **~20 MB/s read / ~13 MB/s write** on real hardware, roughly 10 to 20 times faster than anything OS 9 could do before.

## What's new: the driver now lives in the Mac OS ROM

Earlier releases shipped the driver *inside an app* that installed it at runtime. This release takes the step the project was built toward: the EHCI UIM is injected into the **Mac OS ROM** itself, as a `driver,AAPL,MacOS,PowerPC` parcel bound to the USB 2.0 controller's Name Registry node (built with Elliot Nunn's Mac OS ROM toolchain). Mac OS 9 now **loads and binds our driver at boot, as a genuine OS-owned host-controller driver**, the same way it loads a native Apple one. That is the architectural milestone: on a machine with the patched ROM, USB 2.0 support is part of the operating system, not bolted on by a helper app.

**The operating system now performs the mount itself.** The driver enumerates the device, runs the SCSI Bulk-Only probe, and **installs its own native block driver**, which calls `AddDrive` and posts a disk-inserted event — so Mac OS 9 mounts the volume through its ordinary path, exactly as it would for a built-in controller. Nothing in user space calls `PBMountVol` any more. Apple's mass-storage mounter is not involved at all (it cannot be: it stops after the first `TEST UNIT READY` / `REQUEST SENSE` and never issues `INQUIRY` to a Hi-Speed device), and neither is Apple's USB stack, whose view of our ports we suppress.

**Hot-plug works.** Insert, eject, unplug, re-insert, or swap in a completely different drive — every arrival is re-scanned and re-mounted, on the same drive number, with no leak. Pulling a drive without ejecting unmounts it cleanly instead of leaving a "damaged disk" behind, and both ejection and improper removal produce the same alerts, word for word, that Apple's own USB stack shows.

**Honest scope: a small faceless helper is still required.** It sits silently in Startup Items and does two things: activates the ROM driver (`LoadUIMForEntry`) and pumps the polled service slot that device *discovery* runs on. That slot is only ever driven by Apple's USB Services Library from an application context, which is why the last helper process cannot be removed yet. It is **not** in the data path — once a volume is mounted, all I/O runs at interrupt level. In practice you never interact with it: it starts at boot, shows nothing, and drives are plug-and-play from then on.

The driver serves two kinds of machine, at two different maturity levels:

- **PCI USB 2.0 cards** (a card in a PCI slot, e.g. the **Power Mac G4 "Mirrored Drive Doors"**): **the supported path.** The EHCI card has its own dedicated interrupt line, the driver owns it cleanly, and mounts are reliable.
- **On-board USB 2.0** (built-in ports that are USB 2.0-capable but which OS 9 only ever drove at 1.1, e.g. the **Mac Mini G4**): **experimental.** It has genuinely worked (mounting a drive and copying a whole folder both directions) but it is **not yet reliable:** on these machines the EHCI shares one interrupt line with the keyboard/mouse, and under the wrong timing the shared-line interrupt handling can lock the machine up mid-mount (see "The open problems"). Treat it as a preview, retry from a fresh boot, and do not trust it with data you care about.

> ⚠️ **This is a beta / technology preview.** On a PCI card it works, it is stable, and it moves real files fast. But mounting a Hi-Speed drive still goes through the headless helper and a specific **insertion sequence** (not plug-and-play), and it has real limitations. On-board machines are experimental and may freeze. It is shared in this state so the community can use it *and* help finish it. **Read the steps below carefully: the timing of when you insert the drive matters.**

---

## What it does

- Mounts a USB 2.0 mass-storage device (flash drive / SSD) on the OS 9 desktop at Hi-Speed.
- Reads and writes at the device's real speed — benchmarked at **20 MB/s read, ~13 MB/s write** (both are the flash device's own ceiling; the driver reaches it). Real Finder copies land lower (~8 read / ~5 write) because the Finder's own I/O sizing is the bottleneck above the driver, not the driver itself.
- On an **on-board** machine (e.g. Mac Mini G4, *experimental*), the driver hands the keyboard/mouse ports back to the built-in 1.1 controller and claims only a free port for the drive, so input *can* stay live — but see the reliability caveat above and "The open problem": the shared interrupt line can still lock up mid-mount.
- **Ejects** cleanly (Finder menu or drag-to-Trash), like any removable disk.
- **No Extension to install.** The driver rides *inside the Mac OS ROM* (a one-time ROM patch, see "Install & use"). Nothing goes into your Extensions folder. The driver loads at boot as an OS component; a small headless helper then performs the Hi-Speed mount.
- **Tells you it's really 2.0** — a volume mounted through this driver appears on the desktop with a distinct **"2.0" drive icon**, so a Hi-Speed mount is obvious at a glance versus a plain USB 1.1 one.
- Reliable transfers: byte-verified, with a transfer watchdog and a CSW residue/signature check on every command — and **correct block addressing across the entire volume**, so large files and volumes well past 2 GB / 4 GB read and write without corruption.

## Supported machines

There is now **one universal launcher** — the same app and driver on every machine. It detects your
hardware and adapts automatically (claims all ports on a dedicated card; hands the keyboard/mouse ports
back and claims only a free one on an on-board controller). You do not pick a build.

### PCI USB 2.0 card — *supported*

- A Power Mac running Mac OS 9 with a free PCI slot and a **USB 2.0 host card** (EHCI, class `0x0C0320`).
- **Tested: Power Mac G4 "Mirrored Drive Doors" + NEC-chipset IOGEAR card.** Most generic USB 2.0 PCI cards of the era use similar NEC/VIA EHCI silicon.
- The card has a **dedicated interrupt line**, so the driver owns it cleanly and mounts are reliable. This is the recommended path.

### On-board USB 2.0 — *experimental*

- A Power Mac whose **built-in** USB is 2.0 (an EHCI controller, PCI class `0x0C0320`) and that runs Mac OS 9 — including later models that boot OS 9 via the community's OS 9 patches.
- **Proven possible on the Mac Mini G4** (on-board NEC µPD720100A EHCI): it has mounted a drive and copied a full folder both directions. But it is **not yet reliable** — on these machines the EHCI shares its interrupt line with the keyboard/mouse, and mid-mount the shared-line interrupt handling can lock the machine up (kbd/mouse dead). If it doesn't mount within a few seconds, reboot and retry. See "The open problem."

### Both

- A USB 2.0 mass-storage device formatted **HFS — Mac OS Standard or Mac OS Extended** (either an Apple Partition Map with an `Apple_HFS` partition, or a single partitionless HFS volume). *Not FAT.* (Tested with a SanDisk Ultra USB flash drive, Mac OS Extended, on volumes up to 62 GB.)

*Only a couple of hardware combinations have been tested so far. If it works — or doesn't — on your machine / card / drive, please open an issue; that data directly helps.*

## Install & use

A **one-time setup**: patch the driver into your Mac OS ROM, and drop the faceless helper into Startup Items. After that, drives are plug-and-play.

### One-time setup

1. **Get a patched Mac OS ROM.** Two ways, depending on your machine. Either way, first copy your
   machine's `Mac OS ROM` (it lives in the System Folder) somewhere safe — you will need it to revert.

   **Power Mac G4 MDD — use the prebuilt ROM.** The release page carries the exact ROM that has been
   hardware-tested, so this needs no toolchain at all. It is built from an MDD's own `Mac OS ROM`.

   **Any other NewWorld Mac — patch your own.** A complete ROM is machine-specific: the prebuilt one is an
   MDD's, and it does not contain the drivers a different model needs (it has no `ATY,RockHopper2`, for
   instance, so it is not suitable for a Mac mini G4). Run the injector against your own copy, which binds
   the driver to the USB 2.0 controller's Name Registry node:
   ```sh
   python3 rom/usb_rom_inject.py "Mac OS ROM" -o "Mac OS ROM (USB2)"
   ```
   See [BUILD.md](BUILD.md) for the toolchain this needs.

   > **OldWorld Macs are out of scope.** A beige G3, 8600 or 9600 has no `Mac OS ROM` file for this to patch
   > (on those machines OS 9's whole USB stack is disk Extensions instead), so the injector will stop with
   > *"no Parcelfile found in the dump"*. This is not the same question as whether the machine has PCI slots:
   > a beige G3 has slots and is still out of scope, while a Mac mini G4 has none and is in scope.
   >
   > The injector has only ever been run against two ROMs, both from the same developer's machines. If it
   > will not patch yours, or the result will not boot, please open an issue saying which Mac and OS 9
   > version you are on.

   Then **boot from a CD or another volume** and put the patched ROM into the System Folder in place of the
   original. You cannot swap it while booted from that same System Folder. **Keep the original**, and make
   sure you can boot from something else before you start. This is the only step that touches the ROM;
   everything else is an ordinary file.

2. **Place the helper.** Decode the helper app (`dist/USB2_Activate.bin`) on the Mac and drop it into **System Folder ▸ Startup Items**. It runs faceless at every boot: it brings the ROM driver up and then hands the front back to the Finder, showing no window. (You can also double-click it when you want it, but Startup Items is the intended home.)

Setup is done. From here the driver loads at every boot straight from the ROM, the helper activates it, and the OS mounts drives as you plug them in.

### Each session

1. **Boot with your USB drive unplugged.** Your keyboard and mouse can stay connected. This is the one piece of sequencing that still matters: a drive attached at *bring-up* gets handed to the built-in 1.1 controller (see "The open problems"), so it would mount at 1.1 speed rather than 2.0.

2. **Plug the drive into any free port on the EHCI card** and it mounts, at Hi-Speed, showing the custom **"2.0" drive icon**. There is no prompt to wait for and no window to watch. *(If the icon looks like a generic disk the first time, hold **Cmd-Option at startup** once to rebuild the desktop; the Finder caches volume icons.)*

3. **Use it normally.** Eject in the Finder (drag to Trash, or Special ▸ Eject) before unplugging, as with any removable disk. You can then unplug, re-insert, or plug in a different drive, as many times as you like.

That is the whole flow. You do not need to quit anything, and a **Special ▸ Restart** with the driver active is safe — the driver quiesces the controller through a Shutdown Manager hook before the machine goes down.

If you do quit the helper (Cmd-Q) with a volume still mounted, it unmounts cleanly first and hands the ports back to the built-in 1.1 controller, so ordinary USB 1.1 works again immediately with no reboot.

If a drive does not appear, two logs are written for troubleshooting and bug reports: the helper's own step-by-step log next to the application, and **`EHCIUIM_init.log`** on the boot volume (the driver's detailed trace). The driver *appends* to that log across loads, so delete it first and read from the last banner.

## Known limitations (please read)

This is a beta. These are the things it does **not** do yet:

- **On-board USB 2.0 is experimental and can freeze.** On a PCI card mounting is reliable. On an on-board controller (Mac Mini G4) the EHCI shares its interrupt line with the keyboard/mouse; mid-mount, under the wrong timing, the shared-line interrupt handling can lock the whole machine (keyboard/mouse go dead). It has mounted and copied real files there, but not dependably. If it doesn't mount within a few seconds, reboot and retry — and don't put data you care about on it from an on-board machine yet.
- **A drive attached at boot mounts at 1.1, not 2.0.** Ports that are already occupied when the controller is brought up are handed to the 1.1 companion. Boot with the drive unplugged, then insert it.
- **External hubs are not driven by this stack.** A USB 2.0 hub (including the hub built into an Apple Cinema Display) is handed to Apple's 1.1 companion controller, so devices behind it work at 1.1 speeds. Driving a hub at Hi-Speed needs downstream port control and, for any keyboard or mouse behind it, EHCI **split transactions** — neither is implemented. See "The open problems".
- **A port handed to the 1.1 companion stays there until reboot.** Once ownership is released, EHCI can no longer tell "empty" from "companion-owned" on that port, so the driver deliberately never takes it back — reclaiming it would risk stuttering a keyboard that is working. Use a different port for a Hi-Speed drive, or reboot.
- **Writes are slower than reads.** Reads reach the device's ceiling (~20 MB/s); Finder writes land around 2 to 3 MB/s because the Finder issues small synchronous writes above the driver.
- **Mid-write yank is unsafe** (as on any OS) — always eject first.
- **A few tested hardware combinations** (see Supported machines). Other EHCI cards/controllers are untested.
- The driver writes a verbose `EHCIUIM_init.log` each run. That's intentional for a beta (it's what to attach to a bug report) and does not slow down file transfers.

If you need rock-solid *removable* USB on OS 9 today, the built-in USB 1.1 support is still there and untouched. This driver is about **speed** — and about proving Hi-Speed USB on OS 9 is possible at all.

## The open problems — help wanted

On a **dedicated PCI card** the manual flow above is rock-solid. Two hard problems remain, and both are great targets for anyone who enjoys low-level driver archaeology.

### 1. On-board shared-interrupt reliability (the on-board blocker)

On a machine like the Mac Mini G4 the EHCI shares **one PCI interrupt line** with the OHCI companion controllers that drive the keyboard and mouse. Our interrupt handler chains to the companion's handler so input keeps working — and it does, *most of the time*. But intermittently, during the initial bulk-transfer probe of a freshly inserted drive, a completion interrupt on that shared line wedges the processor and the whole machine locks up (keyboard/mouse dead), before the drive mounts. On other boots the very same drive mounts in a couple of seconds and copies gigabytes cleanly. We've narrowed it to the shared-line interrupt path (a dedicated-card line never does this), but a robust fix for chaining into Apple's OHCI handler under load is still open. **If you understand classic Mac OS PCI interrupt sharing / the OHCI UIM's interrupt handler, this is the one to crack.**

### 2. A drive attached when the controller is brought up

**Hot re-insertion is solved** — this was previously listed here as blocked on Apple's `ExpertIdleTask` monopolizing the USB Expert's task-level idle loop. The fix was to stop depending on Apple's stack at all: the driver now does its own port reset, speed detection, `SET_ADDRESS`, descriptor reads, `SET_CONFIGURATION`, endpoint registration and Bulk-Only transport, then installs its own block driver and lets the OS mount the result. Apple's idle-loop monopoly became irrelevant rather than worked around, and eject / unplug / re-insert / drive-swap all work.

What remains is narrower: **a drive already attached when the EHCI controller is brought up** is handed to the 1.1 companion by the bring-up path, so it mounts at 1.1. The port-ownership handoff is one-way by design (see the next problem), so it stays there. Boot with the drive unplugged and insert it afterwards.

### 3. Driving an external hub at Hi-Speed

A USB 2.0 hub currently gets handed to Apple's 1.1 companion, so anything behind it runs at 1.1. Doing better needs two things this stack does not have:

- **Downstream hub port control** — enumerating the hub itself, then resetting and assigning addresses to devices on its ports. Tractable, and enough on its own for a Hi-Speed *drive* behind a hub.
- **EHCI split transactions** (`siTD` / split `qTD`) — required for any full- or low-speed device (keyboard, mouse) behind a high-speed hub. Not implemented at all.

There is also a wrinkle worth knowing if you touch this area. Releasing a port to the companion (`PORTSC` Port Owner = 1) only works from the state the EHCI spec intends: a port that is **not enabled**, i.e. one where a reset did not bring up a high-speed device. Handing over a port that *is* enabled at Hi-Speed does not take effect. The driver now clears Port Enable first, and records the handoff in software rather than re-reading the ownership bit back out of the hardware — trusting that register as the record of a decision it had already made produced a livelock, with the port bouncing between the two controllers so fast that neither could enumerate anything.

Issues, pointers, and patches are all welcome.

## How it works (for the curious)

There was no USB 2.0 anything for OS 9, so this is built from the ground up:

- A hand-written **EHCI host-controller driver (UIM)** — a native PowerPC `ndrv` — implementing the queue-head / qTD schedule, per-endpoint hardware toggles, and interrupt + heartbeat-timer servicing.
- A **virtual root hub** synthesized in software, so Apple's USB Expert enumerates the bus the normal way even though an EHCI controller presents its ports differently.
- A **self-driven SCSI probe** (INQUIRY / READ CAPACITY / READ) over Bulk-Only Transport — the stock mounter won't advance a Hi-Speed device, so the driver reads the disk itself and publishes a block service.
- A small **block driver** that mounts the volume through that service.
- The driver is delivered as a **Mac OS ROM parcel**: a `driver,AAPL,MacOS,PowerPC` property (our ndrv's PEF) and its descriptor are injected into the ROM and bound to the controller's Name Registry node, so OS 9 loads and prepares it **at boot**, as an OS-owned driver, without any Extension or declaration ROM. The headless helper then calls `LoadUIMForEntry` to bring it up as the live UIM, runs the self-probe, and mounts the volume. On quit, the helper hands the ports back to the 1.1 companion (clearing the EHCI `CONFIGFLAG`), so USB 1.1 works again without a reboot. *(An earlier release created those same properties from the app at runtime instead of from the ROM; the ROM parcel is the same binding, now applied by the OS at boot.)*

**Coexisting with the keyboard/mouse on an on-board controller.** On a machine like the Mac Mini G4 the EHCI shares one physical set of ports (and one PCI interrupt line) with the OHCI "companion" controllers that drive the keyboard and mouse. Rather than seize the whole controller, the driver does a **per-port claim**: it powers the ports, waits out the USB connect debounce, then hands every port that already holds a device back to the 1.1 companion (setting its Port Owner bit) and claims only the empty ports for EHCI — so the drive comes up at Hi-Speed while input stays on the companion. Two hardware details made this finicky: the controller has *Port Power Control*, so a port's connect status is invalid until it's powered (you must power first, then read); and because the interrupt line is shared, the driver's interrupt handler **chains** to the companion's handler so the keyboard/mouse keep being serviced.

Throughput comes from pre-queuing whole commands (one interrupt per command) and multi-qTD 128 KB transfer chains in both directions.

## Building from source

Built with the [Retro68](https://github.com/autc04/Retro68) cross-toolchain (PowerPC / classic Mac OS). With Retro68 installed and its PowerPC toolchain file configured:

```
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<path-to-Retro68 PowerPC toolchain.cmake>
cmake --build build
```

The shippable pieces are the **driver** (injected into the Mac OS ROM) and the **universal helper** that performs the mount:

- **`EHCIUIM`** target → `EHCIUIM.pef`, the driver. `rom/usb_rom_inject.py` injects it into a Mac OS ROM as a `driver,AAPL,MacOS,PowerPC` parcel. A prebuilt copy is in `dist/EHCIUIM.pef`.
- **`EHCIActivate`** target → `EHCIActivate.bin`, **the shipping helper**. Faceless: it activates the ROM driver, hands the front back to the Finder and hides, then pumps the polled slot that discovery runs on. One binary for every machine (PCI card *and* on-board): the driver's per-port claim and its `sharedCompanion` interrupt discriminator adapt to the hardware at runtime, so there is no per-machine build variant. A prebuilt copy is in `dist/USB2_Activate.bin`.
- **`EHCILauncher`** target → `EHCILauncher.bin`, the older interactive launcher, kept for reference. Superseded by `EHCIActivate`.

`EHCITrigger` is the developer harness (verbose on-screen and on-disk logging) used during development. Retro68 emits MacBinary, so decode the `.bin` on the Mac. See **[BUILD.md](BUILD.md)** for the full build (the drivers are built and embedded as byte arrays before the app, and the ROM parcel is injected with `rom/usb_rom_inject.py`).

## Status & disclaimer

Early beta, provided as-is, with no warranty. On a PCI card it now runs reliably — full-speed reads and writes, large folder copies, and volumes well past 4 GB, with correct addressing across the whole disk — but it is new low-level code touching your disks, so **keep backups.**

*Fixed in this build:* a data-corruption bug where writes past the 2 GB / 4 GB volume-offset boundary went to the wrong blocks — silently damaging large files, and sometimes the volume's own structures (boot blocks / catalog). If you ran an earlier release against a volume larger than 2 GB, re-verify or reformat that volume. Bug reports and hardware-compatibility reports are very welcome.
