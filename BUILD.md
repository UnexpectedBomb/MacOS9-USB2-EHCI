# Building

Built with [Retro68](https://github.com/autc04/Retro68) (a GCC toolchain targeting classic
Mac OS / PowerPC CFM). Everything here targets **PowerPC** (the ndrv, the block driver, and
the loader app are all PEF/CFM).

## Prerequisites

- A built Retro68 PowerPC toolchain (`powerpc-apple-macos-*`, `MakePEF`, `Rez`, and the
  `retroppc.toolchain.cmake` toolchain file).
- CMake 3.13+ and Python 3.

## Layout

- `src/`, the EHCI UIM ndrv (`ehci_uim.c`, `ehci_hw.c`, `ehci_os.c`, `ehci_xfer.c`,
  `ehci_vhub.c` + headers) and the block driver (`usb_disk.c`). `*.exp` are the CFM export
  lists. `usb2_icns_blob.h` is the "2.0" volume icon (an `'icns'` family) the block driver
  hands the Finder for a mounted 2.0 volume, committed source, not a generated blob.
- `probe/ehci_launcher.c`, the shippable launcher/mounter app (prompt-driven). Built once as a
  single **universal** `EHCILauncher` that runs on every machine: the driver's per-port claim and
  its `sharedCompanion` interrupt discriminator adapt to a PCI card vs. an on-board controller at
  runtime, and the on-screen wording is neutral, so there is no per-machine build variant.
- `probe/ehci_trigger.c`, the diagnostic/development harness (verbose logging).
- `usl_import/`, import-stub libraries for the (never-published) USB Services Library / USB
  Family Expert UIM entry points the app links against.
- `scripts/pef-to-blob.py`, embeds a built PEF into a C byte array.
- `patch-pef-main.py`, sets a native driver's PEF `main` to its `DoDriverIO` export.
- `rom/usb_rom_inject.py`: injects the built `EHCIUIM.pef` into a Mac OS ROM as a
  `driver,AAPL,MacOS,PowerPC` parcel (see "Injecting the driver into the Mac OS ROM" below).

## The staged build (important, the order is not optional)

Two byte-array headers are **generated** from built PEFs rather than checked in, so the targets
have to be built in dependency order:

- `src/usb_disk_blob.h`, the **block driver**, embedded *into the UIM*. The UIM installs it
  itself with `InstallDriverFromMemory`, which is how the OS ends up mounting the volume with no
  application involved. `src/ehci_vhub.c` `#include`s this header, so **the block driver must be
  built and embedded before the UIM will compile at all.**
- `src/ehci_pef_blob.h`, the UIM, embedded into the helper app. Only the older in-app injection
  path and the diagnostic harness use it; the shipping helper takes the driver from the ROM.

```sh
# configure (once)
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE="$HOME/Retro68-build/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake"

# 1. block driver FIRST, then embed it. The UIM includes this header.
cmake --build build --target blockdrv
python3 scripts/pef-to-blob.py build/USBDisk.pef src/usb_disk_blob.h gUsbDiskPef

# 2. now the EHCI UIM ndrv (this is the driver you inject into the ROM), then embed it
cmake --build build --target ndrv
python3 scripts/pef-to-blob.py build/EHCIUIM.pef src/ehci_pef_blob.h gEHCIPef

# 3. build + package the faceless helper app (MacBinary + disk image)
cmake --build build --target EHCIActivate_APPL
# (EHCILauncher_APPL builds the older interactive launcher; EHCITrigger_APPL the diagnostic harness)
```

Building `ndrv` before step 1 fails with `fatal error: usb_disk_blob.h: No such file or directory`.
That is the expected symptom of running these out of order, not a broken checkout.

Verified: a clean clone built with the sequence above produces an `EHCIUIM.pef` byte-identical to
the driver in the shipping ROM.

The result is `build/EHCIActivate.bin` (MacBinary, copy to the OS 9 machine and decode so the
resource fork survives) plus `.APPL`/`.dsk` variants. The helper app is retired in the shipping
release (the activation extension replaced it; see `resident/`), but the target still builds and
remains useful for bring-up experiments on new machines.

The blob header now regenerates inside the `blockdrv` rule itself, so an edit to `usb_disk.c`
propagates into the UIM automatically on the next build.

> **ROM-integration note.** The shipping helper uses the driver **from the ROM**, not the embedded
> blob, so it no longer installs the byte array at runtime. The blob is still built and embedded (it
> is the fallback used by the older in-app injection path and the diagnostic harness), so the
> two-phase order above still applies.

> **Pairing.** The helper reaches into the driver's `'Eusb'` Gestalt service struct, so the ROM and
> the helper are versioned as a pair and must be updated together. If you rebuild one, rebuild both.

> **Version-stamp your ROMs.** `python3 rom/wrap_macbinary.py <raw-rom> <version> <out.bin>` wraps a
> built ROM as MacBinary with a `vers` resource, so Get Info on the installed `Mac OS ROM` shows
> which build it is. Worth doing: it is the one-second check that you are running the ROM you think
> you are.

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
  fail to compile. (`usb2_icns_blob.h`, the icon, is committed, it is not generated.)
- Retro68 headers are Latin-1; `grep` them with `LC_ALL=C grep -a`.
- Only the `ndrv`, `blockdrv`, `EHCILauncher` (the universal launcher), and `EHCITrigger` targets
  are included here. The dev tree had additional diagnostic probe apps; those were left out of
  this repo.
- `resident/` holds the source of the **activation extension** that ships with the release
  (`bootmain.c` builds against `bootmain_dm.exp` as a PowerPC fragment; `ehci_init_dbg.c`/`.r` is
  the 68K INIT wrapper that loads it at boot, built with the Retro68 m68k toolchain). The shipped
  extensions are packaged from these with `scripts/package-init.py` (name, version resource, icon).
  The prebuilt, hardware-validated extensions are attached to the release; building your own
  requires both Retro68 toolchains and is documented mainly for review.
