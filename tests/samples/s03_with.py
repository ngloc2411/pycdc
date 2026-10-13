class Ctx:
    def __init__(self, name):
        self.name = name
    def __enter__(self):
        print("enter", self.name)
        return self.name
    def __exit__(self, *a):
        print("exit", self.name)
        return False

def one():
    with Ctx("a") as x:
        print("body", x)

def two():
    with Ctx("a") as x, Ctx("b") as y:
        print(x, y)

def inside_loop():
    for i in range(2):
        with Ctx(str(i)):
            if i:
                continue
            print("loop", i)

def returns():
    with Ctx("r"):
        return 5

def with_try():
    try:
        with Ctx("t"):
            raise ValueError("x")
    except ValueError:
        print("handled")

def main():
    one(); two(); inside_loop(); print(returns()); with_try()

main()
