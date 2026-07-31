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
if(NOT ONNXRUNTIME_ROOT OR NOT IS_DIRECTORY "${ONNXRUNTIME_ROOT}")
    message(FATAL_ERROR
        "ONNX Runtime SDK root is invalid. Set ONNXRUNTIME_ROOT to the extracted SDK root.")
endif()

# find_path/find_library 的结果会进入 CMakeCache。复用构建目录切换 ORT SDK 时，
# 必须先清除旧解析结果，否则新的 ONNXRUNTIME_ROOT 不会触发重新查找。
get_filename_component(XEN_ONNXRUNTIME_ROOT_REAL "${ONNXRUNTIME_ROOT}" REALPATH)
file(TO_CMAKE_PATH "${XEN_ONNXRUNTIME_ROOT_REAL}" XEN_ONNXRUNTIME_ROOT_REAL)
string(TOLOWER "${XEN_ONNXRUNTIME_ROOT_REAL}" XEN_ONNXRUNTIME_ROOT_KEY)
if(DEFINED XEN_CONFIGURED_ONNXRUNTIME_ROOT AND
   NOT XEN_CONFIGURED_ONNXRUNTIME_ROOT STREQUAL XEN_ONNXRUNTIME_ROOT_KEY)
    unset(ONNXRUNTIME_INCLUDE_DIR CACHE)
    unset(ONNXRUNTIME_LIB CACHE)
endif()
set(XEN_CONFIGURED_ONNXRUNTIME_ROOT "${XEN_ONNXRUNTIME_ROOT_KEY}"
    CACHE INTERNAL "ONNX Runtime root used by the current build tree" FORCE)

find_path(ONNXRUNTIME_INCLUDE_DIR onnxruntime_cxx_api.h
    HINTS "${XEN_ONNXRUNTIME_ROOT_REAL}"
    PATH_SUFFIXES include build/native/include
    NO_DEFAULT_PATH)
find_library(ONNXRUNTIME_LIB onnxruntime
    HINTS "${XEN_ONNXRUNTIME_ROOT_REAL}"
    PATH_SUFFIXES lib runtimes/win-x64/native
    NO_DEFAULT_PATH)
if(NOT ONNXRUNTIME_INCLUDE_DIR OR NOT ONNXRUNTIME_LIB)
    message(FATAL_ERROR
        "ONNX Runtime SDK not found. Set ONNXRUNTIME_ROOT to the extracted SDK root.")
endif()
get_filename_component(ONNXRUNTIME_INCLUDE_DIR_REAL
    "${ONNXRUNTIME_INCLUDE_DIR}" REALPATH)
get_filename_component(ONNXRUNTIME_LIB_REAL "${ONNXRUNTIME_LIB}" REALPATH)
file(RELATIVE_PATH ONNXRUNTIME_INCLUDE_RELATIVE
    "${XEN_ONNXRUNTIME_ROOT_REAL}" "${ONNXRUNTIME_INCLUDE_DIR_REAL}")
file(RELATIVE_PATH ONNXRUNTIME_LIB_RELATIVE
    "${XEN_ONNXRUNTIME_ROOT_REAL}" "${ONNXRUNTIME_LIB_REAL}")
if(IS_ABSOLUTE "${ONNXRUNTIME_INCLUDE_RELATIVE}" OR
   ONNXRUNTIME_INCLUDE_RELATIVE MATCHES "^\\.\\.($|/)" OR
   IS_ABSOLUTE "${ONNXRUNTIME_LIB_RELATIVE}" OR
   ONNXRUNTIME_LIB_RELATIVE MATCHES "^\\.\\.($|/)")
    message(FATAL_ERROR
        "Resolved ONNX Runtime files are outside ONNXRUNTIME_ROOT. Clear the build cache.")
endif()
get_filename_component(ONNXRUNTIME_RUNTIME_DIR "${ONNXRUNTIME_LIB}" DIRECTORY)
if(WIN32 AND NOT EXISTS "${ONNXRUNTIME_RUNTIME_DIR}/onnxruntime.dll")
    message(FATAL_ERROR
        "onnxruntime.dll not found beside the selected import library: ${ONNXRUNTIME_RUNTIME_DIR}")
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
set(XEN_TENSORRT_MAJOR "" CACHE STRING
    "TensorRT DLL major version required by the selected ONNX Runtime package")
