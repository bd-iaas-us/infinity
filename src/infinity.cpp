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

#include "protocol.h"
#include "utils.h"
#include <string>
#include <map>
#include <iostream>
#include <uv.h>
#include <chrono>


uv_loop_t *loop;
uv_tcp_t server;
#define BUFFER_SIZE (64<<10)

std::map<std::string, void*> kv_map;

void print_header(header_t *header) {
    printf("op: %c\n", header->op);
    if (header->op == OP_SYNC) {
       return;
    }
    printf("body_size: %d\n", header->body_size);
}

typedef enum {
    READ_HEADER,
    READ_BODY,
    CUDA_SYNC,
} read_state_t;


struct Client {
    uv_tcp_t* handle; //uv_stream_t
    read_state_t state; //state of the client, for parsing the request
    size_t bytes_read; //bytes read so far, for parsing the request
    size_t expected_bytes; //expected size of the body
    header_t header;

    char * recv_buffer;
    local_meta_t local_meta;

    char send_buffer[RETURN_CODE_SIZE]; //preallocated buffer for sending return code

    cudaStream_t cuda_stream;
    bool cuda_operation_inflight;
    //Use this flag to avoid multiple threads waiting for the same stream
    bool cuda_sync_inflight;
    //send cudaSyncStream to workqueue 
    uv_work_t work_req;
    
    Client() = default;
    Client(const Client&) = delete;
    ~Client();
};

Client::~Client() {
    if (handle) {
        free(handle);
        handle = NULL;
    }
    if (recv_buffer) {
        free(recv_buffer);
        recv_buffer = NULL;
    }
    //TODO: destroy cuda stream
}
typedef struct Client client_t;

void reset_client_read_state(client_t *client) {
    client->state = READ_HEADER;
    client->bytes_read = 0;
    client->expected_bytes = FIXED_HEADER_SIZE;
    /*
    client->cuda_operation_inflight = false;
    client->cuda_sync_inflight = false;
    */
    memset(&client->header, 0, sizeof(header_t));
    
    //keep the recv_buffer as it is
    memset(client->recv_buffer, 0, client->expected_bytes);
    client->local_meta.blocks.clear();
    client->local_meta.block_size = 0;
    memset(&(client->local_meta.ipc_handle), 0, sizeof(cudaIpcMemHandle_t));
}

void on_close(uv_handle_t* handle) {
    printf("free client...\n");
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
        fprintf(stderr, "Write error %s\n", uv_strerror(status));
        uv_close((uv_handle_t*)req->handle, on_close);
    }
    free(req);
}


int do_read_kvcache(client_t *client) {
    const header_t *header = &client->header;
    const local_meta_t *meta = &client->local_meta;
    void * d_ptr;

    assert(header != NULL);
    //create cuda stream if not exist
    if (!client->cuda_operation_inflight) {
        cudaStream_t cuda_stream;
        CHECK_CUDA(cudaStreamCreate(&cuda_stream));
        client->cuda_stream = cuda_stream;
    }

    CHECK_CUDA(cudaIpcOpenMemHandle(&d_ptr, meta->ipc_handle, cudaIpcMemLazyEnablePeerAccess));


    for (auto &block : meta->blocks) {
        //find the key in the map
        if (kv_map.find(block.key) == kv_map.end()) {
            //key not found
            std::cout << "Key not found: " << block.key << std::endl;
            CHECK_CUDA(cudaIpcCloseMemHandle(d_ptr));
            return KEY_NOT_FOUND;
        }
        //key found
        //std::cout << "Key found: " << block.key << std::endl;
        void * h_src = kv_map[block.key];
        //push the host cpu data to local device
        CHECK_CUDA(cudaMemcpyAsync((char*)d_ptr + block.offset, h_src, meta->block_size, cudaMemcpyHostToDevice, client->cuda_stream));
        client->cuda_operation_inflight = true;
    }
    CHECK_CUDA(cudaIpcCloseMemHandle(d_ptr));


    return TASK_ACCEPTED;
}



