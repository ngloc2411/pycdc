# Source Generated with Decompyle++
# File: match_stmt.3.11.pyc (Python 3.11)


def literals(x):
    match x:
        case 1:
            return 'one'
        case None:
            return 'none'
        case -5:
            return 'neg'
        case 'a' | 'b':
            return 'ab'
        case Color.RED:
            return 'red'
        case _:
            return 'other'


def sequences(x):
    match x:
        case []:
            return 0
        case [a]:
            return a
        case [a, b]:
            return b
        case [first, *rest]:
            return rest
        case [[a, b], c]:
            return c


def mappings(x):
    match x:
        case {}:
            return 0
        case {'k': v}:
            return v
        case {'k': 1, 'j': w, **rest}:
            return rest


def classes(x):
    match x:
        case Point(x=0, y=0):
            return 'origin'
        case Point(a, b) as p:
            return p
        case str() | bytes():
            return 'text'


def guards(x, y):
    match x:
        case [a, b] if a > b:
            return a
        case n if n > y:
            return n


def bodies(x):
    out = []
    match x:
        case 1:
            out.append(1)
            out.append(2)
        case [a, b]:
            if a:
                out.append(a)
            else:
                out.append(b)
        case _:
            out.append(0)
    print(out)
    return out

