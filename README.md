# USB 2.0 for Mac OS 9: ROM-Integrated Beta

**The first USB 2.0 (Hi-Speed) mass-storage driver for classic Mac OS 9.** Mac OS 9 shipped with USB 1.1 only (a 12 Mbit/s ceiling, roughly 1 MB/s in practice). This is a from-scratch EHCI (USB 2.0) stack that mounts a USB flash drive and reads and writes it at **~20 MB/s read / ~13 MB/s write** on real hardware, roughly 10 to 20 times faster than anything OS 9 could do before.

> ## ⚠️ ROM downloads are temporarily withdrawn
>
> **The corrected ROMs published on 2026-08-07 did not boot, and have been pulled.** If you flashed one and
> your machine will not start up, the fix is in the
> [withdrawn release's notes](https://github.com/UnexpectedBomb/MacOS9-USB2-EHCI/releases/tag/hqx-reissue):
> boot from a CD or another volume and put your original `Mac OS ROM` back. That was our mistake, not
> anything you did.
>
> **The cause:** those ROMs carried a `vers` resource so Get Info would name the build. A Mac OS ROM's
> resource fork is the System Enabler's, and a `vers` (1) there describes the *enabler*, so the System saw a
> mis-versioned enabler and left parts of itself uninstalled. Nothing else about the ROM was wrong, and an
> otherwise identical ROM without that resource boots normally. It was published without being booted first,
> which is the actual error.
>
> **Meanwhile:** the ROM in the [`m3` release](https://github.com/UnexpectedBomb/MacOS9-USB2-EHCI/releases/tag/m3)
> boots. It has a separate, older packaging defect (a missing `SysEnabler`, described in its notes) that can
> cause an intermittent frozen cursor at startup, but it starts up reliably and is a safe place to sit.
>
> Corrected ROMs are built and verified at the desk and will be republished **after** they have started a
> real machine. You can also build your own now with `scripts/build-rom-hqx.py`, which refuses to produce a
> ROM containing the resource that caused this.

## What's new: USB 2.0 on the Mac mini G4

**A second kind of machine now works.** Until now this stack ran on one machine, a Power Mac G4 MDD with a
PCI USB 2.0 card. The **Mac mini G4** has now been validated on its **on-board** USB 2.0 controller, which
Mac OS 9 has only ever driven at 1.1: a drive mounts at Hi-Speed in a rear port, copies files both
directions, ejects to the Trash, and mounts again **behind an Apple Cinema Display's built-in hub**. The
keyboard and mouse keep working throughout, on the same controller.

Two things had to be fixed to get there, and both are described in [TECHNICAL.md](TECHNICAL.md): the ROM
parcel was not binding to the mini's controller at all (its Name Registry node advertises itself
differently from a PCI card's, so the parcel's match had to be widened), and once it did bind, a step that
must run at task level was being abandoned on a **wall clock** while task level happened to be starved,
which threw away a drive that had in fact enumerated perfectly.

**Mac mini owners: there is a prebuilt mini ROM, and it carries an experimental display fix as well as this
driver. Please read [the note about it](#a-note-for-mac-mini-g4-owners-the-vbl-display-fix) before flashing.**

## The driver lives in the Mac OS ROM

Earlier releases shipped the driver *inside an app* that installed it at runtime. This release takes the step the project was built toward: the EHCI UIM is injected into the **Mac OS ROM** itself, as a `driver,AAPL,MacOS,PowerPC` parcel bound to the USB 2.0 controller's Name Registry node (built with Elliot Nunn's Mac OS ROM toolchain). Mac OS 9 now **loads and binds our driver at boot, as a genuine OS-owned host-controller driver**, the same way it loads a native Apple one. That is the architectural milestone: on a machine with the patched ROM, USB 2.0 support is part of the operating system, not bolted on by a helper app.

**The operating system now performs the mount itself.** The driver enumerates the device, runs the SCSI Bulk-Only probe, and **installs its own native block driver**, which calls `AddDrive` and posts a disk-inserted event, so Mac OS 9 mounts the volume through its ordinary path, exactly as it would for a built-in controller. Nothing in user space calls `PBMountVol` any more. Apple's mass-storage mounter is not involved at all (it cannot be: it stops after the first `TEST UNIT READY` / `REQUEST SENSE` and never issues `INQUIRY` to a Hi-Speed device), and neither is Apple's USB stack, whose view of our ports we suppress.

**Hot-plug works.** Insert, eject, unplug, re-insert, or swap in a completely different drive, every arrival is re-scanned and re-mounted, on the same drive number, with no leak. Pulling a drive without ejecting unmounts it cleanly instead of leaving a "damaged disk" behind, and both ejection and improper removal produce the same alerts, word for word, that Apple's own USB stack shows.

**Honest scope: a small faceless helper is still required.** It sits silently in Startup Items and does two things: activates the ROM driver (`LoadUIMForEntry`) and pumps the polled service slot that device *discovery* runs on. That slot is only ever driven by Apple's USB Services Library from an application context, which is why the last helper process cannot be removed yet. It is **not** in the data path, once a volume is mounted, all I/O runs at interrupt level. In practice you never interact with it: it starts at boot, shows nothing, and drives are plug-and-play from then on.

The driver serves two kinds of machine, at two different maturity levels:

- **PCI USB 2.0 cards** (a card in a PCI slot, e.g. the **Power Mac G4 "Mirrored Drive Doors"**): **the supported path.** The EHCI card has its own dedicated interrupt line, the driver owns it cleanly, and mounts are reliable.
- **On-board USB 2.0** (built-in ports that are USB 2.0-capable but which OS 9 only ever drove at 1.1, e.g. the **Mac Mini G4**): **now working, newly so.** The **Mac mini G4** has been validated end to end: a drive mounts at Hi-Speed in a rear port *and* behind an Apple Cinema Display's built-in hub, with copies both directions, Trash, and hot-plug on every port. This replaces an earlier, much more pessimistic assessment on this page: the previous "may lock up mid-mount" warning came from the older app-loaded driver, and the ROM-integrated build does not behave that way. It has had **one full validation session**, not months of use, so it is newer and less proven than the PCI card path.

> ⚠️ **This is a beta / technology preview.** On a PCI card it works, it is stable, and it moves real files fast. The Mac mini G4 now works too, on a newer and less-proven footing. But mounting a Hi-Speed drive still goes through the headless helper and a specific **insertion sequence** (not plug-and-play), and it has real limitations. It is shared in this state so the community can use it *and* help finish it. **Read the steps below carefully: the timing of when you insert the drive matters.**

---

## What it does

- Mounts USB 2.0 mass-storage devices (flash drives / SSDs) on the OS 9 desktop at Hi-Speed.
- **Up to four drives at once, in any combination of ports.** Each gets its own volume, icon and geometry.
  Files copy directly between them, they can be ejected individually or all together in a single drag to the
  Trash, and each hot-plugs independently. Validated on hardware with four drives across a PCI card and an
  external hub at the same time.
- **Drives behind an external USB 2.0 hub run at Hi-Speed.** The hub is enumerated and driven by this stack
  (we power, reset and address its downstream ports ourselves), so a drive plugged into a hub, including the
  hub built into an Apple Cinema Display, mounts at 2.0 instead of falling back to 1.1. Keyboards and mice
  behind a hub are a separate problem and still do not work: see "Known limitations".
- Reads and writes at the device's real speed, benchmarked at **20 MB/s read, ~13 MB/s write** (both are the flash device's own ceiling; the driver reaches it). Real Finder copies land lower (~8 read / ~5 write) because the Finder's own I/O sizing is the bottleneck above the driver, not the driver itself.
- On an **on-board** machine (e.g. Mac Mini G4), the driver hands the keyboard/mouse ports back to the built-in 1.1 controller and claims only a free port for the drive, so the keyboard and mouse keep working while the drive runs at Hi-Speed. Validated on a Mac mini G4, including a drive behind the Cinema Display's hub.
- **Ejects** cleanly (Finder menu or drag-to-Trash), like any removable disk.
- **No Extension to install.** The driver rides *inside the Mac OS ROM* (a one-time ROM patch, see "Install & use"). Nothing goes into your Extensions folder. The driver loads at boot as an OS component; a small headless helper then performs the Hi-Speed mount.
- **Tells you it's really 2.0**, a volume mounted through this driver appears on the desktop with a distinct **"2.0" drive icon**, so a Hi-Speed mount is obvious at a glance versus a plain USB 1.1 one.
- Reliable transfers: byte-verified, with a transfer watchdog and a CSW residue/signature check on every command, and **correct block addressing across the entire volume**, so large files and volumes well past 2 GB / 4 GB read and write without corruption.

