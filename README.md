# USB 2.0 for Mac OS 9 — Early Beta

**The first USB 2.0 (Hi-Speed) mass-storage driver for classic Mac OS 9.** Mac OS 9 shipped with USB 1.1 only — a 12 Mbit/s ceiling, roughly 1 MB/s in practice. This is a from-scratch EHCI (USB 2.0) stack that mounts a USB flash drive and reads and writes it at **~20 MB/s read / ~13 MB/s write** on real hardware — about 10–20× faster than anything OS 9 could do before.

It now works on **two kinds of machine**:

- **On-board USB 2.0** — Power Macs whose built-in USB ports are USB 2.0-capable but which OS 9 only ever drove at 1.1, such as the **Mac Mini G4**. The driver brings the *built-in* ports up at Hi-Speed **while your USB keyboard and mouse keep working on the same controller.**
- **PCI USB 2.0 cards** — older Power Macs with a card in a PCI slot, such as the **Power Mac G4 "Mirrored Drive Doors."**

> ⚠️ **This is an early beta / technology preview.** It works, it's stable, and it moves real files fast — but it mounts a drive through a specific **manual launch sequence** (not plug-and-play), and it has real limitations. It is shared in this state so the community can use it *and* help finish it. **Read the steps below carefully — the timing of when you insert the drive matters.**

---

## What it does

- Mounts a USB 2.0 mass-storage device (flash drive / SSD) on the OS 9 desktop at Hi-Speed.
- Reads and writes at the device's real speed — benchmarked at **20 MB/s read, ~13 MB/s write** (both are the flash device's own ceiling; the driver reaches it). Real Finder copies land lower (~8 read / ~5 write) because the Finder's own I/O sizing is the bottleneck above the driver, not the driver itself.
- On an **on-board** machine (e.g. Mac Mini G4), your **USB keyboard and mouse stay live** — they share the physical ports with the USB 2.0 controller, and the driver hands their ports back to the built-in 1.1 controller while claiming only a free port for the drive.
- **Ejects** cleanly (Finder menu or drag-to-Trash), like any removable disk.
- **No Extension to install** — the driver rides *inside* the launcher app. Nothing is copied into your Extensions folder, and nothing loads at boot until you run it.
- **Tells you it's really 2.0** — a volume mounted through this driver appears on the desktop with a distinct **"2.0" drive icon**, so a Hi-Speed mount is obvious at a glance versus a plain USB 1.1 one.
- Reliable transfers: byte-verified, with a transfer watchdog and a CSW residue/signature check on every command.

## Supported machines

Pick the app that matches your hardware — the flow is the same for both.

### On-board USB 2.0 → use **MiniLauncher**

- A Power Mac whose **built-in** USB is 2.0 (an EHCI controller, PCI class `0x0C0320`) and that runs Mac OS 9 — including later models that boot OS 9 via the community's OS 9 patches.
- **Tested: Mac Mini G4** (on-board NEC µPD720100A EHCI).
- Your USB keyboard and mouse can stay plugged in. They may **pause for a second or two** while the driver claims the ports, then come right back — that's normal.

### PCI USB 2.0 card → use the **USB 2.0 Launcher**

- A Power Mac running Mac OS 9 with a free PCI slot and a **USB 2.0 host card** (EHCI, class `0x0C0320`).
- **Tested: Power Mac G4 "Mirrored Drive Doors" + NEC-chipset IOGEAR card.** Most generic USB 2.0 PCI cards of the era use similar NEC/VIA EHCI silicon.

### Both

- A USB 2.0 mass-storage device, **FAT-formatted**. (Tested with a SanDisk Ultra USB flash drive.)

*Only a couple of hardware combinations have been tested so far. If it works — or doesn't — on your machine / card / drive, please open an issue; that data directly helps.*

## Install & use — follow these steps exactly

The insertion **timing** is the whole trick. Do it in this order:

1. **Boot with your USB drive UNPLUGGED.**
   This is the important one. If a drive is attached at startup, Mac OS 9's built-in USB **1.1** driver grabs it before ours can, and it will not hand over to USB 2.0 cleanly (you'll get a "device was unexpectedly disconnected" message, or a hang). Start clean, with no target drive attached. *(Your keyboard and mouse can stay plugged in.)*

2. **At the desktop, double-click the launcher for your machine** — **MiniLauncher** for on-board USB 2.0 (Mac Mini G4 etc.), or **USB 2.0 Launcher** for a PCI card.
   A window opens and it brings up the USB 2.0 controller ("claiming the ports"). On an on-board machine your keyboard/mouse may briefly pause here — that's expected. Do **not** plug the drive in yet.

3. **Wait for the beep and this on-screen banner:**
   ```
   ****************************************************
   *   >>>  INSERT USB DRIVE NOW  <<<
   ****************************************************
   ```

4. **Now plug your drive in** — into a **free port** (on the Mac Mini, a free built-in port, e.g. the one next to the FireWire jack; on a card machine, a port on the card).
   You have about **60 seconds** from the prompt; if the app doesn't see a drive in that window it quits by itself (just run it again).

