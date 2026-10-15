# Source Generated with Decompyle++
# File: multi_assign.3.9.pyc (Python 3.9)


def f(a, b):
    x = y = a
    p, q = a
    (r, s), t = a, 1
    first, *rest = a
    m, (n, o) = a, b
    (c, d), (e, g) = a, b
    k = l = m = a + b
    print(x, y, p, q, r, s, t, first, rest, m, n, o, c, d, e, g, k, l)
    return a

