#include "../include/server.h"
#include <errno.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>

#define BUFFER_SIZE 4096

static int epoll_fd;
static Connection *connections[1024];

extern int set_nonblocking(int fd);
extern int add_to_epoll(int fd, uint32_t events);

Connection *create_connection(int fd, ServerConfig *config) {
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
  conn->protocol = PROTO_UNKNOWN;
  conn->http2_state = NULL;
  connections[fd] = conn;
  return conn;
}

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

  if (conn->fd >= 0 && conn->fd < 1024)
    connections[conn->fd] = NULL;
  free(conn);
}

Connection *get_connection(int fd) {
  if (fd >= 0 && fd < 1024)
    return connections[fd];
  return NULL;
}

int mod_epoll(int fd, uint32_t events) {
  struct epoll_event ev;
  ev.events = events;
  ev.data.fd = fd;
  return epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
}

int add_to_epoll(int fd, uint32_t events) {
  struct epoll_event ev;
  ev.events = events;
  ev.data.fd = fd;
  return epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev);
}

void set_epoll_fd(int fd) {
  epoll_fd = fd;
}
