#!/usr/bin/env python3
"""
wrap_macbinary.py — wrap a raw tbxi ROM (data fork) as a MacBinary "Mac OS ROM" for hand-install on OS 9.

Replaces the ad-hoc inline wrapping we used for lc1..n1 and adds two things the user asked for so the ROM
sitting in the System Folder can be identified at a glance:

  1. REAL DATES. The old wrapping left the MacBinary creation/modification date fields ZERO, which the Finder
     renders as "Fri, Jan 1, 1904". We now stamp both with the current time (Mac epoch = Unix + 2082844800),
     so Date Created / Date Modified are meaningful.

  2. VERSION IN GET INFO. We attach a minimal RESOURCE FORK containing a single 'vers' (ID 1) resource, so the
     Finder's Get Info shows e.g. "n1" plus a longer description. Previously we shipped data-fork-only
     (reslen = 0) and Get Info had no version at all.
     NB the ROM's *data* fork is what Open Firmware reads; the pristine Apple ROM has a large resource fork of
     its own and our data-fork-only builds booted fine, so the resource fork is not consulted at boot. This
     tiny one should therefore be inert — but it IS a change to a boot-critical artifact, so confirm the first
     ROM built this way still boots before trusting it everywhere.

Usage:
    wrap_macbinary.py <raw-rom> <version> <out.bin> [--desc "text"] [--name "Mac OS ROM"]
"""
import struct, sys, time, argparse

MAC_EPOCH_DELTA = 2082844800          # seconds between 1904-01-01 and 1970-01-01


def crc_ccitt(data: bytes) -> int:
    """MacBinary II header CRC: CRC-CCITT/XMODEM, poly 0x1021, init 0."""
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc


def build_vers(version: str, desc: str) -> bytes:
    """A 'vers' resource: numeric version + short and long Pascal strings."""
    short = version.encode("mac_roman")[:255]
    long_ = desc.encode("mac_roman")[:255]
    return (bytes([0x01, 0x00, 0x80, 0x00])      # majorRev 1, minor/bug 0, stage 0x80 (final), nonRelRev 0
            + struct.pack(">H", 0)                # regionCode 0 = verUS
            + bytes([len(short)]) + short
            + bytes([len(long_)]) + long_)


def build_resource_fork(resources) -> bytes:
    """Minimal but standards-correct resource fork. `resources` = [(type, id, data), ...]."""
    DATA_OFF = 256
    data = b""
    placed = []                                   # (type, id, offset-of-length-word within data area)
    for rtype, rid, rdata in resources:
        placed.append((rtype, rid, len(data)))
        data += struct.pack(">I", len(rdata)) + rdata

    by_type = {}
    for rtype, rid, off in placed:
        by_type.setdefault(rtype, []).append((rid, off))

    n_types = len(by_type)
    type_list_off = 28                            # from start of map
    # count word (2) + 8 bytes per type entry, then the ref lists
    ref_base = type_list_off + 2 + 8 * n_types
    type_entries, ref_lists, cursor = b"", b"", ref_base
    for rtype, refs in by_type.items():
        type_entries += (rtype.encode("mac_roman")[:4].ljust(4, b" ")
                         + struct.pack(">HH", len(refs) - 1, cursor - type_list_off))
        for rid, off in refs:
            ref_lists += (struct.pack(">hH", rid, 0xFFFF)        # id, no name
                          + bytes([0])                            # attributes
                          + off.to_bytes(3, "big")                # 3-byte offset into the data area
                          + struct.pack(">I", 0))                 # reserved handle
            cursor += 12

    name_list_off = ref_base + 12 * len(placed)
    m = (b"\0" * 16 + struct.pack(">I", 0) + struct.pack(">HH", 0, 0)
         + struct.pack(">HH", type_list_off, name_list_off)
         + struct.pack(">H", n_types - 1) + type_entries + ref_lists)

    hdr = struct.pack(">IIII", DATA_OFF, DATA_OFF + len(data), len(data), len(m))
    return hdr.ljust(DATA_OFF, b"\0") + data + m


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("raw"); ap.add_argument("version"); ap.add_argument("out")
    ap.add_argument("--desc", default=None)
    ap.add_argument("--name", default="Mac OS ROM")
    a = ap.parse_args()

    raw = open(a.raw, "rb").read()
    if not raw.startswith(b"<CHRP-BOOT>"):
        print("WARNING: input does not start with <CHRP-BOOT> — is this a raw tbxi ROM?", file=sys.stderr)
    desc = a.desc or ("USB 2.0 for Mac OS 9 - build " + a.version)

    rsrc = build_resource_fork([("vers", 1, build_vers(a.version, desc))])
    now = int(time.time()) + MAC_EPOCH_DELTA

    h = bytearray(128)
    name = a.name.encode("mac_roman")[:63]
    h[0] = 0; h[1] = len(name); h[2:2 + len(name)] = name
    h[65:69] = b"tbxi"; h[69:73] = b"chrp"          # type / creator — what makes it the boot image
    h[73] = 0; h[74] = 0
    struct.pack_into(">I", h, 83, len(raw))         # data fork length
    struct.pack_into(">I", h, 87, len(rsrc))        # resource fork length
    struct.pack_into(">I", h, 91, now)              # creation date   (was 0 => "Jan 1, 1904")
    struct.pack_into(">I", h, 95, now)              # modification date
    struct.pack_into(">H", h, 124, crc_ccitt(bytes(h[:124])))

    pad = lambda b: b + b"\0" * ((-len(b)) % 128)
    open(a.out, "wb").write(bytes(h) + pad(raw) + pad(rsrc))

    print("wrote %s" % a.out)
    print("  data fork %d, resource fork %d ('vers' = %r)" % (len(raw), len(rsrc), a.version))
    print("  dates stamped: %s" % time.strftime("%a %b %d %Y %H:%M", time.localtime()))


if __name__ == "__main__":
    main()
