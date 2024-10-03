//single thread right now.
#include <cuda.h>
#include <cuda_runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <time.h>
#include <assert.h>


#include <string>
#include <unordered_map>
#include <iostream>
#include <uv.h>
#include <chrono>
#include <sys/param.h>
#include "log.h"
#include "ibv_helper.h"
#include "protocol.h"
#include "utils.h"
#include "mempool.h"


#define BUFFER_SIZE (64<<10)


struct PTR {
    void *ptr;
    size_t size;
    int pool_idx;
};

std::unordered_map<std::string, PTR> kv_map;
uv_loop_t *loop;
uv_tcp_t server;
//global ibv context
struct ibv_context *ib_ctx;
struct ibv_pd *pd;
MM *mm;

int get_kvmap_len() {
    return kv_map.size();
}

void print_header(header_t *header) {
    INFO("HEADER: op: {}, body_size :{}", header->op, header->body_size);
}

typedef enum {
    READ_HEADER,
    READ_BODY,
    } read_state_t;

struct Client {
    uv_tcp_t* handle; //uv_stream_t
    read_state_t state; //state of the client, for parsing the request
    size_t bytes_read; //bytes read so far, for parsing the request
    size_t expected_bytes; //expected size of the body
    header_t header;

    char * recv_buffer;
    local_meta_t local_meta;
    remote_meta_request remote_meta_req;

    //TODO: remove send_buffer
    char *send_buffer;

    cudaStream_t cuda_stream;
        uv_work_t work_req;

    rdma_conn_info_t remote_info;
    rdma_conn_info_t local_info;

    struct ibv_cq *cq;
    struct ibv_qp *qp;
    bool rdma_connected = false;
    int gidx; //gid index

    int remain;

    Client() = default;
    Client(const Client&) = delete;
    ~Client();
};

Client::~Client() {
    DEBUG("free client resources");
    if (handle) {
        free(handle);
        handle = NULL;
    }
    if (recv_buffer) {
        free(recv_buffer);
        recv_buffer = NULL;
    }
    cudaStreamDestroy(cuda_stream);
    INFO("destroy cuda stream");
    if (qp) {
        struct ibv_qp_attr attr;
        memset(&attr, 0, sizeof(attr));
        attr.qp_state = IBV_QPS_ERR;
        if (ibv_modify_qp(qp, &attr, IBV_QP_STATE)) {
            ERROR("Failed to modify QP to ERR state");
        }
    }
    if (qp) {
        ibv_destroy_qp(qp);
        qp = NULL;
    }
}
typedef struct Client client_t;

typedef struct {
    client_t *client;
    void *d_ptr;
} wqueue_data_t;

void reset_client_read_state(client_t *client) {
    client->state = READ_HEADER;
    client->bytes_read = 0;
    client->expected_bytes = FIXED_HEADER_SIZE;
    memset(&client->header, 0, sizeof(header_t));
    
    //keep the recv_buffer as it is
    memset(client->recv_buffer, 0, client->expected_bytes);
    client->local_meta.blocks.clear();
    client->local_meta.block_size = 0;
    memset(&(client->local_meta.ipc_handle), 0, sizeof(cudaIpcMemHandle_t));
}

void on_close(uv_handle_t* handle) {
    client_t *client = (client_t *) handle->data;
    
    delete client;
}

void alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
    buf->base = (char *) malloc(suggested_size);
    buf->len = suggested_size;
}

int veryfy_header(header_t *header) {
    if (header->magic != MAGIC) {
        return INVALID_REQ;
    }
    //TODO: add more checks
    return 0;
}

void on_write(uv_write_t* req, int status) {
    if (status < 0) {
        ERROR("Write error {}", uv_strerror(status));
        uv_close((uv_handle_t*)req->handle, on_close);
    }
    free(req);
}

void wait_for_ipc_close_completion(uv_work_t* req) {
    wqueue_data_t *wqueue_data = (wqueue_data_t *)req->data;
    CHECK_CUDA(cudaIpcCloseMemHandle(wqueue_data->d_ptr));
    INFO("wait_for_ipc_close_completion done");
}

void after_ipc_close_completion(uv_work_t* req, int status) {
    ERROR("after_ipc_close_comp");
    wqueue_data_t *wqueue_data = (wqueue_data_t *)req->data;
    wqueue_data->client->remain--;
    INFO("after_ipc_close_completion done");
    delete wqueue_data;
}

