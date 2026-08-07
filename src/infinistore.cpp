// single thread right now.
#include "infinistore.h"

#include <arpa/inet.h>
#include <assert.h>
#include <execinfo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <chrono>
#include <deque>
#include <future>
#include <iostream>
#include <string>
#include <unordered_map>

#include "ibv_helper.h"
#include "protocol.h"
#include "rdma.h"

server_config_t global_config;

uv_loop_t *loop;
uv_tcp_t server;

MM *mm;

struct rdma_device rdma_dev;

// indicate if the MM extend is in flight
bool extend_in_flight = false;

// evict memory from head to tail, the PTR is shared by lru_queue and kv_map
// so we have to pop from both to evict the memory
std::list<boost::intrusive_ptr<PTR>> lru_queue;
std::unordered_map<std::string, boost::intrusive_ptr<PTR>> kv_map;

typedef enum {
    READ_HEADER,
    READ_BODY,
    READ_VALUE_THROUGH_TCP,
} read_state_t;

// the max data could be send in uv_write
static const size_t MAX_SEND_SIZE = 256 << 10;

const float ON_DEMAND_MIN_THRESHOLD = 0.8;
const float ON_DEMAND_MAX_THRESHOLD = 0.95;

struct Client {
    uv_tcp_t handle_;            // uv_stream_t
    read_state_t state_;         // state of the client, for parsing the request
    size_t bytes_read_ = 0;      // bytes read so far, for parsing the request
    size_t expected_bytes_ = 0;  // expected size of the body
    header_t header_;

    boost::intrusive_ptr<PTR> current_tcp_task_;

    // RDMA recv buffer
    char *recv_buffer_[MAX_RECV_WR] = {};
    struct ibv_mr *recv_mr_[MAX_RECV_WR] = {};

    // RDMA send buffer
    char *send_buffer_ = NULL;
    struct ibv_mr *send_mr_ = NULL;
    int outstanding_rdma_ops_ = 0;
    std::deque<std::pair<struct ibv_send_wr *, struct ibv_sge *>> outstanding_rdma_ops_queue_;

    // TCP send buffer
    char *tcp_send_buffer_ = NULL;
    char *tcp_recv_buffer_ = NULL;

    rdma_conn_info_t remote_info_;
    rdma_conn_info_t local_info_;

    bool rdma_connected_ = false;

    rdma_context rdma_ctx_;

    // poll handle of the rdma completion channel, only initialized after the
    // rdma exchange is done
    uv_poll_t poll_handle_;
    bool poll_handle_initialized_ = false;

    // libuv requires the memory of a handle to stay valid until its close callback
    // has been called. both handles above live inside this client, so the client
    // must outlive them: close() starts closing every handle it owns and the last
    // close callback to run deletes the client. see close()/on_handle_closed().
    bool closing_ = false;
    int pending_handles_ = 0;

    Client() = default;
    Client(const Client &) = delete;
    ~Client();

    // the only way to tear down a client, safe to call more than once
    void close();
    void on_handle_closed();

    void cq_poll_handle(uv_poll_t *handle, int status, int events);
    int read_rdma_cache(const RemoteMetaRequest *req);
    int write_rdma_cache(const RemoteMetaRequest *req);
    void post_ack(int return_code);
    int allocate_rdma(const RemoteMetaRequest *req);
    // send response to client through TCP
    void send_resp(int return_code, void *buf, size_t size);
    int tcp_payload_request(const TCPPayloadRequest *request);
    int sync_stream();
    void reset_client_read_state();
    int check_key(const std::string &key_to_check);
    int get_match_last_index(const GetMatchLastIndexRequest *request);
    int delete_keys(const DeleteKeysRequest *request);
    int rdma_exchange();
    int prepare_recv_rdma_request(int buf_idx);
    void perform_batch_rdma(const RemoteMetaRequest *remote_meta_req,
                            std::vector<boost::intrusive_ptr<PTR>> *inflight_rdma_ops,
                            enum ibv_wr_opcode opcode);
};

typedef struct Client client_t;

Client::~Client() {
    INFO("free client resources");

    // every handle owned by this client is fully closed at this point, so the
    // resources they were using(the rdma completion channel below) can be released.
    assert(pending_handles_ == 0);

    if (send_mr_) {
        ibv_dereg_mr(send_mr_);
        send_mr_ = NULL;
    }

    if (send_buffer_) {
        free(send_buffer_);
        send_buffer_ = NULL;
    }

    for (int i = 0; i < MAX_RECV_WR; i++) {
        if (recv_mr_[i]) {
            assert(recv_buffer_[i] != NULL);
            ibv_dereg_mr(recv_mr_[i]);
            recv_mr_[i] = NULL;
        }

        if (recv_buffer_[i]) {
            free(recv_buffer_[i]);
            recv_buffer_[i] = NULL;
        }
    }

    if (tcp_send_buffer_) {
        free(tcp_send_buffer_);
        tcp_send_buffer_ = NULL;
    }

    if (tcp_recv_buffer_) {
        free(tcp_recv_buffer_);
        tcp_recv_buffer_ = NULL;
    }

    for (auto &item : outstanding_rdma_ops_queue_) {
        delete[] item.first;
        delete[] item.second;
    }

    destroy_rdma_context(&rdma_ctx_);
}

