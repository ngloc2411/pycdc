# Source Generated with Decompyle++
# File: store_slice.3.12.pyc (Python 3.12)

a = [
    0] * 16
l = [
    1,
    2,
    3]
a[13:] = l
a[5] = 10
a[:2] = [
    1,
    2]
a[:] = range(16)
print(a)
