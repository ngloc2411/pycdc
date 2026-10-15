# Source Generated with Decompyle++
# File: closure_py3.3.13.pyc (Python 3.13)


def outer(x):
    y = 2
    
    def inner(a):
        return x + y + a

    return inner