void on_handle_closed(uv_handle_t *handle) {
    client_t *client = (client_t *)handle->data;
    client->on_handle_closed();
}

void Client::close() {
    if (closing_) {
        // on_read/on_write/on_chunk_write... can all decide to tear down the same
        // client, and a handle may only be closed once.
        return;
    }
    closing_ = true;

    uv_close((uv_handle_t *)&handle_, ::on_handle_closed);
    if (poll_handle_initialized_) {
        uv_close((uv_handle_t *)&poll_handle_, ::on_handle_closed);
    }
}

void Client::on_handle_closed() {
    assert(pending_handles_ > 0);
    // the client owns the memory of its handles, so it can only go away once the
    // loop is done with all of them.
    if (--pending_handles_ == 0) {
        delete this;
    }
}

struct BulkWriteCtx {
    client_t *client;  // weak ptr
    uint32_t header_buf[2];
    boost::intrusive_ptr<PTR> ptr;
    size_t offset;
    size_t total_size;

    BulkWriteCtx(client_t *client, boost::intrusive_ptr<PTR> ptr)
        : client(client), ptr(ptr), offset(0), total_size(ptr->size) {
        header_buf[0] = FINISH;
        header_buf[1] = static_cast<uint32_t>(ptr->size);
    }
    BulkWriteCtx(BulkWriteCtx &) = delete;
    ~BulkWriteCtx() = default;
};

void on_chunk_write(uv_write_t *req, int status) {
    BulkWriteCtx *ctx = (BulkWriteCtx *)req->data;
    uv_handle_t *handle = (uv_handle_t *)req->handle;
    free(req);

    if (uv_is_closing(handle)) {
        delete ctx;
        return;
    }

    if (status < 0) {
        ERROR("Write error {}", uv_strerror(status));
        ctx->client->close();
        delete ctx;
        return;
    }

    if (ctx->offset == ctx->total_size) {
        DEBUG("write done");
        ctx->client->reset_client_read_state();
        delete ctx;
        return;
    }
    size_t remain = ctx->total_size - ctx->offset;
    size_t send_size = MIN(remain, MAX_SEND_SIZE);
    uv_buf_t buf = uv_buf_init((char *)ctx->ptr->ptr + ctx->offset, send_size);
    ctx->offset += send_size;
    uv_write_t *write_req = (uv_write_t *)malloc(sizeof(uv_write_t));
    write_req->data = ctx;
    uv_write(write_req, (uv_stream_t *)&ctx->client->handle_, &buf, 1, on_chunk_write);
}

void on_head_write(uv_write_t *req, int status) {
    BulkWriteCtx *ctx = (BulkWriteCtx *)req->data;
    uv_handle_t *handle = (uv_handle_t *)req->handle;
    free(req);

    if (uv_is_closing(handle)) {
        delete ctx;
        return;
    }

    if (status < 0) {
        ERROR("Write error {}", uv_strerror(status));
        ctx->client->close();
        delete ctx;
        return;
    }

    DEBUG("header write done");
    size_t remain = ctx->total_size;
    size_t send_size = MIN(remain, MAX_SEND_SIZE);
    uv_buf_t buf = uv_buf_init((char *)ctx->ptr->ptr, send_size);
    ctx->offset += send_size;
    uv_write_t *write_req = (uv_write_t *)malloc(sizeof(uv_write_t));
    write_req->data = ctx;
    uv_write(write_req, (uv_stream_t *)&ctx->client->handle_, &buf, 1, on_chunk_write);
}

void evict_cache(float min_threshold, float max_threshold) {
    if (mm->usage() >= max_threshold) {
        // stop when mm->usage is below min_threshold
        float usage = mm->usage();
        while (mm->usage() >= min_threshold && !lru_queue.empty()) {
            auto ptr = lru_queue.front();
            lru_queue.pop_front();
            kv_map.erase(ptr->key);
        }
        INFO("evict memory done, usage: from {:.2f} => {:.2f}", usage, mm->usage());
    }
}

