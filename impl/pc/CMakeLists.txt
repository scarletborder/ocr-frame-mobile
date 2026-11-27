cmake_minimum_required(VERSION 3.20)
project(vlcore_pc CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------- 共享核心
# 与 iOS / Android 编译的是**同一份** impl/shard：版面预处理（拉伸 800x800 + /255）、
# 提示词、两阶段编排、llama.cpp 求值路径全部一致，所以桌面上跑通就等于手机上跑通。
set(SHARD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../shard")
set(VLCORE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${SHARD_DIR}" shard)

# ---------------------------------------------------------------- 参数
# 留空时自动找 scripts/bootstrap.sh 下载到 third_party/ 的依赖，因此
# `cmake -B build -S . && cmake --build build` 在装好依赖后应直接可跑。
set(ORT_ROOT "" CACHE PATH "ONNX Runtime 目录（含 onnxruntime_c_api.h 与动态库/导入库）")
set(ORT_LIB "" CACHE FILEPATH "显式指定 ONNX Runtime 动态库（默认在 ORT_ROOT 内自动查找）")
set(LLAMA_DIR "" CACHE PATH "llama.cpp 源码目录（提供 include/llama.h 与 tools/mtmd/mtmd.h）")
set(LLAMA_LIB_DIR "" CACHE PATH "llama.cpp 构建目录（含 libllama / libmtmd / libggml*）")

set(THIRD_PARTY "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

add_executable(vlcore_pc src/main.cpp)
target_include_directories(vlcore_pc PRIVATE "${THIRD_PARTY}")
target_link_libraries(vlcore_pc PRIVATE vlcore)

# MSVC 默认按本地代码页（简体中文机器是 936）读源码：UTF-8 的中文注释/字符串会被拆坏，
# 甚至出现「UTF-8 尾字节被当成反斜杠 → 续行吞掉下一行」这种诡异报错。
if(MSVC)
    target_compile_options(vlcore PRIVATE /utf-8)
    target_compile_options(vlcore_pc PRIVATE /utf-8)
endif()
if(NOT EXISTS "${THIRD_PARTY}/stb_image.h")
    message(FATAL_ERROR "缺 ${THIRD_PARTY}/stb_image.h：先跑 bash scripts/fetch-stb-image.sh")
endif()
if(NOT EXISTS "${THIRD_PARTY}/stb_image_write.h")
    message(WARNING "缺 stb_image_write.h：不编译叠加图（--overlay），其余功能不受影响；"
                    "跑 bash scripts/fetch-stb-image.sh 可以补上")
endif()

# ---------------------------------------------------------------- ONNX Runtime
if(NOT ORT_ROOT AND EXISTS "${THIRD_PARTY}/onnxruntime/include/onnxruntime_c_api.h")
    set(ORT_ROOT "${THIRD_PARTY}/onnxruntime")
endif()
if(ORT_ROOT)
    set(ORT_INCLUDE "${ORT_ROOT}/include")
    if(NOT EXISTS "${ORT_INCLUDE}/onnxruntime_c_api.h")
        set(ORT_INCLUDE "${ORT_ROOT}")
    endif()

    if(NOT ORT_LIB)
        # 顺序有意义：MSVC 用 .lib 导入库，MinGW/GNU 直接链 DLL（也支持 .so/.dylib）。
        if(MSVC)
            set(CANDIDATES lib/onnxruntime.lib onnxruntime.lib)
        else()
            set(CANDIDATES lib/onnxruntime.dll lib/libonnxruntime.so lib/libonnxruntime.dylib
                           onnxruntime.dll libonnxruntime.so libonnxruntime.dylib)
        endif()
        foreach(candidate ${CANDIDATES})
            if(NOT ORT_LIB AND EXISTS "${ORT_ROOT}/${candidate}")
                set(ORT_LIB "${ORT_ROOT}/${candidate}")
            endif()
        endforeach()
    endif()
    if(NOT ORT_LIB OR NOT EXISTS "${ORT_LIB}")
        message(FATAL_ERROR "在 ${ORT_ROOT} 找不到 ONNX Runtime 库，请用 -DORT_LIB 指定")
    endif()

    target_include_directories(vlcore PRIVATE "${ORT_INCLUDE}")
    target_link_libraries(vlcore_pc PRIVATE "${ORT_LIB}")
    if(WIN32)
        # DLL 必须和 exe 同目录（Windows 的 DLL 搜索顺序）
        get_filename_component(ORT_LIB_DIR "${ORT_LIB}" DIRECTORY)
        file(GLOB ORT_RUNTIME_DLLS "${ORT_LIB_DIR}/*.dll")
        foreach(dll ${ORT_RUNTIME_DLLS})
            add_custom_command(TARGET vlcore_pc POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:vlcore_pc>")
        endforeach()
    endif()
    message(STATUS "ONNX Runtime: ${ORT_LIB}")
else()
    target_compile_definitions(vlcore PRIVATE VLCORE_WITHOUT_ORT=1)
    message(WARNING "未找到 ONNX Runtime（third_party/onnxruntime 或 -DORT_ROOT）："
                    "产物不含版面引擎，只能验证编译链")
endif()

# ---------------------------------------------------------------- llama.cpp
if(NOT LLAMA_DIR AND EXISTS "${THIRD_PARTY}/llama.cpp/CMakeLists.txt")
    set(LLAMA_DIR "${THIRD_PARTY}/llama.cpp")
endif()
if(NOT LLAMA_LIB_DIR AND EXISTS "${THIRD_PARTY}/llama-build/CMakeCache.txt")
    set(LLAMA_LIB_DIR "${THIRD_PARTY}/llama-build")
endif()

if(LLAMA_DIR AND LLAMA_LIB_DIR)
    target_include_directories(vlcore PRIVATE
        "${LLAMA_DIR}/include"
        "${LLAMA_DIR}/tools/mtmd"
        "${LLAMA_DIR}/ggml/include")

    foreach(name llama mtmd ggml ggml-base ggml-cpu)
        string(TOUPPER "${name}" upper)
        string(REPLACE "-" "_" upper "${upper}")
        find_library(LIB_${upper} NAMES ${name}
            PATHS "${LLAMA_LIB_DIR}/src"
                  "${LLAMA_LIB_DIR}/tools/mtmd"
                  "${LLAMA_LIB_DIR}/ggml/src"
                  "${LLAMA_LIB_DIR}/src/Release"
                  "${LLAMA_LIB_DIR}/tools/mtmd/Release"
                  "${LLAMA_LIB_DIR}/ggml/src/Release"
            NO_DEFAULT_PATH)
        if(NOT LIB_${upper})
            message(FATAL_ERROR "在 ${LLAMA_LIB_DIR} 找不到 lib${name}（先跑 scripts/build-llama.sh）")
        endif()
    endforeach()

    target_link_libraries(vlcore PRIVATE ${LIB_MTMD} ${LIB_LLAMA} ${LIB_GGML} ${LIB_GGML_CPU} ${LIB_GGML_BASE})
    # 新版 mtmd 依赖上游 vendor/hash（sha256）；老版本没有这个库，找到了才链。
    find_library(LIB_VENDOR_HASH NAMES vendor-hash
        PATHS "${LLAMA_LIB_DIR}/vendor/hash" "${LLAMA_LIB_DIR}/vendor/hash/Release"
        NO_DEFAULT_PATH)
    if(LIB_VENDOR_HASH)
        target_link_libraries(vlcore PRIVATE ${LIB_VENDOR_HASH})
    endif()
    if(WIN32)
        target_link_libraries(vlcore PRIVATE ws2_32)
        # MSVC 默认构建出的是动态库（llama.dll / mtmd.dll / ggml*.dll），必须和 exe 放一起。
        file(GLOB LLAMA_RUNTIME_DLLS "${LLAMA_LIB_DIR}/bin/Release/*.dll"
                                     "${LLAMA_LIB_DIR}/bin/*.dll")
        foreach(dll ${LLAMA_RUNTIME_DLLS})
            add_custom_command(TARGET vlcore_pc POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:vlcore_pc>")
        endforeach()
    endif()
    message(STATUS "llama.cpp: ${LLAMA_LIB_DIR}")
else()
    target_compile_definitions(vlcore PRIVATE VLCORE_WITHOUT_LLAMA=1)
    message(WARNING "未找到 llama.cpp（third_party/llama.cpp + third_party/llama-build 或 "
                    "-DLLAMA_DIR/-DLLAMA_LIB_DIR）：产物不含本地 VLM，可用 --stub-vlm / --detect-only")
endif()
