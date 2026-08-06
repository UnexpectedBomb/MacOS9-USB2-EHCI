#!/usr/bin/env python3
"""verify-injected-rom.py — structurally verify an injected Mac OS ROM against its base.

Why this exists: injecting into a foreign base ROM can fail in ways that look fine until the machine
refuses to boot, and the failure modes are all detectable WITHOUT hardware. This runs the same
checklist we apply to every build, but base-vs-output instead of build-vs-build, so a new ROM variant
can be cleared (or rejected) from the desk.

    python3 scripts/verify-injected-rom.py <base ROM|dumpdir> <injected ROM|dumpdir> [driver.pef]

Exit 0 = every check passed. Non-zero = at least one FAIL; read the report.

The checks, and what each one catches:
  1. every base parcel still present      — the injection must ADD, never replace or drop
  2. exactly one new parcel, and it is
     our driver                           — a base whose parcel table we disturbed
  3. every shared parcel byte-identical   — collateral damage to a parcel we do not own
  4. SysEnabler present + same size       — trap #1: a lost resource fork silently drops it
  5. Bootscript present + same size       — the boot script must survive untouched
  6. MacOS.elf present + same size        — ditto the nanokernel
  7. output no smaller than the base       — the recorded tell for dropped content
  8. embedded driver PEF cmp-clean         — the ROM must carry the driver we think it does
"""
import os, subprocess, sys, tempfile, shutil
from os import path

HERE = path.dirname(path.abspath(__file__))
OUR_PARCEL = 'pciclass,0c0320-1.0.pef'


def dump(src, dest):
    """tbxi dump, unless src is already a dump directory."""
    if path.isdir(src):
        return src
    r = subprocess.run([sys.executable, '-m', 'tbxi', 'dump', '-o', dest, src],
                       capture_output=True, text=True)
    if not path.isdir(dest):
        raise SystemExit('FATAL: tbxi dump failed on %s\n%s%s' % (src, r.stdout, r.stderr))
    # tbxi only WARNS about a missing resource fork; surface it, because it is trap #1.
    for line in (r.stdout + r.stderr).splitlines():
        if 'resource fork' in line.lower() or 'orphan' in line.lower():
            print('  !! tbxi said: %s' % line.strip())
    return dest


def sizes(d):
    out = {}
    for f in os.listdir(d):
        p = path.join(d, f)
        if path.isfile(p):
            out[f] = path.getsize(p)
    return out


