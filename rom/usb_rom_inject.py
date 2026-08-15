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

# ---- resource-fork helper -----------------------------------------------------
# A real "Mac OS ROM" keeps content in its RESOURCE FORK: on an MDD ROM that is SysEnabler,
# about 185 KB of it. If the fork is lost getting the file off the Mac, tbxi emits a data-fork-only
# ROM that is SMALLER than the original and will not boot, and it only prints a warning you are
# likely to miss in a long log. The checks at the end of this script refuse that output rather than
# letting you discover it at boot time.
#
# NOTE tbxi takes the fork from EITHER a real fork on the file OR a `<name>.rdump` sidecar written
# beside it. So a fork-less file is not automatically wrong -- it is wrong when there is no sidecar
# either. If you copy a ROM around, take its .rdump/.idump with it.
def rsrc_size(p):
    """Resource-fork size in bytes; None if this platform cannot tell us."""
    if not sys.platform.startswith('darwin'):
        return None
    try:
        return path.getsize(path.join(p, '..namedfork', 'rsrc'))
    except OSError:
        return 0

# ---- our EHCI driver core (the exact NDRV the ROM boot-loads) ----------------
# The PEF must be built for the ROM path: self-probe ON, a boot-safe DoDriverIO
# (kInitialize stash-only, bring-up deferred to kOpen), runtime flags 0x05, and
# main == DoDriverIO patched in (patch-pef-main.py) so the Device Manager loads it.
# See BUILD.md. Defaults to the shipped prebuilt; override with $EHCI_PEF.
_HERE    = path.dirname(path.abspath(__file__))
OUR_PEF  = os.environ.get('EHCI_PEF', path.join(_HERE, '..', 'dist', 'EHCIUIM.pef'))
PEF_NAME = 'EHCIUIM.pef'          # referenced uncompressed, like the stock controller ndrvs
# --lzss writes the Parcelfile reference as .pef.lzss so tbxi COMPRESSES the parcel at build time
# (the file on disk stays raw; the stock parcels use the same convention). The B&W G3 ROM ships
# this way to keep the image small; the Mini/MDD ROMs reference it raw. Either form boots.
PEF_LZSS = '--lzss' in sys.argv[1:]
if PEF_LZSS:
    sys.argv.remove('--lzss')     # the tbxi-patches arg parser must not see this private flag
PEF_REF  = PEF_NAME + ('.lzss' if PEF_LZSS else '')

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
#
# ⚠ THE ABOVE IS TRUE OF A PCI CARD AND NOT OF EVERY MACHINE. An on-board controller can advertise
# itself differently, and a Mac mini G4 does: its node DOES carry device_type = ehci. On that machine
# the card entry alone was silently ignored, so the ROM appeared to load the driver and nothing
# happened at all (a bind failure that looks exactly like a broken driver). Matching the way the
# stock parcels in that ROM match, on device_type, is what binds there.
#
# So there is ONE ENTRY PER NODE SHAPE, and one ROM serves both kinds of machine. `deduplicate=1`
# keeps a single copy of the driver PEF in the ROM even though two entries reference it. Adding an
# entry cannot affect a machine whose node does not match it, so the proven card path is unchanged.
MATCH_ENTRIES = [
    # PCI card: node has no device_type, so 0x08 is clear and `b` is unused. Hardware-proven.
    ('0x00004', 'pciclass,0c0320', 'usb'),
    # On-board (e.g. Mac mini G4): device_type == b AND compatible contains a.
    ('0x0000c', 'pciclass,0c0320', 'ehci'),
]
NDRV_FLAGS  = '0x00006'   # mirrors the stock controller ndrv flags


def parcel_lines():
    out = []
    dedup = ' deduplicate=1' if len(MATCH_ENTRIES) > 1 else ''
    for (flags, a, b) in MATCH_ENTRIES:
        out.append('prop flags=%s a=%s b=%s\n' % (flags, a, b))
        out.append('\tndrv flags=%s name=driver,AAPL,MacOS,PowerPC src=%s%s\n\n'
                   % (NDRV_FLAGS, PEF_REF, dedup))
    return out


# ---- INPUT CHECK: measure the base ROM before tbxi consumes it ------------------
# Done before get_src, because that dumps the ROM immediately and the evidence is clearest while the
# original file is still in hand.
_argsrc = next((a for a in sys.argv[1:] if not a.startswith('-')), None)
if _argsrc and path.isfile(_argsrc):
    _din = path.getsize(_argsrc)
    _rin = rsrc_size(_argsrc)
    _sidecar = _argsrc + '.rdump'
    _has_sidecar = path.isfile(_sidecar)
    print('input: %s -- %d bytes data, %s bytes rsrc%s'
          % (_argsrc, _din, _rin if _rin is not None else '?',
             ', .rdump sidecar %d bytes' % path.getsize(_sidecar) if _has_sidecar else ''))
    if not _rin and not _has_sidecar:
        print('')
        print('WARNING: this ROM has NO resource fork and NO .rdump sidecar beside it.')
        print('         A real Mac OS ROM keeps content there (SysEnabler, ~185 KB on an MDD ROM). If this')
        print('         copy should have had one, the patched ROM will come out SMALLER and will not boot.')
        print('         Fork-less is normal for a tbxi-BUILT image; it is a red flag for a ROM copied off a Mac.')
        print('         Ways to keep the fork: StuffIt/BinHex on the Mac and expand with `unar` (most modern')
        print('         unarchivers silently drop it), or copy over AFP rather than FAT or plain HTTP.')
        print('')
