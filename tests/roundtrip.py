#!/usr/bin/env python3
"""Round-trip tests: compile tests/samples/*.py with several CPython versions,
decompile with pycdc, then check (1) pycdc exits cleanly, (2) output parses,
(3) running the decompiled source prints the same as the original.

Usage: roundtrip.py [--pycdc PATH] [--versions 3.8,3.11] [--sample NAME] [-v]
"""
import argparse, glob, os, py_compile, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.realpath(__file__))
ALL = ['3.8', '3.9', '3.10', '3.11', '3.12', '3.13', '3.14']

def find_python(ver):
    for cand in (f'python{ver}', os.path.expanduser(f'~/.local/bin/python{ver}')):
        p = shutil.which(cand)
        if p:
            return p
    return None

def run(cmd, **kw):
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          timeout=60, **kw)

def check(py, pycdc, sample, tmp, verbose):
    name = os.path.splitext(os.path.basename(sample))[0]
    pyc = os.path.join(tmp, name + '.pyc')
    r = run([py, '-c', 'import py_compile,sys;py_compile.compile(sys.argv[1],sys.argv[2],doraise=True)',
             sample, pyc])
    if r.returncode:
        return 'skip', 'does not compile on this version'
    want = run([py, sample])
    if want.returncode:
        return 'skip', 'sample fails on this version'
    r = run([pycdc, pyc])
    out = r.stdout.decode('utf-8', 'replace')
    err = r.stderr.decode('utf-8', 'replace')
    dst = os.path.join(tmp, name + '.dec.py')
    with open(dst, 'w', encoding='utf-8') as f:
        f.write(out)
    if r.returncode != 0:
        return 'crash', f'exit {r.returncode}: {err[-200:]}'
    if 'Unsupported' in out or 'Unsupported' in err or 'Warning:' in out + err:
        bad = [l for l in (out + err).splitlines() if 'Unsupported' in l or 'Warning:' in l]
        return 'unsupported', '; '.join(bad[:3])
    r = run([py, '-c', 'import ast,sys;ast.parse(open(sys.argv[1],encoding="utf-8").read())', dst])
    if r.returncode:
        return 'syntax', r.stderr.decode().strip().splitlines()[-1]
    got = run([py, dst])
    if got.returncode or got.stdout != want.stdout:
        why = got.stderr.decode(errors='replace').strip().splitlines()[-1:] or ['output differs']
        return 'wrong', why[0]
    return 'ok', ''

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pycdc', default=os.path.join(HERE, '..', 'build', 'pycdc'))
    ap.add_argument('--versions', default=','.join(ALL))
    ap.add_argument('--sample')
    ap.add_argument('-v', action='store_true')
    a = ap.parse_args()
    samples = sorted(glob.glob(os.path.join(HERE, 'samples', (a.sample or '*') + '.py')))
    totals = {}
    with tempfile.TemporaryDirectory() as tmp:
        for ver in a.versions.split(','):
            py = find_python(ver)
            if not py:
                print(f'[{ver}] interpreter not found, skipped'); continue
            vt = os.path.join(tmp, ver); os.makedirs(vt)
            for s in samples:
                st, msg = check(py, a.pycdc, s, vt, a.v)
                totals[st] = totals.get(st, 0) + 1
                if st not in ('ok', 'skip'):
                    print(f'[{ver}] {os.path.basename(s):24} {st:12} {msg}')
                elif a.v:
                    print(f'[{ver}] {os.path.basename(s):24} {st}')
    print('summary:', ', '.join(f'{k}={v}' for k, v in sorted(totals.items())))
    return 1 if any(k not in ('ok', 'skip') for k in totals) else 0

if __name__ == '__main__':
    sys.exit(main())
