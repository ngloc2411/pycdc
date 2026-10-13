def deco(fn):
    return fn
@deco
def a():
    pass
class C:
    @property
    def p(self):
        return 1
    @staticmethod
    def s(x):
        return x
    @classmethod
    def c(cls, x):
        return cls
