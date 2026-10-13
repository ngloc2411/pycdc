import asyncio

class ACtx:
    async def __aenter__(self):
        print("aenter"); return 1
    async def __aexit__(self, *a):
        print("aexit")

async def agen(n):
    for i in range(n):
        yield i

async def work(x):
    await asyncio.sleep(0)
    return x * 2

async def main():
    print(await work(2))
    async with ACtx() as v:
        print(v)
    print([i async for i in agen(3)])
    async for i in agen(2):
        print(i)
    print(await asyncio.gather(work(1), work(2)))

asyncio.run(main())