int Client::tcp_payload_request(const TCPPayloadRequest *req) {
    DEBUG("do tcp_payload_request... {}", op_name(req->op()));

    switch (req->op()) {
        case OP_TCP_PUT: {
            evict_cache(ON_DEMAND_MIN_THRESHOLD, ON_DEMAND_MAX_THRESHOLD);

            bool allocated =
                mm->allocate(req->value_length(), 1,
                             [&](void *addr, uint32_t lkey, uint32_t rkey, int pool_idx) {
                                 current_tcp_task_ = boost::intrusive_ptr<PTR>(new PTR(
                                     addr, req->value_length(), pool_idx, req->key()->str()));
                             });
            if (!allocated) {
                ERROR("Failed to allocate memory");
                return OUT_OF_MEMORY;
            }
            DEBUG("allocated memory: addr: {}, lkey: {}, rkey: {}", current_tcp_task_->ptr,
                  mm->get_lkey(current_tcp_task_->pool_idx),
                  mm->get_rkey(current_tcp_task_->pool_idx));

            kv_map[req->key()->str()] = current_tcp_task_;
            // set state machine
            state_ = READ_VALUE_THROUGH_TCP;
            bytes_read_ = 0;
            expected_bytes_ = req->value_length();
            break;
        }
        case OP_TCP_GET: {
            auto it = kv_map.find(req->key()->str());
            if (it == kv_map.end()) {
                return KEY_NOT_FOUND;
            }
            auto ptr = it->second;

            // move ptr to the end of lru_queue
            lru_queue.erase(ptr->lru_it);
            lru_queue.push_back(ptr);
            ptr->lru_it = --lru_queue.end();

            uv_write_t *write_req = (uv_write_t *)malloc(sizeof(uv_write_t));

            // safe PTR to prevent it from being deleted early.
            write_req->data = new BulkWriteCtx(this, ptr);

            uv_buf_t buf = uv_buf_init((char *)((BulkWriteCtx *)write_req->data)->header_buf,
                                       sizeof(uint32_t) * 2);

            uv_write(write_req, (uv_stream_t *)&handle_, &buf, 1, on_head_write);

            break;
        }
    }
    return 0;
}

void Client::post_ack(int return_code) {
    // send an error code back
    struct ibv_send_wr wr = {0};
    struct ibv_send_wr *bad_wr = NULL;
    wr.wr_id = 0;
    wr.opcode = IBV_WR_SEND_WITH_IMM;
    wr.imm_data = return_code;
    wr.send_flags = 0;
    wr.sg_list = NULL;
    wr.num_sge = 0;
    wr.next = NULL;
    int ret = ibv_post_send(rdma_ctx_.qp, &wr, &bad_wr);
    if (ret) {
        ERROR("Failed to send WITH_IMM message: {}", strerror(ret));
    }
}

void Client::cq_poll_handle(uv_poll_t *handle, int status, int events) {
    // TODO: handle completion
    if (status < 0) {
        ERROR("Poll error: {}", uv_strerror(status));
        return;
    }
    struct ibv_cq *cq;
    void *cq_context;

    if (ibv_get_cq_event(rdma_ctx_.comp_channel, &cq, &cq_context) != 0) {
        ERROR("Failed to get CQ event");
        return;
    }
    ibv_ack_cq_events(cq, 1);

    if (ibv_req_notify_cq(cq, 0) != 0) {
        ERROR("Failed to request CQ notification");
        return;
    }
    struct ibv_wc wc = {0};
    while (ibv_poll_cq(cq, 1, &wc) > 0) {
        if (wc.status == IBV_WC_SUCCESS) {
            if (wc.opcode == IBV_WC_RECV) {  // recv RDMA read/write request
                const RemoteMetaRequest *request = GetRemoteMetaRequest(recv_buffer_[wc.wr_id]);

                DEBUG("Received remote meta request OP {}", op_name(request->op()));

                switch (request->op()) {
                    case OP_RDMA_WRITE: {
                        int ret = write_rdma_cache(request);
                        if (ret != 0) {
                            post_ack(ret);
                        }
                        break;
                    }
                    case OP_RDMA_READ: {
                        int ret = read_rdma_cache(request);
                        if (ret != 0) {
                            post_ack(ret);
                        }
                        break;
                    }
                    default:
                        ERROR("Unexpected request op: {}", request->op());
                        break;
                }

                DEBUG("ready for next request");
                if (prepare_recv_rdma_request(wc.wr_id) < 0) {
                    ERROR("Failed to prepare recv rdma request");
                    return;
                }
            }
            else if (wc.opcode == IBV_WC_SEND) {  // allocate: response sent
                DEBUG("allocate response sent");
            }
            else if (wc.opcode ==
                     IBV_WC_RECV_RDMA_WITH_IMM) {  // write cache: we already have all data now.
                // client should not use WRITE_WITH_IMM to notify.
                // it should use COMMIT message to notify.
                WARN("WRITE_WITH_IMM is not supported in server side");
                if (prepare_recv_rdma_request(wc.wr_id) < 0) {
                    ERROR("Failed to prepare recv rdma request");
                    return;
                }
            }
            else if (wc.opcode == IBV_WC_RDMA_WRITE || wc.opcode == IBV_WC_RDMA_READ) {
                // some RDMA write(read cache WRs) is finished

                assert(outstanding_rdma_ops_ >= 0);
                outstanding_rdma_ops_ -= MAX_WR_BATCH;

                if (!outstanding_rdma_ops_queue_.empty()) {
                    auto item = outstanding_rdma_ops_queue_.front();
                    struct ibv_send_wr *wrs = item.first;
                    struct ibv_sge *sges = item.second;
                    ibv_send_wr *bad_wr = nullptr;
                    DEBUG("IBV POST SEND, wr_id: {}", wrs[0].wr_id);
                    int ret = ibv_post_send(rdma_ctx_.qp, &wrs[0], &bad_wr);
                    if (ret) {
                        ERROR("Failed to post RDMA write {}", strerror(ret));
                        throw std::runtime_error("Failed to post RDMA write");
                    }
                    outstanding_rdma_ops_ += MAX_WR_BATCH;
                    delete[] wrs;
                    delete[] sges;
                    outstanding_rdma_ops_queue_.pop_front();
                }

                if (wc.wr_id > 0) {
                    // last WR will inform that all RDMA write is finished,so we can dereference PTR
                    if (wc.opcode == IBV_WC_RDMA_READ) {
                        auto inflight_rdma_writes =
                            (std::vector<boost::intrusive_ptr<PTR>> *)wc.wr_id;
                        for (auto ptr : *inflight_rdma_writes) {
                            kv_map[ptr->key] = ptr;
                            DEBUG("writing key done, {}", ptr->key);
                            lru_queue.push_back(ptr);
                            ptr->lru_it = --lru_queue.end();
                        }
                        delete inflight_rdma_writes;
                        post_ack(FINISH);
                    }
                    else if (wc.opcode == IBV_WC_RDMA_WRITE) {
                        post_ack(FINISH);
                        auto inflight_rdma_reads =
                            (std::vector<boost::intrusive_ptr<PTR>> *)wc.wr_id;
                        delete inflight_rdma_reads;
                    }
                }
            }
            else {
                ERROR("Unexpected wc opcode: {}", (int)wc.opcode);
            }
        }
        else {
            ERROR("CQ error: {}, {}", ibv_wc_status_str(wc.status), wc.wr_id);
        }
    }
}

