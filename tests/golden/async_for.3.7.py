# Source Generated with Decompyle++
# File: async_for.3.7.pyc (Python 3.7)


async def a(b, c):
    async for b in c:
        pass


async def a(b, c):
    async for b in c:
        continue


async def a(b, c):
    async for b in c:
        break


async def time_for_some_fun():
    async for x, y in myfunc(c):
        print(123)
        if x == 3:
            print('something')
            break
        for i in regular_for:
            var1 = var2 + var3
            async for x1 in print:
                print('test LOAD_GLOBAL')
                async for x2 in inner:
                    for x3 in regular:
                        async for x4 in inner2:
                            async for x5 in inner3:
                                async for x6 in inner4:
                                    print('ridiculous nesting')
                    
        
    async for q, w, e, r in qwer:
        u = 6
    async for x4 in inner2:
        async for x5 in inner3:
            pass
    print('outside loop')


async def tryBlocks():
    async for b in c:
        stuff
        await things(x)
        
        try:
            STUFF
        except MyException:
            running = False

        BLOCK_AFTER
    
    try:
        async for b in c:
            stuff
            await things(x)
            async for b2 in c2:
                stuff2
                await things2(x)
                
                try:
                    STUFF2
                except MyException2:
                    running2 = False

                AFTER
            
            try:
                STUFF
            except MyException:
                running = False

            BLOCK_AFTER
    except MyException2:
        OUTEREXCEPT


print('outside function')
