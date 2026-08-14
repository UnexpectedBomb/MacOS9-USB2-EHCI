# USB 2.0 for Mac OS 9: ROM-Integrated Release

**The first USB 2.0 (Hi-Speed) mass-storage driver for classic Mac OS 9.** Mac OS 9 shipped with USB 1.1 only (a 12 Mbit/s ceiling, roughly 1 MB/s in practice). This is a from-scratch EHCI (USB 2.0) stack that mounts USB drives and reads and writes them at **~20 MB/s read / ~13 MB/s write** on real hardware, roughly 10 to 20 times faster than anything OS 9 could do before.

## What's new in this release (2026-08-14)

This is the largest update since the project began, and it retires most of the caveats that used to fill this page:

- **A memory-safety fix for surprise removal.** If a drive was unplugged at the rare moment the system was still talking to it (for example, within the first seconds after insertion, or a slow drive pulled before it finished mounting), an orphaned transfer could later write through pointers the OS had already freed, silently corrupting the system heap; the damage typically surfaced minutes later or at the next restart, with nothing pointing back at the cause. The driver now retires every outstanding transfer for a removed device before the OS is told the device is gone, matching the teardown discipline of Apple's own USB stack. This was found and fixed before any ROM containing the vulnerable code was published; it is called out here because surprise-removal robustness is a headline feature of this stack.

