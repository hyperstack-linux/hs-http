#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include "server.h"

int parse_request(int client_fd, char* buffer, size_t buffer_size,
                  char* method, size_t method_size,
                  char* path, size_t path_size,
                  char* version, size_t version_size) {
    (void)method_size;
    (void)path_size;
    (void)version_size;
    
    ssize_t bytes_read = read(client_fd, buffer, buffer_size - 1);
    if (bytes_read <= 0) {
        return -1;
    }
    buffer[bytes_read] = '\0';

    char* line = strtok(buffer, "\r\n");
    if (!line) return -2;
    if (sscanf(line, "%15s %1023s %15s", method, path, version) != 3) {
        return -2;
    }

    return 0;
}

void proxy_request(int client_fd, const char* target_host, int target_port, const char* suffix, ServerConfig* config) {
    int backend_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (backend_fd < 0) {
        log_message(config, LOG_ERROR, "socket failed: %s", strerror(errno));
        handle_500(client_fd);
        return;
    }

    struct sockaddr_in backend_addr;
    memset(&backend_addr, 0, sizeof(backend_addr));
    backend_addr.sin_family = AF_INET;
    backend_addr.sin_port = htons(target_port);
    if (inet_pton(AF_INET, target_host, &backend_addr.sin_addr) <= 0) {
        log_message(config, LOG_ERROR, "invalid backend address: %s", target_host);
        close(backend_fd);
        handle_500(client_fd);
        return;
    }

    if (connect(backend_fd, (struct sockaddr*)&backend_addr, sizeof(backend_addr)) < 0) {
        log_message(config, LOG_DEBUG, "connect to backend failed: %s", strerror(errno));
        close(backend_fd);
        handle_500(client_fd);
        return;
    }

    char request[BUFFER_SIZE];
    int req_len = snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Connection: close\r\n"
        "\r\n",
        suffix, target_host, target_port);
    if (write(backend_fd, request, req_len) < 0) {
        log_message(config, LOG_ERROR, "write to backend failed: %s", strerror(errno));
        close(backend_fd);
        handle_500(client_fd);
        return;
    }

    char buffer[BUFFER_SIZE];
    ssize_t n;
    while ((n = read(backend_fd, buffer, sizeof(buffer))) > 0) {
        if (write(client_fd, buffer, n) < 0) {
            log_message(config, LOG_ERROR, "write to client failed: %s", strerror(errno));
            break;
        }
    }

    close(backend_fd);
}
