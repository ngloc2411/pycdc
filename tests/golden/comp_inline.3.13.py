# Source Generated with Decompyle++
# File: comp_inline.3.13.pyc (Python 3.13)


def f(xs, ys):
    a = [ x + 1 for x in xs ]
    b = [ x for x in xs if x ]
    c = [ (x, y) for x in xs for y in ys ]
    d = { x for x in xs }
    e = { x: x for x in xs }
    return [
        a,
        b,
        c,
        d,
        e]