int do_read_cache(client_t *client) {
    const header_t *header = &client->header;
    const local_meta_t *meta = &client->local_meta;
    void * d_ptr;

    assert(header != NULL);
    //TODO: check device_id


    CHECK_CUDA(cudaIpcOpenMemHandle(&d_ptr, meta->ipc_handle, cudaIpcMemLazyEnablePeerAccess));

    for (auto &block : meta->blocks) {
        if (kv_map.count(block.key) == 0) {
            std::cout << "Key not found: " << block.key << std::endl;
            CHECK_CUDA(cudaIpcCloseMemHandle(d_ptr));
            return KEY_NOT_FOUND;
        }

        //key found
        //std::cout << "Key found: " << block.key << std::endl;
        void * h_src = kv_map[block.key].ptr;
        if (h_src == NULL) {
            return KEY_NOT_FOUND;
        }
        //push the host cpu data to local device
        CHECK_CUDA(cudaMemcpyAsync((char*)d_ptr + block.offset, h_src, meta->block_size, cudaMemcpyHostToDevice, client->cuda_stream));
    }
    client->remain++;
    wqueue_data_t *wqueue_data = new wqueue_data_t();
    wqueue_data->client = client;
    wqueue_data->d_ptr = d_ptr;
    client->work_req.data = (void *)wqueue_data;
    uv_queue_work(loop, &client->work_req, wait_for_ipc_close_completion, after_ipc_close_completion);

    return TASK_ACCEPTED;
}

int do_write_cache(client_t *client) {
    const local_meta_t * meta =  &client->local_meta;
    assert(meta != NULL);
    // allocate host memory
    void* d_ptr;
    CHECK_CUDA(cudaIpcOpenMemHandle(&d_ptr, meta->ipc_handle, cudaIpcMemLazyEnablePeerAccess));
    
    for (auto &block : meta->blocks) {
        //pull data from local device to CPU host
        void * h_dst;
        int pool_idx;
        h_dst = mm->allocate(meta->block_size, &pool_idx);
        if (h_dst == NULL) {
            ERROR("Failed to allocat host memroy");
            CHECK_CUDA(cudaIpcCloseMemHandle(d_ptr));
            return SYSTEM_ERROR;
        }
        //how to deal with memory overflow? 
        //pull data from local device to CPU host
        CHECK_CUDA(cudaMemcpyAsync(h_dst, (char*)d_ptr + block.offset, meta->block_size, cudaMemcpyDeviceToHost, client->cuda_stream));
        kv_map[block.key] = {
            .ptr = h_dst,
            .size = meta->block_size,
            .pool_idx = pool_idx
        };
    }
    client->remain++;
    wqueue_data_t *wqueue_data = new wqueue_data_t();
    wqueue_data->client = client;
    wqueue_data->d_ptr = d_ptr;
    client->work_req.data = (void *)wqueue_data;
    uv_queue_work(loop, &client->work_req, wait_for_ipc_close_completion, after_ipc_close_completion);
    return TASK_ACCEPTED;
}

int init_rdma_context() {
    struct ibv_device** dev_list;
    int num_devices;
    dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list) {
        ERROR("Failed to get RDMA devices list");
        return -1;
    }

    ib_ctx = ibv_open_device(dev_list[0]);
    if (!ib_ctx) {
        ERROR("Failed to open device");
        return -1;
    }

    pd = ibv_alloc_pd(ib_ctx);
    if (!pd) {
        ERROR("Failed to allocate PD");
        return -1;
    }
    return 0;
}

