cmake_minimum_required(VERSION 3.18)

if(NOT DEFINED XEN_DEPLOY_SCRIPT OR NOT EXISTS "${XEN_DEPLOY_SCRIPT}")
    message(FATAL_ERROR "缺少运行库部署脚本：${XEN_DEPLOY_SCRIPT}")
endif()
if(NOT DEFINED XEN_DEPLOY_TEST_ROOT OR XEN_DEPLOY_TEST_ROOT STREQUAL "")
    message(FATAL_ERROR "缺少运行库部署测试目录。")
endif()

get_filename_component(test_base "${XEN_DEPLOY_TEST_ROOT}" ABSOLUTE)
get_filename_component(repository_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(path_safety_script
    "${repository_root}/scripts/invoke_path_safety.ps1")
set(windows_powershell
    "$ENV{SystemRoot}/System32/WindowsPowerShell/v1.0/powershell.exe")
if(NOT EXISTS "${windows_powershell}" OR NOT EXISTS "${path_safety_script}")
    message(FATAL_ERROR "缺少 Windows PowerShell 或路径 owner adapter。")
endif()
execute_process(
    COMMAND "${windows_powershell}" -NoProfile -ExecutionPolicy Bypass
        -File "${path_safety_script}"
        -Action New
        -BasePath "${test_base}"
        -RepositoryRoot "${repository_root}"
    RESULT_VARIABLE owner_create_result
    OUTPUT_VARIABLE owner_create_output
    ERROR_VARIABLE owner_create_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT owner_create_result EQUAL 0)
    message(FATAL_ERROR
        "无法创建 owner 测试目录：${owner_create_error}")
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

set(source_a "${test_root}/source-a")
set(source_b "${test_root}/source-b")
set(output_directory "${test_root}/output")
set(lock_path "${test_root}/runtime-deployment.lock")
set(manifest_a "${test_root}/manifest-a.cmake")
set(manifest_b "${test_root}/manifest-b.cmake")

file(MAKE_DIRECTORY "${source_a}" "${source_b}" "${output_directory}")

function(assert_exists path description)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "${description}：${path}")
    endif()
endfunction()

function(assert_absent path description)
    if(EXISTS "${path}")
        message(FATAL_ERROR "${description}：${path}")
    endif()
endfunction()

function(assert_same_sha256 actual expected description)
    assert_exists("${actual}" "${description}，实际文件不存在")
    assert_exists("${expected}" "${description}，参考文件不存在")
    file(SHA256 "${actual}" actual_sha256)
    file(SHA256 "${expected}" expected_sha256)
    if(NOT actual_sha256 STREQUAL expected_sha256)
        message(FATAL_ERROR
            "${description}，SHA-256 不一致：${actual_sha256} != ${expected_sha256}")
    endif()
endfunction()

function(run_deployment manifest configuration)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DXEN_RUNTIME_MANIFEST=${manifest}"
            "-DXEN_RUNTIME_OUTPUT=${output_directory}"
            "-DXEN_RUNTIME_LOCK=${lock_path}"
            "-DXEN_RUNTIME_CONFIGURATION=${configuration}"
            -P "${XEN_DEPLOY_SCRIPT}"
        RESULT_VARIABLE deploy_result
        OUTPUT_VARIABLE deploy_output
        ERROR_VARIABLE deploy_error)
    if(NOT deploy_result EQUAL 0)
        message(FATAL_ERROR
            "运行库部署测试失败 (${deploy_result})：\n${deploy_output}\n${deploy_error}")
    endif()
endfunction()

# 第一阶段模拟启用了 TensorRT/cuDNN 的构建，并额外放入一个只能依靠状态
# 文件追踪的通用运行库，验证后续不仅依赖历史文件名模式清理。
file(WRITE "${source_a}/onnxruntime.dll" "ort-source-a")
file(WRITE "${source_a}/nvinfer_10.dll" "tensorrt-source-a")
file(WRITE "${source_a}/cudnn64_9.dll" "cudnn-source-a")
file(WRITE "${source_a}/custom_runtime.dll" "tracked-custom-source-a")
file(WRITE "${manifest_a}"
    "set(XEN_RUNTIME_SOURCES\n"
    "    [==[${source_a}/onnxruntime.dll]==]\n"
    "    [==[${source_a}/nvinfer_10.dll]==]\n"
    "    [==[${source_a}/cudnn64_9.dll]==]\n"
    "    [==[${source_a}/custom_runtime.dll]==]\n"
    ")\n")

run_deployment("${manifest_a}" "fixture-a")
foreach(first_name IN ITEMS
        onnxruntime.dll
        nvinfer_10.dll
        cudnn64_9.dll
        custom_runtime.dll)
    assert_exists("${output_directory}/${first_name}"
        "第一阶段未部署授权运行库")
