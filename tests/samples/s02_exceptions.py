def f(x):
    try:
        return 10 // x
    except ZeroDivisionError:
        return -1
    finally:
        print("finally", x)

def g(x):
    try:
        if x == 1:
            raise ValueError("one")
        if x == 2:
            raise KeyError("two")
        r = "none"
    except ValueError as e:
        r = "value:" + str(e)
    except (KeyError, IndexError) as e:
        r = "key"
    except Exception:
        r = "other"
    else:
        r += "-else"
    return r

def h():
    out = []
    for i in range(3):
        try:
            if i == 1:
                continue
            out.append(i)
        finally:
            out.append("f")
    return out

def nested():
    try:
        try:
            raise RuntimeError("inner")
        except RuntimeError:
            raise ValueError("outer")
    except ValueError as e:
        return str(e)

def reraise():
    try:
        try:
            1 / 0
        except ZeroDivisionError:
            raise
    except ZeroDivisionError:
        return "caught"

def main():
    print(f(2), f(0))
    print([g(i) for i in range(4)])
    print(h(), nested(), reraise())

main()
