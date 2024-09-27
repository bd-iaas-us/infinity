from infinity import _infinity

import torch
import os
from typing import List, Tuple


def register_server(loop):
    #client does not need to call this function
    from uvloop.loop import libuv_get_loop_t_ptr
    import ctypes
    from ctypes import pythonapi, c_void_p, py_object
    PyCapsule_GetPointer = pythonapi.PyCapsule_GetPointer
    PyCapsule_GetPointer.restype = c_void_p
    PyCapsule_GetPointer.argtypes = [py_object, ctypes.c_char_p]
    loop_ptr = PyCapsule_GetPointer(libuv_get_loop_t_ptr(loop), None)

    #from cpython.pycapsule import PyCapsule_GetPointer
    #<uint64_t>PyCapsule_GetPointer(obj, NULL)
    return _infinity.register_server(loop_ptr)

class DisableTorchCaching:
    def __enter__(self):
        os.environ['PYTORCH_NO_CUDA_MEMORY_CACHING'] = '1'
        return self
    def __exit__(self, exc_type, exc_value, traceback):
        del os.environ['PYTORCH_NO_CUDA_MEMORY_CACHING']
        return
    
class InfinityConnection:
    OP_R="R"
    OP_W="W"
    OP_SYNC="S"
    OP_RDMA_EXCHANGE="E"
    OP_RDMA_WRITE="D"
    OP_RDMA_READ="A"
    def __init__(self):
        self.conn = _infinity.Connection()
        self.connected = False
    
    def connect(self):
        ret = _infinity.init_connection(self.conn)
        if ret < 0:
            raise Exception("Failed to initialize connection")
        self.connected = True

    def _verify(self, kv_cache : torch.Tensor):
        if self.connected is False:
            raise Exception("Connection is not established")
        if kv_cache.device.type != "cuda":
            raise Exception("Tensor must be on CUDA device")
        if kv_cache.is_contiguous() is False:
            raise Exception("Tensor must be contiguous")
    
    #size is the element size
    #offset is the offset of the element
    def write_kvcache(self, kvcache : torch.Tensor, blocks: List[Tuple[str, int]], block_size: int):
        self._verify(kvcache)
        ptr = kvcache.data_ptr()
        element_size = kvcache.element_size()
        #each offset should multiply by the element size
        for i in range(len(blocks)):
            key, offset = blocks[i]
            blocks[i] = (key, offset * element_size)
        ret = _infinity.rw_local(self.conn, self.OP_W, blocks, block_size * element_size, ptr)
        if ret < 0:
            raise Exception(f"Failed to write to infinity, ret = {ret}")
        return
    
    def read_kvcache(self, kvcache : torch.Tensor, blocks: List[Tuple[str, int]], block_size: int):
        #TODO: self._verify(kvcache, key, offset, size)
        self._verify(kvcache)
        ptr = kvcache.data_ptr()
        element_size = kvcache.element_size()
        #each offset should multiply by the element size
        for i in range(len(blocks)):
            key, offset = blocks[i]
            blocks[i] = (key, offset * element_size)
        ret = _infinity.rw_local(self.conn, self.OP_R, blocks, block_size * element_size, ptr)
        if ret < 0:
            raise Exception(f"Failed to read to infinity, ret = {ret}")
        return

    def close_connection(self):
        if self.connected:
            _infinity.close_connection(self.conn)
            self.connected = False
        return
    
    def sync_local(self):
        if self.connected:
            _infinity.sync_local(self.conn)

    def __enter__(self):
        self.connect()
        return self
    
    def __exit__(self, exc_type, exc_value, traceback):
        self.close_connection()
