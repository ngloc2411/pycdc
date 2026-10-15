#!/usr/bin/env python3
"""Compare pycdc's per-version opcode maps (bytes/python_3_X.cpp) with the
real opcode tables of the CPython interpreters installed on this machine.

  check_opcode_maps.py            report differences for 3.8 .. 3.14
  check_opcode_maps.py --emit 3.14   print a bytes/python_3_14.cpp skeleton

Opcodes that take an argument are named NAME_A in pycdc.
"""
import argparse, json, os, re, shutil, subprocess, sys

HERE = os.path.dirname(os.path.realpath(__file__))
BYTES = os.path.join(HERE, '..', 'cpp', 'bytes')

DUMP = r'''
import opcode, json, sys
print(json.dumps({"map": opcode.opmap, "have_arg": opcode.HAVE_ARGUMENT,
                  "hasarg": [n for n, v in opcode.opmap.items() if v >= opcode.HAVE_ARGUMENT]}))
'''

def interp(ver):
    for cand in (f'python{ver}', os.path.expanduser(f'~/.local/bin/python{ver}')):
        p = shutil.which(cand)
        if p:
            return p

def real_table(ver):
    py = interp(ver)
    if not py:
        return None
    out = subprocess.run([py, '-c', DUMP], stdout=subprocess.PIPE, check=True).stdout
    return json.loads(out)

def pycdc_table(ver):
    path = os.path.join(BYTES, 'python_%s.cpp' % ver.replace('.', '_'))
    if not os.path.exists(path):
        return None
    tbl = {}
    for m in re.finditer(r'MAP_OP\((\d+),\s*(\w+)\)', open(path).read()):
        tbl[int(m.group(1))] = m.group(2)
    return tbl

def known_names():
    names = set()
    inl = open(os.path.join(HERE, '..', 'cpp', 'bytecode_ops.inl')).read()
    for m in re.finditer(r'OPCODE(_A_FIRST|_A)?\((\w+)\)', inl):
        names.add(m.group(2) + ('_A' if m.group(1) else ''))
    return names

KNOWN = known_names()

def pycdc_name(name, has_arg):
    """pycdc spells opcodes that take an argument NAME_A. Some opcodes changed
    between taking and not taking an argument across versions (e.g. 3.14's
    CALL_FUNCTION_EX); fall back to whichever spelling pycdc defines."""
    first, second = (name + '_A', name) if has_arg else (name, name + '_A')
    if first in KNOWN or second not in KNOWN:
        return first
    return second

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--emit')
    ap.add_argument('--versions', default='3.8,3.9,3.10,3.11,3.12,3.13,3.14')
    a = ap.parse_args()
    if a.emit:
        t = real_table(a.emit)
        maj, mnr = a.emit.split('.')
        print('#include "bytecode_map.h"\n\nBEGIN_MAP(%s, %s)' % (maj, mnr))
        for name, num in sorted(t['map'].items(), key=lambda kv: kv[1]):
            if num > 255:      # compiler pseudo-instructions never reach a .pyc
                continue
            print('    MAP_OP(%d, %s)' % (num, pycdc_name(name, num >= t['have_arg'])))
        print('END_MAP()')
        return
    bad = 0
    for ver in a.versions.split(','):
        real, ours = real_table(ver), pycdc_table(ver)
        if real is None:
            print(f'[{ver}] interpreter missing'); continue
        if ours is None:
            print(f'[{ver}] no pycdc map'); bad += 1; continue
        for name, num in sorted(real['map'].items(), key=lambda kv: kv[1]):
            if num > 255:
                continue
            want = pycdc_name(name, num >= real['have_arg'])
            have = ours.get(num)
            if have != want:
                bad += 1
                print(f'[{ver}] opcode {num:3}: CPython {want:34} pycdc {have}')
        for num, name in ours.items():
            if num not in {v for v in real['map'].values()}:
                bad += 1
                print(f'[{ver}] opcode {num:3}: pycdc has {name}, CPython has nothing')
    print('differences:', bad)
    return 1 if bad else 0

if __name__ == '__main__':
    sys.exit(main())
