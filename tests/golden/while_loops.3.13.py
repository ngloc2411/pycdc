# Source Generated with Decompyle++
# File: while_loops.3.13.pyc (Python 3.13)


def plain(n):
    j = 0
    while j < n:
        j += 1
    return j


def with_break(n):
    j = 0
    while j < n:
        j += 1
        if j > 5:
            break
    print(j)
    return j


def with_continue(n):
    j = 0
    while j < n:
        j += 1
        if j % 2:
            continue
        print(j)
    return j


def forever(n):
    j = 0
    while True:
        j += 1
        if j > n:
            break
        print(j)
    print('end')


def while_else(n):
    j = 0
    while j < n:
        j += 1
        if j == 7:
            break
    else:
        print('no break')
    print('after')


def nested(n):
    total = 0
    i = 0
    while i < n:
        k = 0
        while k < i:
            k += 1
            if k == 3:
                break
            total += k
        i += 1
    print(total)


def if_else_in_loop(n):
    j = 0
    while j < n:
        if j % 2:
            print('odd')
        else:
            print('even')
        j += 1
    print('done')


def for_in_while(items):
    n = 0
    while n < 3:
        for x in items:
            print(x)
        n += 1
    print(n)


def while_not(flag):
    while not flag:
        flag = step(flag)
    print(flag)


def while_call(q):
    while q.pending():
        q.pop()
    print('empty')


def break_continue_mix(n):
    j = 0
    while j < n:
        j += 1
        if j < 3:
            continue
        if j > 8:
            break
        print(j)
    print('x')

