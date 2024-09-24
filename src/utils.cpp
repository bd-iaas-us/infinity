
#include "utils.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>
#include <stdio.h>
#include <iostream>
#include <sstream>
#include "log.h"
#include <iomanip>





int send_exact(int socket, const void *buffer, size_t length) {
    size_t total_sent = 0;
    while (total_sent < length) {
        ssize_t bytes_sent = send(socket, (const char *)buffer + total_sent, length - total_sent, 0);
        if (bytes_sent < 0) {
            return errno; // Return the error code if send fails
        }
        total_sent += bytes_sent;
    }
    return 0; // Successfully sent exactly `length` bytes
}

int recv_exact(int socket, void *buffer, size_t length) {
    size_t total_received = 0;
    while (total_received < length) {
        ssize_t bytes_received = recv(socket, (char *)buffer + total_received, length - total_received, 0);
        //printf("bytes_received: %d\n", bytes_received);
        if (bytes_received < 0) {
            return errno; // Return the error code if recv fails
        } else if (bytes_received == 0) {
            return ECONNRESET; // Connection reset by peer
        }
        total_received += bytes_received;
    }
    return 0; // Successfully received exactly `length` bytes
}


void print_ipc_handle(cudaIpcMemHandle_t ipc_handle) {
    std::ostringstream oss;
    for (int i = 0; i < sizeof(cudaIpcMemHandle_t); i++) {
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)((unsigned char*)&ipc_handle)[i] << " ";
    }
    INFO("ipc_handle content: {}", oss.str().c_str());
}

void compare_ipc_handle(cudaIpcMemHandle_t ipc_handle1, cudaIpcMemHandle_t ipc_handle2) {
    for (int i = 0; i < sizeof(cudaIpcMemHandle_t); i++) {
        unsigned char d1 = ((unsigned char*)&ipc_handle1)[i];
        unsigned char d2 = ((unsigned char*)&ipc_handle2)[i];
        if (d1 != d2) {
            INFO("ipc_handle1 is not equal to ipc_handle2\n");
            return;
        }
    }

}


template <typename T>
void print_vector(T* ptr, size_t size) {
    std::cout << "vector content:\n";
    for (size_t i = 0; i < size; ++i) {
        std::cout << ptr[i] << " ";
    }
    std::cout << std::endl;
}

template void print_vector<float>(float* ptr, size_t size);
template void print_vector<double>(double* ptr, size_t size);
template void print_vector<int>(int* ptr, size_t size);
template void print_vector<char>(char* ptr, size_t size);



