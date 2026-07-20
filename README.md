# USB 2.0 for Mac OS 9 — Early Beta

**The first USB 2.0 (Hi-Speed) mass-storage driver for classic Mac OS 9.** Mac OS 9 shipped with USB 1.1 only — a 12 Mbit/s ceiling, roughly 1 MB/s in practice. This is a from-scratch EHCI (USB 2.0) stack that mounts a USB flash drive and reads and writes it at **~20 MB/s read / ~13 MB/s write** on real hardware — about 10–20× faster than anything OS 9 could do before.

> ⚠️ **This is an early beta / technology preview.** It works, it's stable, and it moves real files fast — but it mounts a drive through a specific **manual launch sequence** (not plug-and-play), and it has real limitations. It is shared in this state so the community can use it *and* help finish it. **Read the steps below carefully — the timing of when you insert the drive matters.**

---

## What it does

- Mounts a USB 2.0 mass-storage device (flash drive / SSD) on the OS 9 desktop at Hi-Speed.
- Reads and writes at the device's real speed — benchmarked at **20 MB/s read, ~13 MB/s write** (both are the flash device's own ceiling; the driver reaches it). Real Finder copies land lower (~8 read / ~5 write) because the Finder's own I/O sizing is the bottleneck above the driver, not the driver itself.
- **Ejects** cleanly (Finder menu or drag-to-Trash), like any removable disk.
- **No Extension to install** — the driver rides *inside* the launcher app. Nothing is copied into your Extensions folder, and nothing loads at boot until you run it.
- **Tells you it's really 2.0** — a volume mounted through this driver appears on the desktop with a distinct **"2.0" drive icon**, so a Hi-Speed mount is obvious at a glance versus a plain USB 1.1 one.
- Reliable transfers: byte-verified, with a transfer watchdog and a CSW residue/signature check on every command.

## Requirements

- A Power Mac running **Mac OS 9** with a free PCI slot. (Developed and tested on a **Power Mac G4 "Mirrored Drive Doors."**)
- A **PCI USB 2.0 host card** — an EHCI controller, PCI class code `0x0C0320`. (Tested with an **NEC-chipset IOGEAR card**; most generic USB 2.0 PCI cards of the era use similar NEC/VIA EHCI silicon.)
- A USB 2.0 mass-storage device, **FAT-formatted**. (Tested with a SanDisk Ultra USB flash drive.)

*Only one hardware combination has been tested so far. If it works — or doesn't — on your card / machine / drive, please open an issue; that data directly helps.*

## Install & use — follow these steps exactly

The insertion **timing** is the whole trick. Do it in this order:

1. **Boot with your USB drive UNPLUGGED from the card.**
   This is the important one. If a drive is attached at startup, Mac OS 9's built-in USB **1.1** driver grabs it before ours can, and it will not hand over to USB 2.0 cleanly (you'll get a "device was unexpectedly disconnected" message, or a hang). Start clean, with nothing in the card.

2. **At the desktop, double-click the `USB 2.0 Launcher` app.**
   A window opens and it brings up the USB 2.0 controller ("claiming the ports"). Do **not** plug anything in yet.

3. **Wait for the beep and this on-screen banner:**
   ```
   ****************************************************
   *   >>>  INSERT USB DRIVE NOW  <<<
   ****************************************************
   ```

4. **Now plug your drive into the card.**
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
- **One tested hardware combination** (see Requirements). Other EHCI cards are untested.
- The driver writes a verbose `EHCIUIM_init.log` each run. That's intentional for a beta (it's what to attach to a bug report) and does not slow down file transfers.

If you need rock-solid *removable* USB on OS 9 today, the built-in USB 1.1 support is still there and untouched. This driver is about **speed** — and about proving Hi-Speed USB on OS 9 is possible at all.

## The open problem — help wanted

The driver mounts perfectly when a drive is inserted onto a port that **already belongs to the EHCI controller** — that's the manual flow above, and it's rock-solid. What does *not* yet work is any scenario where a drive has to **move between Apple's USB 1.1 companion controller and EHCI:**

- **Auto-mount at boot** (drive attached at startup): the 1.1 companion claims it before our driver loads, and handing it over to EHCI stalls or hangs.
- **Hot re-insertion** (eject → unplug → replug mid-session): the re-enumeration makes Apple's USB Mass Storage class driver monopolize the USB Expert's task-level idle loop (`ExpertIdleTask`) and never yield it back, so the task-level re-mount never runs.

Both trace to the same root: **Mac OS 9's USB stack does not gracefully hand a mass-storage device between the 1.1 companion and the EHCI controller.** We've tried preventing the companion from grabbing it (an INIT can't map the card's registers that early in boot) and forcing the hand-off (stalls/hangs) — three angles, same wall. The manual launcher sidesteps it by keeping the companion out of the picture entirely.

**If you know the classic Mac OS USB stack internals** — how to make a companion controller cleanly release a port to EHCI, or why `ExpertIdleTask` won't return after a mass-storage re-probe of a device that won't mount — we'd genuinely love your input. Issues, pointers, and patches are all welcome. This is a great problem for anyone who enjoys low-level driver archaeology.

## How it works (for the curious)

There was no USB 2.0 anything for OS 9, so this is built from the ground up:

- A hand-written **EHCI host-controller driver (UIM)** — a native PowerPC `ndrv` — implementing the queue-head / qTD schedule, per-endpoint hardware toggles, and interrupt + heartbeat-timer servicing.
- A **virtual root hub** synthesized in software, so Apple's USB Expert enumerates the bus the normal way even though an EHCI controller presents its ports differently.
- A **self-driven SCSI probe** (INQUIRY / READ CAPACITY / READ) over Bulk-Only Transport — the stock mounter won't advance a Hi-Speed device, so the driver reads the disk itself and publishes a block service.
- A small **block driver** that mounts the volume through that service.
- The driver is injected without a declaration ROM by attaching it to the controller's Name Registry node as a **property-based driver**: the launcher creates the four registry properties a ROM card would have supplied, then calls `LoadUIMForEntry`.

Throughput comes from pre-queuing whole commands (one interrupt per command) and multi-qTD 128 KB transfer chains in both directions.

## Building from source

Built with the [Retro68](https://github.com/autc04/Retro68) cross-toolchain (PowerPC / classic Mac OS). With Retro68 installed and its PowerPC toolchain file configured:

```
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<path-to-Retro68 PowerPC toolchain.cmake>
cmake --build build
```

The shippable app is the **`EHCILauncher`** target → `EHCILauncher.bin` (a MacBinary — decode it on the Mac). `EHCITrigger` is the developer harness (verbose on-screen + on-disk logging) used during development.

## Status & disclaimer

Early beta, provided as-is, with no warranty. It has run for extended sessions without data loss on the tested setup, but it is new low-level code touching your disks — **keep backups, and don't trust it with your only copy of anything.** Bug reports and hardware-compatibility reports are very welcome.
