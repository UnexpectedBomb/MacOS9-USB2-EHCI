#!/usr/bin/env python3
"""package-init.py — dress a VALIDATED INIT .bin for public release, without rebuilding it.

    package-init.py <src.bin> <out.bin> <internal name> <versBCD major> <vers short> <vers long> [icon-appledouble]

Takes the exact hardware-validated MacBinary artifact and:
  1. renames it INTERNALLY (the MacBinary header name = the filename the extension decodes to);
  2. adds a `vers` (1) resource so ASP's Extensions tab and Get Info show a real version
     (SAFE here: an extension owns its own resource fork. The h23 catastrophe was a `vers` (1)
     in a Mac OS ROM, whose fork belongs to the SYSTEM ENABLER — never do that);
  3. copies a Finder custom-icon family (all resources at ID -16455) from an AppleDouble sidecar
     and sets the kHasCustomIcon Finder flag (clearing kHasBeenInited so the Finder re-reads it);
  4. leaves EVERY existing resource byte-identical, attributes included — the INIT code, the PPC
     fragments and their sysheap/locked flags are the validated payload and must not change.

Writes to the NEW path only (CLAUDE.md rule: transforming scripts never open the original for write).
The resource fork is fully re-serialized; existing resource data, ids, names and attribute bytes are
carried over exactly, then the additions appended (replacing same-type/same-id if present).
"""
import struct, sys, os

