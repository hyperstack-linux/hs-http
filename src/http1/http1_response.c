#include "../include/http1.h"
#include "../include/server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void http1_build_response_headers(Connection *conn, const char *mime,
                                    off_t file_size, const char *etag,
                                    const char *last_modified, double request_time,
                                    char **out_header, size_t *out_len) {
  time_t now = time(NULL);
  struct tm *gmt = gmtime(&now);
  char date_buf[128];
  strftime(date_buf, sizeof(date_buf), "%a, %d %b %Y %H:%M:%S GMT", gmt);

  size_t header_len = snprintf(NULL, 0,
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
                                (long)file_size, last_modified, etag,
                                conn->config->cache_max_age, request_time);

  *out_header = malloc(header_len + 1);
  if (*out_header) {
    snprintf(*out_header, header_len + 1,
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
             (long)file_size, last_modified, etag,
             conn->config->cache_max_age, request_time);
    *out_len = header_len;
  }
}
