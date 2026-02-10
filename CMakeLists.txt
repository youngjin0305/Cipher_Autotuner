cmake_minimum_required(VERSION 3.16)
project(cipher_mvp C)
set(CMAKE_C_STANDARD 11)

add_library(common STATIC common/src/common_time_rdtsc.c)
target_include_directories(common PUBLIC common/include)

add_library(aria_ref STATIC block_ciphers/aria/impl/ref/aria_ref.c)
target_include_directories(aria_ref PUBLIC block_ciphers/aria/api)
target_include_directories(aria_ref PRIVATE ${CMAKE_SOURCE_DIR}/block_ciphers/aria/source/ref)
target_sources(aria_ref PRIVATE ${CMAKE_SOURCE_DIR}/block_ciphers/aria/source/ref/aria_ref_core.c)

add_executable(aria_kat block_ciphers/aria/tests/test_aria_kat.c)
target_link_libraries(aria_kat PRIVATE aria_ref)

add_library(aria_avx2 STATIC
  block_ciphers/aria/impl/avx2/aria_avx2.c
  block_ciphers/aria/source/avx2/aria_avx2_core.c
)
target_include_directories(aria_avx2 PUBLIC
  block_ciphers/aria/api
)
target_include_directories(aria_avx2 PRIVATE
  ${CMAKE_SOURCE_DIR}/block_ciphers/aria/source/avx2
  ${CMAKE_SOURCE_DIR}/block_ciphers/aria/source/ref
)
target_link_libraries(aria_avx2 PRIVATE aria_ref)
target_compile_options(aria_avx2 PRIVATE /arch:AVX2)

add_library(aria_dispatch STATIC
  block_ciphers/aria/dispatch/runtime_dispatch.c
  block_ciphers/aria/dispatch/autotune.c
)
target_include_directories(aria_dispatch PUBLIC
  block_ciphers/aria/api
  block_ciphers/aria/dispatch
)
target_link_libraries(aria_dispatch PUBLIC aria_ref)

add_executable(bench_runner
  bench/runner/main.c
  bench/runner/bench_measure.c
)
target_include_directories(bench_runner PRIVATE
  common/include
  block_ciphers/aria/api
  block_ciphers/aria/dispatch
)
target_link_libraries(bench_runner PRIVATE common aria_dispatch aria_ref)
target_link_libraries(aria_kat PRIVATE aria_avx2)
target_link_libraries(bench_runner PRIVATE aria_avx2)

set_target_properties(bench_runner PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")