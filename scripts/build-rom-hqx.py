#!/usr/bin/env python3
"""build-rom-hqx.py — build a shippable Mac OS ROM that is fork-safe AND self-identifying.

    build-rom-hqx.py <base-ROM-or-dumpdir> <version-tag> <out.hqx>

Does three things in order, and verifies all of them:
  1. injects our EHCI driver with rom/usb_rom_inject.py (output as a dump directory)
  2. stamps a `vers` (1) resource carrying <version-tag> so Get Info NAMES THE BUILD
  3. builds a BinHex .hqx, which carries BOTH FORKS

★ WHY .hqx AND NOT rom/wrap_macbinary.py's .bin. The MacBinary wrapper builds only a small `vers`
resource fork of its own, which DISCARDS the ROM's real 616 KB resource fork and with it the 185 KB
SysEnabler. Every USB 2.0 ROM shipped before 2026-08-07 lost SysEnabler that way, on both machines.
`tbxi build -o X.hqx` preserves the fork. As a bonus .hqx is plain ASCII, so it carries no forks or
xattrs of its own and cannot spawn an AppleDouble "._" sidecar when it crosses a file server that
cannot store forks natively. Feeding such a sidecar to StuffIt Expander by mistake bus-errors an
OS 9 machine into MacsBug, because from that side it looks like a real file of the same name.

★ WHY `vers` (1) AND NOT (2). The ROM already ships `vers` (2) = "Mac OS CPU Software 5.9", which is
real ROM identification and must not be overwritten. `vers` (1) is free and is the one Get Info
shows as the file's version, so ours goes there and both survive.

★ WHERE THE FORK LIVES. tbxi represents the ROM FILE's resource fork as the Rez text in
`SysEnabler.rdump` inside the dump directory (confirmed by size: 616,556 bytes of resources both in
the stock ROM's fork and in the built output). So stamping means editing that Rez, before the build.
"""
import os, re, subprocess, sys, tempfile, shutil
from os import path

HERE = path.dirname(path.abspath(__file__))
REPO = path.dirname(HERE)


def vers_resource(tag, long_text):
    """A classic 'vers' resource: BCD rev bytes, release stage, country, then two Pascal strings."""
    if len(tag) > 255 or len(long_text) > 255:
        raise SystemExit('version strings too long')
    body = bytes([0x01, 0x00, 0x80, 0x00, 0x00, 0x00])      # 1.0, final, non-release 0, country US
    body += bytes([len(tag)]) + tag.encode('mac-roman')
    body += bytes([len(long_text)]) + long_text.encode('mac-roman')
    return body


def rez_block(rtype, rid, data, attrs=''):
    """Render one resource as Rez text in the same shape tbxi emits, so it re-parses."""
    out = ["data '%s' (%d%s) {" % (rtype, rid, (', ' + attrs) if attrs else '')]
    for off in range(0, len(data), 16):
        chunk = data[off:off + 16]
        hexpairs = ''.join('%02X%02X ' % (chunk[i], chunk[i + 1]) if i + 1 < len(chunk)
                           else '%02X ' % chunk[i] for i in range(0, len(chunk), 2)).strip()
        out.append('\t$"%s"' % hexpairs)
    out.append('};')
    return '\n'.join(out) + '\n\n'


def stamp_vers(rdump_path, tag, long_text):
    """Insert or REPLACE data 'vers' (1). Never touches vers (2), the ROM's own CPU Software string."""
    with open(rdump_path, 'r', encoding='mac-roman') as f:
        rez = f.read()

    # Drop any previous vers (1) so re-running is idempotent rather than duplicating.
    pat = re.compile(r"data 'vers' \(1[^)]*\) \{.*?\};\s*", re.S)
    rez, nremoved = pat.subn('', rez)

    block = rez_block('vers', 1, vers_resource(tag, long_text))
    # Write to a temp file and move it into place: never truncate the original in situ.
    tmp = rdump_path + '.new'
    with open(tmp, 'w', encoding='mac-roman') as f:
        f.write(block)
        f.write(rez)
    os.replace(tmp, rdump_path)
    return nremoved


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    base, tag, out = sys.argv[1], sys.argv[2], sys.argv[3]
    if not out.lower().endswith('.hqx'):
        raise SystemExit('output must end in .hqx (that is the fork-safe format)')

    workdir = tempfile.mkdtemp(prefix='romhqx')
    dump = path.join(workdir, 'romdump')
    try:
        print('1. injecting the driver into a dump directory ...')
        r = subprocess.run([sys.executable, path.join(REPO, 'rom', 'usb_rom_inject.py'),
                            base, '-o', dump + '/'])
        if r.returncode != 0:
            raise SystemExit('injection FAILED -- not building a ROM from it')

        rdump = path.join(dump, 'SysEnabler.rdump')
        if not path.isfile(rdump):
            raise SystemExit('no SysEnabler.rdump in the dump: this base has no resource fork, so a '
                             'built ROM would lose SysEnabler. Use a fork-ful base '
                             '(scripts/rehydrate-rom-fork.py).')

        print('2. stamping vers (1) = %r so Get Info names the build ...' % tag)
        n = stamp_vers(rdump, tag, 'USB 2.0 EHCI ROM ' + tag)
        print('   (replaced %d previous vers(1) block(s))' % n)

        print('3. building %s ...' % out)
        r = subprocess.run([sys.executable, '-m', 'tbxi', 'build', '-o', out, dump])
        if r.returncode != 0:
            raise SystemExit('tbxi build FAILED')

        # ---- verify, rather than trust ----
        print('\nverifying ...')
        chk = path.join(workdir, 'verify')
        r = subprocess.run([sys.executable, '-m', 'tbxi', 'dump', '-o', chk, out],
                           stdout=subprocess.DEVNULL)
        if r.returncode != 0:
            raise SystemExit('the built .hqx could not be dumped back -- do NOT ship it')
        se = path.getsize(path.join(chk, 'SysEnabler'))
        if se == 0:
            raise SystemExit('SysEnabler is 0 bytes in the output -- the fork was lost, do NOT ship it')
        vers = open(path.join(chk, 'SysEnabler.rdump'), encoding='mac-roman').read()
        m = re.search(r"data 'vers' \(1[^)]*\) \{(.*?)\};", vers, re.S)
        if not m:
            raise SystemExit('vers (1) did not survive the build -- Get Info would not name the build')
        hexbytes = bytes.fromhex(''.join(re.findall(r'"([0-9A-Fa-f ]+)"', m.group(1))).replace(' ', ''))
        shown = hexbytes[7:7 + hexbytes[6]].decode('mac-roman')
        if shown != tag:
            raise SystemExit('vers (1) says %r, expected %r' % (shown, tag))
        keep2 = "data 'vers' (2" in vers
        print('  SysEnabler        %d bytes  OK' % se)
        print('  Bootscript        %d bytes' % path.getsize(path.join(chk, 'Bootscript')))
        print("  vers (1)          %r  <- what Get Info will show" % shown)
        print('  vers (2) intact   %s  (the ROM\'s own CPU Software string)' % ('yes' if keep2 else 'NO'))
        print('\ndone: %s' % out)
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == '__main__':
    main()
