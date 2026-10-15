# Source Generated with Decompyle++
# File: for_loops.3.12.pyc (Python 3.12)


def for_break(items):
    for x in items:
        if x > 3:
            break
        print(x)
    print('end')


def for_continue(items):
    for x in items:
        if x % 2:
            continue
        print(x)
    print('end')


def for_else(items):
    for x in items:
        if x == 'a':
            break
    else:
        print('not found')
    print('after')


def for_else_nobreak(items):
    for x in items:
        print(x)
    print('after')


def nested_for_break(rows):
    for row in rows:
        for cell in row:
            if cell is None:
                break
            print(cell)
        print('row')
    print('end')


def for_unpack_break(pairs):
    for k, v in pairs:
        if k == v:
            break
        print(k, v)
    print('done')


def for_while_break(items):
    for x in items:
        n = 0
        while n < x:
            n += 1
            if n == 5:
                break
        print(n)


def for_break_continue(items):
    total = 0
    for x in items:
        if x < 0:
            continue
        if x > 100:
            break
        total += x
    print(total)


def for_return(items):
    for x in items:
        if x:
            return x
        print('skip')
    return 0


def for_two_breaks(items):
    for x in items:
        if x == 1:
            break
        if x == 2:
            break
        print(x)
    print('done')