int do_rdma_exchange(client_t *client) {
    INFO("do rdma exchange...");

    int ret;
    // RDMA setup if not already done
    if (!client->qp) {
        client->cq = ibv_create_cq(ib_ctx, 1025, NULL, NULL, 0);
        if (!client->cq) {
            ERROR("Failed to create CQ");
            return SYSTEM_ERROR;
        }

        // Create Queue Pair
        struct ibv_qp_init_attr qp_init_attr = {};
        qp_init_attr.send_cq = client->cq;
        qp_init_attr.recv_cq = client->cq;
        qp_init_attr.qp_type = IBV_QPT_RC; // Reliable Connection
        qp_init_attr.cap.max_send_wr = 1024;
        qp_init_attr.cap.max_recv_wr = 1024;
        qp_init_attr.cap.max_send_sge = 1;
        qp_init_attr.cap.max_recv_sge = 1;

        client->qp = ibv_create_qp(pd, &qp_init_attr);
        if (!client->qp) {
            ERROR("Failed to create QP");
            return SYSTEM_ERROR;
        }
        // Modify QP to INIT state
        struct ibv_qp_attr attr = {};
        attr.qp_state = IBV_QPS_INIT;
        attr.port_num = 1;
        attr.pkey_index = 0;
        attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_LOCAL_WRITE;

        int flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;

        ret = ibv_modify_qp(client->qp, &attr, flags);
        if (ret) {
            ERROR("Failed to modify QP to INIT");
            return SYSTEM_ERROR;
        }

        // Get local connection information
        struct ibv_port_attr port_attr;
        if (ibv_query_port(ib_ctx, 1, &port_attr)) {
            ERROR("Failed to query port");
            return SYSTEM_ERROR;
        }

        int gidx = ibv_find_sgid_type(ib_ctx, 1, IBV_GID_TYPE_ROCE_V2, AF_INET);
        if (gidx < 0) {
            ERROR("Failed to find GID");
            return -1;
        }

        client->gidx = gidx;

        union ibv_gid gid;
        //get gid
        if (ibv_query_gid(ib_ctx, 1, gidx, &gid)) {
            ERROR("Failed to get GID");
            return -1;
        }

        client->local_info.qpn = client->qp->qp_num;
        client->local_info.psn = lrand48() & 0xffffff;
        client->local_info.gid = gid;

        INFO("gid index: {}", client->gidx);
        print_rdma_conn_info(&client->local_info, false);
    }

    //Send server's RDMA connection info to client
    uv_buf_t wbuf = uv_buf_init((char*)&client->local_info, sizeof(client->local_info));

    uv_write_t* write_req = (uv_write_t*)malloc(sizeof(uv_write_t));
    write_req->data = client;
    uv_write(write_req, (uv_stream_t*)client->handle, &wbuf, 1, on_write);
    
    
    // Modify QP to RTR state
    struct ibv_qp_attr attr = {};
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = IBV_MTU_1024; //FIXME: hard coded
    attr.dest_qp_num = client->remote_info.qpn;
    attr.rq_psn = client->remote_info.psn;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 12;
    attr.ah_attr.dlid = 0; //RoCE v2 is used.
    attr.ah_attr.sl = 0;
    attr.ah_attr.src_path_bits = 0;
    attr.ah_attr.port_num = 1;
    //RoCE v2
    attr.ah_attr.is_global = 1;
    attr.ah_attr.grh.dgid = client->remote_info.gid;
    attr.ah_attr.grh.sgid_index = client->gidx;
    attr.ah_attr.grh.hop_limit = 1;


    int flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;

    ret = ibv_modify_qp(client->qp, &attr, flags);
    if (ret) {
        ERROR("Failed to modify QP to RTR");
        return SYSTEM_ERROR;
    }


    // Modify QP to RTS state
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.sq_psn = client->local_info.psn;
    attr.max_rd_atomic = 1;

    flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
            IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;

    ret = ibv_modify_qp(client->qp, &attr, flags);
    if (ret) {
        ERROR("Failed to modify QP to RTS");
        return SYSTEM_ERROR;
    }
    INFO("RDMA exchange done");
    client->rdma_connected = true;


    reset_client_read_state(client);
    return 0;
}

void do_send(client_t *client, void* buf, size_t size) {
    uv_write_t* write_req = (uv_write_t*)malloc(sizeof(uv_write_t));
    int ret = client->remain;
    client->send_buffer = (char*)realloc(client->send_buffer, size);
    memcpy(client->send_buffer, buf, size);
    write_req->data = client;
    uv_buf_t wbuf = uv_buf_init(client->send_buffer, size);
    uv_write(write_req, (uv_stream_t *)client->handle, &wbuf, 1, on_write);    
}

int do_sync_stream(client_t *client) {
    do_send(client, &client->remain, RETURN_CODE_SIZE);
    // Reset client state
    reset_client_read_state(client);
    return 0;
}



//TODO: refactor this function to use RDMA_WRITE_IMM.
int do_rdma_read(client_t *client) {
    INFO("do rdma read keys: {}", client->remote_meta_req.keys.size());
    auto start = std::chrono::high_resolution_clock::now();

    int error_code = TASK_ACCEPTED;
    remote_meta_response resp;
    uv_write_t* write_req = (uv_write_t*)malloc(sizeof(uv_write_t));
    std::string out;


    for (const auto& key : client->remote_meta_req.keys) {
        if (kv_map.find(key) == kv_map.end()) {
            //key not found
            error_code = KEY_NOT_FOUND;
            goto RETURN;
        }
        PTR ptr = kv_map[key];

        resp.blocks.push_back({
            .rkey = mm->get_rkey(ptr.pool_idx),
            .remote_addr = (uintptr_t)ptr.ptr
        });
    }

    //send the response

RETURN:
    resp.error_code = error_code;
    if (!serialize(resp, out)) {
        ERROR("Failed to serialize response");
        return SYSTEM_ERROR;
    }
    client->send_buffer = (char*) realloc(client->send_buffer, out.size() + RETURN_CODE_SIZE);

    int size = out.size();
    memcpy(client->send_buffer, &size, RETURN_CODE_SIZE);
    memcpy(client->send_buffer + RETURN_CODE_SIZE, out.c_str(), out.size());
    uv_buf_t wbuf = uv_buf_init(client->send_buffer, out.size() + RETURN_CODE_SIZE);
    write_req->data = client;
    uv_write(write_req, (uv_stream_t*)client->handle, &wbuf, 1, on_write);

    INFO("send response: size:{}", size);
    reset_client_read_state(client);
    INFO("do rdma read runtime: {} ms", std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - start).count());
    return 0;
}

