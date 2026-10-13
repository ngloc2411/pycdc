def m(v):
    match v:
        case 0:
            return "zero"
        case [a, b]:
            return f"pair {a} {b}"
        case {"k": x}:
            return f"map {x}"
        case str() as s:
            return "str " + s
        case _:
            return "other"

def main():
    print([m(v) for v in (0, [1, 2], {"k": 3}, "q", 4.5)])
    data = [1, 2, 3]
    if (n := len(data)) > 2:
        print(n)
    print([y for x in data if (y := x * 2) > 2])

main()
