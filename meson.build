# Package dependencies
project('AMEX transaction parser', 'c', default_options : ['werror=true'])

deps = [ dependency('glib-2.0') ]

# Project source files
main_sources = files(['amex_parser.c'])

executable('amex-parser',
  sources: main_sources,
  dependencies : [ deps ],
  #include_directories: include_dirs,
  install : true)
