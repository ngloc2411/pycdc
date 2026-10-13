#!/usr/bin/env python3
"""Create/refresh pycdc test fixtures in the upstream layout.

For each given test name (tests/input/NAME.py) and each requested CPython
version this script
  1. writes tokenized/NAME.txt (token_dump of the input source),
  2. compiles the input with that interpreter,
  3. decompiles it with pycdc and checks the token stream matches,
  4. stores the .pyc in compiled/ if it passes, or xfail/ if it does not.

Re-run it after fixing pycdc to promote xfail entries to compiled/.

Usage: mktest.py [--pycdc build/pycdc] [--versions 3.8,3.11] NAME [NAME ...]
       mktest.py                 (no names: every input that has no fixture yet)
"""
import argparse, glob, os, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.realpath(__file__))
sys.path.insert(0, HERE)
import run_tests

ALL = ['3.8', '3.9', '3.10', '3.11', '3.12', '3.13', '3.14']

def find_python(ver):
    for cand in (f'python{ver}', os.path.expanduser(f'~/.local/bin/python{ver}')):
        p = shutil.which(cand)
        if p:
            return p

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pycdc', default=os.path.join(HERE, '..', 'build', 'pycdc'))
    ap.add_argument('--versions', default=','.join(ALL))
    ap.add_argument('names', nargs='*')
    a = ap.parse_args()
    pycdc = os.path.realpath(a.pycdc)
    # run_tests.decompyle_one launches ./pycdc from the cwd
    bindir = tempfile.mkdtemp()
    os.symlink(pycdc, os.path.join(bindir, 'pycdc'))
    os.chdir(bindir)
    def has_fixture(n):
        return any(glob.glob(os.path.join(HERE, d, n + '.?.*.pyc')) for d in ('compiled', 'xfail'))
    names = a.names or sorted(n for n in (os.path.splitext(os.path.basename(f))[0]
                                          for f in glob.glob(os.path.join(HERE, 'input', '*.py')))
                              if not has_fixture(n))
    for name in names:
        src = os.path.join(HERE, 'input', name + '.py')
        tok = subprocess.run([sys.executable, os.path.join(run_tests.SCRIPTS_DIR, 'token_dump'), src],
                             stdout=subprocess.PIPE, universal_newlines=True, encoding='utf-8')
        if tok.returncode:
            print(f'{name}: token_dump failed'); continue
        with open(os.path.join(HERE, 'tokenized', name + '.txt'), 'w', encoding='utf-8') as f:
            f.write(tok.stdout)
        res = []
        for ver in a.versions.split(','):
            py = find_python(ver)
            if not py:
                continue
            for d in ('compiled', 'xfail'):
                old = os.path.join(HERE, d, f'{name}.{ver}.pyc')
                if os.path.exists(old):
                    os.unlink(old)
            tmp = os.path.join(bindir, f'{name}.{ver}.pyc')
            r = subprocess.run([py, '-c', 'import py_compile,sys;py_compile.compile(sys.argv[1],sys.argv[2],dfile=sys.argv[3],doraise=True)',
                                src, tmp, name + '.py'], stderr=subprocess.PIPE)
            if r.returncode:
                continue  # syntax not available on this version
            ok, _ = run_tests.decompyle_one(name, tmp, bindir, tok.stdout)
            dest = os.path.join(HERE, 'compiled' if ok else 'xfail', f'{name}.{ver}.pyc')
            shutil.copy(tmp, dest)
            res.append(f'{ver}:{"ok" if ok else "XFAIL"}')
        print(f'{name:36} ' + ' '.join(res))

if __name__ == '__main__':
    main()
