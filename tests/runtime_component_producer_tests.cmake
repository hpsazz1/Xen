cmake_minimum_required(VERSION 3.18)

foreach(required_variable IN ITEMS
        XEN_SOURCE_DIR
        XEN_COMPONENT_TEST_ROOT
        XEN_GENERATOR
        XEN_OPENCV_DIR
        XEN_CUDA_ROOT
        XEN_TENSORRT_PROVIDER_DLL
        XEN_TENSORRT_MAJOR
        XEN_SPDLOG_SOURCE_DIR
        XEN_SIMPLEINI_SOURCE_DIR
        XEN_NLOHMANN_JSON_SOURCE_DIR
        XEN_IMGUI_SOURCE_DIR)
    if(NOT DEFINED ${required_variable} OR
       "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR
            "运行库组件 producer 测试缺少参数：${required_variable}")
    endif()
endforeach()
if(NOT EXISTS "${XEN_TENSORRT_PROVIDER_DLL}" OR
   NOT XEN_TENSORRT_MAJOR MATCHES "^[0-9]+$")
    message(FATAL_ERROR
        "TensorRT producer 测试需要真实 Provider DLL 与数字 major。")
endif()

get_filename_component(source_dir "${XEN_SOURCE_DIR}" ABSOLUTE)
get_filename_component(test_base "${XEN_COMPONENT_TEST_ROOT}" ABSOLUTE)
set(path_safety_script "${source_dir}/scripts/invoke_path_safety.ps1")
set(windows_powershell
    "$ENV{SystemRoot}/System32/WindowsPowerShell/v1.0/powershell.exe")
if(NOT EXISTS "${windows_powershell}" OR
   NOT EXISTS "${path_safety_script}")
    message(FATAL_ERROR "缺少 Windows PowerShell 或路径 owner adapter。")
endif()

execute_process(
    COMMAND "${windows_powershell}" -NoProfile -ExecutionPolicy Bypass
        -File "${path_safety_script}"
        -Action New
        -BasePath "${test_base}"
        -RepositoryRoot "${source_dir}"
    RESULT_VARIABLE owner_create_result
    OUTPUT_VARIABLE owner_create_output
    ERROR_VARIABLE owner_create_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT owner_create_result EQUAL 0)
    message(FATAL_ERROR
        "无法创建 runtime component owner 测试目录：${owner_create_error}")
endif()
string(REPLACE "\r" "" owner_create_output "${owner_create_output}")
string(REPLACE "\t" ";" owner_record "${owner_create_output}")
list(LENGTH owner_record owner_record_count)
if(NOT owner_record_count EQUAL 2)
    message(FATAL_ERROR "路径 owner adapter 返回格式无效：${owner_create_output}")
endif()
list(GET owner_record 0 test_root)
list(GET owner_record 1 owner_id)
file(TO_CMAKE_PATH "${test_root}" test_root)

set(test_failures)
set(required_cudnn_names
    cudnn64_9.dll
    cudnn_adv64_9.dll
    cudnn_cnn64_9.dll
    cudnn_engines_precompiled64_9.dll
    cudnn_engines_runtime_compiled64_9.dll
    cudnn_engines_tensor_ir64_9.dll
    cudnn_graph64_9.dll
    cudnn_heuristic64_9.dll
    cudnn_ops64_9.dll)
set(required_tensorrt_resource_ids
    ptx
    sm75
    sm80
    sm86
    sm89
    sm90
    sm100
    sm120)
set(expected_component_ids
    cuda
    cudnn
    imgui
    nlohmann-json
    onnxruntime
    opencv
    simpleini
    spdlog
    tensorrt)

