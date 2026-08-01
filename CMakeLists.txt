cmake_minimum_required(VERSION 3.18)
project(xen VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 项目源码和注释统一使用 UTF-8。MSVC 默认跟随系统代码页，中文注释可能被
# 错误解析并进一步破坏相邻的 C++ 词法单元，因此必须显式启用 UTF-8。
if(MSVC)
    add_compile_options(
        "$<$<COMPILE_LANGUAGE:CXX>:/utf-8>"
        "$<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=/utf-8>")
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
set(XEN_NDI_SDK_ROOT "$ENV{NDI_SDK_DIR}" CACHE PATH
    "NDI 6 SDK root used by the optional NDI capture backend")

# NDI 是可选后端。只有 SDK 的头文件、x64 导入库和运行库同时存在时才
# 编译真实实现；缺少 SDK 时仍保留明确 UNSUPPORTED 的构建边界。
set(XEN_HAS_NDI FALSE)
if(XEN_NDI_SDK_ROOT AND IS_DIRECTORY "${XEN_NDI_SDK_ROOT}")
    get_filename_component(XEN_NDI_SDK_ROOT_REAL
        "${XEN_NDI_SDK_ROOT}" REALPATH)
    file(TO_CMAKE_PATH "${XEN_NDI_SDK_ROOT_REAL}" XEN_NDI_SDK_ROOT_REAL)
    string(TOLOWER "${XEN_NDI_SDK_ROOT_REAL}" XEN_NDI_SDK_ROOT_KEY)
    if(DEFINED XEN_CONFIGURED_NDI_SDK_ROOT AND
       NOT XEN_CONFIGURED_NDI_SDK_ROOT STREQUAL XEN_NDI_SDK_ROOT_KEY)
        unset(XEN_NDI_INCLUDE_DIR CACHE)
        unset(XEN_NDI_LIB CACHE)
    endif()
    set(XEN_CONFIGURED_NDI_SDK_ROOT "${XEN_NDI_SDK_ROOT_KEY}"
        CACHE INTERNAL "NDI SDK root used by the current build tree" FORCE)
    find_path(XEN_NDI_INCLUDE_DIR Processing.NDI.Lib.h
        HINTS "${XEN_NDI_SDK_ROOT_REAL}"
        PATH_SUFFIXES Include include
        NO_DEFAULT_PATH)
    find_library(XEN_NDI_LIB Processing.NDI.Lib.x64
        HINTS "${XEN_NDI_SDK_ROOT_REAL}"
        PATH_SUFFIXES Lib/x64 lib/x64
        NO_DEFAULT_PATH)
    set(XEN_NDI_RUNTIME_DLL
        "${XEN_NDI_SDK_ROOT_REAL}/Bin/x64/Processing.NDI.Lib.x64.dll")
    set(XEN_NDI_LICENSE_FILE
        "${XEN_NDI_SDK_ROOT_REAL}/Bin/x64/Processing.NDI.Lib.Licenses.txt")
    if(XEN_NDI_INCLUDE_DIR AND XEN_NDI_LIB)
        get_filename_component(XEN_NDI_INCLUDE_DIR_REAL
            "${XEN_NDI_INCLUDE_DIR}" REALPATH)
        get_filename_component(XEN_NDI_LIB_REAL "${XEN_NDI_LIB}" REALPATH)
        file(RELATIVE_PATH XEN_NDI_INCLUDE_RELATIVE
            "${XEN_NDI_SDK_ROOT_REAL}" "${XEN_NDI_INCLUDE_DIR_REAL}")
        file(RELATIVE_PATH XEN_NDI_LIB_RELATIVE
            "${XEN_NDI_SDK_ROOT_REAL}" "${XEN_NDI_LIB_REAL}")
        if(IS_ABSOLUTE "${XEN_NDI_INCLUDE_RELATIVE}" OR
           XEN_NDI_INCLUDE_RELATIVE MATCHES "^\\.\\.($|/)" OR
           IS_ABSOLUTE "${XEN_NDI_LIB_RELATIVE}" OR
           XEN_NDI_LIB_RELATIVE MATCHES "^\\.\\.($|/)")
            unset(XEN_NDI_INCLUDE_DIR CACHE)
            unset(XEN_NDI_LIB CACHE)
        endif()
    endif()
    if(XEN_NDI_INCLUDE_DIR AND XEN_NDI_LIB AND
       EXISTS "${XEN_NDI_RUNTIME_DLL}" AND
       EXISTS "${XEN_NDI_LICENSE_FILE}")
        set(XEN_HAS_NDI TRUE)
        message(STATUS "NDI SDK: ${XEN_NDI_SDK_ROOT}")
    else()
        message(WARNING
            "NDI SDK root is incomplete; NDI backend will be UNSUPPORTED: ${XEN_NDI_SDK_ROOT}")
    endif()
else()
    message(STATUS "NDI SDK not configured; NDI backend will be UNSUPPORTED")
endif()

# TensorRT CUDA Graph 需要把每帧输入显式复制到固定设备地址，并在图重放后
# 把结果复制回固定 CPU 缓冲区。仅 CUDA/TensorRT ORT 构建链接 CUDA Runtime；
# DirectML 独立构建不查找也不部署任何 CUDA 文件。
if(EXISTS "${ONNXRUNTIME_RUNTIME_DIR}/onnxruntime_providers_tensorrt.dll")
    set(XEN_ONNXRUNTIME_HAS_TENSORRT TRUE)
else()
    set(XEN_ONNXRUNTIME_HAS_TENSORRT FALSE)
endif()

# DirectML SDK 发行包通过 dml_provider_factory.h 暴露设备张量与自定义 D3D12
# command queue API。只按当前 ONNXRUNTIME_ROOT 判定，避免 CUDA 构建误用系统头。
if(WIN32 AND EXISTS "${ONNXRUNTIME_INCLUDE_DIR}/dml_provider_factory.h")
    set(XEN_ONNXRUNTIME_HAS_DIRECTML TRUE)
else()
    set(XEN_ONNXRUNTIME_HAS_DIRECTML FALSE)
endif()

# Intel 官方 NuGet 把 OpenVINO EP、OpenVINO Runtime 和设备 plugin 放在
# onnxruntime.lib 同目录。Provider 存在却缺少工厂头或核心运行库属于残缺 SDK，
# 必须在配置期失败，不能等到 Session 创建时才由 DLL 搜索暴露问题。
if(WIN32 AND
   EXISTS "${ONNXRUNTIME_RUNTIME_DIR}/onnxruntime_providers_openvino.dll")
    if(NOT EXISTS "${ONNXRUNTIME_INCLUDE_DIR}/openvino_provider_factory.h" OR
       NOT EXISTS "${ONNXRUNTIME_RUNTIME_DIR}/openvino.dll" OR
       NOT EXISTS "${ONNXRUNTIME_RUNTIME_DIR}/openvino_onnx_frontend.dll" OR
       NOT EXISTS "${ONNXRUNTIME_RUNTIME_DIR}/tbb12.dll")
        message(FATAL_ERROR
            "Selected ONNX Runtime OpenVINO package is incomplete: ${ONNXRUNTIME_ROOT}")
    endif()
    set(XEN_ONNXRUNTIME_HAS_OPENVINO TRUE)
else()
    set(XEN_ONNXRUNTIME_HAS_OPENVINO FALSE)
endif()

if(EXISTS "${ONNXRUNTIME_RUNTIME_DIR}/onnxruntime_providers_cuda.dll" OR
   XEN_ONNXRUNTIME_HAS_TENSORRT)
    set(XEN_ONNXRUNTIME_HAS_CUDA TRUE)
    if(XEN_CUDA_ROOT)
        set(CUDAToolkit_ROOT "${XEN_CUDA_ROOT}")
    endif()
    find_package(CUDAToolkit REQUIRED)
    # 仅 CUDA/TensorRT ORT 构建启用 CUDA 编译器。DirectML 独立构建不探测
    # nvcc，也不会把 CUDA kernel 或运行库带入产物。
    enable_language(CUDA)
    set(XEN_CUDA_ARCHITECTURES "75;80;86;89;90;100;120" CACHE STRING
        "CUDA architectures for Xen detector preprocessing kernels")
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

# Visual Studio 的同一配置共享一个可执行输出目录。这里只登记当前配置授权的
# 运行文件，全部目标声明完毕后再由单一构建目标加锁完成清理、复制和哈希报告；
# 这样即使可执行目标没有重新链接，SDK 清空或切换也不会跳过部署收敛。
function(xen_copy_runtime_files target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR
            "Runtime deployment target does not exist: ${target}")
    endif()
    foreach(runtime_file IN LISTS ARGN)
        set_property(GLOBAL APPEND PROPERTY
            XEN_RUNTIME_DEPLOY_FILES "${runtime_file}")
    endforeach()
endfunction()

# 可执行目标按实际能力选择 OPENCV / ORT。Windows 系统 DLL、显卡驱动 DLL
# 和系统提供的 MSVC 运行库不复制。
function(xen_deploy_runtime_dlls target)
    set(options OPENCV ORT NDI)
    cmake_parse_arguments(DEPLOY "${options}" "" "" ${ARGN})
    set_property(GLOBAL APPEND PROPERTY
        XEN_RUNTIME_DEPLOY_TARGETS "${target}")

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
                onnxruntime_providers_tensorrt.dll
                onnxruntime_providers_openvino.dll)
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

        # OpenVINO Provider 会动态加载 ONNX frontend 和 device_type 对应
        # plugin，它们不一定出现在静态导入表。Xen 配置允许 CPU/GPU/NPU，
        # 因此 ONNX 前端、三种正式 plugin、核心 Runtime 与 TBB 都必须随
        # 可执行文件部署；OpenCL.dll 由显卡驱动提供。
        if(XEN_ONNXRUNTIME_HAS_OPENVINO)
            set(OPENVINO_RUNTIME_DLLS)
            foreach(runtime_name
                    openvino.dll
                    openvino_onnx_frontend.dll
                    openvino_intel_cpu_plugin.dll
                    openvino_intel_gpu_plugin.dll
                    openvino_intel_npu_plugin.dll
                    tbb12.dll)
                set(runtime_file
                    "${ONNXRUNTIME_RUNTIME_DIR}/${runtime_name}")
                if(NOT EXISTS "${runtime_file}")
                    message(FATAL_ERROR
                        "Required OpenVINO runtime DLL not found: ${runtime_file}")
                endif()
                list(APPEND OPENVINO_RUNTIME_DLLS "${runtime_file}")
            endforeach()
            xen_copy_runtime_files(${target} ${OPENVINO_RUNTIME_DLLS})
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

    if(DEPLOY_NDI AND XEN_HAS_NDI)
        xen_copy_runtime_files(${target}
            "${XEN_NDI_RUNTIME_DLL}"
            "${XEN_NDI_LICENSE_FILE}")
    elseif(DEPLOY_NDI)
        message(STATUS
            "NDI runtime deployment skipped: XEN_NDI_SDK_ROOT is unavailable")
    endif()