## Supported machines

There is now **one universal launcher**, the same app and driver on every machine. It detects your
hardware and adapts automatically (claims all ports on a dedicated card; hands the keyboard/mouse ports
back and claims only a free one on an on-board controller). You do not pick a build.

### PCI USB 2.0 card, *supported*

- A Power Mac running Mac OS 9 with a free PCI slot and a **USB 2.0 host card** (EHCI, class `0x0C0320`).
- **Tested: Power Mac G4 "Mirrored Drive Doors" + NEC-chipset IOGEAR card.** Most generic USB 2.0 PCI cards of the era use similar NEC/VIA EHCI silicon.
- The card has a **dedicated interrupt line**, so the driver owns it cleanly and mounts are reliable. This is the recommended path.

### On-board USB 2.0, *working, newly so*

- A Power Mac whose **built-in** USB is 2.0 (an EHCI controller, PCI class `0x0C0320`) and that runs Mac OS 9, including later models that boot OS 9 via the community's OS 9 patches.
- **Validated on the Mac Mini G4** (on-board NEC EHCI, `1033:00e0`). In one full session: a drive mounted at Hi-Speed in a rear port, copied files both directions, went to the Trash and was emptied, was unplugged, and then mounted again **behind an Apple Cinema Display's built-in hub** with the same set of operations repeated. No lock-ups.
- ⚠️ **Newer and less proven than the card path.** One validation session, one machine, one drive. The PCI card path has months of use behind it; this does not. Please report what happens on yours.
- The earlier version of this page said on-board machines "may lock up mid-mount". That warning came from the older **app-loaded** driver. The ROM-integrated build did not do it, so the warning has been withdrawn rather than left standing as a scare.
- ⚠️ **If your mini is a Mac mini G4, read [the Mac mini ROM note](#a-note-for-mac-mini-g4-owners-the-vbl-display-fix) below before you install a ROM.** It concerns a *separate* display bug, not USB.

### Both

- A USB 2.0 mass-storage device formatted **HFS, Mac OS Standard or Mac OS Extended** (either an Apple Partition Map with an `Apple_HFS` partition, or a single partitionless HFS volume). *Not FAT.* (Tested with a SanDisk Ultra USB flash drive, Mac OS Extended, on volumes up to 62 GB.)

*Only a couple of hardware combinations have been tested so far. If it works, or doesn't, on your machine / card / drive, please open an issue; that data directly helps.*

## Install & use

A **one-time setup**: patch the driver into your Mac OS ROM, and drop the faceless helper into Startup Items. After that, drives are plug-and-play.

### One-time setup

1. **Get a patched Mac OS ROM.** Two ways, depending on your machine. Either way, first copy your
   machine's `Mac OS ROM` (it lives in the System Folder) somewhere safe, you will need it to revert.

   **Power Mac G4 MDD that boots OS 9 on its stock ROM, use the prebuilt MDD ROM.** The release page carries
   the exact ROM that has been hardware-tested, so this needs no toolchain at all. It is built from an MDD's
   own `Mac OS ROM`. It arrives as **BinHex (`.hqx`)**: decode it with **StuffIt Expander** on the Mac, which
   preserves the ROM's resource fork. Do not convert it on a PC or through a tool that drops forks.

   > ⚠ **Not if your machine only boots OS 9 thanks to a community ROM patch**, an FW800 MDD, an aluminium
   > PowerBook and similar. Your `Mac OS ROM` has already been modified to make OS 9 boot at all, and the
   > prebuilt MDD file does not contain that work, so installing it would remove the thing making your
   > machine boot. Use the injector on your own already-patched ROM instead: it appends a parcel rather
   > than replacing anything, so it composes. Boot patch first, then this.

   **Mac mini G4, use the prebuilt mini ROM.** A separate, mini-specific ROM is on the release page. It is
   built on the **MacOS9Lives mini ROM**, so it keeps the patches that make a mini boot OS 9 at all, and it
   is the exact file that was hardware-validated above, also as **BinHex (`.hqx`)**, decoded with StuffIt
   Expander. **It also contains a display fix that is experimental and needs a companion app: read
   [A note for Mac mini G4 owners](#a-note-for-mac-mini-g4-owners-the-vbl-display-fix) before you install
   it.**

   **Any other NewWorld Mac, patch your own.** A complete ROM is machine-specific, and neither prebuilt ROM
   contains the drivers a different model needs. Run the injector against your own copy, which binds
   the driver to the USB 2.0 controller's Name Registry node:
   ```sh
   python3 scripts/build-rom-hqx.py "Mac OS ROM" mybuild "Mac OS ROM (USB2).hqx"
   ```
   That injects the driver, stamps the build name so **Get Info shows which ROM you are running**, and emits
   BinHex, which keeps your ROM's resource fork. It refuses to write a ROM whose `SysEnabler` came out empty,
   which is the mistake described in the notice at the top of this page.

   ⚠ Your input needs its resource fork intact. Getting a `Mac OS ROM` off an OS 9 machine over anything that
   does not understand forks will silently strip it; StuffIt/BinHex on the Mac, or `unar` on the desktop, will
   preserve it. `rom/usb_rom_inject.py` is still there if you want the raw injection step on its own.

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

That is the whole flow. You do not need to quit anything, and a **Special ▸ Restart** with the driver active is safe, the driver quiesces the controller through a Shutdown Manager hook before the machine goes down.

If you do quit the helper (Cmd-Q) with a volume still mounted, it unmounts cleanly first and hands the ports back to the built-in 1.1 controller, so ordinary USB 1.1 works again immediately with no reboot.

If a drive does not appear, two logs are written for troubleshooting and bug reports: the helper's own step-by-step log next to the application, and **`EHCIUIM_init.log`** on the boot volume (the driver's detailed trace). The driver *appends* to that log across loads, so delete it first and read from the last banner.

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

This is a beta. These are the things it does **not** do yet:

- **On-board USB 2.0 is validated but new.** On a PCI card mounting is reliable and has been for months. On an on-board controller (Mac Mini G4) it now mounts reliably too, on a root port and behind a hub, but that rests on **one validation session on one machine**. One thing seen there and not yet explained: on an earlier build, the machine paused for about ten seconds right around a drive being inserted, and then carried on. The driver now rides that out instead of giving up on the drive, but the pause itself is not understood, and the on-board EHCI's shared interrupt line is the leading suspect. Please report it if you see it.
- **A drive attached at boot mounts at 1.1, not 2.0.** Ports that are already occupied when the controller is brought up are handed to the 1.1 companion. Boot with the drive unplugged, then insert it.
- **Four drives is the ceiling.** The per-device DMA structures live in one wired memory page, which holds four
  devices plus the hub's own bookkeeping. A fifth drive is refused cleanly (it simply does not mount, nothing
  else is disturbed) and the driver posts a notification saying so. Raising the limit needs a second DMA page.
