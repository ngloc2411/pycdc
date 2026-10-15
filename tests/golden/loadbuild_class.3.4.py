# Source Generated with Decompyle++
# File: loadbuild_class.3.4.pyc (Python 3.4)


class C:
    
    def foobar(self):
        a = 1



class B(C):
    
    def barfoo(self):
        d = 1



class F(C, B):
    
    def raboof(self):
        e = 1


C.foobar()
F.raboof()
B.foobar()