- **One driver, two machines, fully validated.** The same driver binary now passes the complete hardening suite on both the Power Mac G4 MDD (PCI card) and the Mac mini G4 (on-board USB 2.0): cold boots with drives attached, four drives at once with copies between them, hot-plug, hub support, and clean ejects. The mini is no longer "newer and less proven"; both machines were validated on the same day with the same code.
- **No more Startup Items helper app.** Activation now lives in a small **system extension** (`Mini G4 EHCI` or `MDD G4 EHCI`). Drop it in the Extensions folder once and forget it. There is nothing to see and nothing that can be quit by accident.
- **Drives connected at boot now mount at Hi-Speed.** The old "boot with your drive unplugged" ritual is gone: the extension claims the controller's free ports at boot, so a drive that is already plugged in comes up at 2.0. (You may see a harmless prompt at startup; see "Things you may notice" below.)
- **The intermittent boot slowdowns and freezes are fixed, and the cause was ours.** The beta driver wrote a verbose diagnostic log through the File Manager, synchronously, on the boot volume. Under the right timing that single behavior could hold the Finder's thread for 30 seconds at a stretch, which showed up as slow extension loading, a desktop that took forever, an unresponsive cursor, or a machine that never came back. The release driver writes a two-line log and nothing else. If your earlier install ever felt sluggish or wedged at boot, this was very likely why.
- **Apple System Profiler no longer crashes.** ASP's "Devices and Volumes" tab had crashed to MacsBug on every machine with this driver installed since the first ROM. The cause was subtle (the OS creates a bookkeeping entry for ROM-loaded drivers but never finishes wiring it, and ASP is the only thing that ever walks through it); the driver now repairs that entry at boot. ASP scans cleanly and lists this stack's drives alongside everything else.
- **Pulling a drive without ejecting now shows Apple's own warning.** "The device for disk ... was unexpectedly disconnected", word for word, exactly as the built-in USB 1.1 stack would. No crash, and the surviving drives are untouched.
- **A third machine: the Power Mac G3 Blue & White.** The same driver now runs on the oldest NewWorld Mac, built on the RETAIL Mac OS 9.2.2 ROM (no MacOS9Lives install required), so it plausibly serves other stock-9.2.2 towers with a PCI USB 2.0 card (Sawtooth, Gigabit, Digital Audio, Quicksilver) -- reports welcome. On this era of machine the driver runs in polled mode (the card's interrupt line is not serviceable the same way there); in practice copies remain fast and the full test suite passes.
- **The extension is safe to install on its own.** If it finds no USB2 ROM driver on the controller (wrong machine, or the ROM step was missed), it logs one line and does nothing -- the machine boots normally. Earlier builds could freeze the boot in that mismatched configuration.
- **Troubleshooting: if the ROM will not boot** (blinking "?" folder, or a "checksum error" at an Open Firmware prompt): your StuffIt Expander mangled the decode. Older versions (5.5 confirmed) silently convert line endings inside the file because it begins with readable text. Use StuffIt Expander 6.0 or newer, or disable "convert text files" in its preferences, and expand a fresh copy. The ROM verifies its own checksum at boot, so a bad decode refuses loudly instead of booting corrupted code.
- **Simple, final file names.** The download names below say what each file is for. Version numbers live in the Finder's Get Info and Apple System Profiler (for the extensions) and in the table at the bottom of this page, not in the filenames.

## Downloads: which two files you need

Every machine needs exactly **two files**: a ROM and an extension. Take the pair for your machine.

| Your machine | ROM (BinHex `.hqx`) | Extension (MacBinary `.bin`) |
|---|---|---|
| **Mac mini G4** | `USB2_Mini_G4_ROM.hqx` | `Mini_G4_EHCI_Ext.bin` |
| **Power Mac G4 MDD** with a PCI USB 2.0 card | `USB2_MDD_G4_ROM.hqx` | `MDD_G4_EHCI_Ext.bin` |
| **Power Mac G3 Blue & White** with a PCI USB 2.0 card (retail 9.2.2) | `USB2_BW_G3_ROM.hqx` | `BW_G3_EHCI_Ext.bin` |

Decode both on the Mac with **StuffIt Expander** (both formats preserve the resource forks; converting them on a PC with a fork-blind tool will break them). The ROM decodes to a file named `Mac OS ROM`; the extension decompresses to `Mini G4 EHCI` or `MDD G4 EHCI`, wearing its own USB 2.0 icon.

Checksums and version details are in [Versions](#versions) at the bottom of this page.

## The driver lives in the Mac OS ROM

The EHCI UIM is injected into the **Mac OS ROM** itself, as a `driver,AAPL,MacOS,PowerPC` parcel bound to the USB 2.0 controller's Name Registry node (built with Elliot Nunn's Mac OS ROM toolchain). Mac OS 9 **loads and binds the driver at boot, as a genuine OS-owned host-controller driver**, the same way it loads a native Apple one. On a machine with the patched ROM, USB 2.0 support is part of the operating system, not bolted on by a helper app.

**The operating system performs the mount itself.** The driver enumerates the device, runs the SCSI Bulk-Only probe, and installs its own native block driver, which calls `AddDrive` and posts a disk-inserted event, so Mac OS 9 mounts the volume through its ordinary path, exactly as it would for a built-in controller. Apple's mass-storage mounter is not involved at all (it cannot be: it stops after the first `TEST UNIT READY` / `REQUEST SENSE` and never issues `INQUIRY` to a Hi-Speed device), and neither is Apple's USB stack, whose view of our ports is suppressed.

**The extension is the only other piece.** It runs once at boot: it claims the controller's free ports (leaving keyboard and mouse ports to the built-in 1.1 controller), activates the ROM driver, and arms the small task-level pump the driver borrows from the system. It has no window, no menu and no process to quit. It is not in the data path; once a volume is mounted, all I/O runs at interrupt level.

**Hot-plug works.** Insert, eject, unplug, re-insert, or swap in a completely different drive; every arrival is re-scanned and re-mounted, on the same drive number, with no leak. Ejection and improper removal both produce the same alerts, word for word, that Apple's own USB stack shows.

## What it does

- Mounts USB 2.0 mass-storage devices (flash drives / SSDs) on the OS 9 desktop at Hi-Speed, including **drives that are already connected at boot**.
- **Up to four drives at once, in any combination of ports.** Each gets its own volume, icon and geometry. Files copy directly between them, they can be ejected individually or all together in a single drag to the Trash, and each hot-plugs independently. Re-validated in this release with four drives across a PCI card and an external hub at the same time.
- **Drives behind an external USB 2.0 hub run at Hi-Speed.** The hub is enumerated and driven by this stack (we power, reset and address its downstream ports ourselves), so a drive plugged into a hub, including the hub built into an Apple Cinema Display, mounts at 2.0 instead of falling back to 1.1. Keyboards and mice behind a hub are a separate problem and still do not work: see "Known limitations".
- Reads and writes at the device's real speed, benchmarked at **20 MB/s read, ~13 MB/s write** (both are the flash device's own ceiling; the driver reaches it). Real Finder copies land lower (~4 to 8 MB/s) because the Finder's own I/O sizing is the bottleneck above the driver.
- On an **on-board** machine (Mac mini G4), the driver hands the keyboard/mouse ports back to the built-in 1.1 controller and claims only free ports for drives, so input keeps working while drives run at Hi-Speed, including with the drives behind the Cinema Display's hub on the same controller.
- **Ejects** cleanly (Finder menu or drag-to-Trash), like any removable disk, and warns with Apple's own alert if you pull a disk without ejecting.
- **Tells you it's really 2.0**: a volume mounted through this driver appears on the desktop with a distinct **"2.0" drive icon**, so a Hi-Speed mount is obvious at a glance versus a plain USB 1.1 one.
- Reliable transfers: byte-verified, with a transfer watchdog and a CSW residue/signature check on every command, and **correct block addressing across the entire volume**, so large files and volumes well past 2 GB / 4 GB read and write without corruption.
- Plays fair with the rest of the system: Apple System Profiler scans it cleanly, restarts and shutdowns quiesce the controller through a Shutdown Manager hook, and the built-in USB 1.1 stack is untouched.

