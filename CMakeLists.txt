cmake_minimum_required(VERSION 3.18)
project(xen VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 项目源码和注释统一使用 UTF-8。MSVC 默认跟随系统代码页，中文注释可能被
# 错误解析并进一步破坏相邻的 C++ 词法单元，因此必须显式启用 UTF-8。
if(MSVC)
    add_compile_options(/utf-8)
endif()

# ─── 依赖 ───────────────────────────────────────────────

# ONNX Runtime
if(NOT ONNXRUNTIME_ROOT)
    set(ONNXRUNTIME_ROOT "$ENV{ONNXRUNTIME_ROOT}")
endif()
find_path(ONNXRUNTIME_INCLUDE_DIR onnxruntime_cxx_api.h
    HINTS ${ONNXRUNTIME_ROOT}
    PATH_SUFFIXES include)
find_library(ONNXRUNTIME_LIB onnxruntime
    HINTS ${ONNXRUNTIME_ROOT}
    PATH_SUFFIXES lib)
if(NOT ONNXRUNTIME_INCLUDE_DIR OR NOT ONNXRUNTIME_LIB)
    message(FATAL_ERROR
        "ONNX Runtime SDK not found. Set ONNXRUNTIME_ROOT to the extracted SDK root.")
endif()
message(STATUS "ONNX Runtime: ${ONNXRUNTIME_LIB}")

# OpenCV
find_package(OpenCV REQUIRED)
message(STATUS "OpenCV: ${OpenCV_VERSION}")

find_package(Threads REQUIRED)

# spdlog
include(FetchContent)
FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.17.0)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_SHARED  OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(spdlog)

# ─── Log 模块 ───────────────────────────────────────────

add_library(log STATIC
    Xen/log/log.cpp)

target_include_directories(log
    PUBLIC  ${PROJECT_SOURCE_DIR}/Xen
    PRIVATE ${PROJECT_SOURCE_DIR}/Xen/log)

target_link_libraries(log
    PUBLIC  spdlog::spdlog)

option(LOG_ENABLE_DEBUG "Enable debug/trace logging" OFF)
if(LOG_ENABLE_DEBUG)
    target_compile_definitions(log PUBLIC LOG_ENABLE_DEBUG)
endif()

# ─── Detector 模块 ──────────────────────────────────────

add_library(detector STATIC
    Xen/detector/detector.cpp
    Xen/detector/session.cpp
    Xen/detector/preprocess.cpp
    Xen/detector/postprocess.cpp)

target_include_directories(detector
    PUBLIC
        ${PROJECT_SOURCE_DIR}/Xen
        ${ONNXRUNTIME_INCLUDE_DIR}
        ${OpenCV_INCLUDE_DIRS}
    PRIVATE
        ${PROJECT_SOURCE_DIR}/Xen/detector)

target_link_libraries(detector
    PUBLIC  ${ONNXRUNTIME_LIB} ${OpenCV_LIBS} ${CMAKE_DL_LIBS} log
    PRIVATE Threads::Threads)

# ─── 测试 ───────────────────────────────────────────────

include(CTest)
if(BUILD_TESTING)
    # 纯算法测试不链接 ORT Session，避免测试解码和坐标数学时依赖 GPU
    # 运行环境；同时复用生产源码，防止测试复制一套实现。
    add_executable(detector_tests
        tests/detector_tests.cpp
        Xen/detector/preprocess.cpp
        Xen/detector/postprocess.cpp)
    target_include_directories(detector_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/Xen
        ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(detector_tests PRIVATE ${OpenCV_LIBS})
    add_test(NAME detector_tests COMMAND detector_tests)

    # 真实模型测试默认关闭，避免仓库绑定私有模型；配置时通过
    # -DXEN_TEST_MODEL=<onnx路径> 启用加载和单帧推理冒烟测试。
    set(XEN_TEST_MODEL "" CACHE FILEPATH "用于 Detector 集成测试的 ONNX 模型")
    if(XEN_TEST_MODEL)
        if(NOT EXISTS "${XEN_TEST_MODEL}")
            message(FATAL_ERROR "XEN_TEST_MODEL does not exist: ${XEN_TEST_MODEL}")
        endif()
        add_executable(detector_model_test
            tests/detector_model_test.cpp)
        target_link_libraries(detector_model_test PRIVATE detector)
        if(WIN32)
            # Windows 会优先从 System32 加载 DLL，不能只依赖 PATH；将与编译
            # 头文件/导入库同版本的 ORT 核心 DLL 放到测试程序旁，避免误载旧版。
            add_custom_command(TARGET detector_model_test POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${ONNXRUNTIME_ROOT}/lib/onnxruntime.dll"
                    "$<TARGET_FILE_DIR:detector_model_test>")
        endif()
        add_test(NAME detector_model_test
            COMMAND detector_model_test "${XEN_TEST_MODEL}")
    endif()
endif()
