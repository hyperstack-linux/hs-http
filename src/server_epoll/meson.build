server_epoll_lib = static_library('server_epoll',
  'server_socket.c',
  'server_connection.c',
  'server_cache.c',
  'server_accept.c',
  'server_dispatch.c',
  'server_epoll.c',
  include_directories : '../include',
  install : false)

server_epoll_dep = declare_dependency(link_with : server_epoll_lib)
