#include "../include/http2.h"
#include "../include/server.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <unistd.h>

extern unsigned char ___src_errors_404_html[];
extern unsigned int ___src_errors_404_html_len;
extern unsigned char ___src_errors_500_html[];
extern unsigned int ___src_errors_500_html_len;

extern int mod_epoll(int fd, uint32_t events);
extern uint32_t http2_read_uint32(const uint8_t *buf);
extern int hpack_decode_path(const uint8_t *data, size_t len, char *out_path,
                             size_t out_path_size);
extern int hpack_decode_method(const uint8_t *data, size_t len,
                              char *out_method, size_t out_method_size);
extern unsigned char *hpack_encode_response(int status_code,
                                           const char *content_type,
                                           size_t content_length,
                                           size_t *out_size);
extern int handle_plugin_request(int client_fd, const char *path, char *method,
                                char *version, const char *raw_request,
                                size_t raw_len, ServerConfig *config);
extern void set_plugin_h2_context(Connection* conn, uint32_t stream);
extern const char *get_mime_type(const char *path);

void http2_init(Connection *conn) {
  Http2State *state = calloc(1, sizeof(Http2State));
  if (state) {
    state->initialized = 1;
    state->remote_window_size = 65535;
    state->local_window_size = 65535;
    state->next_stream_id = 1;
    conn->http2_state = state;
  }
}

int http2_check_preface(const char *buffer, size_t len) {
  if (len < H2_PREFACE_LEN)
    return 0;
  return (memcmp(buffer, H2_PREFACE, H2_PREFACE_LEN) == 0);
}

static int append_to_write_buffer(Connection *conn, const void *data, size_t len) {
  char *new_buf = realloc(conn->write_buffer, conn->write_buffer_size + len);
  if (!new_buf)
    return -1;
  memcpy(new_buf + conn->write_buffer_size, data, len);
  conn->write_buffer = new_buf;
  conn->write_buffer_size += len;
  return 0;
}

void http2_send_response(Connection *conn, uint32_t stream_id, int status_code,
                         const char *content_type, const void *body,
                         size_t body_len) {
  size_t headers_len;
  unsigned char *headers =
      hpack_encode_response(status_code, content_type, body_len, &headers_len);
  if (!headers)
    return;

  unsigned char headers_frame[9] = {0};
  headers_frame[0] = (headers_len >> 16) & 0xFF;
  headers_frame[1] = (headers_len >> 8) & 0xFF;
  headers_frame[2] = headers_len & 0xFF;
  headers_frame[3] = H2_FRAME_HEADERS;
  headers_frame[4] = 0x04;
  headers_frame[5] = (stream_id >> 24) & 0x7F;
  headers_frame[6] = (stream_id >> 16) & 0xFF;
  headers_frame[7] = (stream_id >> 8) & 0xFF;
  headers_frame[8] = stream_id & 0xFF;

  append_to_write_buffer(conn, headers_frame, 9);
  append_to_write_buffer(conn, headers, headers_len);
  free(headers);

  unsigned char data_frame[9] = {0};
  data_frame[0] = (body_len >> 16) & 0xFF;
  data_frame[1] = (body_len >> 8) & 0xFF;
  data_frame[2] = body_len & 0xFF;
  data_frame[3] = H2_FRAME_DATA;
  data_frame[4] = 0x01;
  data_frame[5] = (stream_id >> 24) & 0x7F;
  data_frame[6] = (stream_id >> 16) & 0xFF;
  data_frame[7] = (stream_id >> 8) & 0xFF;
  data_frame[8] = stream_id & 0xFF;

  append_to_write_buffer(conn, data_frame, 9);
  append_to_write_buffer(conn, body, body_len);

  mod_epoll(conn->fd, EPOLLIN | EPOLLOUT | EPOLLET);
}

