http1_lib = static_library('http1',
  'http1_read.c',
  'http1_write.c',
  'http1_parse.c',
  'http1_response.c',
  include_directories : '../include',
  install : false)

http1_dep = declare_dependency(link_with : http1_lib)
