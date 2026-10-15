# Source Generated with Decompyle++
# File: closure_py3.3.8.pyc (Python 3.8)


def outer(x):
    y = 2
    
    def inner(a):
        return x + y + a

    return inner

