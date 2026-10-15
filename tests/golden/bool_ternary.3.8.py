# Source Generated with Decompyle++
# File: bool_ternary.3.8.pyc (Python 3.8)


def or_and(a, b, c):
    r = a or b and c
    return use(r) + use(r)


def paren_groups(a, b, c, d):
    r = (a or b) and (c or d)
    return use(r) + use(r)


def and_chain_or(a, b, c, d, e):
    r = a and b and c or d and e
    return use(r) + use(r)


def not_group(a, b):
    r = not (a and b)
    return use(r) + use(r)


def not_or(a, b):
    r = not a or b
    return use(r) + use(r)


def tern_and_cond(a, b, x, y):
    r = x if a and b else y
    return use(r) + use(r)


def tern_or_cond(a, b, x, y):
    r = x if a or b else y
    return use(r) + use(r)


def tern_in_arith(a, x, y):
    r = (x if a else y) + 1
    return use(r) + use(r)


def tern_in_call(f, c, a, b, d, e):
    r = f(a if c else b, d or e)
    return use(r) + use(r)


def tern_then_nested(a, b, c, d, e):
    r = (b if c else d) if a else e
    return use(r) + use(r)


def tern_chain(a, b, c, x, y, z):
    r = x if a else y if b else z if c else 0
    return use(r) + use(r)


def calls_bool(f, g):
    r = f() or g()
    s = f() and g()
    return use(r) + use(s)


def compare_and(a, b, c):
    r = a < b and b < c
    return use(r) + use(r)


def is_none_or(a, b):
    r = a is None or b
    return use(r) + use(r)


def default_arg(x, default):
    x = x or default
    return use(x) + use(x)


def tern_in_lambda(a):
    return lambda x: 1 if x else a


def tern_in_comp(items):
    r = [ x if x else -1 for x in items ]
    return use(r) + use(r)


def tern_none_test(a, b):
    r = 1 if a is None else b
    return use(r) + use(r)


def tern_not_none_test(a, b):
    r = 1 if a is not None else b
    return use(r) + use(r)


def bool_in_cond(a, b, c):
    if a and b or c:
        use(a)
    if not (a or b):
        use(b)
    use(c)


def bool_in_while(a, b):
    while a and not b:
        a = use(a)
    use(b)


def bool_assign_tern_str(a):
    s = 'yes' if a else 'no'
    t = 'yes' if not a else 'no'
    return use(s) + use(t)


def tern_arg_default_mix(a, b, c):
    r = [
        a or b,
        c and a,
        b if c else a]
    return use(r) + use(r)


def tern_nested_cond(a, b, x, y, z):
    r = (x if a else y) if b else z
    return use(r) + use(r)


def tern_nested_both(a, b, c, x, y, z, w):
    r = (x if a else y) if b else z if c else w
    return use(r) + use(r)


def tern_in_args(a, x, y):
    r = use(x if a else y, y if a else x)
    return use(r) + use(r)

