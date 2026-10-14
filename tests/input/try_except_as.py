def f():
    try:
        a = 1
    except ValueError as e:
        print(e)
    except (KeyError, IndexError) as e:
        print(e)
    except Exception:
        pass