- **Keyboards and mice behind a hub do not work.** A drive behind a USB 2.0 hub runs at Hi-Speed, but a
  full-speed or low-speed device behind that same hub (a keyboard, a mouse, or an Apple Cinema Display's own
  brightness buttons) is detected, left powered and skipped. It cannot be handed to the 1.1 companion either,
  because a device behind a Hi-Speed hub is not electrically on the companion's bus at all: it sits behind the
  hub's transaction translator. Reaching it requires EHCI **split transactions**, which are not implemented.
  See "The open problems". **If you need a keyboard and mouse on a hub, put that hub on a USB 1.1 port.**
- **One hub at a time, and no hubs behind hubs.** A second hub, or a hub plugged into our hub, is not driven.
- **A port handed to the 1.1 companion stays there until reboot.** Once ownership is released, EHCI can no
  longer tell "empty" from "companion-owned" on that port, so the driver deliberately never takes it back;
  reclaiming it would risk stuttering a keyboard that is working. Use a different port for a Hi-Speed drive, or
  reboot. (A port the driver merely *gave up on* is different, and does recover: unplug that device and the
  port is usable again without rebooting.)
- **A drive inserted while a large file copy is running will not mount.** Wait for the copy to finish and plug
  it in again, or plug it in before starting the copy. The cause is understood (the copy's transfers keep the
  engine busy so the new drive's setup never gets issued) and it is a known, deliberately deferred gap.
- **Writes are slower than reads.** Reads reach the device's ceiling (~20 MB/s); Finder writes land around 2 to 3 MB/s because the Finder issues small synchronous writes above the driver.
- **Mid-write yank is unsafe** (as on any OS), always eject first.
- **A few tested hardware combinations** (see Supported machines). Other EHCI cards/controllers are untested.
- The driver writes a verbose `EHCIUIM_init.log` each run. That's intentional for a beta (it's what to attach to a bug report) and does not slow down file transfers.

