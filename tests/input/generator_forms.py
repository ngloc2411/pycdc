def gen(n):
    for i in range(n):
        yield i
    yield from range(2)
def gen2():
    x = yield 1
    print(x)
