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

add_executable(quortex-flashnext-op flashnext_op.cpp)
target_include_directories(quortex-flashnext-op PRIVATE
  "${CORTEX_GGML_SOURCE}/include")
target_link_libraries(quortex-flashnext-op PRIVATE ggml ggml-base ggml-cpu)
target_compile_features(quortex-flashnext-op PRIVATE cxx_std_17)

set(CORTEX_LLAMA_BUILD "" CACHE PATH "Cortex FlashNext build directory")
if(CORTEX_LLAMA_BUILD AND EXISTS "${CORTEX_LLAMA_BUILD}/bin/libllama.so.0.5.0")
  add_library(cortex-llama SHARED IMPORTED)
  set_target_properties(cortex-llama PROPERTIES IMPORTED_LOCATION "${CORTEX_LLAMA_BUILD}/bin/libllama.so.0.5.0")
  add_library(cortex-common SHARED IMPORTED)
  set_target_properties(cortex-common PROPERTIES IMPORTED_LOCATION "${CORTEX_LLAMA_BUILD}/bin/libllama-common.so.0.5.0")
  add_library(cortex-ggml SHARED IMPORTED)
  set_target_properties(cortex-ggml PROPERTIES IMPORTED_LOCATION "${CORTEX_LLAMA_BUILD}/bin/libggml.so.0.25.3")
  add_library(cortex-ggml-base SHARED IMPORTED)
  set_target_properties(cortex-ggml-base PROPERTIES IMPORTED_LOCATION "${CORTEX_LLAMA_BUILD}/bin/libggml-base.so.0.25.3")
  add_library(cortex-ggml-cpu SHARED IMPORTED)
  set_target_properties(cortex-ggml-cpu PROPERTIES IMPORTED_LOCATION "${CORTEX_LLAMA_BUILD}/bin/libggml-cpu.so.0.25.3")
  add_executable(quortex-flashnext-layer0 flashnext_layer0.cpp)
  target_sources(quortex-flashnext-layer0 PRIVATE
    quortex_streamed_moe.cpp
    "${CORTEX_LLAMA_BUILD}/../runtime-flashnext-layer-exp/src/flashnext-selected-moe.cpp"
    "${CORTEX_LLAMA_BUILD}/../runtime-flashnext-layer-exp/src/flashnext-shared-expert.cpp"
    "${CORTEX_LLAMA_BUILD}/../runtime-flashnext-layer-exp/src/flashnext-tensor-index.cpp")
  target_include_directories(quortex-flashnext-layer0 PRIVATE
    "${CORTEX_LLAMA_BUILD}/../runtime-flashnext-layer-exp/include"
    "${CORTEX_LLAMA_BUILD}/../runtime-flashnext-layer-exp/src"
    "${CORTEX_GGML_SOURCE}/include")
  target_link_libraries(quortex-flashnext-layer0 PRIVATE cortex-llama cortex-common cortex-ggml cortex-ggml-cpu cortex-ggml-base dl pthread)
  target_compile_features(quortex-flashnext-layer0 PRIVATE cxx_std_17)
  set_target_properties(quortex-flashnext-layer0 PROPERTIES BUILD_RPATH "${CORTEX_LLAMA_BUILD}/bin")
endif()
