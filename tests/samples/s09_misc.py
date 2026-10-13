import os.path
from collections import OrderedDict as OD
from os import path as p, sep

def kwonly(a, /, b, *, c):
    return a, b, c

def annotated(a: int, b: str = "x") -> str:
    v: int = 3
    return b * v

def defaults(a, l=None, d={}):
    return l, d

def chain(x):
    return 0 < x < 10 != 5

def swap_ops(a):
    a += 1; a -= 2; a *= 3; a //= 2; a %= 7; a |= 1; a &= 7; a ^= 2; a <<= 1; a >>= 1
    return a

def ternary_nested(x):
    return "a" if x < 0 else "b" if x == 0 else "c"

def del_attr():
    class O: pass
    o = O(); o.x = 1; del o.x
    return hasattr(o, "x")

def main():
    print(kwonly(1, 2, c=3), annotated(1), defaults(1), chain(3), swap_ops(5))
    print(ternary_nested(-1), ternary_nested(0), ternary_nested(1), del_attr())
    print(OD(a=1), p.basename("/a/b"), sep, os.path.sep)

main()
