# USB 2.0 for Mac OS 9: ROM-Integrated Release

**The first USB 2.0 (Hi-Speed) mass-storage driver for classic Mac OS 9.** Mac OS 9 shipped with USB 1.1 only (a 12 Mbit/s ceiling, roughly 1 MB/s in practice). This is a from-scratch EHCI (USB 2.0) stack that mounts USB drives and reads and writes them at up to **~11 MB/s read / ~5 MB/s write** on real hardware, roughly 12 times faster than anything OS 9 could do before. See [Measured performance](#measured-performance) for the full benchmark.

## What's new in this release (2026-08-27)

- ★ **The Power Mac G4 MDD (FireWire 800) now runs USB 2.0 on its own built-in ports. No PCI card.** The FW800 MDD logic board has always carried a NEC USB 2.0 controller. It is the chip the machine's built-in USB ports are wired to, and Apple shipped only its USB 1.1 half: the USB 2.0 half is switched off by the machine's own BootROM at power-on, before any operating system gets a look at it. (The FW400 MDD board leaves that chip's footprint empty, so this is a FireWire 800 machine only.) This release switches it back on from inside the Mac OS ROM and hands the controller to the same driver the other three machines use. There is nothing to configure, no NVRAM edits and no hardware modification, and the driver itself did not need a single line changed. As far as we know this is the first time this controller has been driven under Mac OS 9 at all: the 2003 Orange Micro drivers written for it were Mac OS X only.

- **How the switch was found.** The 2.0 half does not merely go un-enumerated, it does not answer on the PCI bus at all, so there was nothing to find by inspecting it directly. Instead we dumped all 256 bytes of the USB 1.1 controller's PCI configuration space on a machine where 2.0 works (the Mac mini G4, which uses the same NEC family) and on the FW800 MDD, and compared them. The two were byte-identical except for a single long word. The practical result is a two-line addition to the ROM's boot script.

- **Everything else is unchanged.** Same driver, same extension, same install procedure. The other three machines are unaffected by this release.

## What was new in the 2026-08-15 release

This was the most consequential update since the project began, solving many of the problems that appeared in earlier iterations.

- **No more crash-prone EHCI Activator helper app!** This is probably the single biggest change, and has vastly improved usability. Activation now lives in a small **system extension**. Drop it in the Extensions folder once and forget it. There is nothing to see and nothing that can be quit by accident; it all runs in the background, similar to Apple's own built-in OHCI stack. The desktop "helper apps" were always a bit flaky and were merely a stepping stone towards the ultimate goal of a seamless, native-feeling experience.

- **One driver, THREE machines, fully validated.** The same driver binary has passed all hardening tests on the Power Mac G4 MDD (PCI card), the Mac mini G4 (on-board USB 2.0), and now the oldest NewWorld Mac, the Power Mac G3 Blue & White (PCI Card). The validation on the B&W was particularly important because its ROM was built on the stock RETAIL Mac OS 9.2.2 ROM, so it's possible that it can also be dropped into other non-MDD G4 stock-9.2.2 towers with a PCI USB 2.0 card (Sawtooth, Gigabit, Digital Audio, Quicksilver). It would be VERY helpful and valuable to this project if those who have such towers can test this theory out and report back. **(Since this release: aladds on 68kmla has confirmed the B&W pair working unmodified on a Sawtooth AGP G4. Gigabit Ethernet, Digital Audio and Quicksilver reports are the ones still missing.)** Due to the era of the B&W, its driver runs in polled mode but in practice its copies remain fast reliable. Hardening tests included cold boots with drives attached, four drives at once with copies between them, hot-plug, hub support, and clean ejects.

- **Drives connected at boot now mount at Hi-Speed.** The old "boot with your drive unplugged" ritual is gone: the extension claims the controller's free ports at boot, so a drive that is already plugged in comes up at 2.0. (You may see a harmless prompt at startup; see "Things you may notice" below.)

- **Apple System Profiler no longer crashes.** ASP's "Devices and Volumes" tab had crashed to MacsBug on every machine with this driver installed since the first ROM. The cause was subtle (the OS creates a bookkeeping entry for ROM-loaded drivers but never finishes wiring it, and ASP is the only thing that ever walks through it); the driver now repairs that entry at boot. ASP scans cleanly and lists this stack's drives alongside everything else.

- **Pulling a drive without ejecting now shows Apple's own warning.** "The device for disk ... was unexpectedly disconnected", word for word, exactly as the built-in USB 1.1 stack would. No crash, and the surviving drives are untouched.

- **A memory-safety fix for surprise removal.** If a drive was unplugged at the rare moment the system was still talking to it (for example, within the first seconds after insertion, or a slow drive pulled before it finished mounting), an orphaned transfer could later write through pointers the OS had already freed, silently corrupting the system heap; the damage typically surfaced minutes later or at the next restart, with nothing pointing back at the cause. The driver now retires every outstanding transfer for a removed device before the OS is told the device is gone, matching the teardown discipline of Apple's own USB stack. This was found and fixed before any ROM containing the vulnerable code was published; it is called out here because surprise-removal robustness is a headline feature of this stack.

- **Disk First Aid now shows real drive information.** Utilities that ask a disk driver where a drive lives (Disk First Aid is the visible one) used to get an unanswered query back and would render a line of garbage text under the volume's name. The driver now answers the way Apple's own drivers do: each USB 2.0 volume shows a proper location line ("USB 2.0, drive 1 (v1.0)") and the stack's own USB 2.0 icon in Disk First Aid's list. Verify and Repair work on these volumes as normal, and always did; the label is now honest too.

- **The intermittent boot slowdowns and freezes are fixed.** The beta driver wrote a verbose diagnostic log through the File Manager, synchronously, on the boot volume. Under the right timing that single behavior could hold the Finder's thread for 30 seconds at a stretch, which showed up as slow extension loading, a desktop that took forever, an unresponsive cursor, or a machine that never came back. The release driver writes a two-line log and nothing else. If your earlier install ever felt sluggish or wedged at boot, this was very likely why.

- **The extension is safe to install on its own.** If it finds no USB2 ROM driver on the controller (wrong machine, or the ROM step was missed), it logs one line and does nothing -- the machine boots normally. Earlier builds could freeze the boot in that mismatched configuration.
- **Troubleshooting: if the ROM will not boot** (blinking "?" folder, or a "checksum error" at an Open Firmware prompt): your StuffIt Expander corrupted the decode. Older versions (5.5 confirmed) silently convert line endings inside the file because it begins with readable text. Use StuffIt Expander 6.0 or newer, or disable "convert text files" in its preferences, and expand a fresh copy. The ROM verifies its own checksum at boot, so a bad decode refuses loudly instead of booting corrupted code.
- **Simple, final file names.** The download names below say what each file is for. Version numbers live in the Finder's Get Info and Apple System Profiler (for the extensions) and in the table at the bottom of this page, not in the filenames.

## Downloads: which two files you need

Every machine needs exactly **two files**: a ROM and an extension. Take the pair for your machine.

| Your machine | ROM (BinHex `.hqx`) | Extension (MacBinary `.bin`) |
|---|---|---|
| **Mac mini G4** | `USB2_Mini_G4_ROM.hqx` | `Mini_G4_EHCI_Ext.bin` |
| **Power Mac G4 MDD** with a PCI USB 2.0 card | `USB2_MDD_G4_ROM.hqx` | `MDD_G4_EHCI_Ext.bin` |
| **Power Mac G4 MDD (FireWire 800)**, built-in ports, no card | `USB2_MDD_G4_FW800_ROM.hqx` | `MDD_G4_EHCI_Ext.bin` |
| **Power Mac G3 Blue & White** with a PCI USB 2.0 card (retail 9.2.2) | `USB2_BW_G3_ROM.hqx` | `BW_G3_EHCI_Ext.bin` |
| **Power Mac G4 AGP ("Sawtooth")** with a PCI USB 2.0 card | `USB2_BW_G3_ROM.hqx` | `BW_G3_EHCI_Ext.bin` |
| **Power Mac G4 Gigabit Ethernet, Digital Audio, Quicksilver** with a PCI USB 2.0 card (untested, expected to work) | `USB2_BW_G3_ROM.hqx` | `BW_G3_EHCI_Ext.bin` |

**Sawtooth owners: yes, that is the B&W pair, and it is the right one.** It is built on the plain retail Mac OS 9.2.2 ROM rather than any machine-specific one, so it is not really a "B&W" pair at all; it travels. A Sawtooth running it was confirmed by aladds on the 68kmla forum, which is a community report rather than one of our own test machines. See [Supported machines](#supported-machines) for the detail.

**Gigabit Ethernet, Digital Audio and Quicksilver are untested, which is not the same as unsupported.** We expect that pair to work on them, and the reasons are concrete rather than hopeful: the ROM is built on the plain retail 9.2.2 base rather than a machine-specific one, and this build runs the controller in polled mode, which sidesteps per-machine interrupt wiring entirely. Those are the two properties that carried it from a B&W to a Sawtooth, and neither of them cares which G4 tower it is. What is missing is simply that nobody has posted a report yet. If you try it, please say so either way; that is the single most useful thing anyone can contribute to this project right now.

Decode both on the Mac with **StuffIt Expander** (both formats preserve the resource forks; converting them on a PC with a fork-blind tool will break them). The ROM decodes to a file named `Mac OS ROM`; the extension decompresses to `Mini G4 EHCI`,  `MDD G4 EHCI`, etc, with a USB icon.

Checksums and version details are in [Versions](#versions) at the bottom of this page.

## HOW IT WORKS: The driver now lives in the Mac OS ROM!

The EHCI UIM is injected into the **Mac OS ROM** itself, as a `driver,AAPL,MacOS,PowerPC` parcel bound to the USB 2.0 controller's Name Registry node (built with Elliot Nunn's Mac OS ROM toolchain). Mac OS 9 **loads and binds the driver at boot, as a genuine OS-owned host-controller driver**, the same way it loads a native Apple one. On a machine with the patched ROM, USB 2.0 support is part of the operating system, not bolted on by a helper app.

**The operating system performs the mount itself.** The driver enumerates the device, runs the SCSI Bulk-Only probe, and installs its own native block driver, which calls `AddDrive` and posts a disk-inserted event, so Mac OS 9 mounts the volume through its ordinary path, exactly as it would for a built-in controller. Apple's mass-storage mounter is not involved at all (it cannot be: it stops after the first `TEST UNIT READY` / `REQUEST SENSE` and never issues `INQUIRY` to a Hi-Speed device), and neither is Apple's USB stack, whose view of our ports is suppressed.

**The extension is the only other piece.** It runs once at boot: it claims the controller's free ports (leaving keyboard and mouse ports to the built-in 1.1 controller), activates the ROM driver, and arms the small task-level pump the driver borrows from the system. It has no window, no menu and no process to quit. It is not in the data path; once a volume is mounted, all I/O runs at interrupt level.

**Hot-plug works.** Insert, eject, unplug, re-insert, or swap in a completely different drive; every arrival is re-scanned and re-mounted, on the same drive number, with no leak. Ejection and improper removal both produce the same alerts, word for word, that Apple's own USB stack shows.

## What it does

- Mounts USB 2.0 mass-storage devices (flash drives / SSDs) on the OS 9 desktop at Hi-Speed, including **drives that are already connected at boot**.
- **Up to four drives at once, in any combination of ports.** Each gets its own volume, icon and geometry. Files copy directly between them, they can be ejected individually or all together in a single drag to the Trash, and each hot-plugs independently. Re-validated in this release with four drives across a PCI card and an external hub at the same time.
- **Drives behind an external USB 2.0 hub run at Hi-Speed.** The hub is enumerated and driven by this stack (we power, reset and address its downstream ports ourselves), so a drive plugged into a hub, including the hub built into an Apple Cinema Display, mounts at 2.0 instead of falling back to 1.1. Keyboards and mice behind a hub are a separate problem and still do not work: see "Known limitations".
- Reads at about **11 MB/s** and writes at about **5 MB/s** on large transfers, roughly 12x and 7x what USB 1.1 manages on the same hardware. Real Finder copies land lower, because the Finder issues smaller requests than a benchmark does. See [Measured performance](#measured-performance).
- On an **on-board** machine (Mac mini G4), the driver hands the keyboard/mouse ports back to the built-in 1.1 controller and claims only free ports for drives, so input keeps working while drives run at Hi-Speed, including with the drives behind the Cinema Display's hub on the same controller.
- **Ejects** cleanly (Finder menu or drag-to-Trash), like any removable disk, and warns with Apple's own alert if you pull a disk without ejecting.
- **Tells you it's really 2.0**: a volume mounted through this driver appears on the desktop with a distinct **"2.0" drive icon**, so a Hi-Speed mount is obvious at a glance versus a plain USB 1.1 one.
- Reliable transfers: byte-verified, with a transfer watchdog and a CSW residue/signature check on every command, and **correct block addressing across the entire volume**, so large files and volumes well past 2 GB / 4 GB read and write without corruption.
- Plays fair with the rest of the system: Apple System Profiler scans it cleanly, restarts and shutdowns quiesce the controller through a Shutdown Manager hook, and the built-in USB 1.1 stack is untouched.

## Measured performance

Measured with **QuickBench 2.0 (Intech)** on a Power Mac G4 MDD, using the **same USB flash drive** for both runs: once on the machine's built-in USB 1.1 port (Apple's own stack), once through this driver on a PCI USB 2.0 card. Sequential transfers, MB/s:

| Transfer size | USB 2.0 read | USB 2.0 write | USB 1.1 read | USB 1.1 write |
|---|---|---|---|---|
| 4 KB | 0.52 | 0.47 | 0.58 | 0.28 |
| 16 KB | 2.01 | 0.99 | 0.80 | 0.52 |
| 64 KB | 7.83 | 3.09 | 0.90 | 0.71 |
| 256 KB | 10.42 | 5.01 | 0.92 | 0.63 |
| 1 MB | **11.43** | 2.76 | 0.93 | 0.74 |

Reading what that table says, honestly:

- **On large transfers this stack is about 12x faster than USB 1.1** for reads, and around 6 to 7x for writes.
- **Writes are slower than reads and vary run to run** (note the 1 MB write coming in below the 256 KB write). Some of that is the flash drive's own behavior, which shows up on the USB 1.1 run too.
- **On very small transfers, USB 1.1 is currently faster than this driver.** The two stacks cross over at about 8 KB. That is a real limitation of this driver, not a measurement artifact, and the cause is understood: this driver's transfer engine is driven by an 8 ms timer, so every request waits for the next tick before it is issued. Measured time per transfer is a nearly constant 7.8 ms from 1 KB all the way to 64 KB, whether the transfer moves 1 KB or 64 KB. At 64 KB, only about 1.5 ms of that is time actually spent moving data.

That last point is the single biggest performance item on the list, and it caps everything above: the 11 MB/s ceiling is essentially the driver's 128 KB staging buffer divided by the timer period. Issuing transfers as soon as they arrive, rather than on the next tick, is the obvious fix and is being worked on. It touches the data path, so it gets tested carefully before it ships.

If you run your own benchmark, use a release build (a diagnostic build writes logs during the test and will read slow), a freshly formatted drive, and volume level tests rather than SCSI passthrough, which cannot see this driver.

## Supported machines

### Power Mac G4 MDD + PCI USB 2.0 card

- A Power Mac running Mac OS 9 with a free PCI slot and a **USB 2.0 host card** (EHCI, class `0x0C0320`).
- **Tested: Power Mac G4 "Mirrored Drive Doors" + NEC-chipset IOGEAR card.** Most generic USB 2.0 PCI cards of the era use similar NEC/VIA EHCI silicon.

### Mac mini G4 (on-board USB 2.0)

- The Mini's built-in rear ports are USB 2.0-capable, but Mac OS 9 has only ever driven them at 1.1. With this ROM and extension they run at Hi-Speed.
- **Validated extensively in this release**: repeated cold boots with two drives attached behind the Cinema Display's hub, drive-to-drive copies (byte-verified), hot-plug and rude-removal cycles, and Apple System Profiler scans, all clean, with the keyboard and mouse working throughout on the same controller.
- ⚠️ **Read [the Mac mini ROM note](#a-note-for-mac-mini-g4-owners-the-vbl-display-fix) below before you install the mini ROM.** It concerns a *separate* display bug, not USB.

### Power Mac G4 MDD (FireWire 800), built-in ports

- **No PCI card required.** The FW800 MDD's built-in USB ports are wired to an on-board NEC controller whose USB 2.0 half Apple left switched off. This ROM turns it on, publishes it to Mac OS, and the driver binds to it exactly as it does to a PCI card.
- This applies to the **FireWire 800 MDD only** (`PowerMac3,6`). The FireWire 400 MDD leaves that chip's footprint unpopulated, so there is nothing to enable; on a FW400 machine use the PCI card pair above.
- The controller is presented in **polled mode**, for the same reason the B&W build is: the port Apple never published has no interrupt routing for us to inherit, so the driver drives it from its own timer instead. In practice copies are fast and reliable.
- ⚠ **This ROM is built on a community-modified FW800 MDD `Mac OS ROM`**, the kind that makes an FW800 MDD boot Mac OS 9 in the first place, so it keeps that work. If your machine boots from a *different* community ROM than the one this was built on, patch your own instead of replacing it; see "Patching your own ROM" under Install. The injector appends a parcel rather than replacing anything, so it composes with whatever patches your ROM already carries.

### Power Mac G3 B&W + PCI USB 2.0 card (and, it turns out, other G4 towers)

- A Power Mac running Mac OS 9 with a free PCI slot and a **USB 2.0 host card** (EHCI, class `0x0C0320`).
- **Tested: Power Mac G3 "Blue & White" + NEC-chipset IOGEAR card.** Most generic USB 2.0 PCI cards of the era use similar NEC/VIA EHCI silicon.
- ★ **Confirmed working on a Power Mac G4 AGP ("Sawtooth") by aladds on the 68kmla forum**, using this same B&W pair unmodified. That is the first report from a machine other than the three tested here, and it is the expected result: this ROM is built on the plain retail Mac OS 9.2.2 base rather than any machine-specific one, and this build runs the controller in **polled mode**, which sidesteps per-machine interrupt wiring entirely. Those two properties are most of why this particular pair travels well.
- **So if you have a stock-9.2.2 G4 tower** (Sawtooth, Gigabit Ethernet, Digital Audio, Quicksilver) **with a PCI USB 2.0 card, this is the pair to try.** Gigabit Ethernet, Digital Audio and Quicksilver are **untested rather than unsupported**: we expect them to work, for the two reasons above, and the only thing missing is a report. Please post either way.
- ⚠ **One case where this pair is the wrong choice:** if your machine only boots Mac OS 9 thanks to a modified `Mac OS ROM` (a community ROM for a machine Apple never supported), do not replace it with this one. Inject the driver into the ROM you already have instead; see "Patching your own ROM" under Install.

### Other machines

- **Other NewWorld Macs with an EHCI controller** (a PCI card, or on-board USB 2.0 on late OS 9-capable models): the driver should bind, but you must patch your own ROM; see "Install". On a PCI-card machine, `MDD_G4_EHCI_Ext.bin` is the extension to try; it adapts to the hardware at runtime.
- **OldWorld Macs are out of scope.** A beige G3, 8600 or 9600 has no `Mac OS ROM` file for this to patch (on those machines OS 9's whole USB stack is disk Extensions instead), so the injector will stop with *"no Parcelfile found in the dump"*.
- A USB 2.0 mass-storage device formatted **HFS, Mac OS Standard or Mac OS Extended** (either an Apple Partition Map with an `Apple_HFS` partition, or a single partitionless HFS volume). *Not FAT.* (Tested with SanDisk and generic flash drives, on volumes up to 62 GB.)

*If it works, or doesn't, on your machine / card / drive, please open an issue; that data directly helps.*

## Install

A **one-time setup**, two files. After that, drives are plug-and-play.

1. **Back up your current ROM.** Copy your machine's `Mac OS ROM` (it lives in the System Folder) somewhere safe. You will need it to revert.

2. **Install the ROM.** Decode the `.hqx` for your machine with StuffIt Expander; it decodes to a file named `Mac OS ROM`. **Boot from a CD or another volume**, put the decoded file into the System Folder in place of the original, and reboot. You cannot swap it while booted from that same System Folder. Make sure you can boot from something else before you start.

   > ⚠ **MDD owners: `USB2_MDD_G4_ROM.hqx` is built on the stock MDD ROM**, so it is the wrong file if your machine only boots OS 9 thanks to a community ROM patch. **FireWire 800 MDD owners want `USB2_MDD_G4_FW800_ROM.hqx` instead**, which is built on a community FW800 ROM and keeps that work. If your machine boots from some other patched ROM again, use the injector on the ROM you already have (see "Patching your own ROM" below); it appends a parcel rather than replacing anything, so it composes.
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
- **A USB volume occasionally missing from Disk First Aid's list at its first launch after boot.** The volume is mounted and healthy; DFA just did not pick it up while building its initial list. Eject and reinsert the drive (it pops into the running DFA immediately) or quit and relaunch DFA. Cosmetic only.
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
- **Writes are slower than reads**, roughly half the read speed on large transfers, and they vary more between runs. Part of that is the flash device itself; part is this driver's staging path. See [Measured performance](#measured-performance).
- **Small transfers are slow, and below about 8 KB USB 1.1 beats this driver.** The transfer engine is timer driven, so each request waits for the next 8 ms tick. This is the biggest known performance limitation and the fix is understood; see [Measured performance](#measured-performance).
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
| `USB2_Mini_G4_ROM.hqx` | driver **h97rel** on the MacOS9Lives mini + VBL-fix base | in the driver log banner | `0f9f53636585499c156d1ac903315aad` |
| `USB2_MDD_G4_ROM.hqx` | driver **h97rel** on the MDD stock base | in the driver log banner | `2eb05b9d2583622ed3e56620cba06f71` |
| `Mini_G4_EHCI_Ext.bin` | extension `Mini G4 EHCI` | **11.2** (Get Info / ASP) | `ff84d8b7cb629f2ea8a9f0bfb71b6b40` |
| `MDD_G4_EHCI_Ext.bin` | extension `MDD G4 EHCI` | **8.3** (Get Info / ASP) | `f345db22764507fb3c0e1e607163d2bf` |
| `USB2_BW_G3_ROM.hqx` | driver **b16rel** (polled mode) on the RETAIL 9.2.2 base | in the driver log banner | `5509afe84cd0b19cdc435624667f4f3c` |
| `BW_G3_EHCI_Ext.bin` | extension `BW G3 EHCI` | **8.3** (Get Info / ASP) | `ee4863df8510d444f20804b72171d2e6` |
| `USB2_MDD_G4_FW800_ROM.hqx` | driver **fw1rel** (polled mode) + the on-board controller enable, on a community FW800 MDD base | in the driver log banner | `afcae96313334fa9bcd2abe9b2d284d0` |

The driver writes `EHCIUIM_init.log` on the boot volume: two lines identifying the build (`=== EHCIUIM BUILD h97rel ===`, or `b16rel` on the B&W, or `fw1rel` on the FW800 MDD) and the logging mode. That is the whole log in a release build; if you file a bug report, include those lines and a description, and a diagnostic build can be provided.

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
