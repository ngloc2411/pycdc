def f(xs):
    a = [x * 2 for x in xs]
    b = [x for x in xs if x % 2 if x > 1]
    c = [(x, y) for x in xs for y in xs]
    d = [x if x else -x for x in xs]
    return (a, b, c, d)
