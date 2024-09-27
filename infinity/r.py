from infinity import _infinity
import torch

conn = _infinity.Connection()
_infinity.init_connection(conn)
_infinity.setup_rdma(conn)


#copy from 
OP_R="R"
OP_W="W"
OP_SYNC="S"
OP_RDMA_EXCHANGE="E"
OP_RDMA_WRITE="D"
OP_RDMA_READ="A"

# Create a tensor on cuda
a = torch.tensor([1, 2, 3, 4], device='cuda', dtype=torch.float32)
#int rw_remote(connection_t *conn, char op, const std::vector<std::string>keys, int block_size, void * ptr);
a.element_size()
_infinity.rw_remote(conn, OP_RDMA_WRITE, ["Im_a_big_key"], a.element_size() * a.numel() , a.data_ptr())



b = torch.zeros(4, device='cuda:3', dtype=torch.float32)
print("read from remote")
_infinity.rw_remote(conn, OP_RDMA_READ, ["Im_a_big_key"], b.element_size() * b.numel() , b.data_ptr())
print(b)