endfunction()

function(xen_finalize_runtime_deployment)
    get_property(runtime_targets GLOBAL PROPERTY XEN_RUNTIME_DEPLOY_TARGETS)
    get_property(runtime_files GLOBAL PROPERTY XEN_RUNTIME_DEPLOY_FILES)
    if(NOT runtime_targets)
        return()
    endif()
    if(NOT runtime_files)
        message(FATAL_ERROR
            "Runtime deployment targets exist, but no files were authorized.")
    endif()

    list(REMOVE_DUPLICATES runtime_targets)
    list(REMOVE_DUPLICATES runtime_files)
    set(manifest_content "set(XEN_RUNTIME_SOURCES\n")
    foreach(runtime_file IN LISTS runtime_files)
        string(APPEND manifest_content "    [==[${runtime_file}]==]\n")
    endforeach()
    string(APPEND manifest_content ")\n")

    set(runtime_manifest
        "${CMAKE_BINARY_DIR}/xen-runtime-authorized-$<CONFIG>.cmake")
    file(GENERATE OUTPUT "${runtime_manifest}" CONTENT "${manifest_content}")

    # 项目没有重写 RUNTIME_OUTPUT_DIRECTORY；Visual Studio 多配置生成器的
    # 共享目录固定为 <build>/<config>，与 scripts/build.ps1 的验收路径一致。
    add_custom_target(xen_runtime_deployment
        COMMAND ${CMAKE_COMMAND}
            "-DXEN_RUNTIME_MANIFEST=${runtime_manifest}"
            "-DXEN_RUNTIME_OUTPUT=${CMAKE_BINARY_DIR}/$<CONFIG>"
            "-DXEN_RUNTIME_LOCK=${CMAKE_BINARY_DIR}/xen-runtime-deploy.lock"
            "-DXEN_RUNTIME_CONFIGURATION=$<CONFIG>"
            -P "${PROJECT_SOURCE_DIR}/scripts/deploy_runtime_file.cmake"
        COMMENT "收敛当前配置授权的第三方运行文件并生成来源哈希报告"
        VERBATIM)
    foreach(runtime_target IN LISTS runtime_targets)
        add_dependencies(${runtime_target} xen_runtime_deployment)
    endforeach()
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
    PRIVATE spdlog::spdlog)

