def f(xs):
    for x in xs:
        if x:
            print(x)
    return 1


def g(xs):
    for x in xs:
        print(x)
        if x > 2:
            print('big')
            if x > 5:
                print('huge')


def h(xs):
    for x in xs:
        if not x:
            print('zero')
    print('done')


def gen(xs):
    for x in xs:
        if x:
            yield x
