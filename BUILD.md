# Building

Built with [Retro68](https://github.com/autc04/Retro68) (a GCC toolchain targeting classic
Mac OS / PowerPC CFM). Everything here targets **PowerPC** (the ndrv, the block driver, and
the loader app are all PEF/CFM).

## Prerequisites

- A built Retro68 PowerPC toolchain (`powerpc-apple-macos-*`, `MakePEF`, `Rez`, and the
  `retroppc.toolchain.cmake` toolchain file).
- CMake 3.13+ and Python 3.

## Layout

- `src/` — the EHCI UIM ndrv (`ehci_uim.c`, `ehci_hw.c`, `ehci_os.c`, `ehci_xfer.c`,
  `ehci_vhub.c` + headers) and the block driver (`usb_disk.c`). `*.exp` are the CFM export
  lists. `usb2_icns_blob.h` is the "2.0" volume icon (an `'icns'` family) the block driver
  hands the Finder for a mounted 2.0 volume — committed source, not a generated blob.
- `probe/ehci_launcher.c` — the shippable launcher/mounter app (prompt-driven). Built once as a
  single **universal** `EHCILauncher` that runs on every machine: the driver's per-port claim and
  its `sharedCompanion` interrupt discriminator adapt to a PCI card vs. an on-board controller at
  runtime, and the on-screen wording is neutral — so there is no per-machine build variant.
- `probe/ehci_trigger.c` — the diagnostic/development harness (verbose logging).
- `usl_import/` — import-stub libraries for the (never-published) USB Services Library / USB
  Family Expert UIM entry points the app links against.
- `scripts/pef-to-blob.py` — embeds a built PEF into a C byte array.
- `patch-pef-main.py` — sets a native driver's PEF `main` to its `DoDriverIO` export.
- `rom/usb_rom_inject.py`: injects the built `EHCIUIM.pef` into a Mac OS ROM as a
  `driver,AAPL,MacOS,PowerPC` parcel (see "Injecting the driver into the Mac OS ROM" below).

## The two-phase build (important)

The loader app **embeds** the two drivers (the UIM ndrv and the block driver) as byte arrays
so it can install them at runtime — no Extensions-folder install needed. Those byte arrays
(`src/ehci_pef_blob.h`, `src/usb_disk_blob.h`) are **generated** from the built PEFs, so you
must build the drivers and regenerate the blobs *before* building the app.

```sh
# configure (once)
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE="$HOME/Retro68-build/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake"

# 1. build the EHCI UIM ndrv, then embed it
cmake --build build --target ndrv
python3 scripts/pef-to-blob.py build/EHCIUIM.pef src/ehci_pef_blob.h gEHCIPef

# 2. build the block driver, then embed it
cmake --build build --target blockdrv
python3 scripts/pef-to-blob.py build/USBDisk.pef src/usb_disk_blob.h gUsbDiskPef

# 3. build + package the universal launcher app (MacBinary + disk image)
cmake --build build --target EHCILauncher_APPL
# (the EHCITrigger / EHCITrigger_APPL targets build the diagnostic harness the same way)
```

The result is `build/EHCILauncher.bin` (MacBinary — copy to the OS 9 machine and decode so the
resource fork survives) plus `.APPL`/`.dsk` variants. A prebuilt copy is in `dist/USB2-Launcher.bin`
(the one universal launcher, for both PCI-card and on-board machines).

If you edit a driver source, re-run its build + `pef-to-blob` step and then rebuild the app —
otherwise the app keeps embedding the old driver.

> **ROM-integration note.** The shipping helper (`EHCILauncher` built with `PATHA_ROM_DRIVER`) uses
> the driver **from the ROM**, not the embedded blob, so it no longer installs the byte array at
> runtime. The blob is still built and embedded (it is the fallback used by the older in-app
> injection path and the diagnostic harness), so the two-phase order above still applies.

## Injecting the driver into the Mac OS ROM

This release loads the driver from the **Mac OS ROM** rather than installing it from the app. After
building `EHCIUIM.pef` (step 1 above), inject it into a Mac OS ROM file:

```sh
python3 rom/usb_rom_inject.py "Mac OS ROM" -o "Mac OS ROM (USB2)"
```

The injector adds a `driver,AAPL,MacOS,PowerPC` parcel carrying the PEF, plus a matching driver
descriptor, bound to the EHCI controller node (`pciclass,0c0320`), so the OS loads and prepares the
driver at boot. It builds on **Elliot Nunn's Mac OS ROM toolchain** (`tbxi` and friends), which must
be available; the paths it expects are documented at the top of `rom/usb_rom_inject.py`.

Two driver details make the ROM load safe and useful:

- The driver's runtime flags are **`0x05`** (`kDriverIsLoadedUponDiscovery` + `kDriverIsUnderExpertControl`),
  so the OS prepares it at boot but leaves opening it to the USB Expert path (`LoadUIMForEntry`, which
  the helper calls) rather than opening it immediately.
- `DoDriverIO`'s `kInitialize` is **stash-only**: it records the controller node and returns, with no
  register mapping and no File Manager calls, because at the early PCI-claim phase the machine cannot
  tolerate bring-up work. The real controller bring-up is deferred to `kOpen`.

Put the patched ROM in place of the `Mac OS ROM` in the System Folder, and keep the original to revert.

## Notes

- The PEF blob headers (`ehci_pef_blob.h`, `usb_disk_blob.h`) are `.gitignore`d (generated). A
  fresh clone must run the steps above in order; building the app before the blobs exist will
  fail to compile. (`usb2_icns_blob.h`, the icon, is committed — it is not generated.)
- Retro68 headers are Latin-1; `grep` them with `LC_ALL=C grep -a`.
- Only the `ndrv`, `blockdrv`, `EHCILauncher` (the universal launcher), and `EHCITrigger` targets
  are included here. The dev tree had additional diagnostic probe apps and a resident-INIT vehicle;
  those were left out of this repo.