option(LOG_ENABLE_DEBUG "Enable debug/trace logging" OFF)
if(LOG_ENABLE_DEBUG)
    target_compile_definitions(log PUBLIC LOG_ENABLE_DEBUG)
endif()

# ─── Crash 模块 ─────────────────────────────────────────

add_library(crash STATIC
    Xen/crash/crash.cpp)
target_include_directories(crash PUBLIC ${PROJECT_SOURCE_DIR}/Xen)
target_link_libraries(crash PUBLIC log)

# ─── Detector 模块 ──────────────────────────────────────

set(XEN_DETECTOR_SOURCES
    Xen/detector/detector.cpp
    Xen/detector/session.cpp
    Xen/detector/preprocess.cpp
    Xen/detector/postprocess.cpp)
if(XEN_ONNXRUNTIME_HAS_CUDA)
    list(APPEND XEN_DETECTOR_SOURCES
        Xen/detector/cuda_preprocess.cu)
endif()
if(XEN_ONNXRUNTIME_HAS_DIRECTML)
    list(APPEND XEN_DETECTOR_SOURCES
        Xen/detector/dml_preprocess.cpp)
endif()

add_library(detector STATIC ${XEN_DETECTOR_SOURCES})

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
    # CUDA D3D11 interop 仅依赖 Windows 系统 d3d11 导入库，不新增可部署 DLL。
    target_link_libraries(detector PRIVATE CUDA::cudart d3d11)
    target_compile_definitions(detector PRIVATE XEN_HAS_CUDA_RUNTIME=1)
    set_target_properties(detector PROPERTIES
        CUDA_STANDARD 20
        CUDA_STANDARD_REQUIRED ON
        # cuda_preprocess.cu 与 session.cpp 必须共享同一个动态 cudart；自动运行库
        # 关闭后统一由上面的 CUDA::cudart 导入目标链接和部署。
        CUDA_RUNTIME_LIBRARY None
        CUDA_ARCHITECTURES "${XEN_CUDA_ARCHITECTURES}")
