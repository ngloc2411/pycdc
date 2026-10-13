async def agen(n):
    for i in range(n):
        yield i
async def work(x):
    return x
async def main(a):
    print(await work(2))
    async with a as v:
        print(v)
    async for i in agen(2):
        print(i)
    return [i async for i in agen(3)]
