# Source Generated with Decompyle++
# File: iter_unpack.2.7.pyc (Python 2.7)


def map_with_index(func, lst):
    res = []
    for i, x in enumerate(lst):
        res.append(func(i, x))
    
    return res

