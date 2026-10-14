def outer(x):
    y = 2

    def inner(a):
        return x + y + a
    return inner
