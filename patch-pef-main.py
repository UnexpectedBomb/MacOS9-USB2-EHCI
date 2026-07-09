#!/usr/bin/env python3
"""
patch-pef-main.py <file.pef> <exportedSymbolName>

Set a PEF fragment's `main` symbol to point at an already-EXPORTED symbol
(e.g. DoDriverIO), by rewriting the loader info header's mainSection/mainOffset.

Why: a Mac OS native driver fragment must have BOTH `DoDriverIO` exported AND the
fragment `main` pointing at it (verified by diffing Apple's shipping NVIDIA ndrv:
2 exports = DoDriverIO+TheDriverDescription, and main offset == DoDriverIO's
offset). Using the linker's -e to set the entry makes MakePEF drop that symbol
from the export table, so we instead build normally (symbol stays exported, no
main) and patch the main here to reference the export's (section, offset).

PEF layout authority: PEFBinaryFormat.h. All fields big-endian.
"""
import struct, sys

MAGIC = b"Joy!peff"

def main():
    if len(sys.argv) != 3:
        sys.exit("usage: patch-pef-main.py <file.pef> <symbol>")
    path, want = sys.argv[1], sys.argv[2]
    blob = bytearray(open(path, "rb").read())
    base = blob.find(MAGIC)
    if base < 0:
        sys.exit("patch-pef-main: no PEF 'Joy!peff' magic found")

    sec_count = struct.unpack_from(">H", blob, base + 32)[0]
    sec_hdr = base + 40
    loaderOff = None
    for n in range(sec_count):
        (_no, _da, _tl, _ul, clen, coff, kind, _s, _a, _r) = struct.unpack_from(
            ">iIIIIIBBBB", blob, sec_hdr + n * 28)
        if kind == 4:                      # loader section
            loaderOff = coff
    if loaderOff is None:
        sys.exit("patch-pef-main: no loader section")

    L = base + loaderOff
    (mainSec, mainOff, iS, iO, tS, tO, impLib, impSym, relSec, relOff,
     strsOff, hashOff, hashPow, expCount) = struct.unpack_from(">iIiIiIIIIIIIII", blob, L)

    strs_base = L + strsOff
    keys_base = L + hashOff + (1 << hashPow) * 4
    syms_base = keys_base + expCount * 4
    keylen = [struct.unpack_from(">I", blob, keys_base + n * 4)[0] >> 16 for n in range(expCount)]

    found = None
    for n in range(expCount):
        classAndName, value, secIdx = struct.unpack_from(">IIh", blob, syms_base + n * 10)
        nlen = keylen[n]
        noff = classAndName & 0x00FFFFFF
        name = blob[strs_base + noff: strs_base + noff + nlen].decode("mac_roman", "replace")
        if name == want:
            found = (secIdx, value)
            break
    if found is None:
        sys.exit(f"patch-pef-main: exported symbol '{want}' not found")

    secIdx, value = found
    struct.pack_into(">i", blob, L + 0, secIdx)   # mainSection
    struct.pack_into(">I", blob, L + 4, value)    # mainOffset
    open(path, "wb").write(blob)
    print(f"patch-pef-main: main -> {want} (sec {secIdx}, off 0x{value:x})")

main()
