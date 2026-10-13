def f(xs):
    a = {x: x for x in xs if x}
    b = {x % 3 for x in xs}
    return (a, b)
