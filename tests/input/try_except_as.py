def f():
    try:
        pass
    except ValueError as e:
        print(e)
    except (KeyError, IndexError) as e:
        print(e)
    except Exception:
        pass