## Supported machines

### Power Mac G4 MDD + PCI USB 2.0 card

- A Power Mac running Mac OS 9 with a free PCI slot and a **USB 2.0 host card** (EHCI, class `0x0C0320`).
- **Tested: Power Mac G4 "Mirrored Drive Doors" + NEC-chipset IOGEAR card.** Most generic USB 2.0 PCI cards of the era use similar NEC/VIA EHCI silicon.
- The card has a dedicated interrupt line, and this path has months of use plus this release's full hardening suite behind it.

### Mac mini G4 (on-board USB 2.0)

- The mini's built-in rear ports are USB 2.0-capable, but Mac OS 9 has only ever driven them at 1.1. With this ROM and extension they run at Hi-Speed.
- **Validated extensively in this release**: repeated cold boots with two drives attached behind the Cinema Display's hub, drive-to-drive copies (byte-verified), hot-plug and rude-removal cycles, and Apple System Profiler scans, all clean, with the keyboard and mouse working throughout on the same controller.
- ⚠️ **Read [the Mac mini ROM note](#a-note-for-mac-mini-g4-owners-the-vbl-display-fix) below before you install the mini ROM.** It concerns a *separate* display bug, not USB.

### Other machines

- **Other NewWorld Macs with an EHCI controller** (a PCI card, or on-board USB 2.0 on late OS 9-capable models): the driver should bind, but you must patch your own ROM; see "Install". On a PCI-card machine, `MDD_G4_EHCI_Ext.bin` is the extension to try; it adapts to the hardware at runtime.
- **OldWorld Macs are out of scope.** A beige G3, 8600 or 9600 has no `Mac OS ROM` file for this to patch (on those machines OS 9's whole USB stack is disk Extensions instead), so the injector will stop with *"no Parcelfile found in the dump"*.
- A USB 2.0 mass-storage device formatted **HFS, Mac OS Standard or Mac OS Extended** (either an Apple Partition Map with an `Apple_HFS` partition, or a single partitionless HFS volume). *Not FAT.* (Tested with SanDisk and generic flash drives, on volumes up to 62 GB.)

*If it works, or doesn't, on your machine / card / drive, please open an issue; that data directly helps.*

## Install

A **one-time setup**, two files. After that, drives are plug-and-play.

1. **Back up your current ROM.** Copy your machine's `Mac OS ROM` (it lives in the System Folder) somewhere safe. You will need it to revert.

2. **Install the ROM.** Decode the `.hqx` for your machine with StuffIt Expander; it decodes to a file named `Mac OS ROM`. **Boot from a CD or another volume**, put the decoded file into the System Folder in place of the original, and reboot. You cannot swap it while booted from that same System Folder. Make sure you can boot from something else before you start.

   > ⚠ **MDD owners: not if your machine only boots OS 9 thanks to a community ROM patch** (an FW800 MDD and similar). Your `Mac OS ROM` has already been modified to make OS 9 boot at all, and the prebuilt MDD file does not contain that work. Use the injector on your own already-patched ROM instead (see "Patching your own ROM" below); it appends a parcel rather than replacing anything, so it composes.
   >
   > The **mini** ROM is built on the MacOS9Lives mini ROM, so it keeps the patches that make a mini boot OS 9 at all.

3. **Install the extension.** Decode the `.bin` with StuffIt Expander and drop the resulting file (`Mini G4 EHCI` or `MDD G4 EHCI`) into **System Folder ▸ Extensions**. It shows its version in Apple System Profiler's Extensions tab and in Get Info.

That's it. Reboot and drives mount as you plug them in, or come up already mounted if they were connected at boot.

### Patching your own ROM (other machines, or already-patched ROMs)

```sh
python3 scripts/build-rom-hqx.py "Mac OS ROM" mybuild "Mac OS ROM (USB2).hqx"
```

That injects the driver into your own ROM and emits BinHex, which keeps the ROM's resource fork. It refuses to write a ROM whose `SysEnabler` came out empty (see [Why no version in the ROM's Get Info](#why-no-version-in-the-roms-get-info)). Your input needs its resource fork intact: getting a `Mac OS ROM` off an OS 9 machine over anything that does not understand forks will silently strip it; StuffIt/BinHex on the Mac, or `unar` on the desktop, will preserve it. See [BUILD.md](BUILD.md) for the toolchain.

## Things you may notice (normal, and what to do)

- **"This disk is unreadable" at startup, sometimes, with a drive attached at boot.** Intermittent and harmless: the drive was offered to the system a moment before its filesystem was readable. **Click Eject** (never Initialize, which would erase it). The drive mounts by itself a few seconds later.
- **A pause of 15 to 45 seconds during startup, sometimes with the keyboard and mouse briefly dead**, typically right when a Keychain or file-server dialog appears. That is Mac OS 9 itself trying to reach an AppleShare server from your Servers folder at boot; the whole system's input freezes during that window, including on stock machines. Wait it out; it recovers on its own. It is not the USB driver, and pulling cables during it can interrupt a mount in progress.
- **Always eject (Put Away / drag to the Trash) before unplugging a drive**, as with any removable disk. If you forget, the driver unmounts the volume safely and shows Apple's "unexpectedly disconnected" warning; your other drives are unaffected. Avoid unplugging a **hub** (or the Cinema Display's USB cable) while the drives behind it are still mounting at boot.

## A note for Mac mini G4 owners: the VBL display fix

> The prebuilt **mini** ROM is built on a base that already carries a **second, unrelated patch**: a fix
> for the Mac mini's OS 9 **startup freeze**, where the mouse cursor freezes during boot at a scaled
> screen resolution. That bug is a display-driver fault (`ATY,RockHopper2` does not re-arm the vertical
> blank interrupt after a mode switch) and has nothing to do with USB. It is a
> [separate project](https://github.com/UnexpectedBomb/G4-Mac-Mini-VBL-Fix).
>
> **You need to know how well that part works, because it is bundled into the ROM you are about to flash:**
>
> - ⚠️ **The ROM-integrated VBL fix is experimental and does NOT reliably fix the freeze.** In continued
>   testing after it was first published, boots **still froze** with it installed: roughly **2 frozen-cursor
>   boots in about 25**. Stock, unpatched, the freeze rate is around 5% of boots (higher on warm restarts).
>   Those numbers are statistically indistinguishable, so **there is currently no evidence that the ROM part
>   of the VBL fix helps at all.** It is carried here as an experiment, not as a working fix.
> - ✅ **The fix that does work is the app**, `VBLFix`, from that project's release page. It is proven,
>   including independent confirmation by other people on other minis.
> - ➡️ **So: install `VBLFix` in your Startup Items as well. Treat it as required, not optional.** With the
>   app in place, a boot the ROM misses becomes a brief freeze that clears itself a few seconds later
>   instead of a frozen machine.
>
> **Why the app works better than the ROM:** the leading explanation is timing. The ROM re-arms the
> interrupt inside the display driver's mode-switch, which happens deep in the fragile early-boot window
> where the re-enable often does not latch. The app fires **later**, from Startup Items, after everything
> has settled, which appears to be inherently the more reliable moment. This is a well-supported
> hypothesis, not a proven mechanism.
>
> **None of this affects USB 2.0.** The USB half of this ROM is what was validated above; the VBL half is
> along for the ride because that is the ROM the mini was tested on. If you would rather not take the
> experimental display patch at all, run the injector against your own mini ROM instead of using the
> prebuilt one, and you will get the USB 2.0 driver on your own base.

## Known limitations (please read)

- **Four drives is the ceiling.** The per-device DMA structures live in one wired memory page, which holds four
  devices plus the hub's own bookkeeping. A fifth drive is refused cleanly (it simply does not mount, nothing
  else is disturbed) and the driver posts a notification saying so. Raising the limit needs a second DMA page.
- **Keyboards and mice behind a USB 2.0 hub do not work.** A drive behind a hub runs at Hi-Speed, but a
  full-speed or low-speed device behind that same hub (a keyboard, a mouse, or an Apple Cinema Display's own
  brightness buttons) is detected, left powered and skipped. It cannot be handed to the 1.1 companion either,
  because a device behind a Hi-Speed hub is not electrically on the companion's bus at all: it sits behind the
  hub's transaction translator. Reaching it requires EHCI **split transactions**, which are not implemented.
  See "The open problems". **If you need a keyboard and mouse on a hub, put that hub on a USB 1.1 port.**
- **One hub at a time, and no hubs behind hubs.** A second hub, or a hub plugged into our hub, is not driven.
- **A drive inserted while a large file copy is running may not mount.** Wait for the copy to finish and plug
  it in again, or plug it in before starting the copy. (The engine work to lift this is partly in place, but
  the case has not been validated, so the limitation stands until it is.)
- **Writes are slower than reads.** Reads reach the device's ceiling (~20 MB/s); Finder writes land around 2 to 5 MB/s because the Finder issues small synchronous writes above the driver.
- **Mid-write yank is unsafe** (as on any OS): always eject first.
- **A few tested hardware combinations** (see Supported machines). Other EHCI cards/controllers are untested.

If you need rock-solid *removable* USB on OS 9 today, the built-in USB 1.1 support is still there and untouched. This driver is about **speed**, and about proving Hi-Speed USB on OS 9 is possible at all.

## The open problems, help wanted

These are the problems that remain, and they are good targets for anyone who enjoys low-level driver archaeology.

### 1. Full-speed and low-speed devices behind a Hi-Speed hub

A full-speed or low-speed device behind a Hi-Speed hub (a keyboard, a mouse, a display's own control buttons)
cannot be reached by either controller as things stand:

- **Not by Apple's 1.1 companion**, because the device is not electrically on the companion's bus. The hub is
  on the Hi-Speed bus and the device sits behind the hub's transaction translator. There is no port to hand
  over, so no amount of ownership juggling reaches it.
- **Not by us**, because talking through a transaction translator requires **EHCI split transactions**
  (start-split / complete-split), and for a keyboard or mouse that also means a **periodic schedule** with
  S-mask / C-mask scheduling. This driver is async-only: the periodic frame list is allocated but nothing has
  ever been linked into it. That is the single largest piece of unbuilt work in the project.

This is why Apple layered `IOUSBControllerV2` on top of `IOUSBController` in OS X. The first version could not
do it either.

Note the consequence, because it is a real trap: claiming a hub is **all or nothing**. Everything behind a hub
we claim becomes ours, so anything we cannot drive is dead while we own it. Until split transactions exist, put
a hub carrying input devices on a USB 1.1 port.

There is also a wrinkle worth knowing if you touch this area. Releasing a port to the companion (`PORTSC` Port Owner = 1) only works from the state the EHCI spec intends: a port that is **not enabled**, i.e. one where a reset did not bring up a high-speed device. Handing over a port that *is* enabled at Hi-Speed does not take effect. The driver clears Port Enable first, and records the handoff in software rather than re-reading the ownership bit back out of the hardware; trusting that register as the record of a decision it had already made produced a livelock, with the port bouncing between the two controllers so fast that neither could enumerate anything.

### 2. Insert-during-copy

A drive inserted in the middle of a sustained copy may not mount (its setup transfers queue behind the copy's).
The transfer engine now keeps control traffic and block I/O in separate in-flight slots, which was the
structural half of the fix, but the end-to-end case has not been validated and is deliberately not claimed.

### 3. More machines

The driver adapts to its controller at runtime (dedicated versus shared interrupt line, port-power control,
companion handoff), and the parcel matches by PCI class code, so late iMacs, eMacs and PowerBooks with
on-board USB 2.0 that can run OS 9 are plausible targets. Each needs its ROM patched and a validation pass.
Issues, pointers, and patches are all welcome.

## How it works (for the curious)

There was no USB 2.0 anything for OS 9, so this is built from the ground up:

- A hand-written **EHCI host-controller driver (UIM)**, a native PowerPC `ndrv`, implementing the queue-head / qTD schedule, per-endpoint hardware toggles, and interrupt + heartbeat-timer servicing.
- A **virtual root hub** synthesized in software, so the bus is presented the way the OS expects even though an EHCI controller presents its ports differently.
- A **self-driven SCSI probe** (INQUIRY / READ CAPACITY / READ) over Bulk-Only Transport; the stock mounter won't advance a Hi-Speed device, so the driver reads the disk itself and publishes a block service.
- A small **block driver** that mounts the volume through that service: `AddDrive`, a disk-inserted event, and the OS does the rest.
- The driver is delivered as a **Mac OS ROM parcel**: a `driver,AAPL,MacOS,PowerPC` property (the ndrv's PEF) and its descriptor are injected into the ROM and bound to the controller's Name Registry node, so OS 9 loads and prepares it **at boot**, as an OS-owned driver. The **extension** then activates it (`LoadUIMForEntry`), claims the free ports, and arms the driver's task-level pump, which rides the Notification Manager exactly the way Apple's own mass-storage stack reaches task level.
- **Coexisting with the keyboard/mouse on an on-board controller:** the EHCI shares one physical set of ports (and one PCI interrupt line) with the OHCI "companion" controllers that drive the keyboard and mouse. Rather than seize the whole controller, the driver does a **per-port claim**: it powers the ports, waits out the USB connect debounce, then hands every port that already holds a low-speed device back to the 1.1 companion (setting its Port Owner bit) and claims the rest for EHCI, so drives come up at Hi-Speed while input stays on the companion. Because the interrupt line is shared, the driver's interrupt handler **chains** to the companion's handler so the keyboard and mouse keep being serviced.

Throughput comes from pre-queuing whole commands (one interrupt per command) and multi-qTD 128 KB transfer chains in both directions. The deeper story is in [TECHNICAL.md](TECHNICAL.md).

### Why no version in the ROM's Get Info

The extensions show their version in Get Info and Apple System Profiler. The ROMs deliberately do not: Get Info reads a file's `vers` resource, and a `Mac OS ROM` file's resource fork belongs to the **System Enabler**. Stamping a version there mis-describes the enabler, and the machine stops booting ("No File System Access modules could be found"); this was proven the hard way, with an A/B test, and the build tool now refuses to do it. ROM versions live in the table below and in the driver's own log banner.

## Building from source

Built with the [Retro68](https://github.com/autc04/Retro68) cross-toolchain (PowerPC / classic Mac OS). With Retro68 installed and its PowerPC toolchain file configured:

```
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<path-to-Retro68 PowerPC toolchain.cmake>
cmake --build build
```

Two build-time switches matter for release versus diagnosis:

- `-DEHCI_LOG_LEVEL=` **0** (release: the driver writes a two-line banner and nothing else), **1** (errors only), **2** (full trace; this is the diagnostic mode, and it is heavy enough to distort timing, so do not benchmark or judge stability on a level-2 build).
- `-DEHCI_PAINT=` **0** (release) or **1** (a screen-corner diagnostic readout that survives system hangs; development only).

The shippable pieces are the **driver** (`EHCIUIM` target, injected into a Mac OS ROM with `scripts/build-rom-hqx.py`) and the **activation extension** (built from `resident/`, then dressed for release, name, version resource, icon, with `scripts/package-init.py`). See **[BUILD.md](BUILD.md)** for the full pipeline.

## Versions

| File | Contains | Version shown | MD5 |
|---|---|---|---|
| `USB2_Mini_G4_ROM.hqx` | driver **h96rel** on the MacOS9Lives mini + VBL-fix base | in the driver log banner | `2e2a2363dd4ae06fa83491a517cbaf34` |
| `USB2_MDD_G4_ROM.hqx` | driver **h96rel** on the MDD stock base | in the driver log banner | `904713d2a0e27ed2e488ed99cb006f4a` |
| `Mini_G4_EHCI_Ext.bin` | extension `Mini G4 EHCI` | **11.2** (Get Info / ASP) | `ff84d8b7cb629f2ea8a9f0bfb71b6b40` |
| `MDD_G4_EHCI_Ext.bin` | extension `MDD G4 EHCI` | **8.3** (Get Info / ASP) | `f345db22764507fb3c0e1e607163d2bf` |
| `USB2_BW_G3_ROM.hqx` | driver **b10rel** (polled mode) on the RETAIL 9.2.2 base | in the driver log banner | `ad97f6dd49e5dd89f57c12ec00743081` |
| `BW_G3_EHCI_Ext.bin` | extension `BW G3 EHCI` | **8.3** (Get Info / ASP) | `ee4863df8510d444f20804b72171d2e6` |

The driver writes `EHCIUIM_init.log` on the boot volume: two lines identifying the build (`=== EHCIUIM BUILD h96rel ===`) and the logging mode. That is the whole log in a release build; if you file a bug report, include those lines and a description, and a diagnostic build can be provided.

## History

> **If you flashed a ROM from `n13` or `m3` (before 2026-08-07), please re-flash.** Those ROMs shipped as
> MacBinary, and the wrapper used at the time built its own small resource fork that **replaced** the ROM's
> real one, losing a 185 KB component called `SysEnabler`. They boot, but on the MDD this caused intermittent
> input stalls at startup. ROMs have shipped as BinHex (`.hqx`), which carries both forks, ever since. A
> first corrected release on 2026-08-07 carried a `vers` resource that stopped machines booting and was
> withdrawn within the day (it is also why ROMs no longer carry a Get Info version; see above).
>
> **If you used any release before this one against volumes larger than 2 GB**, note that an early
> data-corruption bug (writes past the 2 GB / 4 GB volume-offset boundary going to the wrong blocks) was
> found and fixed; re-verify or reformat volumes that old builds wrote to.

## Status & disclaimer

Provided as-is, with no warranty. Both supported machines pass the full validation suite as of this release, but this remains new low-level code touching your disks, so **keep backups.** Bug reports and hardware-compatibility reports are very welcome.
