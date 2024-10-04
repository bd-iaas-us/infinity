from infinity.lib import InfinityConnection, DisableTorchCaching
import torch
import time




def run(conn):
    src = [i for i in range(8192)]
    src_tensor0 = torch.tensor(src, device="cuda:0", dtype=torch.float32)
    src_tensor1 = torch.tensor(src, device="cuda:1", dtype=torch.float32)


    t1 = time.time()
    conn.write_cache(src_tensor0, [("key1", 0), ("key2", 1024), ("key3", 2048), ("key4", 1024*3), ("key5", 1024*4)], 1024)
    conn.write_cache(src_tensor1, [("2key1", 0), ("2key2", 1024), ("2key3", 2048)], 1024)

    conn.sync()
    print("write Time:", time.time()-t1)
    

    # dst_tensor = torch.zeros(8192, device="cuda:2", dtype=torch.float32)
    # t2 = time.time()
    # conn.read_cache(dst_tensor, [("key1", 0), ("key2", 1024), ("key3", 2048), ("key1", 1024*3)], 1024)
    # conn.sync()
    # print("read Time:", time.time()-t2)

    # assert torch.equal(src_tensor0[0:1024].cpu(), dst_tensor[0:1024].cpu())

    # assert torch.equal(src_tensor0[1024:2048].cpu(), dst_tensor[1024:2048].cpu())



    # s = 80 <<20
    # big_tensor = torch.zeros(s, device="cuda:2", dtype=torch.float32)
    # t = time.time()
    # conn.write_cache(big_tensor, [("big_one", 0), ("big_one1", s//4)], s//4)
    # print("send write time:", time.time()-t)
    # t = time.time()
    # time.sleep(1)
    # conn.sync()
    # print("sync write time:", time.time()-t)

if __name__ == "__main__":
    conn = InfinityConnection()
    conn.connect("127.0.0.1")
    #conn.local_connect()
    run(conn)
