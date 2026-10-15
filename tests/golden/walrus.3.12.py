# Source Generated with Decompyle++
# File: walrus.3.12.pyc (Python 3.12)


def f(data, items):
    if (n := len(data)) > 3:
        print(n)
    while (chunk := data.pop()):
        print(chunk)
    total = [ y for x in items if (y := x * 2) > 1 ]
    print(total)
    if (a := items[0]) and (b := items[1]):
        print(a, b)
    return n


def g(m):
    print((k := m + 1), k)
    z = (w := m) + 1
    return z, w

