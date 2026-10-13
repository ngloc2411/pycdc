class A:
    def __init__(self, v):
        self.v = v
class B(A):
    def __init__(self, v, w):
        super().__init__(v)
        self.w = w
    def m(self):
        return super().__repr__()
