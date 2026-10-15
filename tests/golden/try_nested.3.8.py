# Source Generated with Decompyle++
# File: try_nested.3.8.pyc (Python 3.8)


def f():
    
    try:
        
        try:
            raise RuntimeError('a')
        except RuntimeError:
            raise ValueError('b')

    except ValueError:
        pass