void add_mempool(uv_work_t *req) { mm->add_mempool(rdma_dev.pd); }

void add_mempool_completion(uv_work_t *req, int status) {
    extend_in_flight = false;
    mm->need_extend = false;
    delete req;
}

void extend_mempool() {
    if (global_config.auto_increase && mm->need_extend && !extend_in_flight) {
        INFO("Extend another mempool");
        uv_work_t *req = new uv_work_t();
        uv_queue_work(loop, req, add_mempool, add_mempool_completion);
        extend_in_flight = true;
    }
}

int Client::prepare_recv_rdma_request(int buf_idx) {
    struct ibv_sge sge = {0};
    struct ibv_recv_wr rwr = {0};
    struct ibv_recv_wr *bad_wr = NULL;
    sge.addr = (uintptr_t)(recv_buffer_[buf_idx]);
    sge.length = PROTOCOL_BUFFER_SIZE;
    sge.lkey = recv_mr_[buf_idx]->lkey;

    rwr.wr_id = buf_idx;
    rwr.next = NULL;
    rwr.sg_list = &sge;
    rwr.num_sge = 1;
    if (ibv_post_recv(rdma_ctx_.qp, &rwr, &bad_wr)) {
        ERROR("Failed to post receive, {}");
        return -1;
    }
    return 0;
}

