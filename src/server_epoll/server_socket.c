#include "../include/server.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1)
    return -1;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int setup_listening_socket(ServerConfig *config) {
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
