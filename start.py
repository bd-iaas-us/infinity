import infinity
import asyncio
import uvloop
import fastapi
from fastapi import FastAPI, Request
import uvicorn



app = FastAPI()

@app.get("/status")
async def status():
    print("HELLO,WORLD")
    return {"Hello": "World"}

@app.get("/kvmapSize")
async def read_status():
    return infinity._infinity.get_kvmap_len()

if __name__ == "__main__":
    loop = uvloop.new_event_loop()
    asyncio.set_event_loop(loop)
    infinity.register_server(loop)
    config = uvicorn.Config(
    app,
    host="0.0.0.0",
    port=8080,
    loop="uvloop",
    )

    server = uvicorn.Server(config)

    # 运行服务器
    loop.run_until_complete(server.serve())