void Client::perform_batch_rdma(const RemoteMetaRequest *remote_meta_req,
                                std::vector<boost::intrusive_ptr<PTR>> *inflight_rdma_ops,
                                enum ibv_wr_opcode opcode) {
    assert(opcode == IBV_WR_RDMA_READ || opcode == IBV_WR_RDMA_WRITE);

    const size_t max_wr = MAX_WR_BATCH;
    struct ibv_send_wr local_wrs[max_wr];
    struct ibv_sge local_sges[max_wr];

    struct ibv_send_wr *wrs = local_wrs;
    struct ibv_sge *sges = local_sges;

    size_t num_wr = 0;
    bool wr_full = false;

    if (outstanding_rdma_ops_ + max_wr > MAX_RDMA_OPS_WR) {
        wr_full = true;
        wrs = new struct ibv_send_wr[max_wr];
        sges = new struct ibv_sge[max_wr];
    }

    int n = remote_meta_req->keys()->size();
    for (int i = 0; i < n; i++) {
        sges[num_wr].addr = (uintptr_t)(*inflight_rdma_ops)[i]->ptr;
        sges[num_wr].length = (*inflight_rdma_ops)[i]->size;
        sges[num_wr].lkey = mm->get_lkey((*inflight_rdma_ops)[i]->pool_idx);

        wrs[num_wr].wr_id = 0;
        wrs[num_wr].opcode = opcode;
        wrs[num_wr].sg_list = &sges[num_wr];
        wrs[num_wr].num_sge = 1;
        wrs[num_wr].wr.rdma.remote_addr = remote_meta_req->remote_addrs()->Get(i);
        wrs[num_wr].wr.rdma.rkey = remote_meta_req->rkey();

        // wrs[num_wr].wr.rdma.rkey = remote_meta_req->rkey();
        wrs[num_wr].next = (num_wr == max_wr - 1 || i == (int)remote_meta_req->keys()->size() - 1)
                               ? nullptr
                               : &wrs[num_wr + 1];

        wrs[num_wr].send_flags =
            (num_wr == max_wr - 1 || i == (int)remote_meta_req->keys()->size() - 1)
                ? IBV_SEND_SIGNALED
                : 0;

        if (i == remote_meta_req->keys()->size() - 1) {
            wrs[num_wr].wr_id = (uintptr_t)inflight_rdma_ops;
        }

        num_wr++;

        if (num_wr == max_wr || i == remote_meta_req->keys()->size() - 1) {
            if (!wr_full) {
                struct ibv_send_wr *bad_wr = nullptr;
                int ret = ibv_post_send(rdma_ctx_.qp, &wrs[0], &bad_wr);
                if (ret) {
                    ERROR("Failed to post RDMA write {}", strerror(ret));
                    return;
                }
                outstanding_rdma_ops_ += max_wr;

                // check if next iteration will exceed the limit
                if (outstanding_rdma_ops_ + max_wr > MAX_RDMA_OPS_WR) {
                    wr_full = true;
                }
            }
            else {
                // if WR queue is full, we need to put them into queue
                WARN(
                    "WR queue full: push into queue, len: {}, first wr_id: {}, last wr_id: {}, "
                    "last op code: {} ",
                    num_wr, wrs[0].wr_id, wrs[num_wr - 1].wr_id,
                    static_cast<int>(wrs[num_wr - 1].opcode));
                outstanding_rdma_ops_queue_.push_back({&wrs[0], &sges[0]});
            }

            if (wr_full) {
                wrs = new struct ibv_send_wr[max_wr];
                sges = new struct ibv_sge[max_wr];
            }

            num_wr = 0;  // Reset the counter for the next batch
        }
    }
}

int Client::write_rdma_cache(const RemoteMetaRequest *remote_meta_req) {
    DEBUG("do rdma write... num of keys: {}", remote_meta_req->keys()->size());

    if (remote_meta_req->keys()->size() != remote_meta_req->remote_addrs()->size()) {
        ERROR("keys size and remote_addrs size mismatch");
        return INVALID_REQ;
    }

    // allocate memory
    int block_size = remote_meta_req->block_size();
    int n = remote_meta_req->keys()->size();

    // create something.

    auto *inflight_rdma_writes = new std::vector<boost::intrusive_ptr<PTR>>;
    inflight_rdma_writes->reserve(n);

    evict_cache(ON_DEMAND_MIN_THRESHOLD, ON_DEMAND_MAX_THRESHOLD);

    int key_idx = 0;
    bool allocated =
        mm->allocate(block_size, n, [&](void *addr, uint32_t lkey, uint32_t rkey, int pool_idx) {
            const auto *key = remote_meta_req->keys()->Get(key_idx);
            auto ptr = boost::intrusive_ptr<PTR>(new PTR(addr, block_size, pool_idx, key->str()));
            DEBUG("writing key: {}", key->str());
            inflight_rdma_writes->push_back(ptr);
            key_idx++;
        });

    if (!allocated) {
        ERROR("Failed to allocate memory");
        delete inflight_rdma_writes;
        return OUT_OF_MEMORY;
    }

    // perform rdma read to receive data from client
    // read remote address data to local address
    perform_batch_rdma(remote_meta_req, inflight_rdma_writes, IBV_WR_RDMA_READ);

    return 0;
}

int Client::read_rdma_cache(const RemoteMetaRequest *remote_meta_req) {
    DEBUG("do rdma read... num of keys: {}", remote_meta_req->keys()->size());

    if (remote_meta_req->keys()->size() != remote_meta_req->remote_addrs()->size()) {
        ERROR("keys size and remote_addrs size mismatch");
        return INVALID_REQ;
    }

    auto *inflight_rdma_reads = new std::vector<boost::intrusive_ptr<PTR>>;

    inflight_rdma_reads->reserve(remote_meta_req->keys()->size());

    for (const auto *key : *remote_meta_req->keys()) {
        auto it = kv_map.find(key->str());
        if (it == kv_map.end()) {
            WARN("Key not found: {}", key->str());
            return KEY_NOT_FOUND;
        }
        const auto &ptr = it->second;

        if (ptr->size > remote_meta_req->block_size()) {
            WARN("remote region does not enough size: key:{}, actual size: {}, remote size :{}",
                 key->str(), ptr->size, remote_meta_req->block_size());
            return INVALID_REQ;
        }

        inflight_rdma_reads->push_back(ptr);
    }

    // loop over inflight_rdma_reads to update lru_queue
    for (auto ptr : *inflight_rdma_reads) {
        lru_queue.erase(ptr->lru_it);
        lru_queue.push_back(ptr);
        ptr->lru_it = --lru_queue.end();
    }

    // write to  remote address data from local address
    perform_batch_rdma(remote_meta_req, inflight_rdma_reads, IBV_WR_RDMA_WRITE);

    return 0;
}

