cmake_minimum_required(VERSION 3.18)
project(xen_runtime_component_fixture LANGUAGES NONE)

# 此工程只运行生产元数据 producer；真实根工程负责 CXX/CUDA 配置和编译。
# 不设置 MSVC、CMAKE_*_COMPILER_WORKS 等编译器事实，也不生成/运行设备代码。
foreach(required_variable IN ITEMS
        XEN_SOURCE_DIR XEN_RUNTIME_LINKER XEN_MSVC_REDIST_ROOT)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "元数据夹具缺少真实构建上下文：${required_variable}")
    endif()
endforeach()
include("${XEN_SOURCE_DIR}/scripts/resolve_onnxruntime.cmake")
find_package(OpenCV REQUIRED)
set(XEN_HAS_NDI FALSE)
set(XEN_RUNTIME_USES_MSVC TRUE)
include("${XEN_SOURCE_DIR}/scripts/runtime_metadata.cmake")

# 仅提供真实部署函数需要的目标属性，不复制白名单、身份序列化或分组生成逻辑。
foreach(runtime_target IN ITEMS xen_app xen_launcher xen_source_context
        xen_clock_source xen_sender xen_capture_evidence xen_auto_stop_capture)
    add_custom_target(${runtime_target})
endforeach()
xen_deploy_runtime_dlls(xen_app OPENCV ORT)
xen_deploy_runtime_dlls(xen_launcher)
xen_deploy_runtime_dlls(xen_source_context)
xen_deploy_runtime_dlls(xen_clock_source)
xen_deploy_runtime_dlls(xen_sender OPENCV)
xen_deploy_runtime_dlls(xen_capture_evidence OPENCV)
xen_deploy_runtime_dlls(xen_auto_stop_capture OPENCV)
xen_generate_runtime_metadata(fixture_targets fixture_manifest)
