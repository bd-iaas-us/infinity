#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include "libinfinity.h"
#include <vector>
#include <string>
#include <tuple>
#include <iostream>

namespace py = pybind11;
extern int register_server(unsigned long loop_ptr);

int rw_local_wrapper(connection_t *conn, char op, const std::vector<std::tuple<std::string, unsigned long>> &blocks, \
            int block_size, unsigned long ptr) {

    std::vector<block_t> c_blocks;
    for (const auto& block : blocks) {
            c_blocks.push_back(block_t{std::get<0>(block), std::get<1>(block)});
    }
    return rw_local(conn, op, c_blocks, block_size, (void*)ptr);
}

int rw_remote_wrapper(connection_t *conn, char op, const std::vector<std::string> &keys, int block_size, unsigned long ptr) {
    return rw_remote(conn, op, keys, block_size, (void*)ptr);
}


PYBIND11_MODULE(_infinity, m) {
    //client side
    py::class_<connection_t>(m, "Connection")
        .def(py::init<>())
        .def_readwrite("sock", &connection_t::sock);
    py::class_<stat_t>(m, "stat_t")
        .def_readwrite("read_cnt", &stat_t::read_cnt)
        .def_readwrite("write_cnt", &stat_t::write_cnt);

    m.def("init_connection", &init_connection, "Initialize a connection");
    m.def("close_connection", &close_connection, "Close a connection");
    m.def("rw_local", &rw_local_wrapper, "Read/Write cpu memory from GPU device");
    m.def("rw_remote", &rw_remote_wrapper, "Read/Write remote memory");
    m.def("sync_local", &sync_local, "sync the cuda stream");
    m.def("get_stat", &get_stat, "get kvcache write/read stat data");
    m.def("setup_rdma", &setup_rdma, "setup rdma connection");


    //server side
    m.def("get_kvmap_len", &get_kvmap_len, "get kv map size");
    m.def("register_server", &register_server, "register the server");
    m.def("get_key_write_status", &get_key_write_status, "get key write status, 2 for not exist, 1 for complete, 0 for in progress");
    m.def("get_key_read_status", &get_key_read_status, "get key read status, 2 for not exist, 1 for complete, 0 for in progress");
}



