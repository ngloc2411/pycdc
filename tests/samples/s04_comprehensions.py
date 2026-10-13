def main():
    xs = range(6)
    print([x * 2 for x in xs])
    print([x for x in xs if x % 2 if x > 1])
    print([(x, y) for x in range(3) for y in range(x)])
    print({x: x * x for x in xs if x})
    print({x % 3 for x in xs})
    print(sorted(x for x in xs if x))
    print(sum(x for x in xs))
    print(list(map(lambda v: v + 1, xs)))
    print([[y for y in range(x)] for x in range(3)])
    print({k: [v for v in range(k)] for k in range(3)})
    print([x if x % 2 else -x for x in xs])
    f = lambda a, b=1: a * b
    print(f(3), f(3, 4))
    print(any(x > 4 for x in xs), all(x >= 0 for x in xs))

main()
