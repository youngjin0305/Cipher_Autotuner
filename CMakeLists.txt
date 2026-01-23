cmake_minimum_required(VERSION 3.16)
project(cipher_mvp C)
set(CMAKE_C_STANDARD 11)

add_executable(mvp
  src/main.c
  src/bench.c
  src/dispatch.c
  src/rdtsc.c
  src/cipher_ref.c
  src/cipher_opt.c
)

target_include_directories(mvp PRIVATE include)
target_compile_options(mvp PRIVATE -O3)