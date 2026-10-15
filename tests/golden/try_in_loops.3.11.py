# Source Generated with Decompyle++
# File: try_in_loops.3.11.pyc (Python 3.11)


def w1(q):
    while q.alive():
        
        try:
            q.step()
        except ValueError:
            print('bad')

    print('over')


def w2(q):
    while q.alive():
        
        try:
            q.step()
        except ValueError:
            print('bad')
        except KeyError:
            print('key')

        print('tick')
    print('over')


def w3(q):
    for i in q:
        
        try:
            q.step(i)
        except ValueError:
            continue

        print(i)
    print('done')


def w4(q):
    for i in q:
        
        try:
            q.step(i)
        except ValueError:
            break

        print('ok')
    print('done')


def w5(q):
    while True:
        
        try:
            q.step()
        except ValueError:
            return 1

        print('f')


def w6(q):
    for i in q:
        
        try:
            q.step(i)
        except ValueError:
            print('bad')



def w7(q):
    for i in q:
        for j in i:
            
            try:
                q.step(j)
            except ValueError:
                print('bad')

        print(i)

