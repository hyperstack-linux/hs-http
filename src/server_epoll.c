#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include "include/server.h"

#define MAX_EVENTS 1024
#define BUFFER_SIZE 4096

typedef enum {
    CONN_STATE_READ_REQUEST,
    CONN_STATE_PROCESS,
    CONN_STATE_WRITE_RESPONSE,
    CONN_STATE_WRITE_FILE,
    CONN_STATE_CLOSING
} ConnectionState;

typedef struct {
    int fd;
    ConnectionState state;
    char *read_buffer;
    size_t read_buffer_size;
    size_t bytes_read;
    char *write_buffer;
    size_t write_buffer_size;
    size_t bytes_written;
    int file_fd;
    off_t file_size;
    off_t file_offset;
    ServerConfig *config;
    struct timespec start_time;
    char if_modified_since[128];
    char if_none_match[256];
} Connection;

static int epoll_fd;
static Connection *connections[MAX_EVENTS];
static FileCache global_cache;

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int setup_listening_socket(ServerConfig *config) {
    int server_fd;
    struct sockaddr_in address;
    int opt = 1;

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        log_message(config, LOG_ERROR, "socket failed: %s", strerror(errno));
        return -1;
    }

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        log_message(config, LOG_ERROR, "setsockopt failed: %s", strerror(errno));
        close(server_fd);
        return -1;
    }

    if (set_nonblocking(server_fd) < 0) {
        log_message(config, LOG_ERROR, "set_nonblocking failed: %s", strerror(errno));
        close(server_fd);
        return -1;
    }

    address.sin_family = AF_INET;
    if (strcmp(config->interface, "0.0.0.0") == 0) {
        address.sin_addr.s_addr = INADDR_ANY;
    } else {
        address.sin_addr.s_addr = inet_addr(config->interface);
    }
    address.sin_port = htons(config->listen);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        log_message(config, LOG_ERROR, "bind failed: %s", strerror(errno));
        close(server_fd);
        return -1;
    }

    if (listen(server_fd, SOMAXCONN) < 0) {
        log_message(config, LOG_ERROR, "listen failed: %s", strerror(errno));
        close(server_fd);
        return -1;
    }

    return server_fd;
}

static Connection* create_connection(int fd, ServerConfig *config) {
    Connection *conn = calloc(1, sizeof(Connection));
    conn->fd = fd;
    conn->state = CONN_STATE_READ_REQUEST;
    conn->read_buffer = malloc(BUFFER_SIZE);
    conn->read_buffer_size = BUFFER_SIZE;
    conn->config = config;
    conn->file_fd = -1;
    conn->if_modified_since[0] = '\0';
    conn->if_none_match[0] = '\0';
    clock_gettime(CLOCK_MONOTONIC, &conn->start_time);
    return conn;
}

static void free_connection(Connection *conn) {
    if (!conn) return;
    if (conn->read_buffer) free(conn->read_buffer);
    if (conn->write_buffer) free(conn->write_buffer);
    if (conn->file_fd >= 0) close(conn->file_fd);
    if (conn->fd >= 0) close(conn->fd);
    connections[conn->fd] = NULL;
    free(conn);
}

static int add_to_epoll(int fd, uint32_t events) {
    struct epoll_event ev;
    ev.events = events;
    ev.data.fd = fd;
    return epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev);
}

static int mod_epoll(int fd, uint32_t events) {
    struct epoll_event ev;
    ev.events = events;
    ev.data.fd = fd;
    return epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
}

static CacheEntry* find_cache_entry(const char* path, struct stat* st, ServerConfig* config) {
    time_t now = time(NULL);
    
    for (int i = 0; i < global_cache.count; i++) {
        CacheEntry* entry = &global_cache.entries[i];
        if (strcmp(entry->path, path) == 0) {
            // Check if still valid (mtime and inode match)
            if (entry->mtime == st->st_mtime && entry->ino == st->st_ino) {
                // Check if cache entry has expired
                if ((now - entry->cached_at) > config->cached_time) {
                    // Cache expired, invalidate
                    free(entry->content);
                    for (int j = i; j < global_cache.count - 1; j++) {
                        global_cache.entries[j] = global_cache.entries[j + 1];
                    }
                    global_cache.count--;
                    return NULL;
                }
                return entry;
            } else {
                // Invalidate old entry
                free(entry->content);
                // Remove by shifting
                for (int j = i; j < global_cache.count - 1; j++) {
                    global_cache.entries[j] = global_cache.entries[j + 1];
                }
                global_cache.count--;
                return NULL;
            }
        }
    }
    return NULL;
}

