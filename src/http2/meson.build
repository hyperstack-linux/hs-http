http2_lib = static_library('http2',
  'hpack_static.c',
  'hpack_decode.c',
  'hpack_encode.c',
  'http2_frame.c',
  'http2.c',
  include_directories : '../include',
  install : false)

http2_dep = declare_dependency(link_with : http2_lib)
