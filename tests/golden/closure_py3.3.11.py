# Source Generated with Decompyle++
# File: closure_py3.3.11.pyc (Python 3.11)


def outer(x):
    y = 2
    
    def inner(a):
        return x + y + a

    return inner

