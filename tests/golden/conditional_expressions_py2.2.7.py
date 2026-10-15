# Source Generated with Decompyle++
# File: conditional_expressions_py2.2.7.pyc (Python 2.7)

import sys
a = 1
result = 'even' if a % 2 == 0 else 'odd'
print result
a = 2
result = 'even' if a % 2 == 0 else 'odd'
print result
a = 1
result = a * 2 if a % 2 == 0 else a * 3
print result
a = 2
result = a * 2 if a % 2 == 0 else a * 3
print result
a = 1
sys.stdout.write('even\n') if a % 2 == 0 else sys.stdout.write('odd\n')
a = 1
if a % 2 == 0:
    print 'even'
else:
    print 'odd'
a = -2
result = 'negative and even' if a < 0 and a % 2 == 0 else 'positive or odd'
print result
a = -1
result = 'negative and even' if a < 0 and a % 2 == 0 else 'positive or odd'
print result
