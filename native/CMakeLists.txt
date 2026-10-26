cmake_minimum_required(VERSION 3.16)
project(quortex_native LANGUAGES C CXX)

set(CORTEX_GGML_SOURCE "" CACHE PATH "Cortex GGML source tree")
if(NOT CORTEX_GGML_SOURCE OR NOT EXISTS "${CORTEX_GGML_SOURCE}/CMakeLists.txt")
  message(FATAL_ERROR "Set -DCORTEX_GGML_SOURCE=/path/to/runtime-src/ggml")
endif()

# Build the exact GGML revision used by Cortex, out of tree.  No Cortex files
# are copied or modified; the source tree is linked into this build only.
set(GGML_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GGML_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
add_subdirectory("${CORTEX_GGML_SOURCE}" cortex-ggml EXCLUDE_FROM_ALL)

add_executable(quortex-gguf-diagnostic gguf_diagnostic.cpp)
target_include_directories(quortex-gguf-diagnostic PRIVATE
  "${CORTEX_GGML_SOURCE}/include")
target_link_libraries(quortex-gguf-diagnostic PRIVATE ggml-base)
target_compile_features(quortex-gguf-diagnostic PRIVATE cxx_std_17)

add_executable(quortex-tensor-allocation tensor_allocation.cpp)
target_include_directories(quortex-tensor-allocation PRIVATE
  "${CORTEX_GGML_SOURCE}/include")
target_link_libraries(quortex-tensor-allocation PRIVATE ggml ggml-base ggml-cpu)
target_compile_features(quortex-tensor-allocation PRIVATE cxx_std_17)
