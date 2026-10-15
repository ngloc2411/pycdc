# Source Generated with Decompyle++
# File: unpack_star.3.10.pyc (Python 3.10)


def use(*a):
    return a


def un_basic(a):
    x, y = a
    p, q, r = a
    return use(x, y, p, q, r)


def un_nested(a):
    (r, s), t = a
    u, v = a
    (m, n), (o, p) = a
    return use(r, s, t, u, v, m, n, o, p)


def un_star_tail(a):
    first, *rest = a
    return use(first, rest)


def un_star_head(a):
    *init, last = a
    return use(init, last)


def un_star_mid(a):
    first, *mid, last = a
    return use(first, mid, last)


def un_star_only(a):
    x, y, *z = a
    return use(x, y, z)


def un_star_nested(a):
    (p, *q), r = a
    return use(p, q, r)


def un_attr(o, a):
    o.x, o.y = a
    return o


def un_sub(d, a):
    d[0], d[1] = a
    return d


def un_mixed(o, d, a):
    o.x, d[0], z = a
    return use(o, d, z)


def un_swap(a, b):
    a, b = b, a
    return use(a, b)


def un_swap3(a, b, c):
    a, b, c = c, a, b
    return use(a, b, c)


def un_literal():
    x, y = 1, 2
    return use(x, y)


def un_for(items):
    for k, v in items:
        use(k, v)
    for a, (b, c) in items:
        use(a, b, c)
    for first, *rest in items:
        use(first, rest)


def un_comp(items):
    return [ k + v for k, v in items ]


def un_chain(a):
    x = y = a
    p = q = r = use(a)
    return use(x, y, p, q, r)


def un_chain_unpack(a):
    (x, y) = z = a
    return use(x, y, z)


def un_with_assign(a):
    x = (a, 1)
    y = ((a, 2), 3)
    return use(x, y)


def un_star_call(a, b):
    return use(*a, *b)


def un_star_list(a, b):
    x = [
        *a,
        *b]
    y = [
        *a,
        1,
        2]
    z = [
        0,
        *a]
    return use(x, y, z)


def un_star_tuple(a, b):
    x = (*a, *b)
    y = (*a, 1)
    z = (0, *a, 2)
    return use(x, y, z)


def un_star_set(a, b):
    x = {
        *a,
        *b}
    y = {
        *a,
        1}
    return use(x, y)


def un_dict_star(a, b):
    x = {
        **a,
        **b }
    y = {
        **a,
        'k': 1 }
    z = {
        'k': 1,
        **a }
    return use(x, y, z)


def un_call_kw(f, a, b):
    f(**a)
    f(1, **a)
    f(*a, **b)
    f(a, *b, c = 1, **b)
    f(k = 1, **a)
    return f()

