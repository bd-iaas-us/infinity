#include <cuda.h>
#include <cuda_runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "protocol.h"
#include "utils.h"
#include <assert.h>
#include <time.h>
#include <math.h>
#include "libinfinity.h"
#include <vector>
#include "ibv_helper.h"
#include "log.h"



Connection::~Connection() {
    DEBUG("destroying connection");
    if (sock) {
        close(sock);
    }
    //print ptr
    if (qp) {
        struct ibv_qp_attr attr;
        memset(&attr, 0, sizeof(attr));
        attr.qp_state = IBV_QPS_ERR;
        ibv_modify_qp(qp, &attr, IBV_QP_STATE);
    }
    if (qp) {
        ibv_destroy_qp(qp);
    }
    if (cq) {
        ibv_destroy_cq(cq);
    }
    if (pd) {
        ibv_dealloc_pd(pd);
    }
    if (ib_ctx) {
        ibv_close_device(ib_ctx);
    }
    for (auto it = local_mr.begin(); it != local_mr.end(); it++) {
        ibv_dereg_mr(it->second);
    }
}

int modify_qp_to_init(struct ibv_qp *qp);
int modify_qp_to_rts(connection_t *conn);
int modify_qp_to_rtr(connection_t *conn);
int exchange_conn_info(connection_t *conn);

int init_rdma_resources(connection_t *conn) {
    // Get list of RDMA devices
    struct ibv_device **dev_list;
    int num_devices;

    dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list) {
        ERROR("Failed to get RDMA devices list");
        return -1;
    }

    // Open the first available device
    conn->ib_ctx = ibv_open_device(dev_list[0]);
    if (!conn->ib_ctx) {
        ERROR("Failed to open RDMA device");
        return -1;
    }
    ibv_free_device_list(dev_list);

    int gidx = ibv_find_sgid_type(conn->ib_ctx, 1, IBV_GID_TYPE_ROCE_V2, AF_INET);
    if (gidx < 0) {
        ERROR("Failed to find GID");
        return -1;
    }
    conn->gidx = gidx;


    union ibv_gid gid;
    //get gid
    if (ibv_query_gid(conn->ib_ctx, 1, gidx, &gid)) {
        ERROR("Failed to get GID");
        return -1;
    }
    
    // Allocate Protection Domain
    conn->pd = ibv_alloc_pd(conn->ib_ctx);
    if (!conn->pd) {
        ERROR("Failed to allocate PD");
        return -1;
    }

    // Create Completion Queue
    conn->cq = ibv_create_cq(conn->ib_ctx, 1025, NULL, NULL, 0);
    if (!conn->cq) {
        ERROR("Failed to create CQ");
        return -1;
    }

    // Create Queue Pair
    struct ibv_qp_init_attr qp_init_attr = {};
    qp_init_attr.send_cq = conn->cq;
    qp_init_attr.recv_cq = conn->cq;
    qp_init_attr.qp_type = IBV_QPT_RC; // Reliable Connection
    qp_init_attr.cap.max_send_wr = 1024;
    qp_init_attr.cap.max_recv_wr = 1024;
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;

    conn->qp = ibv_create_qp(conn->pd, &qp_init_attr);
    if (!conn->qp) {
        ERROR("Failed to create QP");
        return -1;
    }

    // Modify QP to INIT state
    if (modify_qp_to_init(conn->qp)) {
        ERROR("Failed to modify QP to INIT");
        return -1;
    }

    // Get local connection information
    struct ibv_port_attr port_attr;
    if (ibv_query_port(conn->ib_ctx, 1, &port_attr)) {
        ERROR("Failed to query port");
        return -1;
    }

    conn->local_info.qpn = conn->qp->qp_num;
    conn->local_info.psn = lrand48() & 0xffffff;
    conn->local_info.gid = gid;
    DEBUG("gid index: {}", gidx);
    print_rdma_conn_info(&conn->local_info, false);
    print_rdma_conn_info(&conn->remote_info, true);
    return 0;
}