If you need rock-solid *removable* USB on OS 9 today, the built-in USB 1.1 support is still there and untouched. This driver is about **speed**, and about proving Hi-Speed USB on OS 9 is possible at all.

## The open problems, help wanted

On a **dedicated PCI card** the manual flow above is rock-solid. These are the problems that remain, and they are good targets for anyone who enjoys low-level driver archaeology.

### 1. The on-board shared interrupt line: a stall nobody has explained

**This one has changed, and honestly reporting how it changed is more useful than the old text was.** It used to read: on-board machines intermittently *lock up* mid-mount, and that was the on-board blocker. On the ROM-integrated build the Mac mini G4 mounts reliably, so the lock-up as described is no longer what happens, and the entry has been rewritten rather than left to frighten people off.

What is left is smaller but real, and unexplained. On a machine like the Mac mini G4 the EHCI shares **one PCI interrupt line** with the OHCI companion controllers that drive the keyboard and mouse, and our handler chains to the companion's so input keeps working. On one build, inserting a drive was followed by roughly **ten seconds in which nothing running at task level got a turn** (the machine visibly paused, then recovered). Interrupt-level work carried on normally throughout; only task level was starved. That was enough to make the driver give up on the drive, because a step that has to run at task level was being timed against a wall clock. That step now waits for the task-level pump to actually run instead, so the stall costs a moment of latency rather than the device, and the mini mounts.

