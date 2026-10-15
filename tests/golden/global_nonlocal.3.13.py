# Source Generated with Decompyle++
# File: global_nonlocal.3.13.pyc (Python 3.13)

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

