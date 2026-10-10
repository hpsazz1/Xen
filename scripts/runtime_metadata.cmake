# 运行库元数据生产边界：只验证文件、导入关系和目标部署属性，不启用编译器。
# 调用方提供已解析的真实工具链上下文；根工程仍独立启用 CXX/CUDA 并检查编译器。
# 夹具也调用本文件，不能复制另一套白名单或设置 CMAKE_*_COMPILER_WORKS。
get_filename_component(XEN_RUNTIME_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if(NOT DEFINED XEN_RUNTIME_USES_MSVC)
    message(FATAL_ERROR "Runtime metadata requires an explicit toolchain context.")
endif()
if(WIN32 AND XEN_RUNTIME_USES_MSVC AND NOT EXISTS "${XEN_RUNTIME_LINKER}")
    message(FATAL_ERROR "Runtime metadata requires the selected MSVC linker.")
endif()

# NVIDIA ORT Provider 的发布闭包必须由当前已验证文件集合显式组成。SDK 根目录
# 只用于定位这些文件，不能把其中任意 DLL 自动提升为发布组件。
set(XEN_CUDNN_RUNTIME_DLLS)
set(XEN_CUDA_RUNTIME_DLLS)
if(XEN_ONNXRUNTIME_HAS_CUDA)
    if(NOT XEN_CUDNN_ROOT OR NOT IS_DIRECTORY "${XEN_CUDNN_ROOT}")
        message(FATAL_ERROR
            "NVIDIA ONNX Runtime requires an explicit cuDNN runtime root.")
    endif()
    foreach(runtime_name IN ITEMS
            cudnn64_9.dll
            cudnn_adv64_9.dll
            cudnn_cnn64_9.dll
            cudnn_engines_precompiled64_9.dll
            cudnn_engines_runtime_compiled64_9.dll
            cudnn_engines_tensor_ir64_9.dll
            cudnn_graph64_9.dll
            cudnn_heuristic64_9.dll
            cudnn_ops64_9.dll)
        file(GLOB_RECURSE runtime_matches LIST_DIRECTORIES FALSE
            "${XEN_CUDNN_ROOT}/${runtime_name}")
        list(REMOVE_DUPLICATES runtime_matches)
        list(LENGTH runtime_matches runtime_match_count)
        if(NOT runtime_match_count EQUAL 1)
            message(FATAL_ERROR
                "Required cuDNN runtime component file ${runtime_name} must exist exactly once under "
                "XEN_CUDNN_ROOT; found ${runtime_match_count}: ${XEN_CUDNN_ROOT}")
        endif()
        list(GET runtime_matches 0 runtime_file)
        list(APPEND XEN_CUDNN_RUNTIME_DLLS "${runtime_file}")
    endforeach()

    if(NOT XEN_CUDA_ROOT OR NOT IS_DIRECTORY "${XEN_CUDA_ROOT}")
        message(FATAL_ERROR
            "NVIDIA ONNX Runtime requires an explicit CUDA runtime root.")
    endif()
    foreach(runtime_name IN ITEMS
            cublas64_13.dll
            cublasLt64_13.dll
            cufft64_12.dll)
        set(runtime_file "${XEN_CUDA_ROOT}/bin/x64/${runtime_name}")
        if(NOT EXISTS "${runtime_file}")
            message(FATAL_ERROR
                "Required CUDA runtime component file not found: ${runtime_file}")
        endif()
        list(APPEND XEN_CUDA_RUNTIME_DLLS "${runtime_file}")
    endforeach()
    # FindCUDAToolkit 的 CUDA::cudart TARGET_FILE 在 Windows 指向导入库，
    # 因而继续从当前 Toolkit 的 bin 目录解析唯一运行时 DLL。
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
    list(APPEND XEN_CUDA_RUNTIME_DLLS ${CUDA_CUDART_RUNTIME_DLLS})
endif()

# 每个独立构建目录记录配置时的源码提交和运行时归属。正式组包只消费
# dirty=false、提交一致且归属唯一的全新构建，拒绝用旧矩阵二进制拼包。
if(XEN_ONNXRUNTIME_HAS_DIRECTML)
    set(XEN_BUILD_RUNTIME_ID "directml")
elseif(XEN_ONNXRUNTIME_HAS_OPENVINO)
    set(XEN_BUILD_RUNTIME_ID "openvino")
else()
    set(XEN_BUILD_RUNTIME_ID "nvidia")
endif()
find_package(Git REQUIRED)
execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
    WORKING_DIRECTORY "${XEN_RUNTIME_SOURCE_DIR}"
    RESULT_VARIABLE XEN_GIT_COMMIT_RESULT
    OUTPUT_VARIABLE XEN_BUILD_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(
    COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=normal
    WORKING_DIRECTORY "${XEN_RUNTIME_SOURCE_DIR}"
    RESULT_VARIABLE XEN_GIT_STATUS_RESULT
    OUTPUT_VARIABLE XEN_BUILD_GIT_STATUS
    OUTPUT_STRIP_TRAILING_WHITESPACE)
string(LENGTH "${XEN_BUILD_GIT_COMMIT}" XEN_BUILD_GIT_COMMIT_LENGTH)
if(NOT XEN_GIT_COMMIT_RESULT EQUAL 0 OR
   NOT XEN_GIT_STATUS_RESULT EQUAL 0 OR
   NOT XEN_BUILD_GIT_COMMIT MATCHES "^[0-9a-fA-F]+$" OR
   NOT XEN_BUILD_GIT_COMMIT_LENGTH EQUAL 40)
    message(FATAL_ERROR "Cannot record Xen build Git identity")
endif()
if(XEN_BUILD_GIT_STATUS STREQUAL "")
    set(XEN_BUILD_GIT_DIRTY false)
else()
    set(XEN_BUILD_GIT_DIRTY true)
endif()
file(TO_CMAKE_PATH "${XEN_RUNTIME_SOURCE_DIR}" XEN_BUILD_SOURCE_ROOT)

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

    if(XEN_ONNXRUNTIME_HAS_TENSORRT AND WIN32 AND XEN_RUNTIME_USES_MSVC)
        execute_process(
            COMMAND "${XEN_RUNTIME_LINKER}" /dump /dependents
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
elseif(XEN_ONNXRUNTIME_HAS_TENSORRT)
    message(FATAL_ERROR
        "Selected ONNX Runtime TensorRT Provider requires XEN_TENSORRT_ROOT.")
elseif(XEN_TENSORRT_MAJOR)
    message(FATAL_ERROR "XEN_TENSORRT_MAJOR is set but XEN_TENSORRT_ROOT is empty.")
endif()

set(XEN_TENSORRT_RUNTIME_DLLS)
if(XEN_ONNXRUNTIME_HAS_TENSORRT)
    set(XEN_TENSORRT_RUNTIME_DLLS
        "${XEN_TENSORRT_CORE_DLL}"
        "${XEN_TENSORRT_PARSER_DLL}")
    # 当前 TensorRT 10.16 发布闭包只授权已经验证的 PTX 与目标架构资源。
    # 新架构或新文件名必须显式评审，不能通过 wildcard 自动扩包。
    foreach(resource_id IN ITEMS
            ptx
            sm75
            sm80
            sm86
            sm89
            sm90
            sm100
            sm120)
        set(runtime_file
            "${XEN_TENSORRT_ROOT}/bin/nvinfer_builder_resource_${resource_id}_${XEN_TENSORRT_MAJOR}.dll")
        if(NOT EXISTS "${runtime_file}")
            message(FATAL_ERROR
                "Required TensorRT builder runtime component file not found: ${runtime_file}")
        endif()
        list(APPEND XEN_TENSORRT_RUNTIME_DLLS "${runtime_file}")
    endforeach()
endif()

if(XEN_RUNTIME_USES_MSVC)
    file(TO_CMAKE_PATH "${XEN_MSVC_REDIST_ROOT}" XEN_MSVC_REDIST_ROOT)
    string(TOLOWER "${XEN_MSVC_REDIST_ROOT}" msvc_redist_key)
    if(NOT IS_DIRECTORY "${XEN_MSVC_REDIST_ROOT}" OR
       NOT msvc_redist_key MATCHES "/vc/redist/msvc/[^/]+/x64/microsoft\\.vc14[0-9]\\.crt$")
        message(FATAL_ERROR "Official MSVC x64 CRT redist directory is invalid: ${XEN_MSVC_REDIST_ROOT}")
    endif()
    foreach(runtime_name IN ITEMS
            msvcp140.dll msvcp140_1.dll msvcp140_2.dll
            msvcp140_atomic_wait.dll msvcp140_codecvt_ids.dll
            vcruntime140.dll vcruntime140_1.dll concrt140.dll)
        set(runtime_file "${XEN_MSVC_REDIST_ROOT}/${runtime_name}")
        if(NOT EXISTS "${runtime_file}" OR IS_DIRECTORY "${runtime_file}")
            message(FATAL_ERROR "Required official MSVC runtime is missing: ${runtime_file}")
        endif()
        list(APPEND XEN_MSVC_RUNTIME_DLLS "${runtime_file}")
    endforeach()
    get_filename_component(msvc_instance "${XEN_MSVC_REDIST_ROOT}/../../../../../.." ABSOLUTE)
    file(GLOB msvc_redist_licenses "${msvc_instance}/Licenses/*/Redist.txt")
    list(SORT msvc_redist_licenses)
    if(NOT msvc_redist_licenses)
        message(FATAL_ERROR "Official MSVC REDIST license evidence is missing: ${msvc_instance}/Licenses")
    endif()
    list(GET msvc_redist_licenses 0 XEN_MSVC_REDIST_LICENSE)
endif()

# 许可证 consumer 只认识稳定 dependency component id。运行时 ID 和 DLL 文件名
# 分别保留在既有字段/部署报告中，不得混入 components 冒充依赖闭包。
set(XEN_BUILD_COMPONENT_IDS
    imgui
    nlohmann-json
    onnxruntime
    opencv
    simpleini
    spdlog)
if(XEN_RUNTIME_USES_MSVC)
    list(APPEND XEN_BUILD_COMPONENT_IDS msvc-runtime)
endif()
if(XEN_HAS_NDI)
    list(APPEND XEN_BUILD_COMPONENT_IDS ndi)
endif()
if(XEN_ONNXRUNTIME_HAS_DIRECTML)
    list(APPEND XEN_BUILD_COMPONENT_IDS directml)
endif()
if(XEN_ONNXRUNTIME_HAS_OPENVINO)
    list(APPEND XEN_BUILD_COMPONENT_IDS openvino)
endif()
if(XEN_ONNXRUNTIME_HAS_CUDA)
    list(APPEND XEN_BUILD_COMPONENT_IDS cuda cudnn)
endif()
if(XEN_ONNXRUNTIME_HAS_TENSORRT)
    list(APPEND XEN_BUILD_COMPONENT_IDS tensorrt)
endif()
list(REMOVE_DUPLICATES XEN_BUILD_COMPONENT_IDS)
list(SORT XEN_BUILD_COMPONENT_IDS)
set(XEN_BUILD_COMPONENTS_JSON "")
foreach(component_id IN LISTS XEN_BUILD_COMPONENT_IDS)
    if(NOT component_id MATCHES "^[a-z0-9][a-z0-9._-]*$")
        message(FATAL_ERROR
            "Build identity component id is invalid: ${component_id}")
    endif()
    if(NOT XEN_BUILD_COMPONENTS_JSON STREQUAL "")
        string(APPEND XEN_BUILD_COMPONENTS_JSON ", ")
    endif()
    string(APPEND XEN_BUILD_COMPONENTS_JSON "\"${component_id}\"")
endforeach()
configure_file(
    "${XEN_RUNTIME_SOURCE_DIR}/scripts/build_identity.json.in"
    "${CMAKE_BINARY_DIR}/xen-build-identity.json"
    @ONLY)

# Visual Studio 的同一配置共享一个可执行输出目录。这里只登记当前配置授权的
# 运行文件，全部目标声明完毕后再由单一构建目标加锁完成清理、增量复制和来源报告；
# 这样即使可执行目标没有重新链接，SDK 清空或切换也不会跳过部署收敛。
function(xen_copy_runtime_files target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR
            "Runtime deployment target does not exist: ${target}")
    endif()
    foreach(runtime_file IN LISTS ARGN)
        set_property(GLOBAL APPEND PROPERTY
            XEN_RUNTIME_DEPLOY_FILES "${runtime_file}")
        set_property(TARGET ${target} APPEND PROPERTY
            XEN_RUNTIME_DEPLOY_FILES "${runtime_file}")
    endforeach()
endfunction()

# 可执行目标按实际能力选择 OPENCV / ORT / NDI，CRT 来自官方 redist。
# Windows 系统 DLL、显卡驱动 DLL 和调试 CRT 不复制。
function(xen_deploy_runtime_dlls target)
    set(options OPENCV ORT NDI)
    cmake_parse_arguments(DEPLOY "${options}" "" "" ${ARGN})
    set_property(GLOBAL APPEND PROPERTY
        XEN_RUNTIME_DEPLOY_TARGETS "${target}")
    xen_copy_runtime_files(${target} ${XEN_MSVC_RUNTIME_DLLS})

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

        if(XEN_ONNXRUNTIME_HAS_TENSORRT)
            xen_copy_runtime_files(${target} ${XEN_TENSORRT_RUNTIME_DLLS})
        elseif(XEN_ORT_USES_NVIDIA)
            message(STATUS "TensorRT runtime deployment skipped: XEN_TENSORRT_ROOT is empty")
        endif()

        if(XEN_ORT_USES_NVIDIA)
            xen_copy_runtime_files(${target} ${XEN_CUDNN_RUNTIME_DLLS})
        endif()

        if(XEN_ORT_USES_NVIDIA)
            xen_copy_runtime_files(${target} ${XEN_CUDA_RUNTIME_DLLS})
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


function(xen_generate_runtime_metadata targets_var manifest_var)
    set(${targets_var} "" PARENT_SCOPE)
    set(${manifest_var} "" PARENT_SCOPE)
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

    # 发布器从本次部署报告取这些目标的真实子集，不将 Provider DLL 带到根/源工具。
    set(layout_content "{\n  \"schema\": 1,\n  \"configuration\": \"$<CONFIG>\",\n")
    string(APPEND layout_content
        "  \"msvc_runtime\": {\"source_directory\": \"${XEN_MSVC_REDIST_ROOT}\", \"license_file\": \"${XEN_MSVC_REDIST_LICENSE}\"},\n  \"groups\": {\n")
    foreach(group IN ITEMS launcher source)
        if(group STREQUAL "launcher")
            set(group_targets xen_launcher)
        else()
            set(group_targets xen_source_context xen_clock_source xen_sender
                xen_capture_evidence xen_auto_stop_capture)
        endif()
        set(group_files)
        foreach(group_target IN LISTS group_targets)
            get_target_property(target_files ${group_target} XEN_RUNTIME_DEPLOY_FILES)
            if(target_files)
                list(APPEND group_files ${target_files})
            endif()
        endforeach()
        list(REMOVE_DUPLICATES group_files)
        set(group_json "")
        foreach(group_file IN LISTS group_files)
            string(REPLACE "\\" "/" group_file "${group_file}")
            if(NOT group_json STREQUAL "")
                string(APPEND group_json ", ")
            endif()
            string(APPEND group_json "\"${group_file}\"")
        endforeach()
        string(APPEND layout_content "    \"${group}\": [${group_json}]")
        if(group STREQUAL "launcher")
            string(APPEND layout_content ",")
        endif()
        string(APPEND layout_content "\n")
    endforeach()
    string(APPEND layout_content "  }\n}\n")
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/$<CONFIG>/xen-release-layout.json"
        CONTENT "${layout_content}")

    set(${targets_var} "${runtime_targets}" PARENT_SCOPE)
    set(${manifest_var} "${runtime_manifest}" PARENT_SCOPE)
endfunction()
