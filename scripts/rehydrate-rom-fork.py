#!/usr/bin/env python3
"""Rebuild a fork-ful "Mac OS ROM" from a data fork + its AppleDouble ._ sidecar.

Writes to a NEW path only. Never modifies the source files, and never uses `dot_clean -m`
(which deletes the ._ file WITHOUT merging it).

AppleDouble v2: magic 0x00051607, version, 16B filler, uint16 numEntries,
then numEntries * (entryID u32, offset u32, length u32). entryID 2 == resource fork.
"""
import struct, sys, os, shutil

def rsrc_from_appledouble(ad_path):
    b = open(ad_path, 'rb').read()
    magic, version = struct.unpack('>II', b[:8])
    if magic != 0x00051607:
        raise SystemExit("%s: not AppleDouble (magic 0x%08x)" % (ad_path, magic))
    n = struct.unpack('>H', b[24:26])[0]
    for i in range(n):
        eid, off, ln = struct.unpack('>III', b[26 + i * 12: 38 + i * 12])
        if eid == 2:
            return b[off:off + ln]
    raise SystemExit("%s: no resource-fork entry (id 2) among %d entries" % (ad_path, n))

def main(src_data, src_ad, out):
    if os.path.exists(out):
        os.remove(out)
    shutil.copyfile(src_data, out)          # data fork
    rf = rsrc_from_appledouble(src_ad)
    with open(out + '/..namedfork/rsrc', 'wb') as f:   # resource fork
        f.write(rf)
    dsz = os.path.getsize(out)
    rsz = os.path.getsize(out + '/..namedfork/rsrc')
    print("  %s: data %d + rsrc %d" % (os.path.basename(out), dsz, rsz))
    if rsz != len(rf):
        raise SystemExit("  resource fork short write: %d != %d" % (rsz, len(rf)))
    return dsz, rsz

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2], sys.argv[3])
