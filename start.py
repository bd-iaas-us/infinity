import infinity
import asyncio
import uvloop
import fastapi
from fastapi import FastAPI, Request
import uvicorn
import torch




app = FastAPI()

@app.get("/kvmapSize")
async def read_status():
    return infinity._infinity.get_kvmap_len()




def check_p2p_access():
    num_devices = torch.cuda.device_count()
    for i in range(num_devices):
        for j in range(num_devices):
            if i != j:
                can_access = torch.cuda.can_device_access_peer(i, j)
                if can_access:
                    #print(f"Peer access supported between device {i} and {j}")
                    pass
                else:
                    print(f"Peer access NOT supported between device {i} and {j}")

if __name__ == "__main__":

    check_p2p_access()
    infinity.check_infinity_supported()

    loop = uvloop.new_event_loop()
    asyncio.set_event_loop(loop)
    #16 GB pre allocated
    #TODO: find the minimum size for pinning memory and ib_reg_mr
    infinity.register_server(loop, 16<<30)
    config = uvicorn.Config(
    app,
    host="0.0.0.0",
    port=28080,
    loop="uvloop",
    log_level="warning"  # Disables logging
    )

    server = uvicorn.Server(config)

    # 运行服务器
    loop.run_until_complete(server.serve())