else()
    target_compile_definitions(detector PRIVATE XEN_HAS_CUDA_RUNTIME=0)
endif()
if(XEN_ONNXRUNTIME_HAS_DIRECTML)
    # D3D11/D3D12 互操作与运行时 shader 编译只依赖 Windows SDK 导入库，
    # 不新增需要部署的第三方 DLL；DirectML.dll 仍由 ORT 部署规则复制。
    target_link_libraries(detector PRIVATE d3d11 d3d12 dxgi d3dcompiler)
    target_compile_definitions(detector PRIVATE XEN_HAS_DIRECTML_INTEROP=1)
else()
    target_compile_definitions(detector PRIVATE XEN_HAS_DIRECTML_INTEROP=0)
endif()
if(XEN_ONNXRUNTIME_HAS_OPENVINO)
    target_compile_definitions(detector PRIVATE XEN_HAS_OPENVINO_PROVIDER=1)
else()
    target_compile_definitions(detector PRIVATE XEN_HAS_OPENVINO_PROVIDER=0)
endif()

# ─── 实时闭环模块 ───────────────────────────────────────

add_library(capture STATIC
    Xen/capture/capture.cpp
    Xen/capture/duplication.cpp
    Xen/capture/network.cpp
    Xen/capture/ndi.cpp
    Xen/capture/udp.cpp
    Xen/capture/xudp.cpp
    Xen/capture/xudp_protocol.cpp)
target_include_directories(capture PUBLIC
    ${PROJECT_SOURCE_DIR}/Xen
    ${OpenCV_INCLUDE_DIRS})
target_link_libraries(capture PUBLIC ${OpenCV_LIBS} log
    PRIVATE bcrypt d3d11 dxgi ws2_32)
if(XEN_HAS_NDI)
    target_include_directories(capture PUBLIC ${XEN_NDI_INCLUDE_DIR})
    target_link_libraries(capture PRIVATE ${XEN_NDI_LIB})
    target_compile_definitions(capture PUBLIC XEN_HAS_NDI=1)
else()
    target_compile_definitions(capture PUBLIC XEN_HAS_NDI=0)
endif()

add_library(sender STATIC
    Xen/sender/sender.cpp
    Xen/sender/report.cpp)
target_include_directories(sender PUBLIC
    ${PROJECT_SOURCE_DIR}/Xen
    ${OpenCV_INCLUDE_DIRS})
target_link_libraries(sender
    PUBLIC capture log
    PRIVATE bcrypt ws2_32)

add_library(aim STATIC
    Xen/aim/aim.cpp
    Xen/aim/aim_evaluation.cpp)
