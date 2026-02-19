#include "../include/http1.h"
#include "../include/http2.h"
#include "../include/server.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#define MAX_EVENTS 1024

extern Connection *get_connection(int fd);
extern void handle_accept(int server_fd, ServerConfig *config);
extern void free_connection(Connection *conn);
extern int mod_epoll(int fd, uint32_t events);

void handle_read(Connection *conn) {
  if (conn->protocol == PROTO_UNKNOWN) {
    while (1) {
      ssize_t n = read(conn->fd, conn->read_buffer + conn->bytes_read,
                       conn->read_buffer_size - conn->bytes_read - 1);

      if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
          break;
        free_connection(conn);
        return;
      }
      if (n == 0) {
        free_connection(conn);
        return;
      }
      conn->bytes_read += n;

      if (conn->bytes_read >= conn->read_buffer_size - 1) {
        conn->read_buffer_size *= 2;
        conn->read_buffer = realloc(conn->read_buffer, conn->read_buffer_size);
      }
    }

    if (conn->bytes_read < 4) {
      return;
    }

    if (strncmp((char *)conn->read_buffer, "PRI ", 4) == 0) {
      conn->protocol = PROTO_HTTP2;
      http2_handle_read(conn);
    } else {
      conn->protocol = PROTO_HTTP1;
      http1_handle_read(conn);
    }
    return;
  }

  if (conn->protocol == PROTO_HTTP1) {
    http1_handle_read(conn);
  } else if (conn->protocol == PROTO_HTTP2) {
    http2_handle_read(conn);
  }
}

void handle_write(Connection *conn) {
  if (conn->protocol == PROTO_HTTP1) {
    http1_handle_write(conn);
  } else if (conn->protocol == PROTO_HTTP2) {
    if (conn->write_buffer_size > 0) {
      while (conn->bytes_written < conn->write_buffer_size) {
        ssize_t n = write(conn->fd, conn->write_buffer + conn->bytes_written,
                          conn->write_buffer_size - conn->bytes_written);
        if (n < 0) {
          if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;
          free_connection(conn);
          return;
        }
        conn->bytes_written += n;
      }
      if (conn->bytes_written == conn->write_buffer_size) {
        free(conn->write_buffer);
        conn->write_buffer = NULL;
        conn->write_buffer_size = 0;
        conn->bytes_written = 0;
        mod_epoll(conn->fd, EPOLLIN | EPOLLET);
      }
    }
  }
}
