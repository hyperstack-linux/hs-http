#include "../include/http1.h"
#include "../include/http2.h"
#include "../include/server.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>

extern int mod_epoll(int fd, uint32_t events);
extern void free_connection(Connection *conn);
extern void http2_init(Connection *conn);
extern void http2_handle_upgrade_request(Connection *conn, const char *method,
                                         const char *path);

static int find_header_end(Connection *conn) {
  for (size_t i = 0; i + 3 < conn->bytes_read; i++) {
    if (conn->read_buffer[i] == '\r' && conn->read_buffer[i + 1] == '\n' &&
        conn->read_buffer[i + 2] == '\r' && conn->read_buffer[i + 3] == '\n') {
      return (int)(i + 3);
    }
  }
  for (size_t i = 0; i + 1 < conn->bytes_read; i++) {
    if (conn->read_buffer[i] == '\n' && conn->read_buffer[i + 1] == '\n') {
      return (int)(i + 1);
    }
  }
  return -1;
}

static int parse_content_length(Connection *conn, size_t hdr_end) {
  int content_length = 0;
  char *hdr_copy = malloc(hdr_end + 1);
  if (hdr_copy) {
    memcpy(hdr_copy, conn->read_buffer, hdr_end);
    hdr_copy[hdr_end] = '\0';
    char *ln = strtok(hdr_copy, "\r\n");
    while ((ln = strtok(NULL, "\r\n")) != NULL && strlen(ln) > 0) {
      if (strncasecmp(ln, "Content-Length:", 15) == 0) {
        content_length = atoi(ln + 15);
        break;
      }
    }
    free(hdr_copy);
  }
  return content_length;
}

void http1_handle_read(Connection *conn) {
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

  int hdr_end = find_header_end(conn);
  if (hdr_end >= 0) {
    int content_length = parse_content_length(conn, (size_t)hdr_end);
    if (hdr_end + 1 + content_length <= (int)conn->bytes_read) {
      conn->state = CONN_STATE_PROCESS;
    }
  }

  if (conn->state == CONN_STATE_PROCESS) {
    conn->read_buffer[conn->bytes_read] = '\0';

    struct timespec end_time;
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    double request_time = (end_time.tv_sec - conn->start_time.tv_sec) +
                          (end_time.tv_nsec - conn->start_time.tv_nsec) / 1e9;

    char method[16], path[1024], version[16];
    char http2_settings_b64[512] = {0};
    int is_upgrade_h2c = 0;

    extern int http1_parse_request(Connection *conn, char *method, char *path,
                                   char *version, char *http2_settings_b64,
                                   int *is_upgrade_h2c);

    if (http1_parse_request(conn, method, path, version, http2_settings_b64,
                            &is_upgrade_h2c) < 0) {
      handle_error(conn->fd, 400, "Bad Request", conn->config->root, path);
      conn->state = CONN_STATE_CLOSING;
      free_connection(conn);
      return;
    }

    if (is_upgrade_h2c) {
      const char *sw = "HTTP/1.1 101 Switching Protocols\r\n"
                       "Connection: Upgrade\r\n"
                       "Upgrade: h2c\r\n"
                       "\r\n";
      write(conn->fd, sw, strlen(sw));

      conn->protocol = PROTO_HTTP2;
      http2_init(conn);

      char settings_frame[9] = {0, 0, 0, 4, 0, 0, 0, 0, 0};
      write(conn->fd, settings_frame, 9);

      http2_handle_upgrade_request(conn, method, path);

      conn->bytes_read = 0;
      mod_epoll(conn->fd, EPOLLIN | EPOLLOUT | EPOLLET);
      return;
    }

    int is_head = (strcmp(method, "HEAD") == 0);

    if (strcmp(method, "GET") != 0 && !is_head) {
      if (handle_plugin_request(conn->fd, path, method, version,
                                conn->read_buffer, conn->bytes_read,
                                conn->config) == 0) {
        free_connection(conn);
        return;
      }
      handle_error(conn->fd, 405, "Method Not Allowed", conn->config->root, path);
      conn->state = CONN_STATE_CLOSING;
      free_connection(conn);
      return;
    }

    if (!is_head) {
      if (handle_plugin_request(conn->fd, path, method, version,
                                conn->read_buffer, conn->bytes_read,
                                conn->config) == 0) {
        free_connection(conn);
        return;
      }
    }

    char path_without_query[1024];
    strncpy(path_without_query, path, sizeof(path_without_query) - 1);
    char *query_start = strchr(path_without_query, '?');
    if (query_start)
      *query_start = '\0';

    char local_path[2048];
    snprintf(local_path, sizeof(local_path), "%s%s",
             conn->config->root,
             strcmp(path_without_query, "/") == 0 ? "/index.html"
                                                  : path_without_query);

    struct stat st;
    if (stat(local_path, &st) < 0) {
      if (!conn->config->require_extensions &&
          strchr(path_without_query, '.') == NULL) {
        snprintf(local_path, sizeof(local_path), "%s%s.html",
                 conn->config->root, path_without_query);
        if (stat(local_path, &st) >= 0)
          goto file_found;
      }
      handle_error(conn->fd, 404, "Not Found", conn->config->root, path);
      conn->state = CONN_STATE_CLOSING;
      free_connection(conn);
      return;
    }

file_found:
    if (S_ISDIR(st.st_mode)) {
      strcat(local_path, "/index.html");
      if (stat(local_path, &st) < 0) {
        handle_error(conn->fd, 404, "Not Found", conn->config->root, path);
        conn->state = CONN_STATE_CLOSING;
        free_connection(conn);
        return;
      }
    }

    int file_fd = open(local_path, O_RDONLY | O_NONBLOCK);
    if (file_fd < 0) {
      handle_error(conn->fd, 500, "Internal Server Error", conn->config->root,
                   path);
      conn->state = CONN_STATE_CLOSING;
      free_connection(conn);
      return;
    }

    char etag[64];
    snprintf(etag, sizeof(etag), "\"%lx-%lx\"", (unsigned long)st.st_ino,
             (unsigned long)st.st_mtime);

    struct tm *gmt_time = gmtime(&st.st_mtime);
    char last_modified[128];
    strftime(last_modified, sizeof(last_modified), "%a, %d %b %Y %H:%M:%S GMT",
             gmt_time);

    const char *mime = get_mime_type(local_path);

    extern void http1_build_response_headers(Connection *conn, const char *mime,
                                             off_t file_size, const char *etag,
                                             const char *last_modified,
                                             double request_time, char **out_header,
                                             size_t *out_len);

    http1_build_response_headers(conn, mime, st.st_size, etag, last_modified,
                                 request_time, &conn->write_buffer,
                                 &conn->write_buffer_size);

    if (is_head) {
      close(file_fd);
      conn->state = CONN_STATE_WRITE_RESPONSE;
    } else {
      conn->file_fd = file_fd;
      conn->file_size = st.st_size;
      conn->state = CONN_STATE_WRITE_RESPONSE;
    }

    mod_epoll(conn->fd, EPOLLOUT | EPOLLET);
  }
}
