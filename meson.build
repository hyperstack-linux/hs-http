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


executable('hs-http',
  'src/server_epoll.c',
  'src/mime.c',
  'src/handlers.c',
  'src/request.c',
  'src/response.c',
  'src/config.c',
  'src/plugin.c',
  error_headers,
  include_directories : 'src/include',
  dependencies : dl_dep,
  install : true)
