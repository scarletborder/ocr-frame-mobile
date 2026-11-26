cmake_minimum_required(VERSION 3.20)
project(vlcore_pc CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------- 共享核心
# 与 iOS / Android 编译的是同一份源码，PC 示例因此能复现手机上的行为。
set(SHARD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../shard")
set(VLCORE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${SHARD_DIR}" shard)

# ---------------------------------------------------------------- 参数
set(ORT_ROOT "" CACHE PATH "ONNX Runtime 目录（含 onnxruntime_c_api.h 与 onnxruntime 动态库）")
set(ORT_LIB "" CACHE FILEPATH "显式指定 ONNX Runtime 动态库（默认在 ORT_ROOT 内自动查找）")
set(LLAMA_DIR "" CACHE PATH "llama.cpp 源码目录（提供 include/llama.h 与 tools/mtmd/mtmd.h）")
set(LLAMA_LIB_DIR "" CACHE PATH "llama.cpp 构建目录（含 libllama / libmtmd / libggml*）")

add_executable(vlcore_pc src/main.cpp)
target_include_directories(vlcore_pc PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
target_link_libraries(vlcore_pc PRIVATE vlcore)

# ---------------------------------------------------------------- ONNX Runtime
if(ORT_ROOT)
    if(NOT ORT_LIB)
        foreach(candidate onnxruntime.dll libonnxruntime.so libonnxruntime.dylib)
            if(EXISTS "${ORT_ROOT}/${candidate}")
                set(ORT_LIB "${ORT_ROOT}/${candidate}")
                break()
            endif()
        endforeach()
    endif()
    if(NOT ORT_LIB OR NOT EXISTS "${ORT_LIB}")
        message(FATAL_ERROR "在 ${ORT_ROOT} 找不到 ONNX Runtime 动态库，请用 -DORT_LIB 指定")
    endif()
    if(NOT EXISTS "${ORT_ROOT}/onnxruntime_c_api.h")
        message(FATAL_ERROR "在 ${ORT_ROOT} 找不到 onnxruntime_c_api.h")
    endif()

    target_include_directories(vlcore PRIVATE "${ORT_ROOT}")
    target_link_libraries(vlcore_pc PRIVATE "${ORT_LIB}")
    # 动态库必须和可执行文件放一起（Windows 的 DLL 搜索顺序）
    add_custom_command(TARGET vlcore_pc POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${ORT_LIB}" "$<TARGET_FILE_DIR:vlcore_pc>")
    message(STATUS "ONNX Runtime: ${ORT_LIB}")
else()
    target_compile_definitions(vlcore PRIVATE VLCORE_WITHOUT_ORT=1)
    message(WARNING "未指定 -DORT_ROOT：产物不含版面引擎，只能验证编译链")
endif()

# ---------------------------------------------------------------- llama.cpp
if(LLAMA_DIR AND LLAMA_LIB_DIR)
    target_include_directories(vlcore PRIVATE
        "${LLAMA_DIR}/include"
        "${LLAMA_DIR}/tools/mtmd"
        "${LLAMA_DIR}/ggml/include")

    foreach(name llama mtmd ggml ggml-base ggml-cpu)
        string(TOUPPER "${name}" upper)
        string(REPLACE "-" "_" upper "${upper}")
        find_library(LIB_${upper} NAMES ${name}
            PATHS "${LLAMA_LIB_DIR}/src" "${LLAMA_LIB_DIR}/tools/mtmd" "${LLAMA_LIB_DIR}/ggml/src"
            NO_DEFAULT_PATH)
        if(NOT LIB_${upper})
            message(FATAL_ERROR "在 ${LLAMA_LIB_DIR} 找不到 lib${name}")
        endif()
    endforeach()

    target_link_libraries(vlcore PRIVATE ${LIB_MTMD} ${LIB_LLAMA} ${LIB_GGML} ${LIB_GGML_CPU} ${LIB_GGML_BASE})
    if(WIN32)
        target_link_libraries(vlcore PRIVATE ws2_32)
    endif()
    message(STATUS "llama.cpp: ${LLAMA_LIB_DIR}")
else()
    target_compile_definitions(vlcore PRIVATE VLCORE_WITHOUT_LLAMA=1)
    message(WARNING "未指定 -DLLAMA_DIR/-DLLAMA_LIB_DIR：产物不含本地 VLM（可用 --stub-vlm 验证其余链路）")
endif()
