f = lambda a, b=1: a * b
g = lambda: None
h = lambda *a, **k: (a, k)
print(list(map(lambda v: v + 1, [1, 2])))
