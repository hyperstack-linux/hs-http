project('hs-http', 'c',
  version : '0.1',
  default_options : ['warning_level=3'])

xxd = find_program('xxd')

gen_header = generator(xxd,
  output : '@BASENAME@.h',
  arguments : ['-i', '@INPUT@', '@OUTPUT@'])

error_pages = files(
  'src/errors/400.html',
  'src/errors/403.html',
  'src/errors/404.html',
  'src/errors/405.html',
  'src/errors/500.html',
)

error_headers = gen_header.process(error_pages)

dl_dep = dependency('dl', required: true)

subdir('src/http1')
subdir('src/http2')
subdir('src/server_epoll')

executable('hs-http',
  'src/mime.c',
  'src/handlers.c',
  'src/request.c',
  'src/response.c',
  'src/config.c',
  'src/plugin.c',
  'src/request_handler.c',
  error_headers,
  include_directories : 'src/include',
  dependencies : dl_dep,
  link_with : [http1_lib, http2_lib, server_epoll_lib],
  install : true)
