#include "include/http1.h"
#include "include/http2.h"
#include "include/server.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_EVENTS 1024
#define BUFFER_SIZE 4096

static int epoll_fd;
static Connection *connections[MAX_EVENTS];
static FileCache global_cache;

static int set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1)
    return -1;
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
    log_message(config, LOG_ERROR, "set_nonblocking failed: %s",
                strerror(errno));
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

static Connection *create_connection(int fd, ServerConfig *config) {
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
  conn->protocol = PROTO_UNKNOWN; // Initialize new field
  conn->http2_state = NULL;       // Initialize new field
  return conn;
}

// Exposed for modules
int mod_epoll(int fd, uint32_t events) {
  struct epoll_event ev;
  ev.events = events;
  ev.data.fd = fd;
  return epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
}

// Exposed for modules
void free_connection(Connection *conn) {
  if (!conn)
    return;
  if (conn->read_buffer)
    free(conn->read_buffer);
  if (conn->write_buffer)
    free(conn->write_buffer);
  if (conn->file_fd >= 0)
    close(conn->file_fd);
  if (conn->fd >= 0)
    close(conn->fd);
  if (conn->http2_state)
    free(conn->http2_state);

  // Invalidate in global array
  if (connections[conn->fd] == conn) {
    connections[conn->fd] = NULL;
  }
  free(conn);
}

static int add_to_epoll(int fd, uint32_t events) {
  struct epoll_event ev;
  ev.events = events;
  ev.data.fd = fd;
  return epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev);
}

CacheEntry *find_cache_entry(const char *path, struct stat *st,
                             ServerConfig *config) {
  if (!config->file_cache)
    return NULL;
  FileCache *cache = config->file_cache;
  time_t now = time(NULL);

  for (int i = 0; i < cache->count; i++) {
    CacheEntry *entry = &cache->entries[i];
    if (strcmp(entry->path, path) == 0) {
      // Check if still valid (mtime and inode match)
      if (entry->mtime == st->st_mtime && entry->ino == st->st_ino) {
        // Check if cache entry has expired
        if ((now - entry->cached_at) > config->cached_time) {
          // Cache expired, invalidate
          free(entry->content);
          for (int j = i; j < cache->count - 1; j++) {
            cache->entries[j] = cache->entries[j + 1];
          }
          cache->count--;
          return NULL;
        }
        return entry;
      } else {
        // Invalidate old entry
        free(entry->content);
        // Remove by shifting
        for (int j = i; j < cache->count - 1; j++) {
          cache->entries[j] = cache->entries[j + 1];
        }
        cache->count--;
        return NULL;
      }
    }
  }
  return NULL;
}

void add_cache_entry(ServerConfig *config, const char *path, struct stat *st,
                     unsigned char *content, const char *mime, const char *etag,
                     const char *last_modified) {
  if (!config->file_cache)
    return;
  FileCache *cache = config->file_cache;

  // Don't cache large files
  if (st->st_size > MAX_CACHE_FILE_SIZE) {
    return;
  }

  // If cache full, evict oldest entry
  if (cache->count >= MAX_CACHE_ENTRIES) {
    free(cache->entries[0].content);
    for (int i = 0; i < cache->count - 1; i++) {
      cache->entries[i] = cache->entries[i + 1];
    }
    cache->count--;
  }

  CacheEntry *entry = &cache->entries[cache->count];
  strncpy(entry->path, path, sizeof(entry->path) - 1);
  entry->content = content;
  entry->size = st->st_size;
  entry->mtime = st->st_mtime;
  entry->ino = st->st_ino;
  strncpy(entry->etag, etag, sizeof(entry->etag) - 1);
  strncpy(entry->last_modified, last_modified,
          sizeof(entry->last_modified) - 1);
  strncpy(entry->content_type, mime, sizeof(entry->content_type) - 1);
  entry->cached_at = time(NULL);

  cache->count++;
}

static void handle_accept(int server_fd, ServerConfig *config) {
  struct sockaddr_in client_addr;
  socklen_t client_len = sizeof(client_addr);

  int client_fd =
      accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
  if (client_fd < 0) {
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      log_message(config, LOG_ERROR, "accept failed: %s", strerror(errno));
    }
    return;
  }

  if (set_nonblocking(client_fd) < 0) {
    log_message(config, LOG_ERROR, "set_nonblocking failed: %s",
                strerror(errno));
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
  if (conn->protocol == PROTO_UNKNOWN) {
    // Read all available data (EPOLLET requires draining the socket)
    while (1) {
      ssize_t n = read(conn->fd, conn->read_buffer + conn->bytes_read,
                       conn->read_buffer_size - conn->bytes_read - 1);

      if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
          break; // No more data available
        free_connection(conn);
        return;
      }
      if (n == 0) {
        free_connection(conn);
        return;
      }
      conn->bytes_read += n;

      // Expand buffer if needed
      if (conn->bytes_read >= conn->read_buffer_size - 1) {
        conn->read_buffer_size *= 2;
        conn->read_buffer = realloc(conn->read_buffer, conn->read_buffer_size);
      }
    }

    // Now check if we have enough to detect protocol
    if (conn->bytes_read < 4) {
      // Not enough data yet, wait for more
      return;
    }

    // We have at least 4 bytes - detect protocol
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

static void handle_write(Connection *conn) {
  if (conn->protocol == PROTO_HTTP1) {
    http1_handle_write(conn);
  } else if (conn->protocol == PROTO_HTTP2) {
    // http2_handle_write inline implementation for now
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

int main(void) {
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
    log_message(&config, LOG_ERROR, "epoll_create1 failed: %s",
                strerror(errno));
    exit(EXIT_FAILURE);
  }

  if (add_to_epoll(server_fd, EPOLLIN) < 0) {
    log_message(&config, LOG_ERROR, "epoll_ctl failed: %s", strerror(errno));
    exit(EXIT_FAILURE);
  }

  log_message(&config, LOG_INFO, "Server listening on port %d (epoll)",
              config.listen);
  log_message(&config, LOG_INFO, "Serving files from %s/", config.root);

  struct epoll_event events[MAX_EVENTS];

  while (1) {
    int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);

    for (int i = 0; i < n; i++) {
      if (events[i].data.fd == server_fd) {
        handle_accept(server_fd, &config);
      } else {
        Connection *conn = connections[events[i].data.fd];
        if (!conn)
          continue;

        if (events[i].events & EPOLLIN) {
          handle_read(conn);
        }

        // Check conn again as handle_read might have freed it
        if (connections[events[i].data.fd] != conn)
          continue;

        if (events[i].events & EPOLLOUT) {
          handle_write(conn);
        }

        if (events[i].events & (EPOLLERR | EPOLLHUP)) {
          // Only free if not already freed
          if (connections[events[i].data.fd] == conn) {
            free_connection(conn);
          }
        }
      }
    }
  }
  close(epoll_fd);
  close(server_fd);
  unload_plugins(&config);

  return 0;
}
