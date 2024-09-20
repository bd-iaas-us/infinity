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


int rw_remote(connection_t *conn) {
    void * buffer = cudaMemAlloc(1024);
    if (buffer == NULL) {
        fprintf(stderr, "Failed to allocate buffer\n");
        return -1;
    }
    // 获取IB设备列表
    int num_devices;
    struct ibv_device** device_list = ibv_get_device_list(&num_devices);
    if (!device_list) {
        perror("Failed to get IB devices list");
        return -1;
    }

    // 使用第一个可用的设备
    context = ibv_open_device(device_list[0]);
    if (!context) {
        perror("Failed to open IB device");
        ibv_free_device_list(device_list);
        return -1;
    }

    ibv_free_device_list(device_list);

    // 分配保护域
    pd = ibv_alloc_pd(context);
    if (!pd) {
        perror("Failed to allocate PD");
        return -1;
    }

    // 注册内存区域
    mr = ibv_reg_mr(pd, buffer, size, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ);
    if (!mr) {
        perror("Failed to register MR");
        return -1;
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
    if (!serialize_local_meta(meta, serialized_data)) {
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