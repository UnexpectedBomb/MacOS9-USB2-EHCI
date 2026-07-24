#!/usr/bin/env python3
# usb_rom_inject.py — inject the EHCI UIM/driver NDRV into an OS 9 "Mac OS ROM" as a
# CLAIMED device, so OS 9 loads and owns the USB 2.0 (EHCI) controller at boot, with no
# Extension and no app-side install. This is the ROM-integration path: the OS binds the
# driver at boot; a small helper then activates it (LoadUIMForEntry) and performs the mount.
#
# It appends a Parcelfile entry that binds the NDRV to the EHCI node via the "claim" marker:
#     prop flags=0x00004 a=pciclass,0c0320 b=usb
#         ndrv flags=0x00006 name=driver,AAPL,MacOS,PowerPC src=EHCIUIM.pef
# `driver,AAPL,MacOS,PowerPC` is the property that brings a device into the Mac OS Name
# Registry as a claimed, expert-managed device (Apple's PCI / Name Registry model).
#
# Requires Elliot Nunn's Mac OS ROM toolchain: the `tbxi-patches` "patch_common" helper
# must be importable. Point TBXI_PATCHES (below, or via the env var) at that directory.
#
# Usage:
#     python3 rom/usb_rom_inject.py "Mac OS ROM" -o "Mac OS ROM (USB2)"
#   The source may be a "Mac OS ROM" file or an already-expanded dump directory; the output
#   is a ROM file if you pass a file, or a dump directory if you pass one.
#
# The driver PEF injected defaults to the prebuilt dist/EHCIUIM.pef; set $EHCI_PEF to point
# at a freshly built build/EHCIUIM.pef instead.

import sys, os, shutil, fnmatch
from os import path

# Elliot Nunn's tbxi-patches directory (contains patch_common). Override with $TBXI_PATCHES.
TBXI_PATCHES = os.environ.get('TBXI_PATCHES', os.path.expanduser('~/rom-tools/tbxi-patches'))
if not path.isdir(TBXI_PATCHES):
    raise SystemExit(
        "tbxi-patches not found at %r.\n"
        "Set $TBXI_PATCHES to Elliot Nunn's Mac OS ROM toolchain 'tbxi-patches' directory."
        % TBXI_PATCHES)
sys.path.insert(0, TBXI_PATCHES)
import patch_common

# ---- our EHCI driver core (the exact NDRV the ROM boot-loads) ----------------
# The PEF must be built for the ROM path: self-probe ON, a boot-safe DoDriverIO
# (kInitialize stash-only, bring-up deferred to kOpen), runtime flags 0x05, and
# main == DoDriverIO patched in (patch-pef-main.py) so the Device Manager loads it.
# See BUILD.md. Defaults to the shipped prebuilt; override with $EHCI_PEF.
_HERE    = path.dirname(path.abspath(__file__))
OUR_PEF  = os.environ.get('EHCI_PEF', path.join(_HERE, '..', 'dist', 'EHCIUIM.pef'))
PEF_NAME = 'EHCIUIM.pef'          # referenced uncompressed, like the stock controller ndrvs

# ---- device-node MATCH --------------------------------------------------------
# A node-probe of the target EHCI card (a NEC uPD720xx) showed:
#     name        = "pci1735,e0"
#     compatible  = "pci1735,e0" / "pci1033,e0" / "pciclass,0c0320"
#     device_type = ABSENT               <-- the decisive finding
#     driver,AAPL,MacOS,PowerPC = ABSENT (node unclaimed, clean)
# Because device_type is NOT "usb", (a) the boot USB Expert (which only scans device_type=usb
# nodes) will not claim this node, so the Device Manager loads us via DoDriverIO (which our
# boot-safe DoDriverIO + runtime 0x05 are built for); and (b) we cannot match on device_type
# (flags 0x08 would AND-fail). So match on the `compatible` list containing pciclass,0c0320 via
# flags 0x00004 alone, which is card-agnostic (any EHCI controller). tbxi legend
# (parcels_dump.py): 0x08 device_type==b, 0x04 compatible contains a, 0x02 parent name==a,
# 0x01 name==a. With 0x08 off, the match is the (0x04|0x02|0x01) group => "compatible contains
# pciclass,0c0320".
# FALLBACK, if a boot-claim test shows no bind: flags 0x00001 a=<exact OF node name>, or
# 0x00005 a=<name> (compatible OR name). b is unused without 0x08.
MATCHES     = ['pciclass,0c0320']
DEVICE_TYPE = 'usb'       # b field — UNUSED here (0x08 not set); kept for line format
PROP_FLAGS  = '0x00004'   # compatible contains a (device_type absent -> no 0x08)
NDRV_FLAGS  = '0x00006'   # mirrors the stock controller ndrv flags


