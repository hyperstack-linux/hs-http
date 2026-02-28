#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include "server.h"
#include "plugin_api.h"

#define SSE_HEADER "HTTP/1.1 200 OK\r\n" \
    "Server: %s/%s\r\n" \
    "Content-Type: text/event-stream\r\n" \
    "Cache-Control: no-cache\r\n" \
    "Connection: keep-alive\r\n" \
    "Transfer-Encoding: chunked\r\n" \
    "\r\n"

#define SSE_CHUNK_TEMPLATE "%.2x\r\n%s\r\n"
#define SSE_CHUNK_EVENT_TEMPLATE "%.2x\r\nevent: %s\r\ndata: %s\r\n\r\n"
#define SSE_TRAILER "\r\n"

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

int sse_init(int client_fd) {
    char header[BUFFER_SIZE];
    int header_len = snprintf(header, sizeof(header), SSE_HEADER, SERVER_NAME, SERVER_VERSION);
    return write(client_fd, header, header_len) < 0 ? -1 : 0;
}

static void sse_write_chunk(int client_fd, const char* data, size_t len) {
    char chunk_size[16];
    int size_len = snprintf(chunk_size, sizeof(chunk_size), "%zx\r\n", len);
    write(client_fd, chunk_size, size_len);
    if (len > 0) {
        write(client_fd, data, len);
    }
    write(client_fd, "\r\n", 2);
}

int sse_send(int client_fd, const char* event, const unsigned char* data, unsigned int data_len) {
    char buf[BUFFER_SIZE];
    int pos = 0;
    
    if (event) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "event: %s\r\n", event);
    }
    
    const unsigned char* ptr = data;
    const unsigned char* end = data + data_len;
    
    while (ptr < end) {
        const unsigned char* line_end = ptr;
        while (line_end < end && *line_end != '\n') {
            line_end++;
        }
        
        size_t line_len = line_end - ptr;
        if (line_len > sizeof(buf) - pos - 20) line_len = sizeof(buf) - pos - 20;
        
        buf[pos++] = 'd';
        buf[pos++] = 'a';
        buf[pos++] = 't';
        buf[pos++] = 'a';
        buf[pos++] = ':';
        buf[pos++] = ' ';
        
        if (line_len > 0) {
            memcpy(buf + pos, ptr, line_len);
            pos += line_len;
        }
        
        buf[pos++] = '\r';
        buf[pos++] = '\n';
        
        if (line_end < end && *line_end == '\n') {
            line_end++;
        }
        ptr = line_end;
        
        if (pos > (int)sizeof(buf) - 100) break;
    }

    if (data_len > 0 && data[data_len - 1] == '\n') {
        buf[pos++] = 'd';
        buf[pos++] = 'a';
        buf[pos++] = 't';
        buf[pos++] = 'a';
        buf[pos++] = ':';
        buf[pos++] = ' ';
        buf[pos++] = '\r';
        buf[pos++] = '\n';
    }
    
    buf[pos++] = '\r';
    buf[pos++] = '\n';
    
    sse_write_chunk(client_fd, buf, pos);
    return 0;
}

void sse_close(int client_fd) {
    sse_write_chunk(client_fd, "", 0);
}