int modify_qp_to_init(struct ibv_qp *qp) {
    struct ibv_qp_attr attr = {};
    attr.qp_state = IBV_QPS_INIT;
    attr.port_num = 1;
    attr.pkey_index = 0;
    attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_LOCAL_WRITE;

    int flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;

    int ret = ibv_modify_qp(qp, &attr, flags);
    if (ret) {
        ERROR("Failed to modify QP to INIT");
        return ret;
    }
    return 0;
}


int perform_rdma_read(connection_t *conn, uintptr_t src_buf, size_t src_size,
                      char * dst_buf, size_t dst_size, uint32_t rkey, uint32_t lkey) {
    
    // Prepare RDMA read operation
    struct ibv_sge sge = {};
    sge.addr = (uintptr_t)dst_buf;
    sge.length = dst_size;
    sge.lkey = lkey;

    struct ibv_send_wr wr = {};
    wr.wr_id = (uintptr_t)conn;
    wr.opcode = IBV_WR_RDMA_READ;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = src_buf; // Obtain from server
    wr.wr.rdma.rkey = rkey;        // Obtain from server

    struct ibv_send_wr *bad_wr = NULL;
    int ret = ibv_post_send(conn->qp, &wr, &bad_wr);
    if (ret) {
        ERROR("Failed to post RDMA read");
        return -1;
    }

    conn->rdma_write_count++;
    DEBUG("RDMA read completed successfully");
    // RDMA read successful
    return 0;
}


int sync_rdma(connection_t *conn) {
    //TODO: implement this function
    struct ibv_wc wc[32];
    int total_completions = conn->rdma_write_count + conn->rdma_read_count;
    int num_completions = 0;

    DEBUG("Waiting for {} completions", total_completions);
    while (num_completions < total_completions) {
        int waiting_completions = MIN(32, total_completions - num_completions);
        int ne = ibv_poll_cq(conn->cq, waiting_completions, wc);
        if (ne < 0) {
            ERROR("Failed to poll CQ");
            return -1;
        }

        for (int i = 0; i < ne; i++) {
            if (wc[i].status != IBV_WC_SUCCESS) {
                ERROR("Failed status {}: wr_id {}\n", ibv_wc_status_str(wc[i].status), (int)wc[i].wr_id);
                return -1;
            }
            num_completions++;
        }
    }

    conn->rdma_write_count = 0;
    conn->rdma_read_count = 0;
    return 0;
}


int perform_rdma_write(connection_t *conn, char * src_buf, size_t src_size,
                       uintptr_t dst_buf, size_t dst_size, uint32_t rkey, uint32_t lkey) {


    // Prepare RDMA write operation
    struct ibv_sge sge = {};
    sge.addr = (uintptr_t)src_buf;
    sge.length = src_size; 
    sge.lkey = lkey;

    struct ibv_send_wr wr = {};
    wr.wr_id = (uintptr_t)conn;
    wr.opcode = IBV_WR_RDMA_WRITE;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = dst_buf; // Obtain from server
    wr.wr.rdma.rkey = rkey;        // Obtain from server

    struct ibv_send_wr *bad_wr = NULL;
    int ret = ibv_post_send(conn->qp, &wr, &bad_wr);
    if (ret) {
        ERROR("Failed to post RDMA write :{}" , strerror(ret));
        return -1;
    }

    conn->rdma_read_count++;
    return 0;
}


int setup_rdma(connection_t *conn) {
    if(init_rdma_resources(conn) < 0) {
        ERROR("Failed to initialize RDMA resources");
        return -1;
    }

    // Exchange RDMA connection information with the server
    if (exchange_conn_info(conn)) {
        ERROR("Failed to exchange connection information");
        return -1;
    }

    // Modify QP to RTR state
    if (modify_qp_to_rtr(conn)) {
        ERROR("Failed to modify QP to RTR");
        return -1;
    }

    if (modify_qp_to_rts(conn)) {
        ERROR("Failed to modify QP to RTS");
        return -1;
    }

    return 0;
}