int do_rdma_write(client_t *client) {
    INFO("do rdma write keys: {}, remote_block_size: {}", client->remote_meta_req.keys.size(), client->remote_meta_req.block_size);
    remote_meta_response resp;
    uv_write_t* write_req = (uv_write_t*)malloc(sizeof(uv_write_t));
    std::string out;
    int error_code = TASK_ACCEPTED;

    for (std::string &key : client->remote_meta_req.keys) {
        
        void * h_dst;
        int pool_idx;
        h_dst = mm->allocate(client->remote_meta_req.block_size, &pool_idx);
        //FIXME: only one h_dst is sent
        if (h_dst == NULL) {
            error_code = SYSTEM_ERROR;
            goto RETURN;
        }
        //save to the map
        kv_map[key] = {
            .ptr = h_dst,
            .size = client->remote_meta_req.block_size,
            .pool_idx = pool_idx
        };
        INFO("rkey: {}, local_addr: {}, size : {}", mm->get_rkey(pool_idx), (uintptr_t)h_dst, client->remote_meta_req.block_size);
        
        resp.blocks.push_back({
            .rkey = mm->get_rkey(pool_idx),
            .remote_addr = (uintptr_t)h_dst
        });
    }
        

RETURN:
    resp.error_code = error_code;
    INFO("response error code {}", error_code);
    if (!serialize(resp, out)) {
        ERROR("Failed to serialize response");
        return SYSTEM_ERROR;
    }
    client->send_buffer = (char*) realloc(client->send_buffer, out.size() + RETURN_CODE_SIZE);


    int size = out.size();
    memcpy(client->send_buffer, &size, RETURN_CODE_SIZE);
    memcpy(client->send_buffer + RETURN_CODE_SIZE, out.c_str(), out.size());
    uv_buf_t wbuf = uv_buf_init(client->send_buffer, out.size() + RETURN_CODE_SIZE);
    write_req->data = client;
    uv_write(write_req, (uv_stream_t*)client->handle, &wbuf, 1, on_write);

    DEBUG("send response: size:{}", size);
    reset_client_read_state(client);
    return 0;
}

//return value of handle_request:
//if ret is less than 0, it is an system error, outer code will close the connection
//if ret is greater than 0, it is an application error or success
int handle_request(client_t *client) {    
    auto start = std::chrono::high_resolution_clock::now();
    int return_code = SYSTEM_ERROR;

   if (client->header.op == OP_RDMA_WRITE) {
        return_code = do_rdma_write(client);
        if (return_code == 0) {
            return 0;
        }
   }
   else if (client->header.op == OP_RDMA_READ) {
        return_code = do_rdma_read(client);
        if (return_code == 0) {
            return 0;
        }
   }
   else if (client->header.op == OP_RDMA_EXCHANGE) {
        return_code = do_rdma_exchange(client);
        if (return_code == 0) {
            return 0;
        }
   }
   else if (client->header.op == OP_SYNC) {
        return_code = do_sync_stream(client);
        //do_sync_stream will handle return code by itself
        if (return_code == 0) {
            return 0;
        }
    }  else if (client->header.op == OP_W) {
        return_code = do_write_cache(client);
    } else if (client->header.op == OP_R) {
        return_code = do_read_cache(client);
    } else {
        return_code = INVALID_REQ;
    }
    
    INFO("return code: {}", return_code);
    do_send(client, &return_code, RETURN_CODE_SIZE);
    
    //success
    //keep connection alive
    reset_client_read_state(client);

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> elapsed = end - start;
    INFO("handle request runtime: {} ms", elapsed.count());
    return 0;
}

