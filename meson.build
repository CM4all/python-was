project('Python Gateway Interface Bridge',
  'cpp',
  version : '0.1',
  default_options : [
    'warning_level=3',
    'cpp_std=c++2a'
  ]
)

cxxflags = [
  # man gcc says: "This option does not warn about designated initializers", but it does
  # https://gcc.gnu.org/bugzilla/show_bug.cgi?id=96868
  '-Wno-missing-field-initializers',
  # -Wdelete-non-virtual-dtor is the better warning and included in -Wall
  '-Wno-non-virtual-dtor',
]

add_global_arguments(cxxflags, language: 'cpp')

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
subdir('libcommon/src/was')

executable('py-gi-bridge',
  'src/main.cpp',
  'src/python.cpp',
  'src/wsgi.cpp',
  dependencies : [
    fmt_dep,
    python_dep,
    was_dep,
  ],
  include_directories: inc,
  install : true
)
