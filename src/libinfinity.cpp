#include <cuda.h>
#include <cuda_runtime.h>
#include <gdrapi.h>
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
        perror("Failed to get RDMA devices list");
        return -1;
    }

    // Open the first available device
    conn->ib_ctx = ibv_open_device(dev_list[0]);
    if (!conn->ib_ctx) {
        perror("Failed to open RDMA device");
        return -1;
    }
    ibv_free_device_list(dev_list);

    int gidx = ibv_find_sgid_type(conn->ib_ctx, 1, IBV_GID_TYPE_ROCE_V2, AF_INET);
    if (gidx < 0) {
        perror("Failed to find GID");
        return -1;
    }
    conn->gidx = gidx;


    union ibv_gid gid;
    //get gid
    if (ibv_query_gid(conn->ib_ctx, 1, gidx, &gid)) {
        perror("Failed to get GID");
        return -1;
    }
    
    // Allocate Protection Domain
    conn->pd = ibv_alloc_pd(conn->ib_ctx);
    if (!conn->pd) {
        perror("Failed to allocate PD");
        return -1;
    }

    // Create Completion Queue
    conn->cq = ibv_create_cq(conn->ib_ctx, 10, NULL, NULL, 0);
    if (!conn->cq) {
        perror("Failed to create CQ");
        return -1;
    }

    // Create Queue Pair
    struct ibv_qp_init_attr qp_init_attr = {};
    qp_init_attr.send_cq = conn->cq;
    qp_init_attr.recv_cq = conn->cq;
    qp_init_attr.qp_type = IBV_QPT_RC; // Reliable Connection
    qp_init_attr.cap.max_send_wr = 10;
    qp_init_attr.cap.max_recv_wr = 10;
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;

    conn->qp = ibv_create_qp(conn->pd, &qp_init_attr);
    if (!conn->qp) {
        perror("Failed to create QP");
        return -1;
    }

    // Modify QP to INIT state
    if (modify_qp_to_init(conn->qp)) {
        fprintf(stderr, "Failed to modify QP to INIT\n");
        return -1;
    }

    // Get local connection information
    struct ibv_port_attr port_attr;
    if (ibv_query_port(conn->ib_ctx, 1, &port_attr)) {
        perror("Failed to query port");
        return -1;
    }

    conn->local_info.qpn = conn->qp->qp_num;
    conn->local_info.psn = lrand48() & 0xffffff;
    conn->local_info.gid = gid;
    printf("gid index: %d\n", gidx);
    print_rdma_conn_info(&conn->local_info, false);
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
        perror("Failed to modify QP to INIT");
        return ret;
    }
    return 0;
}



int perform_rdma_read(connection_t *conn, uintptr_t src_buf, size_t src_size,
                      char * dst_buf, size_t dst_size, uint32_t rkey) {

    //FIXME
    conn->mr = ibv_reg_mr(conn->pd, dst_buf, dst_size,
                          IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!conn->mr) {
        perror("Failed to register MR");
        return -1;
    }

    // Prepare RDMA read operation
    struct ibv_sge sge = {};
    sge.addr = (uintptr_t)dst_buf;
    sge.length = dst_size;
    sge.lkey = conn->mr->lkey;

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
        perror("Failed to post RDMA read");
        return -1;
    }

    // Poll for completion
    struct ibv_wc wc;
    int num_comp;
    do {
        num_comp = ibv_poll_cq(conn->cq, 1, &wc);
    } while (num_comp == 0);

    if (num_comp < 0) {
        perror("Failed to poll CQ");
        return -1;
    }

    if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "RDMA read failed: %s\n", ibv_wc_status_str(wc.status));
        return -1;
    }
    printf("RDMA read completed successfully\n");
    // RDMA read successful
    return 0;
}

int perform_rdma_write(connection_t *conn, char * src_buf, size_t src_size,
                       uintptr_t dst_buf, size_t dst_size, uint32_t rkey) {
    conn->mr = ibv_reg_mr(conn->pd, src_buf, src_size,
                          IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!conn->mr) {
        perror("Failed to register MR");
        return -1;
    }

    // Prepare RDMA write operation
    struct ibv_sge sge = {};
    sge.addr = (uintptr_t)src_buf;
    sge.length = src_size; 
    sge.lkey = conn->mr->lkey;

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
        perror("Failed to post RDMA write");
        return -1;
    }

    // Poll for completion
    struct ibv_wc wc;
    int num_comp;
    do {
        num_comp = ibv_poll_cq(conn->cq, 1, &wc);
    } while (num_comp == 0);

    if (num_comp < 0) {
        perror("Failed to poll CQ");
        return -1;
    }

    if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "RDMA write failed: %s\n", ibv_wc_status_str(wc.status));
        return -1;
    }
    printf("RDMA write completed successfully\n");
    // RDMA write successful
    return 0;
}


