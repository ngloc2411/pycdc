# Source Generated with Decompyle++
# File: chained_compare.3.10.pyc (Python 3.10)


def use(*a):
    return a


def chain2(a, b, c):
    return a < b < c


def chain_eq(a, b, c):
    return a == b == c


def chain_mixed(a, b, c):
    return a <= b < c


def chain3(a, b, c, d):
    return a < b < c < d


def chain_in(a, b, c):
    return a in b in c


def chain_is(a, b, c):
    return a is b is c


def chain_consts(x):
    return 0 < x < 10


def chain_expr(a, b, c):
    return a + 1 < b * 2 < c - 3


def chain_call(a, b, c):
    return use(a) < use(b) < use(c)


def chain_if(a, b, c):
    if a < b < c:
        use(1)
    use(2)


def chain_if_else(a, b, c):
    if a < b < c:
        use(1)
    else:
        use(2)
    use(3)


def chain_not(a, b, c):
    if not a < b < c:
        use(1)
    use(2)


def chain_and(a, b, c, d):
    if a < b < c and d:
        use(1)
    use(2)


def chain_or(a, b, c, d):
    if a < b < c or d:
        use(1)
    use(2)


def chain_assign(a, b, c):
    r = a < b < c
    return use(r) + use(r)


def chain_while(a, b, c):
    while a < b < c:
        a = a + 1
    use(a)


def chain_in_ternary(a, b, c, x, y):
    r = x if a < b < c else y
    return use(r) + use(r)


def chain_attr(o, lo, hi):
    return lo <= o.v <= hi


def chain_neq(a, b, c):
    return a != b != c


def chain_notin(a, b, c):
    return a not in b not in c


def chain_elif(a, b, c, d):
    if a < b < c:
        use(1)
    elif b < c < d:
        use(2)
    else:
        use(3)
    use(4)


def chain4_if(a, b, c, d):
    if a < b < c < d:
        use(1)
    use(2)


def chain_arg(a, b, c):
    r = use(a < b < c, b)
    return use(r) + use(r)


def chain_value_in_loop(items, lo, hi):
    out = []
    for x in items:
        out.append(lo < x < hi)
    return out


def chain_break(items, lo, hi):
    for x in items:
        if x < lo < hi:
            break
        use(x)
    use(0)


def chain_continue(items, lo, hi):
    for x in items:
        if lo < x < hi:
            continue
        use(x)
    use(0)


def chain_and_chain(a, b, c, d, e):
    if a < b < c and c < d < e:
        use(1)
    use(2)


def chain_or_chain(a, b, c, d, e):
    if a < b < c or c < d < e:
        use(1)
    use(2)


def chain_while_and(a, b, c, d):
    while a < b < c and d:
        a = a + 1
    use(a)


def chain_while_break(a, b, c):
    while a < b < c:
        if a == 3:
            break
        a = a + 1
    use(a)


def chain_neg_value(a, b, c):
    r = not a < b < c
    return use(r) + use(r)


def chain_ret_if(a, b, c):
    if a < b < c:
        return 1
    return 2


def chain_ternary_value(a, b, c, x, y):
    r = x if a < b < c else y
    return use(r) + use(r)


def chain_mixed_ops(a, b, c):
    r = a is not b == c
    return use(r) + use(r)


def chain_value_or(a, b, c, d):
    r = a < b < c or d
    return use(r) + use(r)


def chain_value_and(a, b, c, d):
    r = d and a < b < c
    return use(r) + use(r)

