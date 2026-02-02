#include <stdio.h>
#include <unistd.h>
#include "server.h"
#include "plugin_api.h"

void send_response(int client_fd, int status_code, const char* status_text, 
                   const char* content_type, const unsigned char* body, unsigned int body_len) {
    char header[BUFFER_SIZE];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Server: %s/%s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n"
        "\r\n",
        status_code, status_text, SERVER_NAME, SERVER_VERSION, content_type, body_len);

    if (write(client_fd, header, header_len) < 0) return;
    if (body_len > 0) {
        if (write(client_fd, body, body_len) < 0) return;
    }
}

void send_response_with_headers(int client_fd, int status_code, const char* status_text, 
                   const char* content_type, const unsigned char* body, unsigned int body_len,
                   Header* headers, int header_count) {
    char header[BUFFER_SIZE];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Server: %s/%s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %u\r\n",
        status_code, status_text, SERVER_NAME, SERVER_VERSION, content_type, body_len);

    if (write(client_fd, header, header_len) < 0) return;

    for (int i = 0; i < header_count; i++) {
        int n = snprintf(header, sizeof(header), "%s: %s\r\n", headers[i].key, headers[i].value);
        if (write(client_fd, header, n) < 0) return;
    }

    if (write(client_fd, "Connection: close\r\n\r\n", 22) < 0) return;

    if (body_len > 0) {
        if (write(client_fd, body, body_len) < 0) return;
    }
}
