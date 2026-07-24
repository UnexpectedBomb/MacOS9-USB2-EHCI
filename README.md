# USB 2.0 for Mac OS 9: ROM-Integrated Beta

**The first USB 2.0 (Hi-Speed) mass-storage driver for classic Mac OS 9.** Mac OS 9 shipped with USB 1.1 only (a 12 Mbit/s ceiling, roughly 1 MB/s in practice). This is a from-scratch EHCI (USB 2.0) stack that mounts a USB flash drive and reads and writes it at **~20 MB/s read / ~13 MB/s write** on real hardware, roughly 10 to 20 times faster than anything OS 9 could do before.

## What's new: the driver now lives in the Mac OS ROM

Earlier releases shipped the driver *inside an app* that installed it at runtime. This release takes the step the project was built toward: the EHCI UIM is injected into the **Mac OS ROM** itself, as a `driver,AAPL,MacOS,PowerPC` parcel bound to the USB 2.0 controller's Name Registry node (built with Elliot Nunn's Mac OS ROM toolchain). Mac OS 9 now **loads and binds our driver at boot, as a genuine OS-owned host-controller driver**, the same way it loads a native Apple one. That is the architectural milestone: on a machine with the patched ROM, USB 2.0 support is part of the operating system, not bolted on by a helper app.

**Honest scope: a small headless helper is still needed to mount.** OS 9's built-in mass-storage mounter will not advance a Hi-Speed device (it stops after the first `TEST UNIT READY` / `REQUEST SENSE` and never issues `INQUIRY`), so it cannot bring a 2.0 drive to the desktop on its own. A tiny helper app, which can sit **silently in your Startup Items folder**, activates the ROM driver and runs the mount (the same self-probe the driver has always performed). Fully seamless, no-helper plug-and-play is **not** solved, and this README does not claim it is. What the ROM integration buys is the foundation: the driver is now a first-class OS component, which is what any future seamless mount has to build on.

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

There are two parts: a **one-time setup** (patch the driver into your Mac OS ROM and place the helper), and then a short **per-session** insertion sequence. The insertion timing is the whole trick.

### One-time setup

1. **Patch the driver into your Mac OS ROM.** Work on a *copy* of your machine's `Mac OS ROM` file (it lives in the System Folder). Run the injector, which binds the driver to the USB 2.0 controller's Name Registry node:
   ```sh
   python3 rom/usb_rom_inject.py "Mac OS ROM" -o "Mac OS ROM (USB2)"
   ```
   Put the patched copy in place of the `Mac OS ROM` in your System Folder, and **keep the original** so you can revert. See [BUILD.md](BUILD.md) for the toolchain this step needs. This is the only step that touches the ROM; everything else is an ordinary file.

2. **Place the helper.** Decode the helper app (`dist/USB2-Launcher.bin`) on the Mac. To have the Hi-Speed mount happen automatically at each boot, drop it (or an alias to it) into **System Folder ▸ Startup Items**, where it runs headless at startup. You can also just double-click it when you want it.

Setup is done. From here the driver loads at every boot straight from the ROM; the helper is only what performs the mount.

### Each session

1. **Boot with your USB drive UNPLUGGED.**
   This is the important one. If a drive is attached at startup, Mac OS 9's built-in USB **1.1** driver grabs it first, and it will not hand over to USB 2.0 cleanly (you may get a "device was unexpectedly disconnected" message, or a hang). Start clean, with no target drive attached. *(Your keyboard and mouse can stay plugged in.)*

2. **Let the helper run.** If it is in Startup Items it starts at boot; otherwise double-click it. It activates the ROM driver ("claiming the ports"), then settles the input devices for a few seconds. On an on-board machine your keyboard and mouse may briefly pause here, which is expected. Do **not** plug the drive in yet.

3. **Wait for the beep and this on-screen banner** (the helper shows a small console window while it waits):
   ```
   ****************************************************
   *   >>>  INSERT USB DRIVE NOW  <<<
   ****************************************************
   ```

4. **Now plug your drive into any free port** (one that is empty right now: on a card machine, a port on the card; on the Mac Mini, a free built-in port such as the one next to the FireWire jack; **not** the port your keyboard or mouse uses). On a PCI card it mounts within a few seconds. On an on-board machine the drive may re-try a few times before it appears, and mounts as soon as it stabilizes. *(On-board: if it has not mounted within about 30 seconds it has probably hit the shared-interrupt lockup, so reboot and retry.)*

5. The drive **mounts on the desktop** at USB 2.0 speed (you will hear a second beep), showing the custom **"2.0" drive icon**. The helper's window then hides to reveal the Finder. *(If the icon looks like a generic disk the first time, hold **Cmd-Option at startup** once to rebuild the desktop; the Finder caches volume icons.)*

6. **Use the drive normally, and leave the helper running.** It hosts the USB stack for the whole session.

7. **Eject in the Finder** (drag to Trash, or Special ▸ Eject) **before unplugging.**

8. **When you are finished, quit the helper** with **File ▸ Quit** (Cmd-Q also works). On quit it hands the ports back to the built-in 1.1 controller, so ordinary USB 1.1 works again right away, with no reboot.

**To mount another drive in the same session:** this build mounts one drive per run of the helper. Quit it, **reboot with the drive unplugged**, and let it run again. (See "The open problems" for why re-insertion is not yet supported.)

