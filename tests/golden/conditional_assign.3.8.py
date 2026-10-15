# Source Generated with Decompyle++
# File: conditional_assign.3.8.pyc (Python 3.8)


def f(a, b):
    x = a if a else None
    print(x)
    y = a if a > 1 else b if b else 0
    z = a and b or 3
    w = a and b if b else a
    v = a or b if b else a
    u = a if b else b and a
    print(y, z, w, v, u)
    return a