int do_write_kvcache(client_t *client) {

    const local_meta_t * meta =  &client->local_meta;
    assert(meta != NULL);
    // allocate host memory
    void* d_ptr;

    CHECK_CUDA(cudaIpcOpenMemHandle(&d_ptr, meta->ipc_handle, cudaIpcMemLazyEnablePeerAccess));
    
    //TODO: do we need to synchronize here?
    //CHECK_CUDA(cudaDeviceSynchronize());

    //create cuda stream if not exist
    if (!client->cuda_operation_inflight) {
        cudaStream_t cuda_stream;
        CHECK_CUDA(cudaStreamCreate(&cuda_stream));
        client->cuda_stream = cuda_stream;
    }

    //loop through the blocks
    for (auto &block : meta->blocks) {
        //pull data from local device to CPU host
        void * h_dst = malloc(meta->block_size);
        if (h_dst == NULL) {
            perror("Failed to allocat host memroy");
            CHECK_CUDA(cudaIpcCloseMemHandle(d_ptr));
            return SYSTEM_ERROR;
        }
        //how to deal with memory overflow? 
        //pull data from local device to CPU host
        CHECK_CUDA(cudaMemcpyAsync(h_dst, (char*)d_ptr + block.offset, meta->block_size, cudaMemcpyDeviceToHost, client->cuda_stream));


        client->cuda_operation_inflight = true;

        print_vector((float*)h_dst,10);
        kv_map[block.key] = h_dst;
    }
    CHECK_CUDA(cudaIpcCloseMemHandle(d_ptr));
    return TASK_ACCEPTED;
}


//danger zone
void wait_for_cuda_completion(uv_work_t *req) {
    client_t *client = (client_t *)req->data;
    // Wait for the CUDA stream to complete
    CHECK_CUDA(cudaStreamSynchronize(client->cuda_stream));
}

void after_cuda_completion(uv_work_t *req, int status) {
    client_t *client = (client_t *)req->data;

    // Send the response to the client
    uv_write_t* write_req = (uv_write_t*)malloc(sizeof(uv_write_t));
    int ret = FINISH;
    memcpy(client->send_buffer, &ret, RETURN_CODE_SIZE);
    write_req->data = client;
    uv_buf_t wbuf = uv_buf_init(client->send_buffer, RETURN_CODE_SIZE);
    uv_write(write_req, (uv_stream_t *)client->handle, &wbuf, 1, on_write);

    client->cuda_operation_inflight = false;
    client->cuda_sync_inflight = false;
    // Reset client state
    reset_client_read_state(client);
}


int do_sync_stream(client_t *client) {
    assert(client != NULL);
    if (client->cuda_operation_inflight) {
        client->work_req.data = client;
        client->state = CUDA_SYNC;
        //cudaSyncStream is thread-safe.
        assert(loop != NULL);
        client->cuda_sync_inflight = true;
        uv_queue_work(loop, &client->work_req, wait_for_cuda_completion, after_cuda_completion);
        //sync stream will handle return code by itself
        return 0;
    }
    return FINISH;
}