function(make_fake_sdk fixture_root omit_cudnn_name omit_tensorrt_resource_id)
    set(ort_root "${fixture_root}/ort")
    set(cudnn_root "${fixture_root}/cudnn")
    set(tensorrt_root "${fixture_root}/tensorrt")
    file(MAKE_DIRECTORY
        "${ort_root}/include"
        "${ort_root}/lib"
        "${cudnn_root}/archive/bin/x64"
        "${cudnn_root}/unrelated/nested"
        "${tensorrt_root}/bin")
    file(WRITE "${ort_root}/include/onnxruntime_cxx_api.h"
        "// configure-only fixture\n")
    foreach(ort_name IN ITEMS
            onnxruntime.lib
            onnxruntime.dll
            onnxruntime_providers_shared.dll
            onnxruntime_providers_cuda.dll)
        file(WRITE "${ort_root}/lib/${ort_name}" "fixture-${ort_name}\n")
    endforeach()
    configure_file(
        "${XEN_TENSORRT_PROVIDER_DLL}"
        "${ort_root}/lib/onnxruntime_providers_tensorrt.dll"
        COPYONLY)
    foreach(cudnn_name IN LISTS required_cudnn_names)
        if(NOT cudnn_name STREQUAL omit_cudnn_name)
            file(WRITE
                "${cudnn_root}/archive/bin/x64/${cudnn_name}"
                "fixture-${cudnn_name}\n")
        endif()
    endforeach()
    file(WRITE "${cudnn_root}/unrelated/nested/unrelated-helper.dll"
        "must-not-be-authorized\n")
    file(WRITE
        "${tensorrt_root}/bin/nvinfer_${XEN_TENSORRT_MAJOR}.dll"
        "fixture-nvinfer\n")
    file(WRITE
        "${tensorrt_root}/bin/nvonnxparser_${XEN_TENSORRT_MAJOR}.dll"
        "fixture-nvonnxparser\n")
    foreach(resource_id IN LISTS required_tensorrt_resource_ids)
        if(NOT resource_id STREQUAL omit_tensorrt_resource_id)
            file(WRITE
                "${tensorrt_root}/bin/nvinfer_builder_resource_${resource_id}_${XEN_TENSORRT_MAJOR}.dll"
                "fixture-builder-${resource_id}\n")
        endif()
    endforeach()
    # 旧 wildcard 会把这个未批准 basename 自动提升为发布文件。
    file(WRITE
        "${tensorrt_root}/bin/nvinfer_builder_resource_future_${XEN_TENSORRT_MAJOR}.dll"
        "must-not-be-authorized\n")
endfunction()

function(run_root_configure fixture_root build_name result_var output_var)
    set(build_dir "${test_root}/${build_name}")
    set(command
        "${CMAKE_COMMAND}"
        -S "${source_dir}"
        -B "${build_dir}"
        -G "${XEN_GENERATOR}")
    if(DEFINED XEN_GENERATOR_PLATFORM AND
       NOT "${XEN_GENERATOR_PLATFORM}" STREQUAL "")
        list(APPEND command -A "${XEN_GENERATOR_PLATFORM}")
    endif()
    list(APPEND command
        "-DONNXRUNTIME_ROOT=${fixture_root}/ort"
        "-DOpenCV_DIR=${XEN_OPENCV_DIR}"
        "-DXEN_CUDA_ROOT=${XEN_CUDA_ROOT}"
        "-DXEN_CUDNN_ROOT=${fixture_root}/cudnn"
        "-DXEN_TENSORRT_ROOT=${fixture_root}/tensorrt"
        "-DXEN_TENSORRT_MAJOR=${XEN_TENSORRT_MAJOR}"
        "-DXEN_DIRECTML_ROOT="
        "-DXEN_NDI_SDK_ROOT="
        "-DBUILD_TESTING=OFF"
        "-DFETCHCONTENT_FULLY_DISCONNECTED=ON"
        "-DFETCHCONTENT_SOURCE_DIR_SPDLOG=${XEN_SPDLOG_SOURCE_DIR}"
        "-DFETCHCONTENT_SOURCE_DIR_SIMPLEINI=${XEN_SIMPLEINI_SOURCE_DIR}"
        "-DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=${XEN_NLOHMANN_JSON_SOURCE_DIR}"
        "-DFETCHCONTENT_SOURCE_DIR_IMGUI=${XEN_IMGUI_SOURCE_DIR}")
    execute_process(
        COMMAND ${command}
        RESULT_VARIABLE configure_result
        OUTPUT_VARIABLE configure_output
        ERROR_VARIABLE configure_error)
    set(${result_var} "${configure_result}" PARENT_SCOPE)
    set(${output_var} "${configure_output}\n${configure_error}" PARENT_SCOPE)
endfunction()

set(complete_fixture "${test_root}/complete-sdk")
make_fake_sdk("${complete_fixture}" "" "")
run_root_configure("${complete_fixture}" "build-complete"
    complete_result complete_output)
if(NOT complete_result EQUAL 0)
    list(APPEND test_failures
        "完整 fake SDK 配置失败：${complete_output}")
