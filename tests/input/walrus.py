def f(data):
    if (n := len(data)) > 2:
        print(n)
    return [y for x in data if (y := x * 2) > 2]
