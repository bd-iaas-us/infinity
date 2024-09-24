from infinity.lib import InfinityConnection, DisableTorchCaching
import torch
import time


conn = InfinityConnection()
conn.connect()

# src = [i for i in range(1024)]
# with DisableTorchCaching():
#     src_tensor = torch.tensor(src, device="cuda:0", dtype=torch.float32)

# #write 16 elements

# conn.write_kvcache(src_tensor, [("key1", 0), ("key2", 32), ("key3", 32)], 16)

# conn.sync_local()




with DisableTorchCaching():
    dst_tensor = torch.zeros(1024, device="cuda:2", dtype=torch.float32)

conn.read_kvcache(dst_tensor, [("key1", 0), ("key2", 32)], 16)
conn.sync_local()


#move dst_tensor to the device of src_tensor

# assert torch.equal(src_tensor[0:16].cpu(), dst_tensor[0:16].cpu())
# assert torch.equal(src_tensor[32:48].cpu(), dst_tensor[32:48].cpu())


# big tensor test
# create a 1GB tensor
# size = 1024*1024*1024 // 4
# with DisableTorchCaching():
#     src_tensor = torch.randn(size, device="cuda", dtype=torch.float32)

# now = time.time()
# conn.write_kvcache(src_tensor, [("key1", 0), ("key2", size//2)],  size//2)
# print("1st: Submit 1GB task time: ", time.time() - now)
# now = time.time()
# conn.write_kvcache(src_tensor, [("key3", size//2), ("key4", 0)],  size//2)
# print("2nd: Submit 1GB task time: ", time.time() - now)
# now = time.time()
# conn.sync_local()
# print("cuda stream sync ", time.time() - now)
