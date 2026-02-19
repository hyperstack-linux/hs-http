#include "../include/http1.h"
#include "../include/server.h"
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

extern void free_connection(Connection *conn);

void http1_handle_write(Connection *conn) {
  if (conn->state == CONN_STATE_WRITE_RESPONSE) {
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

    if (conn->file_fd >= 0) {
      conn->state = CONN_STATE_WRITE_FILE;
      conn->bytes_written = 0;
    } else {
      free_connection(conn);
      return;
    }
  }

  if (conn->state == CONN_STATE_WRITE_FILE) {
    char buffer[8192];
    while (conn->file_offset < conn->file_size) {
      size_t bytes_to_read = (size_t)(conn->file_size - conn->file_offset);
      if (bytes_to_read > sizeof(buffer))
        bytes_to_read = sizeof(buffer);

      ssize_t n = read(conn->file_fd, buffer, bytes_to_read);
      if (n < 0) {
        if (errno == EAGAIN)
          return;
        free_connection(conn);
        return;
      }
      if (n == 0)
        break;

      ssize_t written = 0;
      while (written < n) {
        ssize_t w = write(conn->fd, buffer + written, (size_t)(n - written));
        if (w < 0) {
          if (errno == EAGAIN || errno == EWOULDBLOCK) {
            conn->file_offset += written;
            lseek(conn->file_fd, conn->file_offset, SEEK_SET);
            return;
          }
          free_connection(conn);
          return;
        }
        written += w;
      }
      conn->file_offset += written;
    }
    free_connection(conn);
  }
}