//return value of handle_request:
//if ret is less than 0, it is an system error, outer code will close the connection
//if ret is greater than 0, it is an application error or success
int handle_request(client_t *client) {
    printf("handle request %c\n", client->header.op);
    auto start = std::chrono::high_resolution_clock::now();
    int return_code = SYSTEM_ERROR;

   if (client->header.op == OP_SYNC) {
        return_code = do_sync_stream(client);
        //do_sync_stream will handle return code by itself
        if (return_code == 0) {
            return 0;
        }
    }  else if (client->header.op == OP_W) {
        return_code = do_write_kvcache(client);
    } else if (client->header.op == OP_R) {
        return_code = do_read_kvcache(client);
    } else {
        return_code = INVALID_REQ;
    }
    
    printf("return code: %d\n", return_code);

    //if application error or success, send the return code
    uv_write_t* write_req = (uv_write_t*)malloc(sizeof(uv_write_t));
    memcpy(client->send_buffer, &return_code, RETURN_CODE_SIZE);
    write_req->data = client;
    uv_buf_t wbuf = uv_buf_init(client->send_buffer, RETURN_CODE_SIZE);
    uv_write(write_req, (uv_stream_t *)client->handle, &wbuf, 1, on_write);
    //success
    //keep connection alive
    reset_client_read_state(client);

    auto end = std::chrono::high_resolution_clock::now(); // 结束时间
    std::chrono::duration<double, std::milli> elapsed = end - start; // 计算运行时间
    std::cout << "handle request runtime: " << elapsed.count() << " ms" << std::endl;
    return 0;
}


void on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    client_t* client = (client_t*)stream->data;
    ssize_t offset = 0;

    int ret;
    if (nread < 0) {
        if (nread != UV_EOF)
            fprintf(stderr, "Read error %s\n", uv_err_name(nread));
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
                    if (client->header.op == OP_R || client->header.op == OP_W) {
                        int ret = veryfy_header(&client->header);
                        if (ret != 0) {
                            fprintf(stderr, "Invalid header\n");
                            uv_close((uv_handle_t*)stream, on_close);
                            goto clean_up;
                        }

                        //prepare for reading body
                        client->expected_bytes = client->header.body_size;
                        client->bytes_read = 0;
                        printf("reallocate buffer for body\n");
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

                printf("reading body\n");
                size_t to_copy = MIN(nread - offset, client->expected_bytes - client->bytes_read);

                memcpy(client->recv_buffer + client->bytes_read, buf->base + offset, to_copy);
                client->bytes_read += to_copy;
                offset += to_copy;
                if (client->bytes_read == client->expected_bytes) {
                    printf("body read done, size %d\n", client->expected_bytes);
                    print_vector(client->recv_buffer, 10);
                    if (!deserialize_local_meta(client->recv_buffer, client->expected_bytes, client->local_meta)){
                        printf("failed to deserialize local meta\n");
                        uv_close((uv_handle_t*)stream, on_close);
                        goto clean_up;
                    }
                    print_ipc_handle(client->local_meta.ipc_handle);
                    handle_request(client);
                }
                break;
            }

            case CUDA_SYNC: {
                int ret = RETRY;
                uv_write_t* write_req = (uv_write_t*)malloc(sizeof(uv_write_t));
                memcpy(client->send_buffer, &ret, RETURN_CODE_SIZE);
                write_req->data = client;
                uv_buf_t wbuf = uv_buf_init(client->send_buffer, RETURN_CODE_SIZE);
                uv_write(write_req, stream, &wbuf, 1, on_write);
                break;
            }
        }
    }

clean_up:
    free(buf->base);

}

void on_new_connection(uv_stream_t* server, int status) {
    printf("New connection~\n");
    if (status < 0) {
        fprintf(stderr, "New connection error %s\n", uv_strerror(status));
        return;
    }
    uv_tcp_t* client_handle = (uv_tcp_t*)malloc(sizeof(uv_tcp_t));
    uv_tcp_init(loop, client_handle);
    if (uv_accept(server, (uv_stream_t*)client_handle) == 0) {
        client_t *client = new client_t();
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

int register_server(unsigned int loop_ptr) {
    loop = (uv_loop_t *)loop_ptr;
    assert(loop != NULL);
    uv_tcp_init(loop, &server);
    struct sockaddr_in addr;
    uv_ip4_addr("0.0.0.0", PORT, &addr);

    uv_tcp_bind(&server, (const struct sockaddr*)&addr, 0);
    int r = uv_listen((uv_stream_t*) &server, 128, on_new_connection);
    if (r) {
        fprintf(stderr, "Listen error: %s\n", uv_strerror(r));
        return 1;
    }

    return 0;
}