endforeach()
assert_same_sha256(
    "${output_directory}/onnxruntime.dll"
    "${source_a}/onnxruntime.dll"
    "第一阶段同名运行库来源错误")

# 这些文件没有进入新版状态，模拟升级前 POST_BUILD 遗留物；普通 user.dll
# 不属于 Xen 管理家族，第二阶段必须原样保留。
file(WRITE "${output_directory}/nvinfer_11.dll" "legacy-tensorrt")
file(WRITE "${output_directory}/cudnn_legacy.dll" "legacy-cudnn")
file(WRITE "${output_directory}/cudart.lib" "legacy-import-library")
file(WRITE "${output_directory}/user.dll" "user-owned-file")

# 第二阶段在同一输出目录清空 NVIDIA SDK，并把 onnxruntime.dll 切换到
# 另一来源。部署必须移除所有未授权旧项并按 SHA-256 覆盖同名文件。
file(WRITE "${source_b}/onnxruntime.dll" "ort-source-b-with-different-content")
file(WRITE "${source_b}/DirectML.dll" "directml-source-b")
file(WRITE "${manifest_b}"
    "set(XEN_RUNTIME_SOURCES\n"
    "    [==[${source_b}/onnxruntime.dll]==]\n"
    "    [==[${source_b}/DirectML.dll]==]\n"
    ")\n")

run_deployment("${manifest_b}" "fixture-b")
foreach(stale_name IN ITEMS
        nvinfer_10.dll
        nvinfer_11.dll
        cudnn64_9.dll
        cudnn_legacy.dll
        cudart.lib
        custom_runtime.dll)
    assert_absent("${output_directory}/${stale_name}"
        "第二阶段仍残留未授权运行库")
endforeach()
assert_exists("${output_directory}/user.dll"
    "部署清理误删了非 Xen 管理文件")
assert_exists("${output_directory}/DirectML.dll"
    "第二阶段未部署新的授权运行库")
assert_same_sha256(
    "${output_directory}/onnxruntime.dll"
    "${source_b}/onnxruntime.dll"
    "第二阶段同名运行库未切换到当前来源")

set(report_path "${output_directory}/xen-runtime-deployment.json")
set(state_path "${output_directory}/xen-runtime-deployment-state.cmake")
assert_exists("${report_path}" "缺少运行库来源与哈希报告")
assert_exists("${state_path}" "缺少运行库部署状态")
file(READ "${report_path}" report_content)
string(FIND "${report_content}" "\n;" invalid_json_separator)
if(NOT invalid_json_separator EQUAL -1)
    message(FATAL_ERROR "部署报告包含 CMake 列表分隔符，不是有效 JSON。")
endif()
file(SHA256 "${source_b}/onnxruntime.dll" expected_ort_sha256)
foreach(required_text IN ITEMS
        "\"schema\": 1"
        "\"configuration\": \"fixture-b\""
        "\"name\": \"onnxruntime.dll\""
        "\"name\": \"DirectML.dll\""
        "${source_b}/onnxruntime.dll"
        "${expected_ort_sha256}")
    string(FIND "${report_content}" "${required_text}" required_index)
    if(required_index EQUAL -1)
        message(FATAL_ERROR
            "部署报告缺少当前来源或 SHA-256：${required_text}")
    endif()
endforeach()
foreach(forbidden_text IN ITEMS
        "nvinfer_10.dll"
        "cudnn64_9.dll"
        "custom_runtime.dll"
        "${source_a}/onnxruntime.dll")
    string(FIND "${report_content}" "${forbidden_text}" forbidden_index)
    if(NOT forbidden_index EQUAL -1)
        message(FATAL_ERROR
            "部署报告仍包含上一配置记录：${forbidden_text}")
    endif()
endforeach()

execute_process(
    COMMAND "${windows_powershell}" -NoProfile -ExecutionPolicy Bypass
        -File "${path_safety_script}"
        -Action Remove
        -BasePath "${test_base}"
        -RepositoryRoot "${repository_root}"
        -RootPath "${test_root}"
        -OwnerId "${owner_id}"
    RESULT_VARIABLE owner_remove_result
    OUTPUT_VARIABLE owner_remove_output
    ERROR_VARIABLE owner_remove_error)
if(NOT owner_remove_result EQUAL 0)
    message(FATAL_ERROR
        "owner 测试目录清理失败：${owner_remove_error}")
endif()

message(STATUS "运行库授权清单、陈旧清理、来源切换与 SHA-256 报告测试通过。")
