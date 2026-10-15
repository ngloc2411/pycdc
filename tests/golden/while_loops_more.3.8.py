# Source Generated with Decompyle++
# File: while_loops_more.3.8.pyc (Python 3.8)


def and_cond(a, b):
    while a and b:
        a = a - 1
        b = b - 1
    return a


def or_cond(a, b):
    while a or b:
        a = a - 1
    print(a)


def elif_chain(n):
    j = 0
    while j < n:
        if j == 1:
            print('one')
        elif j == 2:
            print('two')
        else:
            print('many')
        j += 1
    print('done')


def true_continue(q):
    while True:
        item = q.get()
        if item is None:
            continue
        if item == 'stop':
            break
        print(item)
    print('stopped')


def loop_in_for(rows):
    for row in rows:
        k = 0
        while k < len(row):
            if row[k] == 0:
                break
            k += 1
        print(k)


def break_in_inner_for(n):
    j = 0
    while j < n:
        for x in range(j):
            if x == 2:
                break
            print(x)
        j += 1
    print('end')


def three_levels(n):
    a = 0
    while a < n:
        b = 0
        while b < n:
            c = 0
            while c < n:
                c += 1
                if c == 2:
                    break
            b += 1
        a += 1
    print(a)


def nested_else(n):
    i = 0
    while i < n:
        j = 0
        while j < n:
            j += 1
            if j == 4:
                break
        else:
            print('inner done')
        i += 1
    print('x')

