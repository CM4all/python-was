project('Python Gateway Interface Bridge',
  'cpp',
  version : '0.1',
  default_options : [
    'warning_level=3',
    'cpp_std=c++2a'
  ]
)

compiler = meson.get_compiler('cpp')

inc = include_directories(
  'src/',
  'libcommon/src',
)

python_dep = dependency('python3-embed')

subdir('libcommon/src/util')
subdir('libcommon/src/lib/fmt')
subdir('libcommon/src/uri')
subdir('libcommon/src/time')
subdir('libcommon/src/http')
subdir('libcommon/src/io')

executable('py-gi-bridge',
  'src/main.cpp',
  'src/python.cpp',
  'src/wsgi.cpp',
  dependencies : [
    fmt_dep,
    python_dep,
  ],
  include_directories: inc,
  install : true
)