def crc16_xmodem(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if (crc & 0x8000) else (crc << 1)
            crc &= 0xFFFF
    return crc

def parse_fork(rf):
    """-> list of (type, id, name|None, attrs, data), preserving order."""
    dataOff, mapOff, dataLen, mapLen = struct.unpack('>IIII', rf[:16])
    m = rf[mapOff:mapOff+mapLen]
    typeListOff = struct.unpack('>H', m[24:26])[0]
    nameListOff = struct.unpack('>H', m[26:28])[0]
    ntypes = (struct.unpack('>h', m[typeListOff:typeListOff+2])[0] + 1) & 0xFFFF
    out = []
    p = typeListOff + 2
    for _ in range(ntypes):
        typ = m[p:p+4]
        cnt = struct.unpack('>h', m[p+4:p+6])[0] + 1
        refOff = struct.unpack('>H', m[p+6:p+8])[0]
        rp = typeListOff + refOff
        for _r in range(cnt):
            rid, nameOff = struct.unpack('>hH', m[rp:rp+4])
            attrs = m[rp+4]
            dOff = (m[rp+5] << 16) | (m[rp+6] << 8) | m[rp+7]
            dlen = struct.unpack('>I', rf[dataOff+dOff:dataOff+dOff+4])[0]
            data = rf[dataOff+dOff+4:dataOff+dOff+4+dlen]
            name = None
            if nameOff != 0xFFFF:
                nl = m[nameListOff+nameOff]
                name = m[nameListOff+nameOff+1:nameListOff+nameOff+1+nl]
            out.append([typ, rid, name, attrs, data])
            rp += 12
        p += 8
    return out

def build_fork(resources):
    """Canonical re-serialization. resources: list of [type,id,name,attrs,data]."""
    # data area, preserving list order
    data_area = bytearray(); offsets = []
    for r in resources:
        offsets.append(len(data_area))
        data_area += struct.pack('>I', len(r[4])) + r[4]
    # group by type, first-appearance order
    types = []
    for r in resources:
        if r[0] not in types: types.append(r[0])
    name_list = bytearray(); ref_lists = bytearray()
    type_list = bytearray(struct.pack('>h', len(types) - 1))
    refoff_base = 2 + 8 * len(types)             # from type-list start
    per_type_refs = {}
    for t in types:
        per_type_refs[t] = [i for i, r in enumerate(resources) if r[0] == t]
    running = refoff_base
    for t in types:
        idxs = per_type_refs[t]
        type_list += t + struct.pack('>hH', len(idxs) - 1, running)
        running += 12 * len(idxs)
    for t in types:
        for i in per_type_refs[t]:
            typ, rid, name, attrs, data = resources[i]
            if name is not None:
                noff = len(name_list)
                name_list += bytes([len(name)]) + name
            else:
                noff = 0xFFFF
            d = offsets[i]
            ref_lists += struct.pack('>hH', rid, noff) + bytes([attrs, (d >> 16) & 0xFF, (d >> 8) & 0xFF, d & 0xFF]) + b'\0\0\0\0'
    tl = type_list + ref_lists
    map_hdr = b'\0' * 16 + b'\0\0\0\0' + b'\0\0' + b'\0\0'   # copy/handle/fileRef/attrs
    typeListOff = len(map_hdr) + 4                            # +4 for the two offset words
    nameListOff = typeListOff + len(tl)
    m = map_hdr + struct.pack('>HH', typeListOff, nameListOff) + tl + name_list
    dataOff = 256
    mapOff = dataOff + len(data_area)
    hdr = struct.pack('>IIII', dataOff, mapOff, len(data_area), len(m)) + b'\0' * 240
    return hdr + data_area + m

def make_vers(major_bcd, short_s, long_s):
    v = struct.pack('>BBBBh', major_bcd, 0x00, 0x80, 0x00, 0)   # final stage, US
    v += bytes([len(short_s)]) + short_s.encode('mac_roman')
    v += bytes([len(long_s)]) + long_s.encode('mac_roman')
    return v

def rsrc_from_appledouble(p):
    b = open(p, 'rb').read()
    if struct.unpack('>I', b[:4])[0] != 0x00051607: raise SystemExit(p + ": not AppleDouble")
    n = struct.unpack('>H', b[24:26])[0]
    for i in range(n):
        eid, off, ln = struct.unpack('>III', b[26+i*12:38+i*12])
        if eid == 2: return b[off:off+ln]
    raise SystemExit(p + ": no resource fork entry")

def main():
    src, out, name, major, shorts, longs = sys.argv[1:7]
    iconad = sys.argv[7] if len(sys.argv) > 7 else None
    raw = open(src, 'rb').read()
    if raw[0] != 0 or raw[1] == 0 or raw[1] > 63: raise SystemExit("not MacBinary")
    dlen, rlen = struct.unpack('>II', raw[83:91])
    dstart = 128
    rstart = dstart + ((dlen + 127) // 128) * 128
    dfork = raw[dstart:dstart+dlen]
    rfork = raw[rstart:rstart+rlen]
    res = parse_fork(rfork)
    kept = [r for r in res if not (r[1] == -16455) and not (r[0] == b'vers' and r[1] == 1)]
    dropped = len(res) - len(kept)
    # vers (1)
    kept.append([b'vers', 1, None, 0x00, make_vers(int(major, 16), shorts, longs)])
    added_icons = 0
    if iconad:
        for r in parse_fork(rsrc_from_appledouble(iconad)):
            if r[1] == -16455:
                kept.append(r); added_icons += 1
    newfork = build_fork(kept)
    # verify round-trip: every original non-replaced resource byte-identical
    back = {(r[0].decode('latin-1'), r[1]): (r[3], r[4]) for r in parse_fork(newfork)}
    for r in res:
        k = (r[0].decode('latin-1'), r[1])
        if r[1] == -16455 or k == ('vers', 1): continue
        if back[k] != (r[3], r[4]): raise SystemExit("round-trip mismatch on %s %d" % k)
    # MacBinary header: new name, custom-icon flag, cleared inited, fork lengths, CRC
    h = bytearray(raw[:128])
    nm = name.encode('mac_roman')
    h[1] = len(nm); h[2:65] = nm.ljust(63, b'\0')
    h[73] = (h[73] | 0x04) & ~0x01          # +kHasCustomIcon (hi byte), -kHasBeenInited
    h[83:87] = struct.pack('>I', len(dfork))
    h[87:91] = struct.pack('>I', len(newfork))
    h[122] = 129; h[123] = 129
    h[124:126] = struct.pack('>H', crc16_xmodem(bytes(h[:124])))
    def pad(b): return b + b'\0' * ((128 - len(b) % 128) % 128)
    open(out, 'wb').write(bytes(h) + pad(dfork) + pad(newfork))
    print("%s -> %s  name=%r  vers=%s  icons+%d  replaced=%d  rsrc %d->%d bytes" %
          (os.path.basename(src), os.path.basename(out), name, shorts, added_icons, dropped, rlen, len(newfork)))

if __name__ == '__main__':
    main()
