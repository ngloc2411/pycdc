def f():
    for i in range(3):
        try:
            if i == 1:
                continue
            print(i)
        finally:
            print('f')