set(XEN_CUDNN_ROOT "$ENV{CUDNN_ROOT}" CACHE PATH
    "cuDNN SDK root used to deploy runtime DLLs")
set(XEN_CUDA_ROOT "$ENV{CUDA_PATH}" CACHE PATH
    "CUDA Toolkit root used to deploy runtime DLLs")
set(XEN_DIRECTML_ROOT "$ENV{DIRECTML_ROOT}" CACHE PATH
    "DirectML redistributable root used to deploy DirectML.dll")

# TensorRT CUDA Graph 需要把每帧输入显式复制到固定设备地址，并在图重放后
# 把结果复制回固定 CPU 缓冲区。仅 CUDA/TensorRT ORT 构建链接 CUDA Runtime；
# DirectML 独立构建不查找也不部署任何 CUDA 文件。
if(EXISTS "${ONNXRUNTIME_RUNTIME_DIR}/onnxruntime_providers_tensorrt.dll")
    set(XEN_ONNXRUNTIME_HAS_TENSORRT TRUE)
else()
    set(XEN_ONNXRUNTIME_HAS_TENSORRT FALSE)
endif()

if(EXISTS "${ONNXRUNTIME_RUNTIME_DIR}/onnxruntime_providers_cuda.dll" OR
   XEN_ONNXRUNTIME_HAS_TENSORRT)
    set(XEN_ONNXRUNTIME_HAS_CUDA TRUE)
    if(XEN_CUDA_ROOT)
        set(CUDAToolkit_ROOT "${XEN_CUDA_ROOT}")
    endif()
    find_package(CUDAToolkit REQUIRED)
else()
    set(XEN_ONNXRUNTIME_HAS_CUDA FALSE)
endif()

# TensorRT SDK 的 DLL major 必须与 ORT TensorRT Provider 的导入依赖一致。
# 不能根据本机安装版本猜测，否则 Provider 会在 Session 创建时才因 ABI 不匹配失败。
if(XEN_TENSORRT_ROOT)
    if(NOT IS_DIRECTORY "${XEN_TENSORRT_ROOT}")
        message(FATAL_ERROR "TensorRT SDK root does not exist: ${XEN_TENSORRT_ROOT}")
    endif()
    if(NOT XEN_TENSORRT_MAJOR MATCHES "^[0-9]+$")
        message(FATAL_ERROR
            "Set XEN_TENSORRT_MAJOR to the DLL major in XEN_TENSORRT_ROOT (for example 10 or 11).")
    endif()

    set(XEN_TENSORRT_CORE_DLL
        "${XEN_TENSORRT_ROOT}/bin/nvinfer_${XEN_TENSORRT_MAJOR}.dll")
    set(XEN_TENSORRT_PARSER_DLL
        "${XEN_TENSORRT_ROOT}/bin/nvonnxparser_${XEN_TENSORRT_MAJOR}.dll")
    foreach(required_file IN ITEMS
            "${XEN_TENSORRT_CORE_DLL}"
            "${XEN_TENSORRT_PARSER_DLL}")
        if(NOT EXISTS "${required_file}")
            message(FATAL_ERROR "Required TensorRT runtime DLL not found: ${required_file}")
        endif()
    endforeach()

    if(XEN_ONNXRUNTIME_HAS_TENSORRT AND WIN32 AND MSVC)
        execute_process(
            COMMAND "${CMAKE_LINKER}" /dump /dependents
                "${ONNXRUNTIME_RUNTIME_DIR}/onnxruntime_providers_tensorrt.dll"
            RESULT_VARIABLE XEN_TENSORRT_DEPENDENCY_RESULT
            OUTPUT_VARIABLE XEN_TENSORRT_DEPENDENCIES
            ERROR_VARIABLE XEN_TENSORRT_DEPENDENCY_ERROR)
        if(NOT XEN_TENSORRT_DEPENDENCY_RESULT EQUAL 0)
            message(FATAL_ERROR
                "Failed to inspect ONNX Runtime TensorRT Provider dependencies: "
                "${XEN_TENSORRT_DEPENDENCY_ERROR}")
        endif()
        string(TOLOWER "${XEN_TENSORRT_DEPENDENCIES}"
            XEN_TENSORRT_DEPENDENCIES_LOWER)
        string(REGEX MATCH "nvinfer_([0-9]+)\\.dll"
            XEN_TENSORRT_PROVIDER_CORE "${XEN_TENSORRT_DEPENDENCIES_LOWER}")
        set(XEN_TENSORRT_PROVIDER_MAJOR "${CMAKE_MATCH_1}")
        string(REGEX MATCH "nvonnxparser_([0-9]+)\\.dll"
            XEN_TENSORRT_PROVIDER_PARSER "${XEN_TENSORRT_DEPENDENCIES_LOWER}")
        set(XEN_TENSORRT_PROVIDER_PARSER_MAJOR "${CMAKE_MATCH_1}")
        if(NOT XEN_TENSORRT_PROVIDER_CORE OR
           NOT XEN_TENSORRT_PROVIDER_PARSER)
            message(FATAL_ERROR
                "Cannot determine TensorRT DLL major required by the selected ONNX Runtime package.")
        endif()
        if(NOT XEN_TENSORRT_PROVIDER_MAJOR STREQUAL XEN_TENSORRT_MAJOR OR
           NOT XEN_TENSORRT_PROVIDER_PARSER_MAJOR STREQUAL XEN_TENSORRT_MAJOR)
            message(FATAL_ERROR
                "TensorRT SDK major ${XEN_TENSORRT_MAJOR} is incompatible with the selected "
                "ONNX Runtime Provider, which requires nvinfer_${XEN_TENSORRT_PROVIDER_MAJOR}.dll "
                "and nvonnxparser_${XEN_TENSORRT_PROVIDER_PARSER_MAJOR}.dll.")
        endif()
    elseif(NOT XEN_ONNXRUNTIME_HAS_TENSORRT)
        message(WARNING
            "XEN_TENSORRT_ROOT is set, but the selected ONNX Runtime package has no TensorRT Provider.")
    endif()
