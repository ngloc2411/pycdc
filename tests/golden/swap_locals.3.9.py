# Source Generated with Decompyle++
# File: swap_locals.3.9.pyc (Python 3.9)


def main():
    a, b = 1, 2
    a, b = b, a
    print(a, b)


def g(a, b):
    a, b = b, a
    return a

