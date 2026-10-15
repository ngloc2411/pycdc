# Source Generated with Decompyle++
# File: closure_py3.3.12.pyc (Python 3.12)


def outer(x):
    y = 2
    
    def inner(a):
        return x + y + a

    return inner