int init_connection(connection_t *conn, std::string ip_addr)   {
    assert(conn != NULL);
    int sock = 0;

    struct sockaddr_in serv_addr;
    // create socket
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        ERROR("Failed to create socket");
        return -1;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);

    // always connect to localhost
    if(inet_pton(AF_INET, ip_addr.data(), &serv_addr.sin_addr) <= 0) {
        ERROR("Invalid address/ Address not supported");
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        ERROR("Failed to connect to server");
        return -1;
    }

    conn->sock = sock;
    return 0;
}



int modify_qp_to_rtr(connection_t *conn) {
    struct ibv_qp *qp = conn->qp;
    rdma_conn_info_t *remote_info = &conn->remote_info;

    struct ibv_qp_attr attr = {};
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = IBV_MTU_1024;
    attr.dest_qp_num = remote_info->qpn;
    attr.rq_psn = remote_info->psn;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 12;
    attr.ah_attr.dlid = 0;
    attr.ah_attr.sl = 0;
    attr.ah_attr.src_path_bits = 0;
    attr.ah_attr.port_num = 1;

    // RoCE v2
    attr.ah_attr.is_global = 1;
    attr.ah_attr.grh.dgid = remote_info->gid;
    attr.ah_attr.grh.sgid_index = conn->gidx; //local gid
    attr.ah_attr.grh.hop_limit = 1;


    int flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;

    int ret = ibv_modify_qp(qp, &attr, flags);
    if (ret) {
        ERROR("Failed to modify QP to RTR");
        return ret;
    }
    return 0;
}

int modify_qp_to_rts(connection_t *conn) {
    struct ibv_qp *qp = conn->qp;
    struct ibv_qp_attr attr = {};
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.sq_psn = conn->local_info.psn; // Use 0 or match with local PSN
    attr.max_rd_atomic = 1;

    int flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;

    int ret = ibv_modify_qp(qp, &attr, flags);
    if (ret) {
        ERROR("Failed to modify QP to RTS");
        return ret;
    }
    return 0;
}

int exchange_conn_info(connection_t *conn) {
    header_t header = {
        .magic = MAGIC,
        .op = OP_RDMA_EXCHANGE,
        .body_size = sizeof(rdma_conn_info_t),
    };
    send_exact(conn->sock, &header, FIXED_HEADER_SIZE);
    send_exact(conn->sock, &conn->local_info, sizeof(rdma_conn_info_t));


    recv(conn->sock, &conn->remote_info, sizeof(rdma_conn_info_t), MSG_WAITALL);
    return 0;
}




int sync_local(connection_t *conn) {
    assert(conn != NULL);
    header_t header;
    header = {
        .magic = MAGIC,
        .op = OP_SYNC,
    };
    send_exact(conn->sock, &header, FIXED_HEADER_SIZE);

    int return_code = -1;
    recv(conn->sock, &return_code, RETURN_CODE_SIZE, MSG_WAITALL);
    return return_code;
}

