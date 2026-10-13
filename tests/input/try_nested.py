def f():
    try:
        try:
            raise RuntimeError('a')
        except RuntimeError:
            raise ValueError('b')
    except ValueError:
        pass