But **the stall itself is still not understood**, and a workaround that survives a fault is not a fix for it. The shared interrupt line is the leading suspect, partly because a dedicated card line has never done this, and partly because this machine already needs one shared-line-specific workaround elsewhere in the driver (a completion callback that deadlocks at interrupt level there and has to be deferred). The awkward part is instrumentation: the driver's tracing writes through the File Manager, which is itself task-level, so a task-level stall silences the very log that would explain it. **If you know how to catch a task-level starvation on classic Mac OS without depending on task level to record it, that is the missing tool.**

### 2. A drive attached when the controller is brought up

**Hot re-insertion is solved**, this was previously listed here as blocked on Apple's `ExpertIdleTask` monopolizing the USB Expert's task-level idle loop. The fix was to stop depending on Apple's stack at all: the driver now does its own port reset, speed detection, `SET_ADDRESS`, descriptor reads, `SET_CONFIGURATION`, endpoint registration and Bulk-Only transport, then installs its own block driver and lets the OS mount the result. Apple's idle-loop monopoly became irrelevant rather than worked around, and eject / unplug / re-insert / drive-swap all work.

What remains is narrower: **a drive already attached when the EHCI controller is brought up** is handed to the 1.1 companion by the bring-up path, so it mounts at 1.1. The port-ownership handoff is one-way by design (see the next problem), so it stays there. Boot with the drive unplugged and insert it afterwards.