// FIXME:
void Client::reset_client_read_state() {
    state_ = READ_HEADER;
    bytes_read_ = 0;
    expected_bytes_ = FIXED_HEADER_SIZE;
    memset(&header_, 0, sizeof(header_t));
    // keep the tcp_recv_buffer/tcp_send_buffer as it is
}

void alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
    buf->base = (char *)malloc(suggested_size);
    buf->len = suggested_size;
}

int verify_header(header_t *header) {
    if (header->magic != MAGIC) {
        return INVALID_REQ;
    }
    // TODO: add more checks
    return 0;
}

void on_write(uv_write_t *req, int status) {
    client_t *client = (client_t *)((uv_handle_t *)req->handle)->data;
    free(req);

    if (status < 0) {
        // libuv flushes the pending write requests with UV_ECANCELED while the
        // handle is being closed, so this is also reached during a normal teardown.
        // Client::close() is idempotent, which makes that harmless.
        ERROR("Write error {}", uv_strerror(status));
        client->close();
    }
}

int Client::rdma_exchange() {
    INFO("do rdma exchange...");

    int ret;

    if (rdma_connected_ == true) {
        ERROR("RDMA already connected");
        return SYSTEM_ERROR;
    }

    // create rdma context per connection.
    // use the global rdma_dev
    init_rdma_context(&rdma_ctx_, &rdma_dev);

    local_info_ = get_rdma_conn_info(&rdma_ctx_, &rdma_dev);

    print_rdma_conn_info(&local_info_, false);
    print_rdma_conn_info(&remote_info_, true);

    // Modify QP to RTR state
    if (modify_qp_to_rtr(&rdma_ctx_, &rdma_dev, &remote_info_)) {
        ERROR("Failed to modify QP to RTR");
        return -1;
    }

    if (modify_qp_to_rts(&rdma_ctx_)) {
        ERROR("Failed to modify QP to RTS");
        return -1;
    }

    INFO("RDMA exchange done");

    rdma_connected_ = true;

    if (posix_memalign((void **)&send_buffer_, 4096, PROTOCOL_BUFFER_SIZE) != 0) {
        ERROR("Failed to allocate send buffer");
        return SYSTEM_ERROR;
    }

    send_mr_ = ibv_reg_mr(rdma_dev.pd, send_buffer_, PROTOCOL_BUFFER_SIZE, IBV_ACCESS_LOCAL_WRITE);
    if (!send_mr_) {
        ERROR("Failed to register MR");
        return SYSTEM_ERROR;
    }

    for (int i = 0; i < MAX_RECV_WR; i++) {
        if (posix_memalign((void **)&recv_buffer_[i], 4096, PROTOCOL_BUFFER_SIZE) != 0) {
            ERROR("Failed to allocate recv buffer");
            return SYSTEM_ERROR;
        }

        recv_mr_[i] =
            ibv_reg_mr(rdma_dev.pd, recv_buffer_[i], PROTOCOL_BUFFER_SIZE, IBV_ACCESS_LOCAL_WRITE);
        if (!recv_mr_[i]) {
            ERROR("Failed to register MR");
            return SYSTEM_ERROR;
        }

        if (prepare_recv_rdma_request(i) < 0) {
            ERROR("Failed to prepare recv rdma request");
            return SYSTEM_ERROR;
        }
    }

    if (ibv_req_notify_cq(rdma_ctx_.cq, 0)) {
        ERROR("Failed to request notify for CQ");
        return SYSTEM_ERROR;
    }

    if (uv_poll_init(loop, &poll_handle_, rdma_ctx_.comp_channel->fd) < 0) {
        ERROR("Failed to init poll handle");
        return SYSTEM_ERROR;
    }
    // the handle is registered in the loop now, from here on it has to be closed
    // before this client can be deleted.
    poll_handle_initialized_ = true;
    pending_handles_++;

    poll_handle_.data = this;
    uv_poll_start(&poll_handle_, UV_READABLE | UV_WRITABLE,
                  [](uv_poll_t *handle, int status, int events) {
                      client_t *client = static_cast<client_t *>(handle->data);
                      client->cq_poll_handle(handle, status, events);
                  });

    // Send server's RDMA connection info to client
    send_resp(FINISH, &local_info_, sizeof(local_info_));
    reset_client_read_state();
    return 0;
}

