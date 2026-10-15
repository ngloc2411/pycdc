# Source Generated with Decompyle++
# File: slices_forms.3.13.pyc (Python 3.13)


def use(*a):
    return a


def sl_basic(a):
    return use(a[1:2], a[:2], a[1:], a[:], a[::2], a[1:2:3], a[::-1])


def sl_neg(a, n):
    return use(a[-1], a[:-1], a[-2:], a[n:], a[:n], a[n:n + 1])


def sl_tuple(a):
    return use(a[:, 1], a[1:2, ::3], a[1, 2], a[..., 0])


def sl_store(a, b):
    a[1:2] = b
    a[::2] = b
    a[0] = b


def sl_del(a):
    del a[1:2]
    del a[::2]
    del a[0]


def sl_aug(a, b):
    a[1:3] += b
    a[0] -= b


def sl_nested(a, i, j):
    return a[i][j:i][::2]


def sl_expr(a, i):
    return a[i + 1:i * 2]


def sl_call(a):
    return use(a)[1:2]


def sl_attr(o):
    return o.items[1:], o.items[:-1]


def sl_str(s):
    return s[1:3] + s[::-1]


def sl_cond(a, c):
    if a[1:2]:
        use(1)
    r = a[:c] if c else a[c:]
    return use(r) + use(r)


def sl_loop(a):
    for x in a[1:]:
        use(x)
    return [ y for y in a[::2] ]


def sl_ellipsis(a):
    return a[..., 1:2]


def sl_single(a):
    return a[1,]

