cmake_minimum_required(VERSION 3.18)
project(xen VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─── 依赖 ───────────────────────────────────────────────

# ONNX Runtime
if(NOT ONNXRUNTIME_ROOT)
    set(ONNXRUNTIME_ROOT "$ENV{ONNXRUNTIME_ROOT}")
endif()
find_path(ONNXRUNTIME_INCLUDE_DIR onnxruntime_cxx_api.h
    PATHS ${ONNXRUNTIME_ROOT}/include)
find_library(ONNXRUNTIME_LIB onnxruntime
    PATHS ${ONNXRUNTIME_ROOT}/lib)
message(STATUS "ONNX Runtime: ${ONNXRUNTIME_LIB}")

# OpenCV
find_package(OpenCV REQUIRED)
message(STATUS "OpenCV: ${OpenCV_VERSION}")

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
    log/log.cpp)

target_include_directories(log
    PUBLIC  .
    PRIVATE log)

target_link_libraries(log
    PUBLIC  spdlog::spdlog)

option(LOG_ENABLE_DEBUG "Enable debug/trace logging" OFF)
if(LOG_ENABLE_DEBUG)
    target_compile_definitions(log PUBLIC LOG_ENABLE_DEBUG)
endif()

# ─── Detector 模块 ──────────────────────────────────────

add_library(detector STATIC
    detector/detector.cpp
    detector/session.cpp
    detector/preprocess.cpp
    detector/postprocess.cpp)

target_include_directories(detector
    PUBLIC  .
    PRIVATE detector)

target_link_libraries(detector
    PUBLIC  ${ONNXRUNTIME_LIB} ${OpenCV_LIBS} ${CMAKE_DL_LIBS} log
    PRIVATE Threads::Threads)
