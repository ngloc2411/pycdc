def f():
    try:
        1 / 0
    except ZeroDivisionError:
        raise