target_include_directories(aim PUBLIC
    ${PROJECT_SOURCE_DIR}/Xen
    ${OpenCV_INCLUDE_DIRS})
target_link_libraries(aim PUBLIC log ${OpenCV_LIBS})

add_library(mouse STATIC
    Xen/mouse/mouse.cpp
    Xen/mouse/kmbox_net.cpp
    Xen/mouse/makcu.cpp)
target_include_directories(mouse PUBLIC ${PROJECT_SOURCE_DIR}/Xen)
target_link_libraries(mouse PUBLIC log PRIVATE user32 ws2_32)

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
target_link_libraries(config PUBLIC log)

add_library(runtime STATIC
    Xen/runtime/runtime.cpp
    Xen/runtime/runtime_queue.cpp)
target_include_directories(runtime PUBLIC ${PROJECT_SOURCE_DIR}/Xen)
target_link_libraries(runtime
    PUBLIC detector capture aim mouse keyboard config log
    PRIVATE Threads::Threads)

add_library(xen_debug STATIC
    Xen/debug/debug.cpp)
target_include_directories(xen_debug PUBLIC
    ${PROJECT_SOURCE_DIR}/Xen
    ${OpenCV_INCLUDE_DIRS})
target_link_libraries(xen_debug PUBLIC log aim mouse)

add_library(benchmark STATIC
    Xen/benchmark/benchmark.cpp)
target_include_directories(benchmark PUBLIC ${PROJECT_SOURCE_DIR}/Xen)
target_link_libraries(benchmark
    PUBLIC runtime xen_debug
    PRIVATE crash log)

add_library(mouse_benchmark STATIC
    Xen/mouse_benchmark/mouse_benchmark.cpp)
target_include_directories(mouse_benchmark PUBLIC ${PROJECT_SOURCE_DIR}/Xen)
target_link_libraries(mouse_benchmark
    PUBLIC mouse
    PRIVATE crash log)

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
target_link_libraries(xen_app PRIVATE
    overlay runtime xen_debug config crash log)
if(WIN32)
    xen_deploy_runtime_dlls(xen_app OPENCV ORT NDI)
endif()

add_executable(xen_sender
    Xen/sender/main.cpp)
set_target_properties(xen_sender PROPERTIES OUTPUT_NAME XenSender)
target_link_libraries(xen_sender PRIVATE sender capture crash log)
if(WIN32)
    xen_deploy_runtime_dlls(xen_sender OPENCV NDI)
endif()

add_executable(xen_benchmark
    Xen/benchmark/main.cpp)
set_target_properties(xen_benchmark PROPERTIES OUTPUT_NAME XenBenchmark)
target_link_libraries(xen_benchmark PRIVATE benchmark)
if(WIN32)
    xen_deploy_runtime_dlls(xen_benchmark OPENCV ORT NDI)
endif()

add_executable(xen_mouse_benchmark
    Xen/mouse_benchmark/main.cpp)
set_target_properties(
    xen_mouse_benchmark PROPERTIES OUTPUT_NAME XenMouseBenchmark)
target_link_libraries(xen_mouse_benchmark PRIVATE mouse_benchmark)

# ─── 测试 ───────────────────────────────────────────────

