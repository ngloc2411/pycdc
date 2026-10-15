# Source Generated with Decompyle++
# File: decorators_forms.3.8.pyc (Python 3.8)


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