int rw_rdma(connection_t *conn, char op, const std::vector<block_t>& blocks, int block_size, void * ptr) {
    assert(conn != NULL);
    assert(op == OP_RDMA_READ || op == OP_RDMA_WRITE);
    assert(ptr != NULL);


    std::vector<std::string> keys;
    for (auto &block : blocks) {
        keys.push_back(block.key);
    }
    remote_meta_request request = {
        .keys = keys,
        .block_size = block_size,
    };

    std::string serialized_data;
    if (!serialize(request, serialized_data)) {
        ERROR("Failed to serialize remote meta request");
        return -1;
    }

    header_t header = {
        .magic = MAGIC,
        .op = op,
        .body_size = static_cast<unsigned int>(serialized_data.size()),
    };

    // Send header
    if (send_exact(conn->sock, &header, FIXED_HEADER_SIZE) < 0) {
        ERROR("Failed to send header");
        return -1;
    }
    // Send body
    if (send_exact(conn->sock, serialized_data.data(), serialized_data.size()) < 0) {
        ERROR("Failed to send body");
        return -1;
    }
    remote_meta_response response;
    int return_size;
    if(recv(conn->sock, &return_size, RETURN_CODE_SIZE, MSG_WAITALL) < 0) {
        ERROR("Failed to receive return size");
        return -1;
    }
    char response_data[return_size];
    if(recv(conn->sock, &response_data, return_size, MSG_WAITALL) < 0 ) {
        ERROR("Failed to receive response data");
        return -1;
    }

    if(!deserialize(response_data, return_size, response)) {
        ERROR("deserialize failed");
        return -1;
    }

    if (response.error_code != TASK_ACCEPTED) {
        ERROR("Remote operation failed {}", response.error_code);
        return -1;
    }


    if (response.blocks.size() != blocks.size()) {
        ERROR("Invalid response");
        return -1;
    }

    struct ibv_mr *mr = NULL;
    if (conn->local_mr.find((uintptr_t)ptr) == conn->local_mr.end()) {
        mr = ibv_reg_mr(conn->pd, ptr, block_size * blocks.size(),
                                       IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
        if (!mr) {
            ERROR("Failed to register MR");
            return -1;
        }
        conn->local_mr[(uintptr_t)ptr] = mr;
    } else {
        mr = conn->local_mr[(uintptr_t)ptr];
    }

    for (int i = 0; i < response.blocks.size(); i++) {
        DEBUG("remote response: addr: {}, rkey: {}, lkey : {}", response.blocks[i].remote_addr, response.blocks[i].rkey, mr->lkey);
        int ret;
        if (op == OP_RDMA_WRITE) {
            ret = perform_rdma_write(conn, (char *)ptr + blocks[i].offset, block_size, response.blocks[i].remote_addr, block_size, response.blocks[i].rkey, mr->lkey);
        } else if (op == OP_RDMA_READ) {
            ret = perform_rdma_read(conn, response.blocks[i].remote_addr, block_size, (char *)ptr + blocks[i].offset, block_size, response.blocks[i].rkey, mr->lkey);
        } else {
            ERROR("Invalid operation");
            return -1;
        }
        if (ret < 0) {
            ERROR("Failed to perform RDMA operation");
            return -1;
        }
    }

    return 0;
}

int rw_local(connection_t *conn, char op, const std::vector<block_t>& blocks, int block_size, void *ptr) {
    assert(conn != NULL);
    assert(ptr != NULL);

    cudaIpcMemHandle_t ipc_handle;
    memset(&ipc_handle, 0, sizeof(cudaIpcMemHandle_t));
     
    CHECK_CUDA(cudaIpcGetMemHandle(&ipc_handle, ptr));

    local_meta_t meta = {
        .ipc_handle = ipc_handle,
        .block_size = block_size,
        .blocks = blocks,
    };

    std::string serialized_data;
    if (!serialize(meta, serialized_data)) {
        ERROR("Failed to serialize local meta");
        return -1;
    }

    header_t header = {
        .magic = MAGIC,
        .op = op,
        .body_size = static_cast<unsigned int>(serialized_data.size()),
    };

    // Send header
    if (send_exact(conn->sock, &header, FIXED_HEADER_SIZE) < 0) {
        ERROR("Failed to send header");
        return -1;
    }
    // Send body
    if (send_exact(conn->sock, serialized_data.data(), serialized_data.size()) < 0) {
        ERROR("Failed to send body");
        return -1;
    }

    int return_code;
    if (recv(conn->sock, &return_code, RETURN_CODE_SIZE, MSG_WAITALL) != RETURN_CODE_SIZE) {
        ERROR("Failed to receive return code");
        return -1;
    }

    if (return_code != FINISH && return_code != TASK_ACCEPTED) {
        return -1;
    }
    return 0;
}