void http2_handle_read(Connection *conn) {
  while (1) {
    ssize_t n = read(conn->fd, conn->read_buffer + conn->bytes_read,
                     conn->read_buffer_size - conn->bytes_read - 1);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        break;
      return;
    }
    if (n == 0)
      return;

    conn->bytes_read += n;
    if (conn->bytes_read >= conn->read_buffer_size - 1) {
      conn->read_buffer_size *= 2;
      char *new_buf = realloc(conn->read_buffer, conn->read_buffer_size);
      if (!new_buf)
        return;
      conn->read_buffer = new_buf;
    }
  }

  if (conn->http2_state == NULL) {
    if (conn->bytes_read >= H2_PREFACE_LEN &&
        memcmp(conn->read_buffer, H2_PREFACE, H2_PREFACE_LEN) == 0) {
      http2_init(conn);
      memmove(conn->read_buffer, conn->read_buffer + H2_PREFACE_LEN,
              conn->bytes_read - H2_PREFACE_LEN);
      conn->bytes_read -= H2_PREFACE_LEN;

      log_message(conn->config, LOG_INFO, "HTTP/2 connection initialized");

      char settings_frame[9] = {0, 0, 0, 4, 0, 0, 0, 0, 0};
      conn->write_buffer = malloc(9);
      if (conn->write_buffer) {
        memcpy(conn->write_buffer, settings_frame, 9);
        conn->write_buffer_size = 9;
        conn->bytes_written = 0;
        mod_epoll(conn->fd, EPOLLIN | EPOLLOUT | EPOLLET);
      }
    } else {
      return;
    }
  } else {
    if (conn->bytes_read >= H2_PREFACE_LEN &&
        memcmp(conn->read_buffer, H2_PREFACE, H2_PREFACE_LEN) == 0) {
      memmove(conn->read_buffer, conn->read_buffer + H2_PREFACE_LEN,
              conn->bytes_read - H2_PREFACE_LEN);
      conn->bytes_read -= H2_PREFACE_LEN;
      log_message(conn->config, LOG_DEBUG,
                  "HTTP/2 client preface received (h2c upgrade)");
    }
  }

  while (conn->bytes_read >= 9) {
    uint32_t length = ((uint8_t)conn->read_buffer[0] << 16) |
                      ((uint8_t)conn->read_buffer[1] << 8) |
                      (uint8_t)conn->read_buffer[2];
    uint8_t type = conn->read_buffer[3];
    uint8_t flags = conn->read_buffer[4];
    uint32_t stream_id = http2_read_uint32((uint8_t *)conn->read_buffer + 5) & 0x7FFFFFFF;

    if (conn->bytes_read < 9 + (size_t)length)
      return;

    log_message(conn->config, LOG_DEBUG,
                "HTTP/2 Frame: Type=%d, Len=%u, Stream=%u, Flags=0x%02x", type,
                length, stream_id, flags);

    if (type == H2_FRAME_SETTINGS) {
      if (!(flags & 0x1)) {
        char ack_frame[9] = {0, 0, 0, 4, 1, 0, 0, 0, 0};
        append_to_write_buffer(conn, ack_frame, 9);
        log_message(conn->config, LOG_DEBUG, "Sent SETTINGS ACK");
      }
    } else if (type == H2_FRAME_HEADERS) {
      const uint8_t *headers_payload = (uint8_t *)conn->read_buffer + 9;
      size_t headers_payload_len = length;
      uint8_t pad_length = 0;

      if (flags & 0x08) {
        if (headers_payload_len < 1)
          goto next_frame;
        pad_length = headers_payload[0];
        headers_payload++;
        headers_payload_len--;
        if (headers_payload_len < pad_length)
          goto next_frame;
        headers_payload_len -= pad_length;
      }

      if (flags & 0x20) {
        if (headers_payload_len < 5)
          goto next_frame;
        headers_payload += 5;
        headers_payload_len -= 5;
      }

      char path[1024];
      char method[16];

      if (hpack_decode_path(headers_payload, headers_payload_len, path, sizeof(path)) != 0) {
        log_message(conn->config, LOG_WARN, "Failed to decode :path header");
        http2_send_response(conn, stream_id, 400, "text/html",
                            ___src_errors_404_html, ___src_errors_404_html_len);
        goto next_frame;
      }

      if (hpack_decode_method(headers_payload, headers_payload_len, method, sizeof(method)) != 0) {
        strcpy(method, "GET");
      }

      log_message(conn->config, LOG_INFO, "HTTP/2 request: %s %s (stream %u)",
                  method, path, stream_id);

      char *query = strchr(path, '?');
      if (query)
        *query = '\0';

      if (strstr(path, "..") || path[0] != '/') {
        http2_send_response(conn, stream_id, 400, "text/html",
                            ___src_errors_404_html, ___src_errors_404_html_len);
        goto next_frame;
      }

      set_plugin_h2_context(conn, stream_id);
      int plugin_rv = handle_plugin_request(conn->fd, path, method, "HTTP/2.0",
                                           NULL, 0, conn->config);
      set_plugin_h2_context(NULL, 0);

      if (plugin_rv == 0)
        goto next_frame;

      char local_path[2048];
      if (strcmp(path, "/") == 0) {
        snprintf(local_path, sizeof(local_path), "%s/index.html",
                 conn->config->root);
      } else {
        snprintf(local_path, sizeof(local_path), "%s%s", conn->config->root, path);
      }

      struct stat st;
      if (stat(local_path, &st) < 0) {
        if (!conn->config->require_extensions && strchr(path, '.') == NULL) {
          snprintf(local_path, sizeof(local_path), "%s%s.html", conn->config->root,
                   path);
          if (stat(local_path, &st) == 0)
            goto file_found;
        }

        http2_send_response(conn, stream_id, 404, "text/html",
                            ___src_errors_404_html, ___src_errors_404_html_len);
        goto next_frame;
      }

file_found:
      if (S_ISDIR(st.st_mode)) {
        strncat(local_path, "/index.html",
                sizeof(local_path) - strlen(local_path) - 1);
        if (stat(local_path, &st) < 0) {
          http2_send_response(conn, stream_id, 404, "text/html",
                              ___src_errors_404_html, ___src_errors_404_html_len);
          goto next_frame;
        }
      }

      int fd = open(local_path, O_RDONLY);
      if (fd < 0) {
        http2_send_response(conn, stream_id, 500, "text/html",
                            ___src_errors_500_html, ___src_errors_500_html_len);
        goto next_frame;
      }

      char *file_content = malloc(st.st_size);
      if (!file_content) {
        close(fd);
        http2_send_response(conn, stream_id, 500, "text/html",
                            ___src_errors_500_html, ___src_errors_500_html_len);
        goto next_frame;
      }

      ssize_t bytes_read = read(fd, file_content, st.st_size);
      close(fd);

      if (bytes_read != (ssize_t)st.st_size) {
        free(file_content);
        http2_send_response(conn, stream_id, 500, "text/html",
                            ___src_errors_500_html, ___src_errors_500_html_len);
        goto next_frame;
      }

      const char *mime = get_mime_type(local_path);
      http2_send_response(conn, stream_id, 200, mime, file_content, bytes_read);
      free(file_content);
    }

next_frame:;
    size_t frame_len = 9 + length;
    memmove(conn->read_buffer, conn->read_buffer + frame_len,
            conn->bytes_read - frame_len);
    conn->bytes_read -= frame_len;
  }
}

