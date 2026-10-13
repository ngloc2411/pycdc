def m(v):
    match v:
        case 0:
            return 'zero'
        case [a, b]:
            return a
        case {'k': x}:
            return x
        case str() as s:
            return s
        case _:
            return 'other'
