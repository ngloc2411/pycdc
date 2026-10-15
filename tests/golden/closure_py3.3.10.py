# Source Generated with Decompyle++
# File: closure_py3.3.10.pyc (Python 3.10)


def outer(x):
    y = 2
    
    def inner(a):
        return x + y + a

    return inner

