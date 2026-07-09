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
  lists.
- `probe/ehci_trigger.c` — the loader/mounter application.
- `usl_import/` — import-stub libraries for the (never-published) USB Services Library / USB
  Family Expert UIM entry points the app links against.
- `scripts/pef-to-blob.py` — embeds a built PEF into a C byte array.
- `patch-pef-main.py` — sets a native driver's PEF `main` to its `DoDriverIO` export.

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

# 3. build + package the loader app (MacBinary + disk image)
cmake --build build --target EHCITrigger
cmake --build build --target EHCITrigger_APPL
```

The result is `build/EHCITrigger.bin` (MacBinary — copy to the OS 9 machine and decode so the
resource fork survives) plus `.APPL`/`.dsk` variants.

If you edit a driver source, re-run its build + `pef-to-blob` step and then rebuild the app —
otherwise the app keeps embedding the old driver.

## Notes

- The blob headers are `.gitignore`d (generated). A fresh clone must run the steps above in
  order; building `EHCITrigger` before the blobs exist will fail to compile.
- Retro68 headers are Latin-1; `grep` them with `LC_ALL=C grep -a`.
- Only the `ndrv`, `blockdrv`, and `EHCITrigger` targets are included here. The dev tree had
  additional diagnostic probe apps and a resident-INIT vehicle; those were left out of this
  repo.
