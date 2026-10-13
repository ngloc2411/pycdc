def f(a, b, c):
    x = a and b
    y = a or b
    z = a and b or c
    w = a or b and c
    if a and b:
        return 1
    if a or b:
        return 2
    if a and (b or c):
        return 3
    return not a