elseif(XEN_TENSORRT_MAJOR)
    message(FATAL_ERROR "XEN_TENSORRT_MAJOR is set but XEN_TENSORRT_ROOT is empty.")
endif()

# 使用 copy_if_different 避免增量构建反复重写数百 MB 的 GPU 运行库。
function(xen_copy_runtime_files target)
    foreach(runtime_file IN LISTS ARGN)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${runtime_file}" "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
    endforeach()
endfunction()

# 可执行目标按实际能力选择 OPENCV / ORT。Windows 系统 DLL、显卡驱动 DLL
# 和系统提供的 MSVC 运行库不复制。
function(xen_deploy_runtime_dlls target)
    set(options OPENCV ORT)
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

    if(DEPLOY_ORT)
        set(ORT_RUNTIME_DLLS)
        foreach(runtime_name
                onnxruntime.dll
                onnxruntime_providers_shared.dll
                onnxruntime_providers_cuda.dll
                onnxruntime_providers_tensorrt.dll)
            set(runtime_file "${ONNXRUNTIME_RUNTIME_DIR}/${runtime_name}")
            if(EXISTS "${runtime_file}")
                list(APPEND ORT_RUNTIME_DLLS "${runtime_file}")
            endif()
        endforeach()
        xen_copy_runtime_files(${target} ${ORT_RUNTIME_DLLS})

        # 只有当前 ORT 包确实包含 CUDA/TensorRT Provider 时才部署 NVIDIA 依赖。
        # DirectML 独立构建不会因为机器上设置了 CUDA 环境变量而夹带无关 DLL。
        if(EXISTS "${ONNXRUNTIME_RUNTIME_DIR}/onnxruntime_providers_cuda.dll" OR
           EXISTS "${ONNXRUNTIME_RUNTIME_DIR}/onnxruntime_providers_tensorrt.dll")
            set(XEN_ORT_USES_NVIDIA TRUE)
        else()
            set(XEN_ORT_USES_NVIDIA FALSE)
        endif()

        if(XEN_ORT_USES_NVIDIA AND XEN_TENSORRT_ROOT)
            set(TENSORRT_RUNTIME_DLLS
                "${XEN_TENSORRT_CORE_DLL}"
                "${XEN_TENSORRT_PARSER_DLL}")
            # Engine Cache 不能跨 GPU 架构复用；部署 SDK 提供的全部 Builder Resource，
            # 使同一构建目录可在不同 NVIDIA 架构上首次生成各自的 Engine。
            file(GLOB TENSORRT_BUILDER_RUNTIME_DLLS CONFIGURE_DEPENDS
                "${XEN_TENSORRT_ROOT}/bin/nvinfer_builder_resource_*_${XEN_TENSORRT_MAJOR}.dll")
            list(APPEND TENSORRT_RUNTIME_DLLS ${TENSORRT_BUILDER_RUNTIME_DLLS})
            xen_copy_runtime_files(${target} ${TENSORRT_RUNTIME_DLLS})
        elseif(XEN_ORT_USES_NVIDIA)
            message(STATUS "TensorRT runtime deployment skipped: XEN_TENSORRT_ROOT is empty")
        endif()

        if(XEN_ORT_USES_NVIDIA AND
           XEN_CUDNN_ROOT AND IS_DIRECTORY "${XEN_CUDNN_ROOT}")
            file(GLOB_RECURSE CUDNN_RUNTIME_DLLS CONFIGURE_DEPENDS
                "${XEN_CUDNN_ROOT}/*.dll")
            xen_copy_runtime_files(${target} ${CUDNN_RUNTIME_DLLS})
        elseif(XEN_ORT_USES_NVIDIA)
            message(STATUS "cuDNN runtime deployment skipped: XEN_CUDNN_ROOT is empty")
        endif()

        if(XEN_ORT_USES_NVIDIA AND
           XEN_CUDA_ROOT AND IS_DIRECTORY "${XEN_CUDA_ROOT}")
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
            # FindCUDAToolkit 在 Windows 上的 CUDA::cudart TARGET_FILE 指向导入库
            # cudart.lib，不是运行时 DLL。必须从 Toolkit bin 目录解析实际 DLL。
            file(GLOB CUDA_CUDART_RUNTIME_DLLS CONFIGURE_DEPENDS
                "${XEN_CUDA_ROOT}/bin/x64/cudart64_*.dll"
                "${XEN_CUDA_ROOT}/bin/cudart64_*.dll")
            list(REMOVE_DUPLICATES CUDA_CUDART_RUNTIME_DLLS)
            list(LENGTH CUDA_CUDART_RUNTIME_DLLS CUDA_CUDART_RUNTIME_COUNT)
            if(NOT CUDA_CUDART_RUNTIME_COUNT EQUAL 1)
                message(FATAL_ERROR
                    "Expected exactly one cudart64_*.dll under XEN_CUDA_ROOT, found "
                    "${CUDA_CUDART_RUNTIME_COUNT}: ${XEN_CUDA_ROOT}")
            endif()
            # 清理旧规则曾错误复制到输出目录的导入库，避免增量构建留下伪运行库。
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E rm -f
                    "$<TARGET_FILE_DIR:${target}>/cudart.lib"
                VERBATIM)
            xen_copy_runtime_files(${target} ${CUDA_CUDART_RUNTIME_DLLS})
        elseif(XEN_ORT_USES_NVIDIA)
            message(STATUS "CUDA runtime deployment skipped: XEN_CUDA_ROOT is empty")
        endif()

        # DML EP 编译进 onnxruntime.dll，但 DirectML 可再发行 DLL 必须位于进程
        # 搜索路径。只在当前 ORT 头文件含 DML 工厂时部署 x64 正式版。
        if(EXISTS "${ONNXRUNTIME_INCLUDE_DIR}/dml_provider_factory.h")
            set(DIRECTML_RUNTIME_DLL "${XEN_DIRECTML_ROOT}/bin/x64-win/DirectML.dll")
            if(NOT EXISTS "${DIRECTML_RUNTIME_DLL}")
                message(FATAL_ERROR
                    "DirectML.dll not found. Set DIRECTML_ROOT to Microsoft.AI.DirectML root.")
            endif()
            xen_copy_runtime_files(${target} "${DIRECTML_RUNTIME_DLL}")
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

