cmake_minimum_required(VERSION 3.16)
project(llvm_mpi_sanitizer LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
include(AddLLVM)

find_package(MPI REQUIRED)

add_library(MPISanitizePass SHARED
  passes/MPISanitizePass.cpp
)

target_include_directories(MPISanitizePass PRIVATE
  ${LLVM_INCLUDE_DIRS}
)

target_compile_definitions(MPISanitizePass PRIVATE
  ${LLVM_DEFINITIONS}
)

# Pass plugins should not statically link LLVM component libraries, otherwise
# 'opt -load-pass-plugin' can crash due to duplicate global option registries.
# Prefer the monolithic shared LLVM library.
execute_process(
  COMMAND llvm-config --libs --link-shared
  OUTPUT_VARIABLE LLVM_DYLIB_LIBS
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
execute_process(
  COMMAND llvm-config --ldflags --link-shared
  OUTPUT_VARIABLE LLVM_DYLIB_LDFLAGS
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
execute_process(
  COMMAND llvm-config --system-libs --link-shared
  OUTPUT_VARIABLE LLVM_DYLIB_SYSTEM_LIBS
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

separate_arguments(LLVM_DYLIB_LIBS)
separate_arguments(LLVM_DYLIB_LDFLAGS)
separate_arguments(LLVM_DYLIB_SYSTEM_LIBS)

target_link_options(MPISanitizePass PRIVATE ${LLVM_DYLIB_LDFLAGS})
target_link_libraries(MPISanitizePass PRIVATE ${LLVM_DYLIB_LIBS} ${LLVM_DYLIB_SYSTEM_LIBS})

add_library(msan_runtime SHARED
  runtime/runtime.c
  runtime/detector.c
)

target_include_directories(msan_runtime PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/runtime
)

find_package(OpenSSL REQUIRED)

target_link_libraries(msan_runtime PRIVATE
  MPI::MPI_C
  OpenSSL::Crypto
)