5. The drive **mounts on the desktop** at USB 2.0 speed (you'll hear a second beep), showing the custom **"2.0" drive icon**. The window then hides to reveal the Finder. *(If the icon looks like a generic disk the first time, hold **Cmd-Option at startup** once to rebuild the desktop — the Finder caches volume icons.)*

6. **Use the drive normally, and leave the launcher running** — it hosts the USB stack for the session. Quitting it removes the driver.

7. **Eject in the Finder** (drag to Trash, or Special ▸ Eject) **before unplugging.**

**To use a drive again in a new session:** quit the launcher, **reboot with the drive unplugged**, and run it again. (This build mounts one drive per launch — see "The open problem" for why.)

If a drive doesn't appear, two logs are written next to the app for troubleshooting / bug reports: **`USB2 Launcher Log`** (the app's own step-by-step) and **`EHCIUIM_init.log`** (the driver's detailed trace).

## Known limitations (please read)

This is a beta, and the flow above is deliberate — these are the things it does **not** do yet:

- **Not plug-and-play.** You must boot with the drive unplugged and insert it only when prompted (steps above). A drive attached at boot, or hot-plugged without the launcher, will not mount at 2.0 (and may throw a disconnect error).
- **One mount per launch.** After ejecting, reboot (unplugged) and relaunch to mount again. Hot re-insertion isn't supported yet.
- **Mid-write yank is unsafe** (as on any OS) — always eject first.
- **A few tested hardware combinations** (see Supported machines). Other EHCI cards/controllers are untested.
- The driver writes a verbose `EHCIUIM_init.log` each run. That's intentional for a beta (it's what to attach to a bug report) and does not slow down file transfers.

If you need rock-solid *removable* USB on OS 9 today, the built-in USB 1.1 support is still there and untouched. This driver is about **speed** — and about proving Hi-Speed USB on OS 9 is possible at all.

## The open problem — help wanted

The driver mounts perfectly when a drive is inserted onto a port that **already belongs to the EHCI controller** — that's the manual flow above, and it's rock-solid. What does *not* yet work is any scenario where a drive has to **move between Apple's USB 1.1 companion controller and EHCI:**

- **Auto-mount at boot** (drive attached at startup): the 1.1 companion claims it before our driver loads, and handing it over to EHCI stalls or hangs.
- **Hot re-insertion** (eject → unplug → replug mid-session): the re-enumeration makes Apple's USB Mass Storage class driver monopolize the USB Expert's task-level idle loop (`ExpertIdleTask`) and never yield it back, so the task-level re-mount never runs.

Both trace to the same root: **Mac OS 9's USB stack does not gracefully hand a mass-storage device between the 1.1 companion and the EHCI controller.** We've tried preventing the companion from grabbing it (an INIT can't map the card's registers that early in boot) and forcing the hand-off (stalls/hangs) — three angles, same wall. The manual launcher sidesteps it by keeping the companion out of the picture for the *target drive* entirely.

**If you know the classic Mac OS USB stack internals** — how to make a companion controller cleanly release a port to EHCI, or why `ExpertIdleTask` won't return after a mass-storage re-probe of a device that won't mount — we'd genuinely love your input. Issues, pointers, and patches are all welcome. This is a great problem for anyone who enjoys low-level driver archaeology.

## How it works (for the curious)

There was no USB 2.0 anything for OS 9, so this is built from the ground up:

- A hand-written **EHCI host-controller driver (UIM)** — a native PowerPC `ndrv` — implementing the queue-head / qTD schedule, per-endpoint hardware toggles, and interrupt + heartbeat-timer servicing.
- A **virtual root hub** synthesized in software, so Apple's USB Expert enumerates the bus the normal way even though an EHCI controller presents its ports differently.
- A **self-driven SCSI probe** (INQUIRY / READ CAPACITY / READ) over Bulk-Only Transport — the stock mounter won't advance a Hi-Speed device, so the driver reads the disk itself and publishes a block service.
- A small **block driver** that mounts the volume through that service.
- The driver is injected without a declaration ROM by attaching it to the controller's Name Registry node as a **property-based driver**: the launcher creates the four registry properties a ROM card would have supplied, then calls `LoadUIMForEntry`.

**Coexisting with the keyboard/mouse on an on-board controller.** On a machine like the Mac Mini G4 the EHCI shares one physical set of ports (and one PCI interrupt line) with the OHCI "companion" controllers that drive the keyboard and mouse. Rather than seize the whole controller, the driver does a **per-port claim**: it powers the ports, waits out the USB connect debounce, then hands every port that already holds a device back to the 1.1 companion (setting its Port Owner bit) and claims only the empty ports for EHCI — so the drive comes up at Hi-Speed while input stays on the companion. Two hardware details made this finicky: the controller has *Port Power Control*, so a port's connect status is invalid until it's powered (you must power first, then read); and because the interrupt line is shared, the driver's interrupt handler **chains** to the companion's handler so the keyboard/mouse keep being serviced.

Throughput comes from pre-queuing whole commands (one interrupt per command) and multi-qTD 128 KB transfer chains in both directions.

## Building from source

Built with the [Retro68](https://github.com/autc04/Retro68) cross-toolchain (PowerPC / classic Mac OS). With Retro68 installed and its PowerPC toolchain file configured:

```
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<path-to-Retro68 PowerPC toolchain.cmake>
cmake --build build
```

The shippable apps are:

- **`MiniLauncher`** target → `MiniLauncher.bin` — for on-board USB 2.0 (Mac Mini G4 etc.).
- **`EHCILauncher`** target → `EHCILauncher.bin` — for a PCI USB 2.0 card.

Both are the same program built from `probe/ehci_launcher.c` (the Mini build just adds `-DMINI_LAUNCHER` for on-board wording); they share one driver. `EHCITrigger` is the developer harness (verbose on-screen + on-disk logging) used during development. Retro68 emits MacBinary — decode the `.bin` on the Mac.

## Status & disclaimer

Early beta, provided as-is, with no warranty. It has run for extended sessions without data loss on the tested setups (including full folder copies both directions on a Mac Mini G4), but it is new low-level code touching your disks — **keep backups, and don't trust it with your only copy of anything.** Bug reports and hardware-compatibility reports are very welcome.
