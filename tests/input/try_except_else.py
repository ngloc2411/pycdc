def f():
    try:
        a = 1
    except ValueError:
        a = 2
    else:
        a = 3
    return a
