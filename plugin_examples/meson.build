plugin_c = shared_library('plugin',
  'plugin.c',
  include_directories : '../src/include',
  install : true,
  install_dir : 'plugins'
)

plugin_cpp = shared_library('plugin_cpp',
  'plugin_cpp.cpp',
  include_directories : '../src/include',
  cpp_args : ['-std=c++17'],
  install : true,
  install_dir : 'plugins'
)
