# Source Generated with Decompyle++
# File: assert_raise.3.9.pyc (Python 3.9)


def f(a):
    assert a == 4, 'msg'
    assert a
    raise ValueError('x') from None

