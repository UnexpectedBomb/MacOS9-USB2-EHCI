# Mac OS 9 — USB 2.0 (EHCI) disk stack

A **from-scratch USB 2.0 driver stack for Mac OS 9**, driving a PCI USB 2.0 card that the OS
refuses to touch. It brings up the EHCI host controller, enumerates a high-speed device, and
**mounts a USB flash drive read/write on the OS 9 desktop** — the whole path invented in
userland + a native driver, because classic Mac OS never shipped an EHCI (USB 2.0) driver.

> **Status: working, seeking testers.** On my hardware it reliably mounts an HFS-formatted
> USB 2.0 stick on every boot and reads/writes files through the Finder. It has real
> limitations (throughput, big copies — see below), and it's only been exercised on one
> card + a couple of drives, so broad testing is how we build confidence. The design and the
> hard-won bug hunts are written up in [TECHNICAL.md](TECHNICAL.md).

---

## TL;DR

- Mac OS 9 has **OHCI (USB 1.1)** drivers but **no EHCI (USB 2.0)** driver. Plug a USB 2.0
  PCI card into an OS 9 PowerMac and its high-speed controller sits dead; devices fall back
  to the 1.1 companion at best.
- This project is a complete **EHCI stack built from nothing**: an EHCI **UIM plugin** loaded
  into Apple's USB Manager, a **virtual root hub**, a self-driven **SCSI/Bulk-Only-Transport
  probe**, and a native **block driver** that mounts the volume via OS 9's built-in HFS
  mounter.
- It runs as a small **application** — launch it, and a plugged-in HFS USB drive appears on
  the desktop, high-speed, read/write.

## Hardware

Developed and tested on:

- **Power Mac G4 (Mirrored Drive Doors)** running Mac OS 9.2.2, with
- a **PCI USB 2.0 card** using the **NEC / Renesas µPD720100A** EHCI controller
  (an IOGEAR card; many cheap early USB-2.0 PCI cards use this chip).

Other OS 9 PowerMacs and other EHCI PCI cards *may* work but are untested — reports welcome.

## Use it

1. **Format your USB drive as "Mac OS Extended" (HFS / HFS+)** on a Mac (Drive Setup / Disk
   Utility). This stack mounts Mac volumes through OS 9's built-in mounter; FAT/PC-Exchange is
   deliberately *not* used (see TECHNICAL.md for why).
2. Copy **`dist/USB2-HFS-Mount.bin`** (a MacBinary app) onto the OS 9 machine and decode it
   (Stuffit Expander / any MacBinary tool) so its resource fork is intact.
3. Run the app. It brings up the EHCI card, then:
4. **Plug the USB drive into a card port** a few seconds after it prints `pumping…`. It
   enumerates at high speed and the volume mounts on the desktop, read/write.

The app writes a verbose log next to itself — it's a research/diagnostic build, not a
polished product, so expect chatter.

## What works / what doesn't

**Works**
- Reliable high-speed (480 Mbps link) **enumeration + mount, every boot**.
- **Read and write** to an HFS volume through the Finder (copy files, rename, trash).
- Native HFS mount via OS 9's built-in mounter — no "Audio CD" mis-identification.

**Known limitations (v1)**
- **Throughput ~0.8 MB/s** — functional but USB-1.1-era. The transfer engine is deliberately
  simple (one Bulk-Only-Transport command in flight, 3.5 KB chunks, a bounce-buffer copy on
  the interrupt path). Real 2.0 speed needs an engine rework (see TECHNICAL.md → *Roadmap*).
- **Large Finder copies (hundreds of MB) can hang.** Small/normal file work is fine;
  hammering it with a multi-hundred-MB folder trips a re-entrancy bug in the copy path
  (in-app sequential writes of the same size are fine — it's the Finder's interleaved access).
  Under investigation.
- **No auto-load yet** — it's an app you run, not a resident extension. A faceless
  Startup-Items version is planned.
- Exercised on **one card + a couple of drives**; some devices behave differently during
  the mass-storage probe.

## How it works (short version)

Classic Mac OS's USB Manager can load a third-party **UIM** (USB Interface Module — a
host-controller plugin) for a controller it doesn't natively support, *if* something presents
the controller node as a driver candidate. This stack:

1. Attaches its ndrv to the EHCI card's Name Registry node and calls the USB Expert's
   `LoadUIMForEntry`, so our driver loads as the EHCI UIM.
2. The UIM brings up the controller (PCI enable, register map, reset, run) and presents a
   **virtual root hub** to the USB Manager.
3. Apple's hub driver enumerates the downstream device through our UIM; we then run our own
   **Bulk-Only-Transport SCSI probe** (INQUIRY / READ CAPACITY / READ) over the device's bulk
   endpoints and publish a block-read/write service.
4. A tiny native **block driver** consumes that service, scans the Apple Partition Map (or a
   partitionless HFS volume), `AddDrive`s it, and `PBMountVol`s it — and OS 9's built-in HFS
   mounter takes it from there.

Full detail — including the two multi-day bug hunts (an intermittent-enumeration race in the
EHCI async schedule, and an "Audio CD" mis-mount cured by pivoting to HFS) — is in
[TECHNICAL.md](TECHNICAL.md).

## Building from source

See [BUILD.md](BUILD.md). Built with [Retro68](https://github.com/autc04/Retro68).

## Credits

Thanks to the **MacOS9Lives** community for the OS 9 builds, hardware knowledge, and prior
art that make projects like this possible, and to **Retro68** for a modern toolchain that can
still target classic Mac OS.

## License

[MIT](LICENSE).
