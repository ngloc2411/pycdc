# Source Generated with Decompyle++
# File: try_except_finally.2.6.pyc (Python 2.6)


try:
    import sys
    
    try:
        print 'something else'
    except AssertionError:
        print '...failed'

except ImportError:
    print 'Oh Noes!'
finally:
    print 'Exiting'

