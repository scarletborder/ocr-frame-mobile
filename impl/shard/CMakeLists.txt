cmake_minimum_required(VERSION 3.20)
project(vlcore LANGUAGES CXX)

# 共享核心：iOS 侧由 Xcode 直接编译这些源码；Android 侧由 app/src/main/cpp 的
# CMake 通过 add_subdirectory 引入；桌面用于单测。
option(VLCORE_WITH_ORT "编译 ONNX Runtime 版面引擎" OFF)
option(VLCORE_WITH_LLAMA "编译 llama.cpp 视觉引擎" OFF)
option(VLCORE_BUILD_TESTS "构建桌面单元测试" ON)

add_library(vlcore STATIC
    src/core.cpp
    src/pipeline.cpp
    src/vlcore_c.cpp
    # 引擎文件自带 #if 分支：未开启开关时只编译出「返回 nullptr + 可读错误」的工厂，
    # 保证核心与 C 门面永远能链接。
    src/engines/ort_layout.cpp
    src/engines/mtmd_vlm.cpp
)
target_include_directories(vlcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(vlcore PUBLIC cxx_std_17)

if(VLCORE_WITH_ORT)
    target_compile_definitions(vlcore PUBLIC VLCORE_WITH_ORT=1)
    if(DEFINED ONNXRUNTIME_INCLUDE_DIR)
        target_include_directories(vlcore PRIVATE ${ONNXRUNTIME_INCLUDE_DIR})
    endif()
    if(DEFINED ONNXRUNTIME_LIBRARY)
        target_link_libraries(vlcore PUBLIC ${ONNXRUNTIME_LIBRARY})
    endif()
endif()

if(VLCORE_WITH_LLAMA)
    target_compile_definitions(vlcore PUBLIC VLCORE_WITH_LLAMA=1)
    if(TARGET llama)
        target_link_libraries(vlcore PUBLIC llama)
    endif()
    if(TARGET mtmd)
        target_link_libraries(vlcore PUBLIC mtmd)
    endif()
endif()

if(VLCORE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
