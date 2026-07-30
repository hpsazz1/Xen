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

# 可选 GPU 运行库根目录。它们不参与编译和链接，只用于把 ORT Provider 在运行时
# 直接或动态加载的 DLL 部署到可执行文件旁，确保测试程序无需手工配置 PATH。
set(XEN_TENSORRT_ROOT "$ENV{TENSORRT_ROOT}" CACHE PATH
    "TensorRT SDK root used to deploy runtime DLLs")
set(XEN_CUDNN_ROOT "$ENV{CUDNN_ROOT}" CACHE PATH
    "cuDNN SDK root used to deploy runtime DLLs")
set(XEN_CUDA_ROOT "$ENV{CUDA_PATH}" CACHE PATH
    "CUDA Toolkit root used to deploy runtime DLLs")

# 使用 copy_if_different 避免增量构建反复重写数百 MB 的 GPU 运行库。
function(xen_copy_runtime_files target)
    foreach(runtime_file IN LISTS ARGN)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${runtime_file}" "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
    endforeach()
endfunction()

# 可执行目标按实际能力选择 OPENCV / ORT_GPU。Windows 系统 DLL、显卡驱动 DLL
# 和系统提供的 MSVC 运行库不复制。
function(xen_deploy_runtime_dlls target)
    set(options OPENCV ORT_GPU)
    cmake_parse_arguments(DEPLOY "${options}" "" "" ${ARGN})

    if(DEPLOY_OPENCV)
        get_filename_component(OPENCV_PLATFORM_DIRECTORY "${OpenCV_DIR}" DIRECTORY)
        set(OPENCV_BIN_DIRECTORY "${OPENCV_PLATFORM_DIRECTORY}/bin")
        string(REPLACE "." "" OPENCV_VERSION_SUFFIX "${OpenCV_VERSION}")
        set(OPENCV_RELEASE_DLL
            "${OPENCV_BIN_DIRECTORY}/opencv_world${OPENCV_VERSION_SUFFIX}.dll")
        set(OPENCV_DEBUG_DLL
            "${OPENCV_BIN_DIRECTORY}/opencv_world${OPENCV_VERSION_SUFFIX}d.dll")
        if(EXISTS "${OPENCV_RELEASE_DLL}")
            if(EXISTS "${OPENCV_DEBUG_DLL}")
                set(OPENCV_WORLD_DLL
                    "$<IF:$<CONFIG:Debug>,${OPENCV_DEBUG_DLL},${OPENCV_RELEASE_DLL}>")
            else()
                set(OPENCV_WORLD_DLL "${OPENCV_RELEASE_DLL}")
            endif()
            xen_copy_runtime_files(${target} "${OPENCV_WORLD_DLL}")
        else()
            message(WARNING "OpenCV runtime DLL not found: ${OPENCV_RELEASE_DLL}")
        endif()

        file(GLOB OPENCV_VIDEO_RUNTIME_DLLS CONFIGURE_DEPENDS
            "${OPENCV_BIN_DIRECTORY}/opencv_videoio_ffmpeg*_64.dll")
        xen_copy_runtime_files(${target} ${OPENCV_VIDEO_RUNTIME_DLLS})
    endif()

    if(DEPLOY_ORT_GPU)
        set(ORT_RUNTIME_DLLS)
        foreach(runtime_name
                onnxruntime.dll
                onnxruntime_providers_shared.dll
                onnxruntime_providers_cuda.dll
                onnxruntime_providers_tensorrt.dll)
            set(runtime_file "${ONNXRUNTIME_ROOT}/lib/${runtime_name}")
            if(EXISTS "${runtime_file}")
                list(APPEND ORT_RUNTIME_DLLS "${runtime_file}")
            endif()
        endforeach()
        xen_copy_runtime_files(${target} ${ORT_RUNTIME_DLLS})

        if(XEN_TENSORRT_ROOT AND IS_DIRECTORY "${XEN_TENSORRT_ROOT}")
            set(TENSORRT_RUNTIME_DLLS)
            foreach(runtime_name
                    nvinfer_10.dll
                    nvonnxparser_10.dll)
                set(runtime_file "${XEN_TENSORRT_ROOT}/bin/${runtime_name}")
                if(EXISTS "${runtime_file}")
                    list(APPEND TENSORRT_RUNTIME_DLLS "${runtime_file}")
                endif()
            endforeach()
            # Engine Cache 不能跨 GPU 架构复用；部署 SDK 提供的全部 Builder Resource，
            # 使同一构建目录可在不同 NVIDIA 架构上首次生成各自的 Engine。
            file(GLOB TENSORRT_BUILDER_RUNTIME_DLLS CONFIGURE_DEPENDS
                "${XEN_TENSORRT_ROOT}/bin/nvinfer_builder_resource_*.dll")
            list(APPEND TENSORRT_RUNTIME_DLLS ${TENSORRT_BUILDER_RUNTIME_DLLS})
            xen_copy_runtime_files(${target} ${TENSORRT_RUNTIME_DLLS})
        else()
            message(STATUS "TensorRT runtime deployment skipped: XEN_TENSORRT_ROOT is empty")
        endif()

        if(XEN_CUDNN_ROOT AND IS_DIRECTORY "${XEN_CUDNN_ROOT}")
            file(GLOB_RECURSE CUDNN_RUNTIME_DLLS CONFIGURE_DEPENDS
                "${XEN_CUDNN_ROOT}/*.dll")
            xen_copy_runtime_files(${target} ${CUDNN_RUNTIME_DLLS})
        else()
            message(STATUS "cuDNN runtime deployment skipped: XEN_CUDNN_ROOT is empty")
        endif()

        if(XEN_CUDA_ROOT AND IS_DIRECTORY "${XEN_CUDA_ROOT}")
            set(CUDA_RUNTIME_DLLS)
            foreach(runtime_name
                    cublas64_13.dll
                    cublasLt64_13.dll
                    cufft64_12.dll)
                set(runtime_file "${XEN_CUDA_ROOT}/bin/x64/${runtime_name}")
                if(EXISTS "${runtime_file}")
                    list(APPEND CUDA_RUNTIME_DLLS "${runtime_file}")
                endif()
            endforeach()
            xen_copy_runtime_files(${target} ${CUDA_RUNTIME_DLLS})
        else()
            message(STATUS "CUDA runtime deployment skipped: XEN_CUDA_ROOT is empty")
        endif()
    endif()
endfunction()

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
    xen_deploy_runtime_dlls(detector_tests OPENCV)
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
            xen_deploy_runtime_dlls(detector_model_test OPENCV ORT_GPU)
        endif()
        add_test(NAME detector_model_test
            COMMAND detector_model_test "${XEN_TEST_MODEL}")
    endif()
endif()
