def add(a, b=2, *args, c=3, **kw):
    return a + b + c + sum(args) + len(kw)

def classify(n):
    if n < 0:
        return "neg"
    elif n == 0:
        return "zero"
    else:
        return "pos"

def loops(n):
    total = 0
    for i in range(n):
        if i % 2:
            continue
        total += i
    j = 0
    while j < n:
        j += 1
        if j > 5:
            break
    else:
        total += 100
    return total, j

def main():
    print(add(1), add(1, 2, 3, 4, c=5, x=1))
    print([classify(x) for x in (-1, 0, 1)])
    print(loops(4), loops(10))
    x = 5
    x **= 2
    y = x if x > 10 else -x
    print(x, y, not x, x and y, x or y)
    print(1 < x < 100, x is not None, x in (25,), x not in (1, 2))
    a, b = 1, 2
    a, b = b, a
    print(a, b)

main()