else()
    set(identity_path "${test_root}/build-complete/xen-build-identity.json")
    set(runtime_manifest
        "${test_root}/build-complete/xen-runtime-authorized-Release.cmake")
    if(NOT EXISTS "${identity_path}")
        list(APPEND test_failures "完整配置未生成 build identity")
    else()
        file(READ "${identity_path}" identity_content)
        string(REGEX MATCH
            "\"components\"[ \t\r\n]*:[ \t\r\n]*\\[([^]]*)\\]"
            identity_components_match "${identity_content}")
        if(identity_components_match STREQUAL "")
            list(APPEND test_failures
                "build identity 未声明稳定 components")
        else()
            set(component_body "${CMAKE_MATCH_1}")
            string(REGEX MATCHALL "\"[a-z0-9][a-z0-9._-]*\""
                component_tokens "${component_body}")
            set(actual_component_ids)
            foreach(component_token IN LISTS component_tokens)
                string(REPLACE "\"" "" component_id "${component_token}")
                list(APPEND actual_component_ids "${component_id}")
            endforeach()
            list(REMOVE_DUPLICATES actual_component_ids)
            list(SORT actual_component_ids)
            if(NOT actual_component_ids STREQUAL expected_component_ids)
                list(APPEND test_failures
                    "components 不是当前依赖稳定 ID：${actual_component_ids}")
            endif()
            if("nvidia" IN_LIST actual_component_ids)
                list(APPEND test_failures
                    "runtime id 不得冒充 dependency component id")
            endif()
            foreach(component_id IN LISTS actual_component_ids)
                if(component_id MATCHES "\\.dll$")
                    list(APPEND test_failures
                        "DLL 文件名不得冒充 component id：${component_id}")
                endif()
            endforeach()
        endif()
    endif()
    if(NOT EXISTS "${runtime_manifest}")
        list(APPEND test_failures "完整配置未生成 Release 运行库授权清单")
    else()
        file(READ "${runtime_manifest}" runtime_manifest_content)
        string(FIND "${runtime_manifest_content}"
            "unrelated-helper.dll" unrelated_index)
        if(NOT unrelated_index EQUAL -1)
            list(APPEND test_failures
                "无关 nested DLL 进入了正式运行库授权清单")
        endif()
        string(FIND "${runtime_manifest_content}"
            "nvinfer_builder_resource_future_${XEN_TENSORRT_MAJOR}.dll"
            future_resource_index)
        if(NOT future_resource_index EQUAL -1)
            list(APPEND test_failures
                "未批准的 TensorRT builder resource 进入了正式运行库授权清单")
        endif()
    endif()
endif()

set(missing_fixture "${test_root}/missing-sdk")
make_fake_sdk("${missing_fixture}" "cudnn_ops64_9.dll" "")
run_root_configure("${missing_fixture}" "build-missing"
    missing_result missing_output)
if(missing_result EQUAL 0 OR
   NOT missing_output MATCHES
       "Required.*cuDNN.*cudnn_ops64_9\\.dll|cudnn_ops64_9\\.dll.*required")
    list(APPEND test_failures
        "缺少必需 cuDNN component 文件时未在配置期失败：${missing_output}")
endif()

set(missing_tensorrt_fixture "${test_root}/missing-tensorrt-sdk")
make_fake_sdk("${missing_tensorrt_fixture}" "" "sm120")
run_root_configure("${missing_tensorrt_fixture}" "build-missing-tensorrt"
    missing_tensorrt_result missing_tensorrt_output)
if(missing_tensorrt_result EQUAL 0 OR
   NOT missing_tensorrt_output MATCHES
       "Required.*TensorRT.*nvinfer_builder_resource_sm120_${XEN_TENSORRT_MAJOR}\\.dll|nvinfer_builder_resource_sm120_${XEN_TENSORRT_MAJOR}\\.dll.*required")
    list(APPEND test_failures
        "缺少当前必需 TensorRT builder resource 时未在配置期失败：${missing_tensorrt_output}")
endif()

execute_process(
    COMMAND "${windows_powershell}" -NoProfile -ExecutionPolicy Bypass
        -File "${path_safety_script}"
        -Action Remove
        -BasePath "${test_base}"
        -RepositoryRoot "${source_dir}"
        -RootPath "${test_root}"
        -OwnerId "${owner_id}"
    RESULT_VARIABLE owner_remove_result
    OUTPUT_VARIABLE owner_remove_output
    ERROR_VARIABLE owner_remove_error)
if(NOT owner_remove_result EQUAL 0)
    message(FATAL_ERROR
        "runtime component owner 目录清理失败：${owner_remove_error}")
endif()

if(test_failures)
    list(JOIN test_failures "\n---\n" failure_text)
    message(FATAL_ERROR "运行库 component producer 合同未闭合：\n${failure_text}")
endif()

message(STATUS
    "运行库 producer nested allowlist、必需文件与 build identity components 测试通过。")