// send_resp send fixed size response to client.
void Client::send_resp(int return_code, void *buf, size_t size) {
    if (size > 0) {
        assert(buf != NULL);
    }
    uv_write_t *write_req = (uv_write_t *)malloc(sizeof(uv_write_t));

    tcp_send_buffer_ = (char *)realloc(tcp_send_buffer_, size + RETURN_CODE_SIZE);

    memcpy(tcp_send_buffer_, &return_code, RETURN_CODE_SIZE);
    memcpy(tcp_send_buffer_ + RETURN_CODE_SIZE, buf, size);
    write_req->data = this;
    uv_buf_t wbuf = uv_buf_init(tcp_send_buffer_, size + RETURN_CODE_SIZE);
    uv_write(write_req, (uv_stream_t *)&handle_, &wbuf, 1, on_write);
}

int Client::check_key(const std::string &key_to_check) {
    int ret;
    // check if the key exists and committed
    if (kv_map.count(key_to_check) > 0) {
        ret = 0;
    }
    else {
        ret = 1;
    }

    send_resp(FINISH, &ret, sizeof(ret));
    reset_client_read_state();
    return 0;
}

int Client::get_match_last_index(const GetMatchLastIndexRequest *request) {
    int left = 0, right = request->keys()->size();
    while (left < right) {
        int mid = left + (right - left) / 2;
        request->keys()->Get(mid);
        if (kv_map.count(request->keys()->Get(mid)->str())) {
            left = mid + 1;
        }
        else {
            right = mid;
        }
    }
    left--;
    send_resp(FINISH, &left, sizeof(left));
    reset_client_read_state();
    return 0;
}

/**
 * The function deletes a list of keys in kv_map.
 *
 * Input:
 *     request: the request contains a list of the keys to delete
 *
 * Output:
 *     the actual count of the keys deleted.
 *
 * Return:
 *     success code
 *
 * NOTE: The function handles the runtime_error and continue for next key
 **/
int Client::delete_keys(const DeleteKeysRequest *request) {
    int count = 0;
    for (const auto *key : *request->keys()) {
        auto it = kv_map.find(key->str());
        if (it != kv_map.end()) {
            auto ptr = it->second;
            kv_map.erase(it);
            lru_queue.erase(ptr->lru_it);
            count++;
        }
    }
    send_resp(FINISH, &count, sizeof(count));
    reset_client_read_state();
    return 0;
}