static void add_cache_entry(const char* path, struct stat* st, unsigned char* content, 
                           const char* mime, const char* etag, const char* last_modified) {
    // Don't cache large files
    if (st->st_size > MAX_CACHE_FILE_SIZE) {
        return;
    }
    
    // If cache full, evict oldest entry
    if (global_cache.count >= MAX_CACHE_ENTRIES) {
        free(global_cache.entries[0].content);
        for (int i = 0; i < global_cache.count - 1; i++) {
            global_cache.entries[i] = global_cache.entries[i + 1];
        }
        global_cache.count--;
    }
    
    CacheEntry* entry = &global_cache.entries[global_cache.count];
    strncpy(entry->path, path, sizeof(entry->path) - 1);
    entry->content = content;
    entry->size = st->st_size;
    entry->mtime = st->st_mtime;
    entry->ino = st->st_ino;
    strncpy(entry->etag, etag, sizeof(entry->etag) - 1);
    strncpy(entry->last_modified, last_modified, sizeof(entry->last_modified) - 1);
    strncpy(entry->content_type, mime, sizeof(entry->content_type) - 1);
    entry->cached_at = time(NULL);
    
    global_cache.count++;
}

static void handle_accept(int server_fd, ServerConfig *config) {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    
    int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
    if (client_fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            log_message(config, LOG_ERROR, "accept failed: %s", strerror(errno));
        }
        return;
    }
    
    if (set_nonblocking(client_fd) < 0) {
        log_message(config, LOG_ERROR, "set_nonblocking failed: %s", strerror(errno));
        close(client_fd);
        return;
    }
    
    Connection *conn = create_connection(client_fd, config);
    connections[client_fd] = conn;
    
    if (add_to_epoll(client_fd, EPOLLIN | EPOLLET) < 0) {
        log_message(config, LOG_ERROR, "epoll_ctl failed: %s", strerror(errno));
        free_connection(conn);
        return;
    }
}

