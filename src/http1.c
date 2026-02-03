#include "include/http1.h"
#include "include/server.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>

// Forward declaration from server_epoll.c (static there, so we might need to
// expose it or reimplement) For now, we will perform reads/writes here.
extern int
    epoll_fd; // We might need this if we mod epoll. But modifying epoll from
              // here is messy if epoll_fd is static in server_epoll.c.
// Alternative: Passing logic to mod_epoll.
// But we can just use the mod_epoll function if exposed.
// Let's assume we can export mod_epoll or move it to a utilitarian place.
// For now, I'll redeclare mod_epoll as extern and remove static from
// server_epoll.c later.
int mod_epoll(int fd, uint32_t events);

// Also free_connection needs to be accessible or we return a status to caller.
void free_connection(Connection *conn);

void http1_handle_read(Connection *conn) {
  while (1) {
    ssize_t n = read(conn->fd, conn->read_buffer + conn->bytes_read,
                     conn->read_buffer_size - conn->bytes_read - 1);

    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
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
  } // End of while loop

  // Check if we have complete headers
  // Find header end: \r\n\r\n or \n\n
  char *hdr_end = NULL;
  for (size_t i = 0; i + 3 < conn->bytes_read; i++) {
    if (conn->read_buffer[i] == '\r' && conn->read_buffer[i + 1] == '\n' &&
        conn->read_buffer[i + 2] == '\r' && conn->read_buffer[i + 3] == '\n') {
      hdr_end = conn->read_buffer + i + 3; // Point to last \n
      break;
    }
  }

  // Also check for \n\n pattern (some clients use only \n)
  if (!hdr_end) {
    for (size_t i = 0; i + 1 < conn->bytes_read; i++) {
      if (conn->read_buffer[i] == '\n' && conn->read_buffer[i + 1] == '\n') {
        hdr_end = conn->read_buffer + i + 1; // Point to second \n
        break;
      }
    }
  }

  if (hdr_end) {
    int content_length = 0;
    size_t hdr_len = hdr_end - conn->read_buffer;
    char *hdr_copy = malloc(hdr_len + 1);
    if (hdr_copy) {
      memcpy(hdr_copy, conn->read_buffer, hdr_len);
      hdr_copy[hdr_len] = '\0';
      char *ln = strtok(hdr_copy, "\r\n");
      while ((ln = strtok(NULL, "\r\n")) != NULL && strlen(ln) > 0) {
        if (strncasecmp(ln, "Content-Length:", 15) == 0) {
          content_length = atoi(ln + 15);
          break;
        }
      }
      free(hdr_copy);
    }

    size_t hdr_index = hdr_end - conn->read_buffer;
    if (hdr_index + 1 + (size_t)content_length > (size_t)conn->bytes_read) {
      // need more data
    } else {
      conn->state = CONN_STATE_PROCESS;
    }
  }

  if (conn->state == CONN_STATE_PROCESS) {
    conn->read_buffer[conn->bytes_read] = '\0';

    struct timespec end_time;
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    double request_time = (end_time.tv_sec - conn->start_time.tv_sec) +
                          (end_time.tv_nsec - conn->start_time.tv_nsec) / 1e9;

    time_t now = time(NULL);
    struct tm *gmt = gmtime(&now);
    char date_buf[128];
    strftime(date_buf, sizeof(date_buf), "%a, %d %b %Y %H:%M:%S GMT", gmt);

    char method[16], path[1024], version[16];
    char *line = strtok(conn->read_buffer, "\r\n");

    if (!line || sscanf(line, "%15s %1023s %15s", method, path, version) != 3) {
      handle_error(conn->fd, 400, "Bad Request", conn->config->root, path);
      conn->state = CONN_STATE_CLOSING;
      free_connection(conn);
      return;
    }

    // Parse headers for cache validation (simplified for now)
    while ((line = strtok(NULL, "\r\n")) != NULL && strlen(line) > 0) {
      if (strncasecmp(line, "If-Modified-Since:", 18) == 0) {
        strncpy(conn->if_modified_since, line + 19,
                sizeof(conn->if_modified_since) - 1);
        char *start = conn->if_modified_since;
        while (*start == ' ')
          start++;
        if (start != conn->if_modified_since)
          memmove(conn->if_modified_since, start, strlen(start) + 1);
      } else if (strncasecmp(line, "If-None-Match:", 14) == 0) {
        strncpy(conn->if_none_match, line + 15,
                sizeof(conn->if_none_match) - 1);
        char *start = conn->if_none_match;
        while (*start == ' ')
          start++;
        if (start != conn->if_none_match)
          memmove(conn->if_none_match, start, strlen(start) + 1);
      }
    }

    int is_head = (strcmp(method, "HEAD") == 0);

    if (strcmp(method, "GET") != 0 && !is_head) {
      if (handle_plugin_request(conn->fd, path, method, version,
                                conn->read_buffer, conn->bytes_read,
                                conn->config) == 0) {
        free_connection(conn);
        return;
      }
      handle_error(conn->fd, 405, "Method Not Allowed", conn->config->root,
                   path);
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
    snprintf(local_path, sizeof(local_path), "%s%s", conn->config->root,
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

    // Check match for 304 (missing logic: cache checking)
    // ... (Keep it simple for now or copy logic)

    const char *mime = get_mime_type(local_path);

    char header[1024];
    int header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 200 OK\r\n"
                              "Server: %s/%s\r\n"
                              "Date: %s\r\n"
                              "Content-Type: %s\r\n"
                              "Content-Length: %ld\r\n"
                              "Last-Modified: %s\r\n"
                              "ETag: %s\r\n"
                              "Cache-Control: public, max-age=%d\r\n"
                              "X-Request-Time: %.6f\r\n"
                              "X-Cache: MISS\r\n"
                              "Connection: close\r\n"
                              "\r\n",
                              SERVER_NAME, SERVER_VERSION, date_buf, mime,
                              st.st_size, last_modified, etag,
                              conn->config->cache_max_age, request_time);

    conn->write_buffer = malloc(header_len);
    memcpy(conn->write_buffer, header, header_len);
    conn->write_buffer_size = header_len;

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
      ssize_t bytes_to_read = conn->file_size - conn->file_offset;
      if (bytes_to_read > sizeof(buffer))
        bytes_to_read = sizeof(buffer);

      ssize_t n = read(conn->file_fd, buffer, bytes_to_read);
      if (n < 0) { // e.g., disk read error, unlikely but possible.
        // For file reads, usually blocking unless O_NONBLOCK used?
        // We opened with O_NONBLOCK.
        if (errno == EAGAIN)
          return;
        free_connection(conn);
        return;
      }
      if (n == 0)
        break;

      ssize_t written = 0;
      while (written < n) {
        ssize_t w = write(conn->fd, buffer + written, n - written);
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