# SimpleIni 当前可验证的最新稳定 tag 为 4.19。仅使用其单头文件实现，禁止
# FetchContent 把上游示例或测试加入 Xen 构建图。
FetchContent_Declare(simpleini
    GIT_REPOSITORY https://github.com/brofield/simpleini.git
    GIT_TAG        4.19
    SOURCE_SUBDIR  _xen_no_cmake)
FetchContent_MakeAvailable(simpleini)

# Dear ImGui 上游不提供正式 CMake target，由 Xen 明确列出 Win32/D3D11 后端
# 与 std::string 适配源码，避免隐式编译无关示例和平台后端。
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.9)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${imgui_SOURCE_DIR}/misc/cpp)
target_link_libraries(imgui PRIVATE d3d11 dxgi)

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
if(XEN_ONNXRUNTIME_HAS_CUDA)
    target_link_libraries(detector PRIVATE CUDA::cudart)
    target_compile_definitions(detector PRIVATE XEN_HAS_CUDA_RUNTIME=1)
else()
    target_compile_definitions(detector PRIVATE XEN_HAS_CUDA_RUNTIME=0)
endif()

# ─── 实时闭环模块 ───────────────────────────────────────

add_library(capture STATIC
    Xen/capture/capture.cpp
    Xen/capture/duplication.cpp
    Xen/capture/udp.cpp)