// return value of handle_request:
// if ret is less than 0, it is an system error, outer code will close the
// connection if ret is greater than 0, it is an application error or success
void handle_request(uv_stream_t *stream, client_t *client) {
    auto start = std::chrono::high_resolution_clock::now();
    int error_code = 0;
    // if error_code is not 0, close the connection
    switch (client->header_.op) {
        case OP_RDMA_EXCHANGE: {
            memcpy((void *)(&client->remote_info_), client->tcp_recv_buffer_,
                   client->expected_bytes_);
            error_code = client->rdma_exchange();
            break;
        }
        case OP_CHECK_EXIST: {
            std::string key_to_check(client->tcp_recv_buffer_, client->expected_bytes_);
            DEBUG("check key: {}", key_to_check);
            error_code = client->check_key(key_to_check);
            break;
        }
        case OP_GET_MATCH_LAST_IDX: {
            const GetMatchLastIndexRequest *request =
                GetGetMatchLastIndexRequest(client->tcp_recv_buffer_);
            error_code = client->get_match_last_index(request);
            break;
        }
        case OP_DELETE_KEYS: {
            DEBUG("delete keys...");
            const DeleteKeysRequest *request = GetDeleteKeysRequest(client->tcp_recv_buffer_);
            error_code = client->delete_keys(request);
            break;
        }
        case OP_TCP_PAYLOAD: {
            DEBUG("TCP GET/PUT data...");
            const TCPPayloadRequest *request = GetTCPPayloadRequest(client->tcp_recv_buffer_);
            error_code = client->tcp_payload_request(request);
            break;
        }
        default:
            ERROR("Invalid request");
            error_code = INVALID_REQ;
            break;
    }

    if (error_code != 0) {
        client->send_resp(error_code, NULL, 0);
        client->reset_client_read_state();
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> elapsed = end - start;
}

void on_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
    client_t *client = (client_t *)stream->data;
    ssize_t offset = 0;

    if (nread < 0) {
        if (nread != UV_EOF)
            ERROR("Read error {}", uv_err_name(nread));
        client->close();
        goto clean_up;
    }

    while (offset < nread) {
        switch (client->state_) {
            case READ_HEADER: {
                size_t to_copy = MIN(nread - offset, FIXED_HEADER_SIZE - client->bytes_read_);
                memcpy(((char *)&client->header_) + client->bytes_read_, buf->base + offset,
                       to_copy);
                client->bytes_read_ += to_copy;
                offset += to_copy;
                if (client->bytes_read_ == FIXED_HEADER_SIZE) {
                    DEBUG("HEADER: op: {}, body_size :{}", client->header_.op,
                          (unsigned int)client->header_.body_size);

                    int ret = verify_header(&client->header_);
                    if (ret != 0) {
                        ERROR("Invalid header");
                        client->close();
                        goto clean_up;
                    }
                    // prepare for reading body
                    client->expected_bytes_ = client->header_.body_size;
                    client->bytes_read_ = 0;
                    client->tcp_recv_buffer_ =
                        (char *)realloc(client->tcp_recv_buffer_, client->expected_bytes_);
                    client->state_ = READ_BODY;
                }
                break;
            }

            case READ_BODY: {
                assert(client->tcp_recv_buffer_ != NULL);

                DEBUG("reading body, bytes_read: {}, expected_bytes: {}", client->bytes_read_,
                      client->expected_bytes_);
                size_t to_copy = MIN(nread - offset, client->expected_bytes_ - client->bytes_read_);

                memcpy(client->tcp_recv_buffer_ + client->bytes_read_, buf->base + offset, to_copy);
                client->bytes_read_ += to_copy;
                offset += to_copy;
                if (client->bytes_read_ == client->expected_bytes_) {
                    DEBUG("body read done, size {}", client->expected_bytes_);
                    handle_request(stream, client);
                }
                break;
            }
            case READ_VALUE_THROUGH_TCP: {
                size_t to_copy = MIN(nread - offset, client->expected_bytes_ - client->bytes_read_);
                memcpy(client->current_tcp_task_->ptr + client->bytes_read_, buf->base + offset,
                       to_copy);
                client->bytes_read_ += to_copy;
                offset += to_copy;
                if (client->bytes_read_ == client->expected_bytes_) {
                    auto ptr = client->current_tcp_task_;
                    kv_map[ptr->key] = ptr;

                    // put the ptr into lru queue
                    lru_queue.push_back(ptr);
                    ptr->lru_it = --lru_queue.end();

                    client->current_tcp_task_.reset();
                    client->send_resp(FINISH, NULL, 0);
                    client->reset_client_read_state();
                }
            }
        }
    }
clean_up:
    free(buf->base);
}

void on_new_connection(uv_stream_t *server, int status) {
    INFO("new connection...");
    if (status < 0) {
        ERROR("New connection error {}", uv_strerror(status));
        return;
    }
    client_t *client = new client_t();
    if (uv_tcp_init(loop, &client->handle_) < 0) {
        // the handle never made it into the loop, no close callback will come
        ERROR("Failed to init tcp handle");
        delete client;
        return;
    }
    // the handle is registered in the loop now, from here on it has to be closed
    // before this client can be deleted.
    client->pending_handles_ = 1;
    client->handle_.data = client;

    if (uv_accept(server, (uv_stream_t *)&client->handle_) != 0) {
        client->close();
        return;
    }

    // TODO: use constructor
    client->state_ = READ_HEADER;
    client->bytes_read_ = 0;
    client->expected_bytes_ = FIXED_HEADER_SIZE;
    uv_read_start((uv_stream_t *)&client->handle_, alloc_buffer, on_read);
}

int register_server(unsigned long loop_ptr, server_config_t config) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);

    signal(SIGSEGV, signal_handler);
    signal(SIGABRT, signal_handler);
    signal(SIGBUS, signal_handler);
    signal(SIGFPE, signal_handler);
    signal(SIGILL, signal_handler);

    global_config = config;

    loop = uv_default_loop();

    loop = (uv_loop_t *)loop_ptr;
    assert(loop != NULL);
    uv_tcp_init(loop, &server);
    struct sockaddr_in addr;
    uv_ip4_addr("0.0.0.0", config.service_port, &addr);

    uv_tcp_bind(&server, (const struct sockaddr *)&addr, 0);
    int r = uv_listen((uv_stream_t *)&server, 128, on_new_connection);
    if (r) {
        fprintf(stderr, "Listen error: %s\n", uv_strerror(r));
        return -1;
    }

    INFO("open rdma device {}, link_type {}, hint_gid_index {},", config.dev_name, config.link_type,
         config.hint_gid_index);
    if (open_rdma_device(config.dev_name, config.ib_port, config.link_type, config.hint_gid_index,
                         &rdma_dev) < 0) {
        ERROR("Failed to open RDMA device");
        return -1;
    }
    mm = new MM(config.prealloc_size << 30, config.minimal_allocate_size << 10, rdma_dev.pd);

    INFO("register server done");

    return 0;
}
