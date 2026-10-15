# Source Generated with Decompyle++
# File: swap_locals.3.13.pyc (Python 3.13)


def main():
    a, b = 1, 2
    a, b = b, a
    print(a, b)


def g(a, b):
    a, b = b, a
    return a