static void handle_read(Connection *conn) {
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
        
        // Find header end robustly (allow CRLF, LF or CR replaced by NUL)
        char *hdr_end = NULL;
        // helper: find two '\n' characters not far apart (handles CRLF CRLF, LF LF, or NUL+LF patterns)
        for (size_t i = 0; i + 1 < (size_t)conn->bytes_read; i++) {
            if (conn->read_buffer[i] == '\n') {
                // find next newline within a small window
                for (size_t j = i + 1; j < i + 5 && j < (size_t)conn->bytes_read; j++) {
                    if (conn->read_buffer[j] == '\n') {
                        hdr_end = conn->read_buffer + j;
                        break;
                    }
                }
                if (hdr_end) break;
            }
        }

        if (hdr_end) {
            // If there's a Content-Length header, ensure we've read the whole body before processing
            int content_length = 0;
            // Make a temporary copy of headers area to scan for Content-Length safely
            size_t hdr_len = hdr_end - conn->read_buffer;
            char* hdr_copy = malloc(hdr_len + 1);
            if (hdr_copy) {
                memcpy(hdr_copy, conn->read_buffer, hdr_len);
                hdr_copy[hdr_len] = '\0';
                char* ln = strtok(hdr_copy, "\r\n"); // request line
                while ((ln = strtok(NULL, "\r\n")) != NULL && strlen(ln) > 0) {
                    if (strncasecmp(ln, "Content-Length:", 15) == 0) {
                        content_length = atoi(ln + 15);
                        break;
                    }
                }
                free(hdr_copy);
            }

            // total bytes needed = headers_end_index + 1 + content_length
            size_t hdr_index = hdr_end - conn->read_buffer;
            if (hdr_index + 1 + (size_t)content_length > (size_t)conn->bytes_read) {
                // need more data
            } else {
                conn->state = CONN_STATE_PROCESS;
            }
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
        
        // Parse headers for cache validation
        while ((line = strtok(NULL, "\r\n")) != NULL && strlen(line) > 0) {
            if (strncasecmp(line, "If-Modified-Since:", 18) == 0) {
                strncpy(conn->if_modified_since, line + 19, sizeof(conn->if_modified_since) - 1);
                // Trim whitespace
                char *start = conn->if_modified_since;
                while (*start == ' ') start++;
                if (start != conn->if_modified_since) {
                    memmove(conn->if_modified_since, start, strlen(start) + 1);
                }
            } else if (strncasecmp(line, "If-None-Match:", 14) == 0) {
                strncpy(conn->if_none_match, line + 15, sizeof(conn->if_none_match) - 1);
                char *start = conn->if_none_match;
                while (*start == ' ') start++;
                if (start != conn->if_none_match) {
                    memmove(conn->if_none_match, start, strlen(start) + 1);
                }
            }
        }
        
        int is_head = (strcmp(method, "HEAD") == 0);

        // For non-GET/HEAD methods, try plugin first; if not handled, return 405
        if (strcmp(method, "GET") != 0 && !is_head) {
            if (handle_plugin_request(conn->fd, path, method, version, conn->read_buffer, conn->bytes_read, conn->config) == 0) {
                free_connection(conn);
                return;
            }
            handle_error(conn->fd, 405, "Method Not Allowed", conn->config->root, path);
            conn->state = CONN_STATE_CLOSING;
            free_connection(conn);
            return;
        }

        // For GET requests, allow plugin to override file serving (HEAD is not passed to plugins)
        if (!is_head) {
            if (handle_plugin_request(conn->fd, path, method, version, conn->read_buffer, conn->bytes_read, conn->config) == 0) {
                free_connection(conn);
                return;
            }
        }
        
        char path_without_query[1024];
        strncpy(path_without_query, path, sizeof(path_without_query) - 1);
        char *query_start = strchr(path_without_query, '?');
        if (query_start) *query_start = '\0';
        
        char local_path[2048];
        if (strcmp(path_without_query, "/") == 0) {
            snprintf(local_path, sizeof(local_path), "%s/index.html", conn->config->root);
        } else {
            snprintf(local_path, sizeof(local_path), "%s%s", conn->config->root, path_without_query);
        }
        
        struct stat st;
        if (stat(local_path, &st) < 0) {
            // Try with .html extension if require_extensions is false
            if (!conn->config->require_extensions && strchr(path_without_query, '.') == NULL) {
                snprintf(local_path, sizeof(local_path), "%s%s.html", conn->config->root, path_without_query);
                if (stat(local_path, &st) >= 0) {
                    goto file_found;
                }
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
            handle_error(conn->fd, 500, "Internal Server Error", conn->config->root, path);
            conn->state = CONN_STATE_CLOSING;
            free_connection(conn);
            return;
        }
        
        // Generate ETag from inode and mtime
        char etag[64];
        snprintf(etag, sizeof(etag), "\"%lx-%lx\"", 
                (unsigned long)st.st_ino, (unsigned long)st.st_mtime);
        
        // Prepare Last-Modified header
        struct tm *gmt_time = gmtime(&st.st_mtime);
        char last_modified[128];
        strftime(last_modified, sizeof(last_modified), "%a, %d %b %Y %H:%M:%S GMT", gmt_time);
        
        // Check cache first
        CacheEntry* cached = find_cache_entry(local_path, &st, conn->config);
        
        // Check If-None-Match (ETag)
        if (conn->if_none_match[0] != '\0' && strcmp(conn->if_none_match, etag) == 0) {
            if (file_fd >= 0) close(file_fd);
            
            char response[1024];
            snprintf(response, sizeof(response),
                "HTTP/1.1 304 Not Modified\r\n"
                "Server: %s/%s\r\n"
                "Date: %s\r\n"
                "Last-Modified: %s\r\n"
                "ETag: %s\r\n"
                "Cache-Control: public, max-age=%d\r\n"
                "X-Request-Time: %.6f\r\n"
                "Connection: close\r\n"
                "\r\n",
                SERVER_NAME, SERVER_VERSION, date_buf, last_modified, etag, 
                conn->config->cache_max_age, request_time);
            
            conn->write_buffer = strdup(response);
            conn->write_buffer_size = strlen(response);
            conn->state = CONN_STATE_WRITE_RESPONSE;
            mod_epoll(conn->fd, EPOLLOUT | EPOLLET);
            return;
        }
        
        // Check If-Modified-Since
        if (conn->if_modified_since[0] != '\0' && strcmp(conn->if_modified_since, last_modified) == 0) {
            if (file_fd >= 0) close(file_fd);
            
            char response[1024];
            snprintf(response, sizeof(response),
                "HTTP/1.1 304 Not Modified\r\n"
                "Server: %s/%s\r\n"
                "Date: %s\r\n"
                "Last-Modified: %s\r\n"
                "ETag: %s\r\n"
                "Cache-Control: public, max-age=%d\r\n"
                "X-Request-Time: %.6f\r\n"
                "Connection: close\r\n"
                "\r\n",
                SERVER_NAME, SERVER_VERSION, date_buf, last_modified, etag,
                conn->config->cache_max_age, request_time);
            
            conn->write_buffer = strdup(response);
            conn->write_buffer_size = strlen(response);
            conn->state = CONN_STATE_WRITE_RESPONSE;
            mod_epoll(conn->fd, EPOLLOUT | EPOLLET);
            return;
        }
        
        const char *mime = get_mime_type(local_path);
        
        // If cached, serve from RAM
        if (cached) {
            close(file_fd);
            
            char header[1024];
            int header_len = snprintf(header, sizeof(header),
                "HTTP/1.1 200 OK\r\n"
                "Server: %s/%s\r\n"
                "Date: %s\r\n"
                "Content-Type: %s\r\n"
                "Content-Length: %zu\r\n"
                "Last-Modified: %s\r\n"
                "ETag: %s\r\n"
                "Cache-Control: public, max-age=%d\r\n"
                "X-Request-Time: %.6f\r\n"
                "X-Cache: HIT\r\n"
                "Connection: close\r\n"
                "\r\n",
                SERVER_NAME, SERVER_VERSION, date_buf, cached->content_type, cached->size,
                cached->last_modified, cached->etag, conn->config->cache_max_age, request_time);
            
            if (is_head) {
                conn->write_buffer = malloc(header_len);
                memcpy(conn->write_buffer, header, header_len);
                conn->write_buffer_size = header_len;
            } else {
                conn->write_buffer = malloc(header_len + cached->size);
                memcpy(conn->write_buffer, header, header_len);
                memcpy(conn->write_buffer + header_len, cached->content, cached->size);
                conn->write_buffer_size = header_len + cached->size;
            }
            
            conn->state = CONN_STATE_WRITE_RESPONSE;
            mod_epoll(conn->fd, EPOLLOUT | EPOLLET);
            return;
        }
        
        // Not cached - read from disk and potentially cache it
        unsigned char* file_content = NULL;
        if (st.st_size <= MAX_CACHE_FILE_SIZE) {
            file_content = malloc(st.st_size);
            ssize_t bytes_read = read(file_fd, file_content, st.st_size);
            if (bytes_read == st.st_size) {
                // Successfully read, add to cache
                add_cache_entry(local_path, &st, file_content, mime, etag, last_modified);
            } else {
                free(file_content);
                file_content = NULL;
            }
            lseek(file_fd, 0, SEEK_SET);
        }
        
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
            SERVER_NAME, SERVER_VERSION, date_buf, mime, st.st_size, 
            last_modified, etag, conn->config->cache_max_age, request_time);
        
        conn->write_buffer = malloc(header_len);
        memcpy(conn->write_buffer, header, header_len);
        conn->write_buffer_size = header_len;
        
        // For HEAD requests, don't send file content
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

static void handle_write(Connection *conn) {
    if (conn->state == CONN_STATE_WRITE_RESPONSE) {
        while (conn->bytes_written < conn->write_buffer_size) {
            ssize_t n = write(conn->fd, 
                            conn->write_buffer + conn->bytes_written,
                            conn->write_buffer_size - conn->bytes_written);
            
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return;
                }
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
            if (bytes_to_read > sizeof(buffer)) {
                bytes_to_read = sizeof(buffer);
            }
            
            ssize_t n = read(conn->file_fd, buffer, bytes_to_read);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return;
                }
                free_connection(conn);
                return;
            }
            
            if (n == 0) break;
            
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

int main() {
    ServerConfig config;
    load_config(NULL, &config);
    
    // Initialize file cache
    memset(&global_cache, 0, sizeof(global_cache));
    config.file_cache = &global_cache;
    
    load_plugins(&config);
    
    int server_fd = setup_listening_socket(&config);
    if (server_fd < 0) {
        exit(EXIT_FAILURE);
    }
    
    epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        log_message(&config, LOG_ERROR, "epoll_create1 failed: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }
    
    if (add_to_epoll(server_fd, EPOLLIN) < 0) {
        log_message(&config, LOG_ERROR, "epoll_ctl failed: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }
    
    log_message(&config, LOG_INFO, "Server listening on port %d (epoll)", config.listen);
    log_message(&config, LOG_INFO, "Serving files from %s/", config.root);
    
    struct epoll_event events[MAX_EVENTS];
    
    while (1) {
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        
        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == server_fd) {
                handle_accept(server_fd, &config);
            } else {
                Connection *conn = connections[events[i].data.fd];
                if (!conn) continue;
                
                if (events[i].events & EPOLLIN) {
                    handle_read(conn);
                } else if (events[i].events & EPOLLOUT) {
                    handle_write(conn);
                } else if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                    free_connection(conn);
                }
            }
        }
    }
    
    close(epoll_fd);
    close(server_fd);
    unload_plugins(&config);
    
    return 0;
}