void http2_handle_upgrade_request(Connection *conn, const char *method,
                                  const char *path) {
  char clean_path[1024];
  strncpy(clean_path, path, sizeof(clean_path) - 1);
  clean_path[sizeof(clean_path) - 1] = '\0';
  char *q = strchr(clean_path, '?');
  if (q)
    *q = '\0';

  if (strstr(clean_path, "..") || clean_path[0] != '/') {
    http2_send_response(conn, 1, 400, "text/html", ___src_errors_404_html,
                        ___src_errors_404_html_len);
    return;
  }

  log_message(conn->config, LOG_INFO,
              "HTTP/2 upgrade request: %s %s (stream 1)", method, clean_path);

  set_plugin_h2_context(conn, 1);
  int plugin_rv = handle_plugin_request(conn->fd, clean_path, (char *)method,
                                       "HTTP/2.0", NULL, 0, conn->config);
  set_plugin_h2_context(NULL, 0);
  if (plugin_rv == 0)
    return;

  char local_path[2048];
  if (strcmp(clean_path, "/") == 0) {
    snprintf(local_path, sizeof(local_path), "%s/index.html",
             conn->config->root);
  } else {
    snprintf(local_path, sizeof(local_path), "%s%s", conn->config->root,
             clean_path);
  }

  struct stat st;
  if (stat(local_path, &st) < 0) {
    if (!conn->config->require_extensions && strchr(clean_path, '.') == NULL) {
      snprintf(local_path, sizeof(local_path), "%s%s.html", conn->config->root,
               clean_path);
      if (stat(local_path, &st) == 0)
        goto file_found;
    }
    http2_send_response(conn, 1, 404, "text/html", ___src_errors_404_html,
                        ___src_errors_404_html_len);
    return;
  }

file_found:
  if (S_ISDIR(st.st_mode)) {
    strncat(local_path, "/index.html",
            sizeof(local_path) - strlen(local_path) - 1);
    if (stat(local_path, &st) < 0) {
      http2_send_response(conn, 1, 404, "text/html", ___src_errors_404_html,
                          ___src_errors_404_html_len);
      return;
    }
  }

  int fd = open(local_path, O_RDONLY);
  if (fd < 0) {
    http2_send_response(conn, 1, 500, "text/html", ___src_errors_500_html,
                        ___src_errors_500_html_len);
    return;
  }

  char *file_content = malloc(st.st_size);
  if (!file_content) {
    close(fd);
    http2_send_response(conn, 1, 500, "text/html", ___src_errors_500_html,
                        ___src_errors_500_html_len);
    return;
  }

  ssize_t bytes_read = read(fd, file_content, st.st_size);
  close(fd);
  if (bytes_read != st.st_size) {
    free(file_content);
    http2_send_response(conn, 1, 500, "text/html", ___src_errors_500_html,
                        ___src_errors_500_html_len);
    return;
  }

  const char *mime = get_mime_type(local_path);
  http2_send_response(conn, 1, 200, mime, file_content, bytes_read);
  free(file_content);
}