def parcel_lines():
    out = []
    dedup = ' deduplicate=1' if len(MATCHES) > 1 else ''
    for m in MATCHES:
        out.append('prop flags=%s a=%s b=%s\n' % (PROP_FLAGS, m, DEVICE_TYPE))
        out.append('\tndrv flags=%s name=driver,AAPL,MacOS,PowerPC src=%s%s\n\n'
                   % (NDRV_FLAGS, PEF_NAME, dedup))
    return out


src, cleanup = patch_common.get_src(
    desc='Inject the EHCI UIM NDRV into the OS 9 Mac OS ROM as a claimed USB 2.0 controller.')

if not path.exists(OUR_PEF):
    raise SystemExit('driver PEF not found: %s  (build EHCIUIM.pef first, or set $EHCI_PEF)' % OUR_PEF)

injected = False
for (parent, folders, files) in os.walk(src):
    folders.sort(); files.sort()
    if 'Parcelfile' not in files:
        continue
    if any(fnmatch.fnmatch(fn, 'EHCIUIM*.pef') for fn in os.listdir(parent)):
        print('EHCIUIM PEF already present in %s — skipping (idempotent)' % parent)
        injected = True
        continue
    shutil.copy(OUR_PEF, path.join(parent, PEF_NAME))
    with open(path.join(parent, 'Parcelfile'), 'a') as f:
        f.write('\n')
        f.writelines(parcel_lines())
    print('Injected USB2 parcel(s): a=%s  b=%s' % (' / a='.join(MATCHES), DEVICE_TYPE))
    print('  copied %s into %s' % (PEF_NAME, parent))
    injected = True

if not injected:
    raise SystemExit('no Parcelfile found in the dump — is this a NewWorld ROM?')

cleanup()   # rebuilds the ROM if -o was a file (no-op if -o was a dump dir)
print('done.')

# ============================================================================
# How this stays safe and actually binds (all hardware-proven on a G4 MDD):
#
#  MATCH: the node's compatible list contains pciclass,0c0320, and device_type is
#    absent, so PROP_FLAGS=0x00004 (compatible-contains) binds card-agnostically. If a
#    target does not bind, re-probe the node and fall back to an exact-name match.
#
#  DRIVER boot-safety: TheDriverDescription.driverRuntime is 0x05
#    (kDriverIsLoadedUponDiscovery | kDriverIsUnderExpertControl, NOT OpenedUponLoad), and
#    DoDriverIO kInitialize stashes the node only (no bring-up), with the full EHCI bring-up
#    deferred to kOpen. Doing bring-up during the early PCI-claim phase freezes the boot;
#    stash-only + deferred kOpen boots to the desktop with the card claimed.
#
#  main == DoDriverIO: run patch-pef-main.py on EHCIUIM.pef (Retro68 -e leaves main unset
#    = 0xffffffff) so the generic Device-Manager loader enters at DoDriverIO.
#
#  TARGET ROM: patch the machine's real "Mac OS ROM". (A Mac Mini needs its OS 9 boot ROM
#    patch applied first, then this injection on top.)
#
#  RECOVERY: back up "Mac OS ROM" before replacing it, and keep the original so you can
#    restore it (boot from a CD/other volume if the patched ROM ever fails to boot).
# ============================================================================
