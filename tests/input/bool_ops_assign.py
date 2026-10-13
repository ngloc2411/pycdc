def f(a, b):
    x = a or {}
    y = a and b
    z = (a or b) and a
    return (x, y, z)
