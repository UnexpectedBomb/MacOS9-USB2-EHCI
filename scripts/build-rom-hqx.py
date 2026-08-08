#!/usr/bin/env python3
"""build-rom-hqx.py — build a shippable Mac OS ROM that keeps BOTH FORKS.

    build-rom-hqx.py <base-ROM-or-dumpdir> <version-tag> <out.hqx>

Does two things, and verifies both:
  1. injects our EHCI driver with rom/usb_rom_inject.py (output as a dump directory)
  2. builds a BinHex .hqx, which carries BOTH FORKS
The <version-tag> names the OUTPUT FILE only. It is deliberately NOT written into the ROM: see below.

★ WHY .hqx AND NOT rom/wrap_macbinary.py's .bin. The MacBinary wrapper builds only a small `vers`
resource fork of its own, which DISCARDS the ROM's real 616 KB resource fork and with it the 185 KB
SysEnabler. Every USB 2.0 ROM shipped before 2026-08-07 lost SysEnabler that way, on both machines.
`tbxi build -o X.hqx` preserves the fork. As a bonus .hqx is plain ASCII, so it carries no forks or
xattrs of its own and cannot spawn an AppleDouble "._" sidecar when it crosses a file server that
cannot store forks natively. Feeding such a sidecar to StuffIt Expander by mistake bus-errors an
OS 9 machine into MacsBug, because from that side it looks like a real file of the same name.

⚠⚠⚠ THIS SCRIPT NO LONGER STAMPS A VERSION, AND MUST NOT BE MADE TO AGAIN.

An earlier version added a `vers` (1) resource so Get Info would name the build. That ROM (h23) FAILED
TO BOOT: "No File System Access modules could be found in your System folder", then a grey screen.
Proven by discriminator on 2026-08-07 -- the identical ROM built WITHOUT the stamp boots fine.

WHY, and it is not corruption: the stamped ROM was verified resource-by-resource against stock and
nothing was damaged, lost or altered beyond the added `vers` and an expected cfrg relocation. The fault
is MEANING. This ROM's resource fork IS THE SYSTEM ENABLER'S resource fork, and `vers` (1) is a FILE'S
OWN VERSION. Stock ships `vers` (2) = "Mac OS CPU Software 5.9" and deliberately NO `vers` (1). Adding
one tells the System the enabler is version 1.0.0, which is a claim about the ENABLER, not about us,
and an enabler that is mis-versioned leaves enabler-dependent components uninstalled.

★ And it explains why rom/wrap_macbinary.py got away with the same trick for months: it REPLACED the
resource fork, so its `vers` (1) was the only resource present and there was no enabler in the fork for
it to misdescribe. Preserving the fork and adding a `vers` (1) are each fine alone and jointly fatal.

⇒ The build tag lives in the FILENAME only. Getting it into Get Info without lying about the enabler is
an open problem; do not solve it by writing into `vers` (2) either, which is equally the enabler's.

★ WHERE THE FORK LIVES. tbxi represents the ROM FILE's resource fork as the Rez text in
`SysEnabler.rdump` inside the dump directory (confirmed by size: 616,556 bytes of resources both in
the stock ROM's fork and in the built output). That is why the removed stamping edited that Rez, and
why the verifier below reads it back to prove no `vers` (1) crept in.
"""
import os, re, subprocess, sys, tempfile, shutil
from os import path

HERE = path.dirname(path.abspath(__file__))
REPO = path.dirname(HERE)


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

        # ⚠⚠⚠ STAMPING IS DELIBERATELY GONE. It broke boot. See the banner at the top of this file.
        print('2. NOT stamping a vers resource (see the note at the top of this script) ...')
        print('   build tag %r is recorded here and in the filename only, NOT in the ROM.' % tag)

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
        # ★ The check is now INVERTED: a vers (1) here is a BOOT FAILURE, not a feature.
        if re.search(r"data 'vers' \(1[^)]*\) \{", vers):
            raise SystemExit('the output carries a vers (1) in the ROM resource fork. That fork is the '
                             'System Enabler\'s, and a vers (1) there describes the ENABLER, not our build. '
                             'It stops the machine booting. Do NOT ship this ROM.')
        keep2 = "data 'vers' (2" in vers
        print('  SysEnabler        %d bytes  OK' % se)
        print('  Bootscript        %d bytes' % path.getsize(path.join(chk, 'Bootscript')))
        print('  vers (1)          absent  <- REQUIRED; a vers (1) here breaks boot')
        print('  vers (2) intact   %s  (the ROM\'s own CPU Software string)' % ('yes' if keep2 else 'NO'))
        print('\ndone: %s' % out)
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == '__main__':
    main()
