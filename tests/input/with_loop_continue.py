def f(a):
    for i in range(2):
        with a:
            if i:
                continue
            print(i)
