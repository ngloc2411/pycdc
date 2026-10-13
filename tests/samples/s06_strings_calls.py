def main():
    name, n = "x", 3
    print(f"{name}={n!r:>5} {n + 1}")
    print("%s-%d" % (name, n), "{}-{}".format(name, n))
    print("a" "b", 'q"q', "it's", b"by", r"\n", """tri
ple""")
    print(1_000, 0x1F, 1e3, 1.5j, True, None, ...)
    d = {"a": 1, **{"b": 2}}
    l = [1, *range(3)]
    t = (1,)
    s = {1, 2, *l}
    print(d, l, t, sorted(s))
    print(l[1:], l[:2], l[::2], l[-1], d["a"])
    l[0] = 9; del l[1]; d["c"] = 3; del d["a"]
    print(l, d)
    print(max(*l, key=lambda v: -v), dict(a=1, **d))
    a = b = 4
    print(a, b, -a, ~a, a // 3, a % 3, a << 1, a & 3, a | 8, a ^ 1)
    print(len(l) > 1 and "yes" or "no")
    assert a == 4, "msg"

main()
