#include "../include/server.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#define MAX_EVENTS 1024

extern int setup_listening_socket(ServerConfig *config);
extern void set_epoll_fd(int fd);
extern void handle_accept(int server_fd, ServerConfig *config);
extern Connection *get_connection(int fd);
extern void handle_read(Connection *conn);
extern void handle_write(Connection *conn);

static FileCache global_cache;

int main(void) {
  ServerConfig config;
  memset(&config, 0, sizeof(config));
  load_config(NULL, &config);

  memset(&global_cache, 0, sizeof(global_cache));
  config.file_cache = &global_cache;

  load_plugins(&config);

  int server_fd = setup_listening_socket(&config);
  if (server_fd < 0) {
    exit(EXIT_FAILURE);
  }

  int epoll_fd = epoll_create1(0);
  if (epoll_fd < 0) {
    log_message(&config, LOG_ERROR, "epoll_create1 failed: %s", strerror(errno));
    exit(EXIT_FAILURE);
  }

  set_epoll_fd(epoll_fd);

  extern int add_to_epoll(int fd, uint32_t events);
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
        Connection *conn = get_connection(events[i].data.fd);
        if (!conn)
          continue;

        if (events[i].events & EPOLLIN) {
          handle_read(conn);
        }

        if (get_connection(events[i].data.fd) != conn)
          continue;

        if (events[i].events & EPOLLOUT) {
          handle_write(conn);
        }

        if (events[i].events & (EPOLLERR | EPOLLHUP)) {
          if (get_connection(events[i].data.fd) == conn) {
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
