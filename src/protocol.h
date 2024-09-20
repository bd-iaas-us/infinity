#ifndef PROTOCOL_H
#define PROTOCOL_H


#include <cuda.h>
#include <cuda_runtime.h>
#include <vector>
#include <string>
#include <msgpack.hpp>

/*

Error code:
+--------------------+
| ERROR_CODE(4 bytes)|
+--------------------+

*/
#define PORT 12345

#define MAGIC 0xdeadbeef
#define MAGIC_SIZE 4

#define OP_R 'R'
#define OP_W 'W'
#define OP_SYNC 'S'
#define OP_SIZE 1

//error code: int
#define INVALID_REQ 400
#define FINISH 200
#define TASK_ACCEPTED 202
#define INTERNAL_ERROR 500
#define KEY_NOT_FOUND 404
#define RETRY 408
#define SYSTEM_ERROR 503


#define RETURN_CODE_SIZE sizeof(int)

typedef struct __attribute__((packed)){
    unsigned int magic;
    char op;
    unsigned int body_size;
} header_t;

typedef struct {
    std::string key;
    unsigned long offset;
    MSGPACK_DEFINE(key, offset)
} block_t;



//https://github.com/msgpack/msgpack-c/wiki/v2_0_cpp_packer
//implement pack for ipcHandler
// 定义一个宏来生成msgpack的适配器模板
#define DEFINE_MSGPACK_FIXSIZE_ADAPTER(type_name) \
namespace msgpack { \
MSGPACK_API_VERSION_NAMESPACE(MSGPACK_DEFAULT_API_NS) { \
namespace adaptor { \
\
template <> \
struct pack<type_name> { \
    template <typename Stream> \
    packer<Stream>& operator()(msgpack::packer<Stream>& o, const type_name& v) const { \
        o.pack_bin(sizeof(type_name)); \
        o.pack_bin_body(reinterpret_cast<const char*>(&v), sizeof(type_name)); \
        return o; \
    } \
}; \
\
template <> \
struct convert<type_name> { \
    msgpack::object const& operator()(msgpack::object const& o, type_name& v) const { \
        if (o.type != msgpack::type::BIN || o.via.bin.size != sizeof(type_name)) { \
            throw msgpack::type_error(); \
        } \
        std::memcpy(&v, o.via.bin.ptr, sizeof(type_name)); \
        return o; \
    } \
}; \
\
} /* namespace adaptor */ \
} /* MSGPACK_API_VERSION_NAMESPACE(MSGPACK_DEFAULT_API_NS) */ \
} /* namespace msgpack */

DEFINE_MSGPACK_FIXSIZE_ADAPTER(cudaIpcMemHandle_t)

typedef struct {
    cudaIpcMemHandle_t ipc_handle;
    int block_size;
    std::vector<block_t> blocks;
    MSGPACK_DEFINE(ipc_handle, block_size, blocks)

} local_meta_t;


typedef struct {
    uint64_t addr;
    uint32_t rkey;
    uint32_t size;
} remote_meta_t;

bool serialize_local_meta(const local_meta_t& meta, std::string& out);
bool deserialize_local_meta(const char* data, size_t size, local_meta_t& out);


#define FIXED_HEADER_SIZE sizeof(header_t)

#endif