def parcels(d):
    pd = path.join(d, 'Parcels.src')
    if not path.isdir(pd):
        return {}
    return {f: (path.getsize(path.join(pd, f)) if path.isfile(path.join(pd, f)) else -1)
            for f in os.listdir(pd)}


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    base_in, out_in = sys.argv[1], sys.argv[2]
    # Default to the shipped prebuilt, then a freshly built one; override as argv[3].
    driver = sys.argv[3] if len(sys.argv) > 3 else next(
        (c for c in (path.join(HERE, '..', 'dist', 'EHCIUIM.pef'),
                     path.join(HERE, '..', 'build', 'EHCIUIM.pef')) if path.isfile(c)),
        path.join(HERE, '..', 'dist', 'EHCIUIM.pef'))

    tmp = tempfile.mkdtemp(prefix='verifyrom.')
    fails, warns = [], []
    try:
        print('base:     %s' % base_in)
        print('injected: %s' % out_in)
        print()
        b = dump(base_in, path.join(tmp, 'base'))
        o = dump(out_in, path.join(tmp, 'out'))
        bp, op = parcels(b), parcels(o)
        bs, os_ = sizes(b), sizes(o)

        # 1 + 2 — the parcel table: additive only, and the addition is ours.
        missing = sorted(set(bp) - set(op))
        added = sorted(set(op) - set(bp))
        if missing:
            fails.append('parcels DROPPED from the base: %s' % ', '.join(missing))
        else:
            print('PASS  1. all %d base parcels still present' % len(bp))
        # ★ UPDATE MODE. If the base ALREADY carries our parcel, this was a re-inject to refresh the driver, and
        # then "no parcel added" and "our parcel changed" are the CORRECT outcomes, not failures. Detecting it
        # from the base rather than a flag means the caller cannot get it wrong.
        update_mode = any(f.startswith('pciclass,0c0320') or f.startswith('EHCIUIM') for f in bp)
        if update_mode:
            print('MODE: the base already carries our parcel — verifying this as a DRIVER UPDATE, so no new')
            print('      parcel and no Parcelfile change are expected.')
        if update_mode:
            if added:
                fails.append('this is an update, but parcels were ADDED (the entry got duplicated): %s'
                             % ', '.join(added))
            else:
                print('PASS  2. update: no parcel added, so the entry was not duplicated')
        elif added == [OUR_PARCEL]:
            print('PASS  2. exactly one parcel added, and it is %s' % OUR_PARCEL)
        elif not added:
            fails.append('NO parcel was added — the driver never got injected')
        else:
            fails.append('unexpected parcels added: %s' % ', '.join(added))

        # 3 — shared parcels must be untouched.
        # ⚠ `Parcelfile` is the parcel INDEX, not a parcel, and appending our entry to it is the whole
        # injection mechanism — so it MUST differ. Checking it as "byte-identical" was a false positive in
        # the first cut of this script. What matters is that the change is strictly ADDITIVE: every line the
        # base had must survive, and the only new lines are ours.
        diff = []
        for f in sorted(set(bp) & set(op)):
            if f == 'Parcelfile':
                continue
            if update_mode and (f.startswith('pciclass,0c0320') or f.startswith('EHCIUIM')):
                continue      # our own parcel is SUPPOSED to change in an update
            fb, fo = path.join(b, 'Parcels.src', f), path.join(o, 'Parcels.src', f)
            if path.isdir(fb):
                r = subprocess.run(['diff', '-rq', fb, fo], capture_output=True)
                if r.returncode:
                    diff.append(f + '/')
            elif open(fb, 'rb').read() != open(fo, 'rb').read():
                diff.append(f)
        if diff:
            fails.append('shared parcels CHANGED (we should not have touched these): %s' % ', '.join(diff))
        else:
            print('PASS  3. every shared parcel byte-identical (Parcelfile checked separately)')

        # 3b — Parcelfile must be a strict append of our two lines.
        pb_, po_ = path.join(b, 'Parcels.src', 'Parcelfile'), path.join(o, 'Parcels.src', 'Parcelfile')
        if path.isfile(pb_) and path.isfile(po_):
            lb = [l.rstrip() for l in open(pb_, errors='replace') if l.strip()]
            lo = [l.rstrip() for l in open(po_, errors='replace') if l.strip()]
            lost = [l for l in lb if l not in lo]
            new = [l for l in lo if l not in lb]
            ours = [l for l in new if 'pciclass,0c0320' in l or 'EHCIUIM' in l]
            if lost:
                fails.append('Parcelfile LOST %d base line(s) — the index was rewritten, not appended to: %s'
                             % (len(lost), lost[0][:70]))
            elif update_mode:
                if new:
                    fails.append('this is an update, but Parcelfile gained line(s): %s'
                                 % [l[:70] for l in new])
                else:
                    print('PASS  3b. update: Parcelfile unchanged, every base line intact')
            elif not new:
                fails.append('Parcelfile gained no lines — the parcel entry was never added')
            elif len(new) != len(ours):
                fails.append('Parcelfile gained line(s) that are not ours: %s'
                             % [l[:70] for l in new if l not in ours])
            else:
                print('PASS  3b. Parcelfile is a strict append of our %d line(s), every base line intact'
                      % len(ours))

        # 4-6 — the three big non-parcel components. A lost resource fork shows up here first.
        for n, label in (('SysEnabler', 4), ('Bootscript', 5), ('MacOS.elf', 6)):
            if n not in bs:
                # ★★ A BROKEN BASE MUST NOT YIELD A PASSING OUTPUT. This compares output-to-base, so if the
                # base is ALREADY missing SysEnabler — i.e. its resource fork was lost before we ever saw it —
                # then "unchanged from the base" is true and useless: both are unbootable. That is exactly the
                # mdd-original-rom case (build trap #1), and an earlier version of this script passed it.
                # SysEnabler is where a NewWorld ROM keeps its enabler, so its absence is disqualifying.
                if n == 'SysEnabler':
                    fails.append('SysEnabler is missing from the BASE as well as the output — this base ROM has '
                                 'already lost its resource fork, so the result cannot boot no matter what we '
                                 'inject. Get a complete copy of the ROM. (If some variant legitimately has no '
                                 'SysEnabler, this check needs revisiting — but every NewWorld ROM seen so far '
                                 'has one.)')
                else:
                    warns.append('%s absent from the BASE too — nothing to compare' % n)
                    print('WARN  %d. %s not in the base dump' % (label, n))
            elif n not in os_:
                fails.append('%s is MISSING from the output (base had %d bytes) — the classic '
                             'dropped-resource-fork symptom' % (n, bs[n]))
            elif bs[n] != os_[n]:
                fails.append('%s changed size: base %d -> output %d' % (n, bs[n], os_[n]))
            else:
                print('PASS  %d. %s present and unchanged (%d bytes)' % (label, n, bs[n]))

        # 7 — the universal tell.
        if not path.isdir(base_in) and not path.isdir(out_in):
            db, do = path.getsize(base_in), path.getsize(out_in)
            if do < db:
                fails.append('output is SMALLER than the base (%d vs %d, %d KB lost) — content was '
                             'dropped' % (do, db, (db - do) // 1024))
            else:
                print('PASS  7. output no smaller than base (grew %d KB)' % ((do - db) // 1024))
        else:
            print('SKIP  7. size check needs two files (a dump directory was given)')

        # 8 — the ROM must carry the driver we believe it does.
        emb = path.join(o, 'Parcels.src', OUR_PARCEL)
        if not path.isfile(emb):
            fails.append('no %s in the output to compare' % OUR_PARCEL)
        elif not path.isfile(driver):
            warns.append('driver PEF not found at %s — skipped the cmp' % driver)
            print('WARN  8. driver PEF not found, cmp skipped')
        elif open(emb, 'rb').read() != open(driver, 'rb').read():
            fails.append('the embedded driver PEF does NOT match %s' % driver)
        else:
            print('PASS  8. embedded driver PEF cmp-clean against %s' % path.basename(driver))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print()
    for w in warns:
        print('WARN: %s' % w)
    if fails:
        print('RESULT: %d CHECK(S) FAILED — do NOT install this ROM' % len(fails))
        for f in fails:
            print('  FAIL: %s' % f)
        return 1
    print('RESULT: all checks passed — structurally sound.')
    print('        (Structural only. It says the ROM is well-formed and carries our driver; it cannot')
    print('         say the machine will boot it, or that the EHCI node matches on that hardware.)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
