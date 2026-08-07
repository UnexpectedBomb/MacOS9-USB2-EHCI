#!/usr/bin/env python3
"""Unwrap a MacBinary file into a real two-fork file on macOS.

Why this exists: a "Mac OS ROM" keeps content in its RESOURCE FORK (SysEnabler, about 185 KB), and
MacBinary is the format that carries both forks through non-HFS media intact. Archives distribute
ROMs this way, so testing the injector against a downloaded variant means unwrapping first, and a
fork dropped here produces exactly the silently-unbootable ROM the injector now refuses.

Writes to a NEW path. Never modifies the input.

MacBinary II header (128 bytes, big-endian):
   0      version, must be 0
   1      filename length (1..63)
   2..64  filename
  65..68  file type          69..72  creator
  83..86  data fork length   87..90  resource fork length
 122      minimum version    123     version
 124..125 CRC-16 of bytes 0..123 (MacBinary II only; 0 in MacBinary I)
Each fork is padded to a 128-byte boundary.

Usage: unwrap-macbinary.py <in.bin> <out-path>
"""
import struct, sys, os


def crc16_ccitt(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def unwrap(src, out):
    b = open(src, 'rb').read()
    if len(b) < 128:
        raise SystemExit("%s: too short to be MacBinary" % src)
    if b[0] != 0 or b[74] != 0:
        raise SystemExit("%s: not MacBinary (byte0=%d byte74=%d)" % (src, b[0], b[74]))
    nlen = b[1]
    if not (1 <= nlen <= 63):
        raise SystemExit("%s: bad filename length %d" % (src, nlen))
    name = b[2:2 + nlen].decode('mac-roman', 'replace')
    ftype = b[65:69].decode('mac-roman', 'replace')
    creator = b[69:73].decode('mac-roman', 'replace')
    dlen, rlen = struct.unpack('>II', b[83:91])

    # CRC is MacBinary II only; a zero CRC means MacBinary I, which is not an error.
    stored = struct.unpack('>H', b[124:126])[0]
    crc_ok = (stored == 0) or (stored == crc16_ccitt(b[0:124]))

    need = 128 + ((dlen + 127) // 128) * 128 + rlen
    if len(b) < need:
        raise SystemExit("%s: truncated (need >= %d bytes, have %d)" % (src, need, len(b)))

    doff = 128
    roff = 128 + ((dlen + 127) // 128) * 128
    data = b[doff:doff + dlen]
    rsrc = b[roff:roff + rlen]

    if os.path.exists(out):
        os.remove(out)
    with open(out, 'wb') as f:
        f.write(data)
    if rlen:
        with open(out + '/..namedfork/rsrc', 'wb') as f:
            f.write(rsrc)

    print("  %-22s type '%s' creator '%s'  data %d + rsrc %d%s"
          % (name[:22], ftype, creator, dlen, rlen, "" if crc_ok else "  [!! header CRC mismatch]"))
    # Verify what actually landed rather than trusting the writes.
    got_d = os.path.getsize(out)
    got_r = os.path.getsize(out + '/..namedfork/rsrc') if rlen else 0
    if got_d != dlen or got_r != rlen:
        raise SystemExit("  short write: data %d/%d rsrc %d/%d" % (got_d, dlen, got_r, rlen))
    return dlen, rlen, ftype, creator


if __name__ == '__main__':
    unwrap(sys.argv[1], sys.argv[2])
