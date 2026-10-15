# Source Generated with Decompyle++
# File: aug_assign.3.9.pyc (Python 3.9)


def use(*a):
    return a


def aug_idx(a, b):
    a[0] -= b


def aug_idx_expr(a, b, i):
    a[i + 1] *= b


def aug_slice(a, b):
    a[1:3] += b


def aug_slice_open(a, b):
    a[:2] += b
    a[2:] += b
    a[:] += b


def aug_slice_step(a, b):
    a[::2] += b


def aug_attr(o, b):
    o.x += b
    o.y.z -= b


def aug_attr_call(o, b):
    use(o).x += b


def aug_sub_call(a, b):
    a[use(1)] += b


def aug_nested(a, b, i, j):
    a[i][j] += b


def aug_global_like(a, b):
    a[b] //= 2
    a[b] <<= 1
    a[b] |= 3


def aug_tuple_idx(a, b):
    a[1, 2] += b

