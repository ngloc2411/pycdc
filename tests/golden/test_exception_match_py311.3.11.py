# Source Generated with Decompyle++
# File: test_exception_match_py311.3.11.pyc (Python 3.11)


def handle_specific(flag):
    
    try:
        if flag:
            raise ValueError('boom')
    except ValueError:
        return 'value'

    return 'ok'

