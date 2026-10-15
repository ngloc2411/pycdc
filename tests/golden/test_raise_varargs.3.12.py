# Source Generated with Decompyle++
# File: test_raise_varargs.3.12.pyc (Python 3.12)

import struct

def bytes_to_words(b):
    '''Convert a byte string (little-endian) to a list of 32-bit words.'''
    if len(b) % 4 != 0:
        raise ValueError('Input bytes length must be a multiple of 4 for word conversion.')
    return struct.unpack('<' + 'I' * (len(b) // 4), b)