### 3. Full-speed and low-speed devices behind a Hi-Speed hub

**The first half of this is now done.** Hubs are enumerated and driven by this stack, and a USB 2.0 *drive*
behind a hub mounts at Hi-Speed. What remains is the harder half.

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

There is also a wrinkle worth knowing if you touch this area. Releasing a port to the companion (`PORTSC` Port Owner = 1) only works from the state the EHCI spec intends: a port that is **not enabled**, i.e. one where a reset did not bring up a high-speed device. Handing over a port that *is* enabled at Hi-Speed does not take effect. The driver now clears Port Enable first, and records the handoff in software rather than re-reading the ownership bit back out of the hardware, trusting that register as the record of a decision it had already made produced a livelock, with the port bouncing between the two controllers so fast that neither could enumerate anything.

Issues, pointers, and patches are all welcome.

## How it works (for the curious)

There was no USB 2.0 anything for OS 9, so this is built from the ground up:

- A hand-written **EHCI host-controller driver (UIM)**, a native PowerPC `ndrv`, implementing the queue-head / qTD schedule, per-endpoint hardware toggles, and interrupt + heartbeat-timer servicing.
- A **virtual root hub** synthesized in software, so Apple's USB Expert enumerates the bus the normal way even though an EHCI controller presents its ports differently.
- A **self-driven SCSI probe** (INQUIRY / READ CAPACITY / READ) over Bulk-Only Transport, the stock mounter won't advance a Hi-Speed device, so the driver reads the disk itself and publishes a block service.
- A small **block driver** that mounts the volume through that service.
- The driver is delivered as a **Mac OS ROM parcel**: a `driver,AAPL,MacOS,PowerPC` property (our ndrv's PEF) and its descriptor are injected into the ROM and bound to the controller's Name Registry node, so OS 9 loads and prepares it **at boot**, as an OS-owned driver, without any Extension or declaration ROM. The headless helper then calls `LoadUIMForEntry` to bring it up as the live UIM, runs the self-probe, and mounts the volume. On quit, the helper hands the ports back to the 1.1 companion (clearing the EHCI `CONFIGFLAG`), so USB 1.1 works again without a reboot. *(An earlier release created those same properties from the app at runtime instead of from the ROM; the ROM parcel is the same binding, now applied by the OS at boot.)*

**Coexisting with the keyboard/mouse on an on-board controller.** On a machine like the Mac Mini G4 the EHCI shares one physical set of ports (and one PCI interrupt line) with the OHCI "companion" controllers that drive the keyboard and mouse. Rather than seize the whole controller, the driver does a **per-port claim**: it powers the ports, waits out the USB connect debounce, then hands every port that already holds a device back to the 1.1 companion (setting its Port Owner bit) and claims only the empty ports for EHCI, so the drive comes up at Hi-Speed while input stays on the companion. Two hardware details made this finicky: the controller has *Port Power Control*, so a port's connect status is invalid until it's powered (you must power first, then read); and because the interrupt line is shared, the driver's interrupt handler **chains** to the companion's handler so the keyboard/mouse keep being serviced.

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

Early beta, provided as-is, with no warranty. On a PCI card it now runs reliably, full-speed reads and writes, large folder copies, and volumes well past 4 GB, with correct addressing across the whole disk. On a Mac mini G4's on-board controller it now works too, on the strength of one validation session rather than months of use. Either way it is new low-level code touching your disks, so **keep backups.**

*If you are installing the prebuilt Mac mini ROM,* note again that it bundles an **experimental** fix for the mini's OS 9 startup freeze which has been observed **not** to prevent that freeze reliably, and that the companion `VBLFix` app should be treated as required. The details are [in the install section](#a-note-for-mac-mini-g4-owners-the-vbl-display-fix).

*Fixed in this build:* a data-corruption bug where writes past the 2 GB / 4 GB volume-offset boundary went to the wrong blocks, silently damaging large files, and sometimes the volume's own structures (boot blocks / catalog). If you ran an earlier release against a volume larger than 2 GB, re-verify or reformat that volume. Bug reports and hardware-compatibility reports are very welcome.
