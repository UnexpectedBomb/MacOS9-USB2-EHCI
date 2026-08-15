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
xattrs of its own and cannot spawn an AppleDouble "._" sidecar on a network share -- one of those was
fed to StuffIt Expander and bus-errored the machine into MacsBug.

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
    args = list(sys.argv[1:])
    # ★ b15b: --lzss passes through to the injector — the B&W recipe compresses the driver parcel
    # (src=...pef.lzss in the Parcelfile); Mini/MDD ship it raw. See usb_rom_inject.py for history.
    lzss = '--lzss' in args
    if lzss:
        args.remove('--lzss')
    if len(args) != 3:
        raise SystemExit(__doc__)
    base, tag, out = args
    if not out.lower().endswith('.hqx'):
        raise SystemExit('output must end in .hqx (that is the fork-safe format)')

    workdir = tempfile.mkdtemp(prefix='romhqx')
    dump = path.join(workdir, 'romdump')
    try:
        print('1. injecting the driver into a dump directory ...')
        r = subprocess.run([sys.executable, path.join(REPO, 'rom', 'usb_rom_inject.py'),
                            base, '-o', dump + '/'] + (['--lzss'] if lzss else []))
        if r.returncode != 0:
            raise SystemExit('injection FAILED -- not building a ROM from it')

        rdump = path.join(dump, 'SysEnabler.rdump')
        if not path.isfile(rdump):
            raise SystemExit('no SysEnabler.rdump in the dump: this base has no resource fork, so a '
                             'built ROM would lose SysEnabler. Use a fork-ful base '
                             '(scripts/rehydrate-rom-fork.py).')
        # ★★ b1 (2026-08-13): remember what the BASE's fork looked like, because the verify step below now
        # compares OUTPUT against BASE rather than testing absolutes. A generic RETAIL ROM (the B&W's 8.7,
        # 'Mac OS 9.2.2') legitimately has NO enabler and DOES ship its own vers (1) = "Mac OS ROM 8.7" that
        # the machine boots with — both of the old absolute checks are false positives on such a base. See
        # docs/BW-ROM-INTAKE.md, "TWO GUARDS BLOCK THE BUILD". Nothing here stamps anything, ever.
        base_rdump = open(rdump, encoding='mac-roman').read()
        base_se    = path.getsize(path.join(dump, 'SysEnabler'))
        if base_se == 0 and len(base_rdump.strip()) < 200:
            raise SystemExit('SysEnabler is empty AND the resource fork is trivial — this base lost its '
                             'fork (a fork-ful retail base still dumps ~3.5 KB of its own resources). '
                             'Rehydrate a proper copy; do NOT build from this.')

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
        # ★★ Guard 1, STRICTLY STRONGER (b1): "SysEnabler non-empty" was written to catch a LOST FORK, but a
        # retail base has no enabler at all. The real invariant is "the output's enabler matches the base's",
        # which catches fork loss on enabler-ful bases AND passes honest enabler-less retail bases — and
        # unlike the old test it also catches a fork-loss that leaves a stub.
        if se != base_se:
            raise SystemExit('SysEnabler CHANGED: base %d bytes -> output %d. The fork was damaged in the '
                             'build. Do NOT ship this ROM.' % (base_se, se))
        vers = open(path.join(chk, 'SysEnabler.rdump'), encoding='mac-roman').read()
        # ★★ Guard 2, STRICTLY STRONGER (b1): the h23 failure was a vers (1) WE ADDED to an enabler's fork.
        # The retail 8.7 base ships its OWN vers (1) ("Mac OS ROM 8.7") and boots with it, so "no vers (1)
        # anywhere" is the wrong test. The right one: the fork must be BYTE-IDENTICAL to the base's — we add
        # nothing, ever, to any fork. That refuses a stamp into ANY resource, not just vers (1).
        if vers != base_rdump:
            raise SystemExit('the ROM resource fork CHANGED between base and output. This tool must never '
                             'modify that fork (h23: a stamped vers (1) mis-versioned the System Enabler '
                             'and stopped the machine booting). Do NOT ship this ROM.')
        keep2 = "data 'vers' (2" in vers
        print('  SysEnabler        %d bytes  (matches the base%s)' % (se, ', enabler-less retail base' if se == 0 else ''))
        print('  Bootscript        %d bytes' % path.getsize(path.join(chk, 'Bootscript')))
        print('  resource fork     byte-identical to the base  <- REQUIRED; we add NOTHING to that fork')
        print('  vers (2) intact   %s' % ('yes' if keep2 else 'n/a on this base'))
        print('\ndone: %s' % out)
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == '__main__':
    main()
