# Source Generated with Decompyle++
# File: assert_raise.3.8.pyc (Python 3.8)


def f(a):
    assert a == 4, 'msg'
    assert a
    raise ValueError('x') from None

