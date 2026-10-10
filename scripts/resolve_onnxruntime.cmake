# 解析当前 ORT 包事实，供真实构建与运行库元数据 producer 共用。
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
else()
    set(XEN_ONNXRUNTIME_HAS_CUDA FALSE)
endif()