target_include_directories(capture PUBLIC
    ${PROJECT_SOURCE_DIR}/Xen
    ${OpenCV_INCLUDE_DIRS})
target_link_libraries(capture PUBLIC ${OpenCV_LIBS} log
    PRIVATE d3d11 dxgi ws2_32)

add_library(aim STATIC
    Xen/aim/aim.cpp)
target_include_directories(aim PUBLIC
    ${PROJECT_SOURCE_DIR}/Xen
    ${OpenCV_INCLUDE_DIRS})
target_link_libraries(aim PUBLIC log)

add_library(mouse STATIC
    Xen/mouse/mouse.cpp)
target_include_directories(mouse PUBLIC ${PROJECT_SOURCE_DIR}/Xen)
target_link_libraries(mouse PUBLIC log PRIVATE user32)

add_library(keyboard STATIC
    Xen/keyboard/keyboard.cpp)
target_include_directories(keyboard PUBLIC ${PROJECT_SOURCE_DIR}/Xen)
target_link_libraries(keyboard PUBLIC log PRIVATE user32)

add_library(config STATIC
    Xen/config/config.cpp)
target_include_directories(config
    PUBLIC
        ${PROJECT_SOURCE_DIR}/Xen
        ${OpenCV_INCLUDE_DIRS}
    PRIVATE
        ${simpleini_SOURCE_DIR})

add_library(runtime STATIC
    Xen/runtime/runtime.cpp
    Xen/runtime/runtime_queue.cpp)
target_include_directories(runtime PUBLIC ${PROJECT_SOURCE_DIR}/Xen)
target_link_libraries(runtime
    PUBLIC detector capture aim mouse keyboard config log
    PRIVATE Threads::Threads)

add_library(overlay STATIC
    Xen/overlay/overlay.cpp)
target_include_directories(overlay PUBLIC ${PROJECT_SOURCE_DIR}/Xen)
target_link_libraries(overlay
    PUBLIC runtime config
    PRIVATE imgui d3d11 dxgi dwmapi)

add_executable(xen_app WIN32
    Xen/app/main.cpp
    Xen/app/xen.rc)
set_target_properties(xen_app PROPERTIES OUTPUT_NAME Xen)
target_link_libraries(xen_app PRIVATE overlay runtime config log)
if(WIN32)
    xen_deploy_runtime_dlls(xen_app OPENCV ORT)
endif()

# ─── 测试 ───────────────────────────────────────────────

include(CTest)
if(BUILD_TESTING)
    add_executable(log_tests
        tests/log_tests.cpp)
    target_link_libraries(log_tests PRIVATE log)
    add_test(NAME log_tests COMMAND log_tests)

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

    add_executable(aim_tests
        tests/aim_tests.cpp)
    target_link_libraries(aim_tests PRIVATE aim log)
    add_test(NAME aim_tests COMMAND aim_tests)

    add_executable(runtime_tests
        tests/runtime_tests.cpp
        Xen/runtime/runtime_queue.cpp)
    target_include_directories(runtime_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/Xen
        ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(runtime_tests PRIVATE ${OpenCV_LIBS} Threads::Threads)
    xen_deploy_runtime_dlls(runtime_tests OPENCV)
    add_test(NAME runtime_tests COMMAND runtime_tests)

    add_executable(config_tests
        tests/config_tests.cpp)
    target_link_libraries(config_tests PRIVATE config)
    add_test(NAME config_tests COMMAND config_tests)

    add_executable(io_tests
        tests/io_tests.cpp)
    target_link_libraries(io_tests PRIVATE
        capture mouse keyboard log ws2_32)
    xen_deploy_runtime_dlls(io_tests OPENCV)
    add_test(NAME io_tests COMMAND io_tests)

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
            xen_deploy_runtime_dlls(detector_model_test OPENCV ORT)
        endif()
        add_test(NAME detector_model_test
            COMMAND detector_model_test "${XEN_TEST_MODEL}")
    endif()
endif()
