def f(a):
    try:
        with a:
            raise ValueError('x')
    except ValueError:
        print('handled')