else:
    _din = _rin = None

src, cleanup = patch_common.get_src(
    desc='Inject the EHCI UIM NDRV into the OS 9 Mac OS ROM as a claimed USB 2.0 controller.')

if not path.exists(OUR_PEF):
    raise SystemExit('driver PEF not found: %s  (build EHCIUIM.pef first, or set $EHCI_PEF)' % OUR_PEF)

injected = False
for (parent, folders, files) in os.walk(src):
    folders.sort(); files.sort()
    if 'Parcelfile' not in files:
        continue
    # RE-INJECTION MUST UPDATE, NOT DUPLICATE.
    # This check used to look only for `EHCIUIM*.pef`, which is the name we WRITE -- but once tbxi has
    # built the ROM and it is dumped again, the parcel comes back named after itself,
    # `pciclass,0c0320-1.0.pef`. So on an already-patched ROM the check missed, a SECOND parcel entry
    # was appended, and tbxi deduplicated the pair into hash-suffixed names: a ROM carrying two EHCI
    # parcel entries. That is exactly what happens if you re-run this script to update to a newer
    # driver, which is the obvious thing to do.
    existing = [fn for fn in os.listdir(parent)
                if fnmatch.fnmatch(fn, 'EHCIUIM*.pef') or fnmatch.fnmatch(fn, 'pciclass,0c0320*.pef')]
    if existing:
        for fn in existing:
            shutil.copy(OUR_PEF, path.join(parent, fn))
        print('USB2 parcel already present in %s -- UPDATED the driver in place (%s); Parcelfile untouched'
              % (parent, ', '.join(existing)))
        injected = True
        continue
    shutil.copy(OUR_PEF, path.join(parent, PEF_NAME))
    with open(path.join(parent, 'Parcelfile'), 'a') as f:
        f.write('\n')
        f.writelines(parcel_lines())
    for (flags, a, b) in MATCH_ENTRIES:
        print('Injected USB2 parcel: flags=%s a=%s b=%s' % (flags, a, b))
    print('  copied %s into %s' % (PEF_NAME, parent))
    injected = True

if not injected:
    raise SystemExit('no Parcelfile found in the dump — is this a NewWorld ROM?')

cleanup()   # rebuilds the ROM if -o was a file (no-op if -o was a dump dir)

# ---- OUTPUT CHECK: refuse an implausible ROM before anyone installs it -----------
# This script only ADDS a driver (~210 KB), so an output SMALLER than its input means content was
# lost on the way through -- almost always a resource fork that did not survive the trip off the Mac.
# "Smaller than its own base" is the reliable tell, and it is platform-independent.
_out = None
for _i, _a in enumerate(sys.argv[1:]):
    if _a == '-o' and _i + 2 <= len(sys.argv[1:]):
        _out = sys.argv[_i + 2]
if _out and path.isfile(_out) and _din is not None:
    _dout = path.getsize(_out)
    if _dout < _din:
        raise SystemExit(
            'ERROR: the patched ROM at "%s" is NOT USABLE:\n'
            '  - it is SMALLER than the input (%d vs %d bytes, %d KB LOST) even though this script only\n'
            '    ADDS a driver, so content was dropped on the way through.\n'
            '\n'
            'Do NOT install it -- OS 9 will reject it at boot, or boot and misbehave. The usual cause is a\n'
            'resource fork lost getting the ROM off the Mac (see the WARNING this script prints for how to\n'
            'avoid that). The bad output has been left in place for inspection.'
            % (_out, _dout, _din, (_din - _dout) // 1024))
    if (_dout - _din) < 150 * 1024:
        print('WARNING: the ROM grew by only %d KB, but the driver alone is about 210 KB.'
              % ((_dout - _din) // 1024))
        print('         If you were UPDATING an already-patched ROM this is expected. Otherwise, verify')
        print('         before installing: scripts/verify-injected-rom.py <base> <output>')
    else:
        print('output verified: %d bytes (input was %d) -- grew by %d KB'
              % (_dout, _din, (_dout - _din) // 1024))
print('done.')

# ============================================================================
# How this stays safe and actually binds (all hardware-proven on a G4 MDD):
#
#  MATCH: one entry per node shape (see MATCH_ENTRIES). On a PCI card the node's compatible
#    list contains pciclass,0c0320 and device_type is absent, so flags 0x00004
#    (compatible-contains) binds card-agnostically. On an on-board controller that publishes
#    device_type = ehci, flags 0x0000c is what binds. If a target does not bind, probe the
#    node and fall back to an exact-name match.
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