If a drive does not appear, two logs are written next to the helper for troubleshooting and bug reports: **`USB2 Launcher Log`** (the helper's own step-by-step) and **`EHCIUIM_init.log`** (the driver's detailed trace).

## Known limitations (please read)

This is a beta, and the flow above is deliberate — these are the things it does **not** do yet:

- **On-board USB 2.0 is experimental and can freeze.** On a PCI card mounting is reliable. On an on-board controller (Mac Mini G4) the EHCI shares its interrupt line with the keyboard/mouse; mid-mount, under the wrong timing, the shared-line interrupt handling can lock the whole machine (keyboard/mouse go dead). It has mounted and copied real files there, but not dependably. If it doesn't mount within a few seconds, reboot and retry — and don't put data you care about on it from an on-board machine yet.
- **Not plug-and-play.** You must boot with the drive unplugged and insert it only when prompted (steps above). A drive attached at boot, or hot-plugged without the launcher, will not mount at 2.0 (and may throw a disconnect error).
- **One mount per launch.** After ejecting, reboot (unplugged) and relaunch to mount again. Hot re-insertion isn't supported yet.
- **Mid-write yank is unsafe** (as on any OS) — always eject first.
- **A few tested hardware combinations** (see Supported machines). Other EHCI cards/controllers are untested.
- The driver writes a verbose `EHCIUIM_init.log` each run. That's intentional for a beta (it's what to attach to a bug report) and does not slow down file transfers.

If you need rock-solid *removable* USB on OS 9 today, the built-in USB 1.1 support is still there and untouched. This driver is about **speed** — and about proving Hi-Speed USB on OS 9 is possible at all.

## The open problems — help wanted

On a **dedicated PCI card** the manual flow above is rock-solid. Two hard problems remain, and both are great targets for anyone who enjoys low-level driver archaeology.

### 1. On-board shared-interrupt reliability (the on-board blocker)

On a machine like the Mac Mini G4 the EHCI shares **one PCI interrupt line** with the OHCI companion controllers that drive the keyboard and mouse. Our interrupt handler chains to the companion's handler so input keeps working — and it does, *most of the time*. But intermittently, during the initial bulk-transfer probe of a freshly inserted drive, a completion interrupt on that shared line wedges the processor and the whole machine locks up (keyboard/mouse dead), before the drive mounts. On other boots the very same drive mounts in a couple of seconds and copies gigabytes cleanly. We've narrowed it to the shared-line interrupt path (a dedicated-card line never does this), but a robust fix for chaining into Apple's OHCI handler under load is still open. **If you understand classic Mac OS PCI interrupt sharing / the OHCI UIM's interrupt handler, this is the one to crack.**

### 2. Moving a device between the 1.1 companion and EHCI

The driver mounts perfectly when a drive is inserted onto a port that **already belongs to EHCI** — the manual flow. What does *not* yet work is any scenario where a drive has to **move between Apple's USB 1.1 companion controller and EHCI:**

- **Auto-mount at boot** (drive attached at startup): the 1.1 companion claims it before our driver loads, and handing it over to EHCI stalls or hangs.
- **Hot re-insertion** (eject → unplug → replug mid-session): the re-enumeration makes Apple's USB Mass Storage class driver monopolize the USB Expert's task-level idle loop (`ExpertIdleTask`) and never yield it back, so the task-level re-mount never runs.

Both trace to the same root: **Mac OS 9's USB stack does not gracefully hand a mass-storage device between the 1.1 companion and the EHCI controller.** We tried preventing the companion from grabbing it and forcing the hand-off (stalls or hangs), from several angles, same wall. The ROM integration in this release changes one part of the picture: the driver is now present and bound at boot (an INIT could never map the card's registers that early, and the ROM parcel sidesteps that entirely), so early-load is no longer the obstacle. What keeps auto-mount and hot re-insertion open is the hand-off itself, plus the fact that OS 9's own mounter will not advance a Hi-Speed device at all. The headless helper sidesteps both by keeping the companion out of the picture for the *target drive* and performing the mount itself.

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
- **`EHCILauncher`** target → `EHCILauncher.bin`, the helper. One binary for every machine (PCI card *and* on-board): the driver's per-port claim and its `sharedCompanion` interrupt discriminator adapt to the hardware at runtime, so there is no per-machine build variant. It can run headless from Startup Items. A prebuilt copy is in `dist/USB2-Launcher.bin`.

`EHCITrigger` is the developer harness (verbose on-screen and on-disk logging) used during development. Retro68 emits MacBinary, so decode the `.bin` on the Mac. See **[BUILD.md](BUILD.md)** for the full build (the drivers are built and embedded as byte arrays before the app, and the ROM parcel is injected with `rom/usb_rom_inject.py`).

## Status & disclaimer

Early beta, provided as-is, with no warranty. On a PCI card it now runs reliably — full-speed reads and writes, large folder copies, and volumes well past 4 GB, with correct addressing across the whole disk — but it is new low-level code touching your disks, so **keep backups.**

*Fixed in this build:* a data-corruption bug where writes past the 2 GB / 4 GB volume-offset boundary went to the wrong blocks — silently damaging large files, and sometimes the volume's own structures (boot blocks / catalog). If you ran an earlier release against a volume larger than 2 GB, re-verify or reformat that volume. Bug reports and hardware-compatibility reports are very welcome.