include(CTest)
if(BUILD_TESTING)
    add_executable(log_tests
        tests/log_tests.cpp)
    # 生产消费者只依赖 log.h；本测试额外验证 Xen 不触碰外部 spdlog registry。
    target_link_libraries(log_tests PRIVATE log spdlog::spdlog)
    add_test(NAME log_tests COMMAND log_tests)

    add_executable(crash_tests
        tests/crash_tests.cpp)
    target_link_libraries(crash_tests PRIVATE crash log)
    add_test(NAME crash_tests COMMAND crash_tests)

    # 部署闭包是 CMake 脚本级契约，不需要真实 SDK。两阶段测试在同一临时
    # 输出目录切换授权清单，覆盖陈旧 DLL 清理、同名来源替换和 SHA-256 报告。
    add_test(NAME runtime_deployment_tests
        COMMAND ${CMAKE_COMMAND}
            "-DXEN_DEPLOY_SCRIPT=${PROJECT_SOURCE_DIR}/scripts/deploy_runtime_file.cmake"
            "-DXEN_DEPLOY_TEST_ROOT=${CMAKE_BINARY_DIR}/runtime-deployment-tests"
            -P "${PROJECT_SOURCE_DIR}/tests/runtime_deployment_tests.cmake")

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

    # 分割真实模型测试独立于私有 detect 模型。只有模型与包含实例的真实图像
    # 同时显式配置时才加入构建图，避免默认测试下载权重或依赖外部数据。
    set(XEN_TEST_SEGMENTATION_MODEL "" CACHE FILEPATH
        "用于实例分割集成测试的 YOLOv8 兼容 ONNX 模型")
    set(XEN_TEST_SEGMENTATION_IMAGE "" CACHE FILEPATH
        "用于实例分割集成测试且确实包含目标的真实图像")
    if(XEN_TEST_SEGMENTATION_MODEL OR XEN_TEST_SEGMENTATION_IMAGE)
        if(NOT XEN_TEST_SEGMENTATION_MODEL OR
           NOT EXISTS "${XEN_TEST_SEGMENTATION_MODEL}")
            message(FATAL_ERROR
                "XEN_TEST_SEGMENTATION_MODEL does not exist: ${XEN_TEST_SEGMENTATION_MODEL}")
        endif()
        if(NOT XEN_TEST_SEGMENTATION_IMAGE OR
           NOT EXISTS "${XEN_TEST_SEGMENTATION_IMAGE}")
            message(FATAL_ERROR
                "XEN_TEST_SEGMENTATION_IMAGE does not exist: ${XEN_TEST_SEGMENTATION_IMAGE}")
        endif()
        add_executable(segmentation_model_test
            tests/segmentation_model_test.cpp)
        target_link_libraries(segmentation_model_test PRIVATE detector)
        if(WIN32)
            xen_deploy_runtime_dlls(
                segmentation_model_test OPENCV ORT)
        endif()
        add_test(NAME segmentation_model_test
            COMMAND segmentation_model_test
                "${XEN_TEST_SEGMENTATION_MODEL}"
                "${XEN_TEST_SEGMENTATION_IMAGE}")
    endif()

    # Pose 与 detect/segment 使用不同的输出契约和真实权重，独立配置可避免
    # 默认构建依赖外部模型；模型和真实图像必须成对提供。
    set(XEN_TEST_POSE_MODEL "" CACHE FILEPATH
        "用于姿态估计集成测试的 YOLOv8 兼容 ONNX 模型")
    set(XEN_TEST_POSE_IMAGE "" CACHE FILEPATH
        "用于姿态估计集成测试且包含人物的真实图像")
    if(XEN_TEST_POSE_MODEL OR XEN_TEST_POSE_IMAGE)
        if(NOT XEN_TEST_POSE_MODEL OR
           NOT EXISTS "${XEN_TEST_POSE_MODEL}")
            message(FATAL_ERROR
                "XEN_TEST_POSE_MODEL does not exist: ${XEN_TEST_POSE_MODEL}")
        endif()
        if(NOT XEN_TEST_POSE_IMAGE OR
           NOT EXISTS "${XEN_TEST_POSE_IMAGE}")
            message(FATAL_ERROR
                "XEN_TEST_POSE_IMAGE does not exist: ${XEN_TEST_POSE_IMAGE}")
        endif()
        add_executable(pose_model_test tests/pose_model_test.cpp)
        target_link_libraries(pose_model_test PRIVATE detector)
        if(WIN32)
            xen_deploy_runtime_dlls(pose_model_test OPENCV ORT)
        endif()
        add_test(NAME pose_model_test
            COMMAND pose_model_test
                "${XEN_TEST_POSE_MODEL}"
                "${XEN_TEST_POSE_IMAGE}")
    endif()

    # OBB 需要真实航拍图像验证旋转 NMS 和角度/宽高缩放，独立于其他
    # Detector 模型门禁，模型与图像必须成对显式配置。
    set(XEN_TEST_OBB_MODEL "" CACHE FILEPATH
        "用于旋转目标检测集成测试的 YOLOv8 兼容 ONNX 模型")
    set(XEN_TEST_OBB_IMAGE "" CACHE FILEPATH
        "用于旋转目标检测集成测试且包含旋转目标的真实图像")
    if(XEN_TEST_OBB_MODEL OR XEN_TEST_OBB_IMAGE)
        if(NOT XEN_TEST_OBB_MODEL OR
           NOT EXISTS "${XEN_TEST_OBB_MODEL}")
            message(FATAL_ERROR
                "XEN_TEST_OBB_MODEL does not exist: ${XEN_TEST_OBB_MODEL}")
        endif()
        if(NOT XEN_TEST_OBB_IMAGE OR
           NOT EXISTS "${XEN_TEST_OBB_IMAGE}")
            message(FATAL_ERROR
                "XEN_TEST_OBB_IMAGE does not exist: ${XEN_TEST_OBB_IMAGE}")
        endif()
        add_executable(obb_model_test tests/obb_model_test.cpp)
        target_link_libraries(obb_model_test PRIVATE detector)
        if(WIN32)
            xen_deploy_runtime_dlls(obb_model_test OPENCV ORT)
        endif()
        add_test(NAME obb_model_test
            COMMAND obb_model_test
                "${XEN_TEST_OBB_MODEL}"
                "${XEN_TEST_OBB_IMAGE}")
    endif()

    # 视频真值解析和 Recall 统计保持为无 Session 的纯算法测试；文件哈希使用
    # Windows CNG，JSON 使用已部署的 OpenCV FileStorage，不增加第三方依赖。
    add_executable(video_visibility_tests
        tests/video_visibility_tests.cpp
        Xen/detector/video_visibility.cpp)
    target_include_directories(video_visibility_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/Xen
        ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(video_visibility_tests PRIVATE
        ${OpenCV_LIBS} bcrypt)
    xen_deploy_runtime_dlls(video_visibility_tests OPENCV)
    add_test(NAME video_visibility_tests COMMAND video_visibility_tests)

    add_executable(aim_tests
        tests/aim_tests.cpp)
    target_link_libraries(aim_tests PRIVATE aim log)
    xen_deploy_runtime_dlls(aim_tests OPENCV)
    add_test(NAME aim_tests COMMAND aim_tests)

    # Aim 真实视频真值解析和追踪指标保持为离线纯算法测试，不加载 ORT Session。
    add_executable(aim_evaluation_tests
        tests/aim_evaluation_tests.cpp)
    target_link_libraries(aim_evaluation_tests PRIVATE aim log)
    xen_deploy_runtime_dlls(aim_evaluation_tests OPENCV)
    add_test(NAME aim_evaluation_tests COMMAND aim_evaluation_tests)

    add_executable(mouse_tests
        tests/mouse_tests.cpp)
    target_link_libraries(mouse_tests PRIVATE mouse log ws2_32)
    add_test(NAME mouse_tests COMMAND mouse_tests)

    add_executable(mouse_benchmark_tests
        tests/mouse_benchmark_tests.cpp)
    target_link_libraries(mouse_benchmark_tests PRIVATE
        mouse_benchmark ws2_32)
    add_test(NAME mouse_benchmark_tests COMMAND mouse_benchmark_tests)

    add_executable(runtime_tests
        tests/runtime_tests.cpp
        Xen/runtime/runtime_queue.cpp)
    target_include_directories(runtime_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/Xen
        ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(runtime_tests PRIVATE ${OpenCV_LIBS} Threads::Threads)
    xen_deploy_runtime_dlls(runtime_tests OPENCV)
    add_test(NAME runtime_tests COMMAND runtime_tests)

    # Overlay 标量历史环是纯算法契约；测试不创建 Win32/D3D11 窗口，也不链接 ImGui。
    add_executable(overlay_tests
        tests/overlay_tests.cpp)
    target_include_directories(overlay_tests PRIVATE ${PROJECT_SOURCE_DIR}/Xen)
    add_test(NAME overlay_tests COMMAND overlay_tests)

    add_executable(debug_tests
        tests/debug_tests.cpp)
    target_link_libraries(debug_tests PRIVATE xen_debug log)
    add_test(NAME debug_tests COMMAND debug_tests)

    add_executable(benchmark_tests
        tests/benchmark_tests.cpp)
    target_link_libraries(benchmark_tests PRIVATE benchmark)
    if(WIN32)
        xen_deploy_runtime_dlls(benchmark_tests OPENCV ORT NDI)
    endif()
    add_test(NAME benchmark_tests COMMAND benchmark_tests)

    add_executable(config_tests
        tests/config_tests.cpp)
    target_link_libraries(config_tests PRIVATE config)
    add_test(NAME config_tests COMMAND config_tests)

    add_executable(io_tests
        tests/io_tests.cpp)
    target_link_libraries(io_tests PRIVATE
        capture mouse keyboard log ws2_32)
    xen_deploy_runtime_dlls(io_tests OPENCV NDI)
    add_test(NAME io_tests COMMAND io_tests)

    add_executable(sender_tests
        tests/sender_tests.cpp)
    target_link_libraries(sender_tests PRIVATE
        sender capture log ws2_32)
    xen_deploy_runtime_dlls(sender_tests OPENCV NDI)
    add_test(NAME sender_tests COMMAND sender_tests)

    # 真实模型测试默认关闭，避免仓库绑定私有模型；配置时通过
    # -DXEN_TEST_MODEL=<onnx路径> 启用加载和单帧推理冒烟测试。
    set(XEN_TEST_MODEL "" CACHE FILEPATH "用于 Detector 集成测试的 ONNX 模型")
    set(XEN_TEST_OPENVINO_DEVICE "" CACHE STRING
        "OpenVINO 真实模型测试设备：CPU/GPU/NPU；空值表示不启用")
    string(TOUPPER "${XEN_TEST_OPENVINO_DEVICE}"
        XEN_TEST_OPENVINO_DEVICE_NORMALIZED)
    if(XEN_TEST_OPENVINO_DEVICE_NORMALIZED)
        if(NOT XEN_TEST_MODEL)
            message(FATAL_ERROR
                "XEN_TEST_OPENVINO_DEVICE requires XEN_TEST_MODEL")
        endif()
        if(NOT XEN_ONNXRUNTIME_HAS_OPENVINO)
            message(FATAL_ERROR
                "XEN_TEST_OPENVINO_DEVICE requires an OpenVINO ORT package")
        endif()
        if(NOT XEN_TEST_OPENVINO_DEVICE_NORMALIZED MATCHES "^(CPU|GPU|NPU)$")
            message(FATAL_ERROR
                "XEN_TEST_OPENVINO_DEVICE must be CPU, GPU or NPU")
        endif()
    endif()
    if(XEN_TEST_MODEL)
        if(NOT EXISTS "${XEN_TEST_MODEL}")
            message(FATAL_ERROR "XEN_TEST_MODEL does not exist: ${XEN_TEST_MODEL}")
        endif()
        add_executable(detector_model_test
            tests/detector_model_test.cpp
            Xen/detector/video_visibility.cpp)
        target_link_libraries(detector_model_test PRIVATE detector aim bcrypt)
        if(WIN32)
            xen_deploy_runtime_dlls(detector_model_test OPENCV ORT)
        endif()
        add_test(NAME detector_model_test
            COMMAND detector_model_test "${XEN_TEST_MODEL}")

        if(XEN_TEST_OPENVINO_DEVICE_NORMALIZED)
            add_executable(openvino_model_test
                tests/openvino_model_test.cpp)
            target_link_libraries(openvino_model_test PRIVATE detector)
            if(WIN32)
                xen_deploy_runtime_dlls(openvino_model_test OPENCV ORT)
            endif()
            add_test(NAME openvino_model_test
                COMMAND openvino_model_test
                    "${XEN_TEST_MODEL}"
                    "${XEN_TEST_OPENVINO_DEVICE_NORMALIZED}"
                    "${CMAKE_BINARY_DIR}/cache/openvino/ctest")
        endif()

        if(XEN_ONNXRUNTIME_HAS_CUDA)
            add_executable(d3d11_cuda_interop_tests
                tests/d3d11_cuda_interop_tests.cpp)
            target_link_libraries(d3d11_cuda_interop_tests PRIVATE
                detector CUDA::cudart d3d11 dxgi)
            xen_deploy_runtime_dlls(
                d3d11_cuda_interop_tests OPENCV ORT)
            add_test(NAME d3d11_cuda_interop_tests
                COMMAND d3d11_cuda_interop_tests
                    "${XEN_TEST_MODEL}"
                    "${CMAKE_BINARY_DIR}/cache/tensorrt/d3d11-interop-test")
        endif()

        if(XEN_ONNXRUNTIME_HAS_DIRECTML)
            add_executable(d3d11_directml_interop_tests
                tests/d3d11_directml_interop_tests.cpp)
            target_link_libraries(d3d11_directml_interop_tests PRIVATE
                detector d3d11 d3d12 dxgi)
            xen_deploy_runtime_dlls(
                d3d11_directml_interop_tests OPENCV ORT)
            add_test(NAME d3d11_directml_interop_tests
                COMMAND d3d11_directml_interop_tests
                    "${XEN_TEST_MODEL}")
        endif()

        add_executable(runtime_model_reload_test
            tests/runtime_model_reload_test.cpp)
        target_link_libraries(runtime_model_reload_test PRIVATE runtime ws2_32)
        if(WIN32)
            xen_deploy_runtime_dlls(
                runtime_model_reload_test OPENCV ORT NDI)
        endif()
        add_test(NAME runtime_model_reload_test
            COMMAND runtime_model_reload_test "${XEN_TEST_MODEL}")
    endif()
endif()

if(WIN32)
    xen_finalize_runtime_deployment()
endif()