int setup_rdma(connection_t *conn) {
    if(init_rdma_resources(conn) < 0) {
        fprintf(stderr, "Failed to initialize RDMA resources\n");
        return -1;
    }

    // Exchange RDMA connection information with the server
    if (exchange_conn_info(conn)) {
        fprintf(stderr, "Failed to exchange connection information\n");
        return -1;
    }

    // Modify QP to RTR state
    if (modify_qp_to_rtr(conn)) {
        fprintf(stderr, "Failed to modify QP to RTR\n");
        return -1;
    }

    if (modify_qp_to_rts(conn)) {
        fprintf(stderr, "Failed to modify QP to RTS\n");
        return -1;
    }

    printf("start to rdma write\n");
    return 0;
}

int init_connection(connection_t *conn)   {
    assert(conn != NULL);
    int sock = 0;

    struct sockaddr_in serv_addr;
    // create socket
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        printf("\n Socket creation error \n");
        return -1;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);

    // always connect to localhost
    if(inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr) <= 0) {
        printf("\nInvalid address/ Address not supported \n");
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        printf("\nConnection Failed \n");
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
        perror("Failed to modify QP to RTR");
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
        perror("Failed to modify QP to RTS");
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

void close_connection(connection_t *conn) {
    if (conn->sock > 0) {
        close(conn->sock);
    }
}




int sync_local(connection_t *conn) {
    assert(conn != NULL);
    header_t header;
    header = {
        .magic = MAGIC,
        .op = OP_SYNC,
    };
    send_exact(conn->sock, &header, FIXED_HEADER_SIZE);

    int return_code;
    recv(conn->sock, &return_code, RETURN_CODE_SIZE, MSG_WAITALL);
    if (return_code != FINISH) {
        return -1;
    }
    return 0;
}

//FIXME: implement this function
//should be
//rw_remote(connection_t *conn, char op, const std::vector<std::string>keys, std::vector<int> offsets, int block_size, void * ptr)
//这样ptr只需要注册一次
int rw_remote(connection_t *conn, char op, const std::vector<std::string>keys, int block_size, void * ptr) {
    assert(conn != NULL);
    assert(op == OP_RDMA_READ || op == OP_RDMA_WRITE);
    assert(ptr != NULL);

    remote_meta_request request = {
        .keys = keys,
    };

    std::string serialized_data;
    if (!serialize(request, serialized_data)) {
        fprintf(stderr, "Failed to serialize remote meta request\n");
        return -1;
    }

    header_t header = {
        .magic = MAGIC,
        .op = op,
        .body_size = static_cast<unsigned int>(serialized_data.size()),
    };

    // Send header
    if (send_exact(conn->sock, &header, FIXED_HEADER_SIZE) < 0) {
        fprintf(stderr, "Failed to send header\n");
        return -1;
    }
    // Send body
    if (send_exact(conn->sock, serialized_data.data(), serialized_data.size()) < 0) {
        fprintf(stderr, "Failed to send body\n");
        return -1;
    }
    remote_meta_response response;
    int return_size;
    if(recv(conn->sock, &return_size, RETURN_CODE_SIZE, MSG_WAITALL) < 0) {
        perror("Failed to receive return size");
        return -1;
    }
    char response_data[return_size];
    if(recv(conn->sock, &response_data, return_size, MSG_WAITALL) < 0 ) {
        perror("Failed to receive response data");
        return -1;
    }

    if(!deserialize(response_data, return_size, response)) {
        perror("deserialize failed");
        return -1;
    }
    printf("remote meta response: %llu, rkey: %d\n", response.blocks[0].remote_addr, response.blocks[0].rkey);

    for (auto &block : response.blocks) {
        if (op == OP_RDMA_WRITE) {
            perform_rdma_write(conn, (char *)ptr, block_size, block.remote_addr, block_size, block.rkey);
        } else if (op == OP_RDMA_READ) {
            perform_rdma_read(conn, block.remote_addr, block_size, (char *)ptr, block_size, block.rkey);
        } else {
            fprintf(stderr, "Invalid operation\n");
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
        fprintf(stderr, "Failed to serialize local meta\n");
        return -1;
    }

    header_t header = {
        .magic = MAGIC,
        .op = op,
        .body_size = static_cast<unsigned int>(serialized_data.size()),
    };

    // Send header
    if (send_exact(conn->sock, &header, FIXED_HEADER_SIZE) < 0) {
        fprintf(stderr, "Failed to send header\n");
        return -1;
    }
    // Send body
    if (send_exact(conn->sock, serialized_data.data(), serialized_data.size()) < 0) {
        fprintf(stderr, "Failed to send body\n");
        return -1;
    }

    int return_code;
    if (recv(conn->sock, &return_code, RETURN_CODE_SIZE, MSG_WAITALL) != RETURN_CODE_SIZE) {
        fprintf(stderr, "Failed to receive return code\n");
        return -1;
    }

    if (return_code != FINISH && return_code != TASK_ACCEPTED) {
        return -1;
    }
    return 0;
}