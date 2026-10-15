# Source Generated with Decompyle++
# File: try_nested.3.9.pyc (Python 3.9)


def f():
    
    try:
        
        try:
            raise RuntimeError('a')
        except RuntimeError:
            raise ValueError('b')

    except ValueError:
        pass


