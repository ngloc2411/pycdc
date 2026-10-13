G = 0
def setg():
    global G
    G = 7
def counter():
    n = 0
    def inc():
        nonlocal n
        n += 1
        return n
    return inc
