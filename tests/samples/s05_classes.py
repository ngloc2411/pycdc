import functools

def deco(fn):
    @functools.wraps(fn)
    def wrapper(*a, **k):
        return fn(*a, **k) + 1
    return wrapper

class Base:
    count = 0
    def __init__(self, v):
        self.v = v
        Base.count += 1
    @property
    def double(self):
        return self.v * 2
    @staticmethod
    def sm(x):
        return x + 1
    @classmethod
    def cm(cls, x):
        return cls(x)
    def __repr__(self):
        return f"{type(self).__name__}({self.v})"

class Child(Base):
    def __init__(self, v, w=0):
        super().__init__(v)
        self.w = w
    @deco
    def calc(self):
        return self.v + self.w

def counter():
    n = 0
    def inc():
        nonlocal n
        n += 1
        return n
    return inc

G = 0
def setg():
    global G
    G = 7

def gen(n):
    for i in range(n):
        yield i
    yield from range(2)

def main():
    c = Child(1, 2)
    print(c, c.double, c.calc(), Base.sm(1), Base.cm(4))
    inc = counter(); inc()
    print(inc(), Base.count)
    setg(); print(G, list(gen(3)))
    print(isinstance(c, Base), c.__dict__)

main()
