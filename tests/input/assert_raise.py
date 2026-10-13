def f(a):
    assert a == 4, 'msg'
    assert a
    raise ValueError('x') from None
