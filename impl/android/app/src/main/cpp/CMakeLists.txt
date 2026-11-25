cmake_minimum_required(VERSION 3.22)
project(vlocr_jni CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# 共享核心：与 impl/ios 编译的是同一份源码（impl/shard）
# ---------------------------------------------------------------------------
set(SHARD_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../shard)
if(NOT EXISTS ${SHARD_DIR}/CMakeLists.txt)
    message(FATAL_ERROR "找不到共享核心：${SHARD_DIR}")
endif()

# ---------------------------------------------------------------------------
# ONNX Runtime（scripts/fetch-onnxruntime-android.sh 解出 headers + .so）
# ---------------------------------------------------------------------------
set(ORT_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/onnxruntime)
set(ORT_ENABLED OFF)
if(EXISTS ${ORT_ROOT}/libonnxruntime.so AND EXISTS ${ORT_ROOT}/onnxruntime_c_api.h)
    add_library(onnxruntime SHARED IMPORTED)
    set_target_properties(onnxruntime PROPERTIES IMPORTED_LOCATION ${ORT_ROOT}/libonnxruntime.so)
    set(ORT_ENABLED ON)
else()
    message(WARNING "未找到 ${ORT_ROOT}/libonnxruntime.so：本次构建不含版面引擎（先跑 scripts/fetch-onnxruntime-android.sh）")
endif()

# ---------------------------------------------------------------------------
# llama.cpp（scripts/build-llama-android.sh 把源码放到 cpp/llama.cpp）
# ---------------------------------------------------------------------------
set(LLAMA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/llama.cpp)
set(LLAMA_ENABLED OFF)
if(EXISTS ${LLAMA_DIR}/CMakeLists.txt)
    set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_COMMON OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_MTMD ON CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
    set(GGML_LLAMAFILE OFF CACHE BOOL "" FORCE)
    add_subdirectory(${LLAMA_DIR} llama.cpp)
    if(TARGET llama AND TARGET mtmd)
        set(LLAMA_ENABLED ON)
    else()
        message(WARNING "llama.cpp 未提供 llama/mtmd 目标：本次构建不含本地 VLM")
    endif()
else()
    message(WARNING "未找到 ${LLAMA_DIR}：本次构建不含本地 VLM（先跑 scripts/build-llama-android.sh）")
endif()

# ---------------------------------------------------------------------------
# 共享核心本体
# ---------------------------------------------------------------------------
set(VLCORE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${SHARD_DIR} shard)

if(ORT_ENABLED)
    target_include_directories(vlcore PUBLIC ${ORT_ROOT})
    target_link_libraries(vlcore PUBLIC onnxruntime)
else()
    target_compile_definitions(vlcore PRIVATE VLCORE_WITHOUT_ORT=1)
endif()

if(LLAMA_ENABLED)
    target_link_libraries(vlcore PUBLIC llama mtmd)
else()
    target_compile_definitions(vlcore PRIVATE VLCORE_WITHOUT_LLAMA=1)
endif()

# ---------------------------------------------------------------------------
# JNI 胶水
# ---------------------------------------------------------------------------
add_library(vlocr SHARED vlcore_jni.cpp)
target_link_libraries(vlocr PRIVATE vlcore android log)
