# Source Generated with Decompyle++
# File: genexpr_call.3.9.pyc (Python 3.9)


def f(xs):
    print(sum(x for x in xs))
    print(any(x > 4 for x in xs))
    return sorted(x for x in xs if x)

