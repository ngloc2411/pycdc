def f(a, b):
    with a as x, b as y:
        print(x, y)