void on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    client_t* client = (client_t*)stream->data;
    ssize_t offset = 0;

    if (nread < 0) {
        if (nread != UV_EOF)
            ERROR("Read error {}", uv_err_name(nread));
        uv_close((uv_handle_t*)stream, on_close);
        goto clean_up;
    }

    while (offset < nread) {
        switch (client->state) {
            
            case READ_HEADER: {
                size_t to_copy = MIN(nread - offset, FIXED_HEADER_SIZE - client->bytes_read);
                memcpy(((char*)&client->header) + client->bytes_read, buf->base + offset, to_copy);
                client->bytes_read += to_copy;
                offset += to_copy;
                if (client->bytes_read == FIXED_HEADER_SIZE) {
                    print_header(&client->header);
                    if (client->header.op == OP_R || client->header.op == OP_W 
                    || client->header.op == OP_RDMA_EXCHANGE || client->header.op == OP_RDMA_WRITE || client->header.op == OP_RDMA_READ) {
                        int ret = veryfy_header(&client->header);
                        if (ret != 0) {
                            ERROR("Invalid header");
                            uv_close((uv_handle_t*)stream, on_close);
                            goto clean_up;
                        }

                        //prepare for reading body
                        client->expected_bytes = client->header.body_size;
                        client->bytes_read = 0;
                        client->recv_buffer = (char*)realloc(client->recv_buffer, client->expected_bytes);
                        client->state = READ_BODY;
                    } else if (client->header.op == OP_SYNC){
                        handle_request(client);
                    }
                }
                break;
            }

            case READ_BODY: {
                assert(client->recv_buffer != NULL);

                DEBUG("reading body, bytes_read: {}, expected_bytes: {}", client->bytes_read, client->expected_bytes);
                size_t to_copy = MIN(nread - offset, client->expected_bytes - client->bytes_read);

                memcpy(client->recv_buffer + client->bytes_read, buf->base + offset, to_copy);
                client->bytes_read += to_copy;
                offset += to_copy;
                if (client->bytes_read == client->expected_bytes) {
                    DEBUG("body read done, size {}", client->expected_bytes);
                    switch (client->header.op) {
                        case OP_R:
                        case OP_W:
                            if (!deserialize(client->recv_buffer, client->expected_bytes, client->local_meta)){
                                printf("failed to deserialize local meta\n");
                                uv_close((uv_handle_t*)stream, on_close);
                                goto clean_up;
                            }
                            print_ipc_handle(client->local_meta.ipc_handle);
                            handle_request(client);
                            break;
                        case OP_RDMA_EXCHANGE:
                            memcpy((void*)(&client->remote_info), client->recv_buffer, client->expected_bytes);
                            print_rdma_conn_info(&client->remote_info, true);
                            handle_request(client);
                            break;
                        case OP_RDMA_WRITE:
                        case OP_RDMA_READ:
                            if (!deserialize(client->recv_buffer, client->expected_bytes, client->remote_meta_req)){
                                printf("failed to deserialize remote meta\n");
                                uv_close((uv_handle_t*)stream, on_close);
                                goto clean_up;
                            }
                            handle_request(client);
                        default:
                            break;
                    }
                }
                break;
            }
        }
    }

clean_up:
    free(buf->base);

}

void on_new_connection(uv_stream_t* server, int status) {
    INFO("new connection...");
    if (status < 0) {
        ERROR("New connection error {}", uv_strerror(status));
        return;
    }
    uv_tcp_t* client_handle = (uv_tcp_t*)malloc(sizeof(uv_tcp_t));
    uv_tcp_init(loop, client_handle);
    if (uv_accept(server, (uv_stream_t*)client_handle) == 0) {
        client_t *client = new client_t();
        CHECK_CUDA(cudaStreamCreate(&client->cuda_stream));
        client->handle = client_handle;
        client_handle->data = client;
        client->state = READ_HEADER;
        client->bytes_read = 0;
        client->expected_bytes = FIXED_HEADER_SIZE;
        client->recv_buffer = NULL;
        uv_read_start((uv_stream_t*)client_handle, alloc_buffer, on_read);
    } else {
        uv_close((uv_handle_t*)client_handle, NULL);
    }
}

int register_server(unsigned long loop_ptr, size_t prealloc_size) {

    signal(SIGSEGV, signal_handler);

    loop = (uv_loop_t *)loop_ptr;
    assert(loop != NULL);
    uv_tcp_init(loop, &server);
    struct sockaddr_in addr;
    uv_ip4_addr("0.0.0.0", PORT, &addr);

    uv_tcp_bind(&server, (const struct sockaddr*)&addr, 0);
    int r = uv_listen((uv_stream_t*) &server, 128, on_new_connection);
    if (r) {
        fprintf(stderr, "Listen error: %s\n", uv_strerror(r));
        return -1;
    }

    if (init_rdma_context() < 0) {
        return -1;
    }
    mm = new MM(prealloc_size, 32<<10, pd);

    INFO("register server done");

    return 0;
}