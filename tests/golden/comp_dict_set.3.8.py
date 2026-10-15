# Source Generated with Decompyle++
# File: comp_dict_set.3.8.pyc (Python 3.8)


def f(xs):
    a = { x: x for x in xs if x }
    b = { x % 3 for x in xs }
    return a, b

