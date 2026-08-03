cmake_minimum_required(VERSION 3.16)
project(cipher_mvp C ASM)
set(CMAKE_C_STANDARD 11)
enable_testing()

set(ARIA_HAVE_LINUX_X86_ASM FALSE)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
   CMAKE_SIZEOF_VOID_P EQUAL 8 AND
   CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64" AND
   CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  set(ARIA_HAVE_LINUX_X86_ASM TRUE)
endif()

add_library(common STATIC common/src/common_time_rdtsc.c)
target_include_directories(common PUBLIC common/include)

add_library(bench_core STATIC bench/runner/bench_measure.c)
target_include_directories(bench_core PUBLIC
  bench/runner
  block_ciphers/aria/api
  common/include
)
target_link_libraries(bench_core PUBLIC common)
if(UNIX)
  target_link_libraries(bench_core PUBLIC m)
endif()

add_executable(bench_stats_test bench/runner/test_bench_stats.c)
target_link_libraries(bench_stats_test PRIVATE bench_core)
add_test(NAME bench_stats COMMAND bench_stats_test)

add_library(aria_ref STATIC
  block_ciphers/aria/impl/ref/aria_ref.c
  block_ciphers/aria/source/ref/aria_ref_core.c
)
target_include_directories(aria_ref PUBLIC
  block_ciphers/aria/api
)
target_include_directories(aria_ref PRIVATE
  ${CMAKE_SOURCE_DIR}/block_ciphers/aria/source/ref
)

add_library(aria_cpu_features STATIC
  block_ciphers/aria/dispatch/cpu_features.c
)
target_include_directories(aria_cpu_features PUBLIC
  block_ciphers/aria/dispatch
)

add_library(aria_linux_aesni_avx STATIC
  block_ciphers/aria/impl/linux_aesni_avx/aria_linux_aesni_avx.c
  block_ciphers/aria/source/linux_kernal/common/aria_linux_compat.c
)
target_include_directories(aria_linux_aesni_avx PUBLIC
  block_ciphers/aria/api
)
target_include_directories(aria_linux_aesni_avx PRIVATE
  ${CMAKE_SOURCE_DIR}/block_ciphers/aria/dispatch
  ${CMAKE_SOURCE_DIR}/block_ciphers/aria/source/linux_kernal/common
  ${CMAKE_SOURCE_DIR}/block_ciphers/aria/source/ref
)
target_link_libraries(aria_linux_aesni_avx PRIVATE aria_ref aria_cpu_features)

add_library(aria_linux_aesni_avx2 STATIC
  block_ciphers/aria/impl/linux_aesni_avx2/aria_linux_aesni_avx2.c
)
target_include_directories(aria_linux_aesni_avx2 PUBLIC
  block_ciphers/aria/api
)
target_include_directories(aria_linux_aesni_avx2 PRIVATE
  ${CMAKE_SOURCE_DIR}/block_ciphers/aria/dispatch
  ${CMAKE_SOURCE_DIR}/block_ciphers/aria/source/linux_kernal/common
)
target_link_libraries(aria_linux_aesni_avx2 PRIVATE aria_ref aria_cpu_features)

add_library(aria_linux_gfni_avx512 STATIC
  block_ciphers/aria/impl/linux_gfni_avx512/aria_linux_gfni_avx512.c
)
target_include_directories(aria_linux_gfni_avx512 PUBLIC
  block_ciphers/aria/api
)
target_include_directories(aria_linux_gfni_avx512 PRIVATE
  ${CMAKE_SOURCE_DIR}/block_ciphers/aria/dispatch
  ${CMAKE_SOURCE_DIR}/block_ciphers/aria/source/linux_kernal/common
)
target_link_libraries(aria_linux_gfni_avx512 PRIVATE
  aria_ref
  aria_cpu_features
  aria_linux_aesni_avx2
)
target_compile_definitions(aria_linux_gfni_avx512 PRIVATE
  ARIA_HAVE_LINUX_X86_ASM=$<BOOL:${ARIA_HAVE_LINUX_X86_ASM}>
)

if(ARIA_HAVE_LINUX_X86_ASM)
  target_sources(aria_linux_aesni_avx PRIVATE
    block_ciphers/aria/source/linux_kernal/aesni_avx/aria-aesni-avx-asm_64.S
  )
  target_sources(aria_linux_aesni_avx2 PRIVATE
    block_ciphers/aria/source/linux_kernal/aesni_avx2/aria-aesni-avx2-asm_64.S
  )
  target_sources(aria_linux_gfni_avx512 PRIVATE
    block_ciphers/aria/source/linux_kernal/gfni_avx512/aria-gfni-avx512-asm_64.S
  )
  target_compile_options(aria_linux_aesni_avx PRIVATE -maes -mavx)
  target_compile_options(aria_linux_aesni_avx2 PRIVATE -maes -mavx2)
endif()

add_library(aria_dispatch STATIC
  block_ciphers/aria/dispatch/runtime_dispatch.c
  block_ciphers/aria/dispatch/autotune.c
  block_ciphers/aria/dispatch/policy_metrics.c
)
target_include_directories(aria_dispatch PUBLIC
  block_ciphers/aria/api
  block_ciphers/aria/dispatch
  bench/runner
)
target_link_libraries(aria_dispatch PUBLIC
  aria_cpu_features
  aria_ref
  aria_linux_aesni_avx
  aria_linux_aesni_avx2
  aria_linux_gfni_avx512
  bench_core
)

add_executable(aria_kat block_ciphers/aria/tests/test_aria_kat.c)
target_link_libraries(aria_kat PRIVATE
  aria_ref
  aria_cpu_features
  aria_linux_aesni_avx
  aria_linux_aesni_avx2
  aria_linux_gfni_avx512
)
add_test(NAME aria_kat COMMAND aria_kat)

add_executable(aria_policy_dispatch_test
  block_ciphers/aria/tests/test_policy_dispatch.c
)
target_link_libraries(aria_policy_dispatch_test PRIVATE aria_dispatch)
add_test(NAME aria_policy_dispatch COMMAND aria_policy_dispatch_test)

add_executable(aria_policy_metrics_test
  block_ciphers/aria/tests/test_policy_metrics.c
  block_ciphers/aria/dispatch/policy_metrics.c
)
target_include_directories(aria_policy_metrics_test PRIVATE block_ciphers/aria/dispatch)
add_test(NAME aria_policy_metrics COMMAND aria_policy_metrics_test)

add_executable(bench_runner
  bench/runner/main.c
  bench/runner/dispatch_evaluation.c
)
target_include_directories(bench_runner PRIVATE
  common/include
  bench/runner
  block_ciphers/aria/api
  block_ciphers/aria/dispatch
)
target_link_libraries(bench_runner PRIVATE common bench_core aria_dispatch aria_ref)

set_target_properties(bench_runner PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
