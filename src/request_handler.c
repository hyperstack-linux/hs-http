#include "include/request_handler.h"
#include "include/server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

int handle_request_common(Connection *conn, const char *path, const char *method, const char *version, 
                          const char *raw_request, size_t raw_len) {
    // First, try to handle as plugin request
    if (strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0) {
        if (handle_plugin_request(conn->fd, path, (char*)method, (char*)version,
                                  raw_request, raw_len,
                                  conn->config) == 0) {
            return 0; // Plugin handled the request
        }
    } else {
        // For non-GET/HEAD methods, try plugin first
        if (handle_plugin_request(conn->fd, path, (char*)method, (char*)version,
                                  raw_request, raw_len,
                                  conn->config) == 0) {
            return 0; // Plugin handled the request
        }
        
        // If plugin didn't handle it, return method not allowed
        handle_error(conn->fd, 405, "Method Not Allowed", conn->config->root, path);
        return -1;
    }

    // If no plugin handled the request, serve static files
    char path_without_query[1024];
    strncpy(path_without_query, path, sizeof(path_without_query) - 1);
    path_without_query[sizeof(path_without_query) - 1] = '\0';
    
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
        return -1;
    }

file_found:
    if (S_ISDIR(st.st_mode)) {
        strcat(local_path, "/index.html");
        if (stat(local_path, &st) < 0) {
            handle_error(conn->fd, 404, "Not Found", conn->config->root, path);
            return -1;
        }
    }

    int file_fd = open(local_path, O_RDONLY);
    if (file_fd < 0) {
        handle_error(conn->fd, 500, "Internal Server Error", conn->config->root,
                     path);
        return -1;
    }

    char etag[64];
    snprintf(etag, sizeof(etag), "\"%lx-%lx\"", (unsigned long)st.st_ino,
             (unsigned long)st.st_mtime);

    struct tm *gmt_time = gmtime(&st.st_mtime);
    char last_modified[128];
    strftime(last_modified, sizeof(last_modified), "%a, %d %b %Y %H:%M:%S GMT",
             gmt_time);

    const char *mime = get_mime_type(local_path);

    // Calculate request time
    struct timespec end_time;
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    double request_time = (end_time.tv_sec - conn->start_time.tv_sec) +
                          (end_time.tv_nsec - conn->start_time.tv_nsec) / 1e9;

    time_t now = time(NULL);
    struct tm *gmt = gmtime(&now);
    char date_buf[128];
    strftime(date_buf, sizeof(date_buf), "%a, %d %b %Y %H:%M:%S GMT", gmt);

    int is_head = (strcmp(method, "HEAD") == 0);

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
    if (!conn->write_buffer) {
        close(file_fd);
        return -1;
    }
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

    return 0;
}