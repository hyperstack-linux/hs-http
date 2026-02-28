#include "../include/server.h"
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

extern int set_nonblocking(int fd);
extern Connection *create_connection(int fd, ServerConfig *config);
extern int add_to_epoll(int fd, uint32_t events);
extern void free_connection(Connection *conn);

void handle_accept(int server_fd, ServerConfig *config) {
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

  if (add_to_epoll(client_fd, EPOLLIN | EPOLLET) < 0) {
    log_message(config, LOG_ERROR, "epoll_ctl failed: %s", strerror(errno));
    free_connection(conn);
    return;
  }
}
