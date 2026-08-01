cmake_minimum_required(VERSION 3.18)

if(NOT DEFINED XEN_RUNTIME_MANIFEST OR
   NOT DEFINED XEN_RUNTIME_OUTPUT OR
   NOT DEFINED XEN_RUNTIME_LOCK)
    message(FATAL_ERROR
        "Runtime deployment requires manifest, output and lock paths.")
endif()
if(NOT EXISTS "${XEN_RUNTIME_MANIFEST}")
    message(FATAL_ERROR
        "Runtime deployment manifest does not exist: ${XEN_RUNTIME_MANIFEST}")
endif()

set(XEN_RUNTIME_SOURCES)
include("${XEN_RUNTIME_MANIFEST}")
if(NOT XEN_RUNTIME_SOURCES)
    message(FATAL_ERROR
        "Runtime deployment manifest contains no authorized files: ${XEN_RUNTIME_MANIFEST}")
endif()

file(MAKE_DIRECTORY "${XEN_RUNTIME_OUTPUT}")
# 同一配置的全部可执行目标共享输出目录。部署、清理和报告发布必须持有同一
# 构建树级锁，避免并行构建在校验与复制之间观察到半份运行库闭包。
file(LOCK "${XEN_RUNTIME_LOCK}" GUARD PROCESS TIMEOUT 300
    RESULT_VARIABLE XEN_RUNTIME_LOCK_RESULT)
if(NOT XEN_RUNTIME_LOCK_RESULT STREQUAL "0")
    message(FATAL_ERROR
        "Runtime deployment lock failed: ${XEN_RUNTIME_LOCK_RESULT}")
endif()

function(xen_json_escape input output)
    set(value "${input}")
    string(REPLACE "\\" "\\\\" value "${value}")
    string(REPLACE "\"" "\\\"" value "${value}")
    string(REPLACE "\r" "\\r" value "${value}")
    string(REPLACE "\n" "\\n" value "${value}")
    string(REPLACE "\t" "\\t" value "${value}")
    set(${output} "${value}" PARENT_SCOPE)
endfunction()

# 先完整验证授权清单，再触碰输出目录。若 SDK 路径在配置后失效，本轮部署
# 必须失败并保留上一轮产物，不能先删后报错。
set(XEN_AUTHORIZED_NAMES)
set(XEN_AUTHORIZED_KEYS)
set(XEN_AUTHORIZED_SOURCES)
set(XEN_AUTHORIZED_SOURCE_KEYS)
foreach(runtime_source IN LISTS XEN_RUNTIME_SOURCES)
    if(runtime_source MATCHES "\\$<")
        message(FATAL_ERROR
            "Runtime deployment manifest contains an unresolved generator expression: ${runtime_source}")
    endif()
    if(NOT EXISTS "${runtime_source}")
        message(FATAL_ERROR
            "Authorized runtime source does not exist: ${runtime_source}")
    endif()

    get_filename_component(runtime_source_real "${runtime_source}" REALPATH)
    file(TO_CMAKE_PATH "${runtime_source_real}" runtime_source_real)
    get_filename_component(runtime_name "${runtime_source_real}" NAME)
    if(runtime_name STREQUAL "")
        message(FATAL_ERROR
            "Authorized runtime source has no file name: ${runtime_source_real}")
    endif()

    string(TOLOWER "${runtime_name}" runtime_key)
    string(TOLOWER "${runtime_source_real}" runtime_source_key)
    list(FIND XEN_AUTHORIZED_KEYS "${runtime_key}" existing_index)
    if(NOT existing_index EQUAL -1)
        list(GET XEN_AUTHORIZED_SOURCE_KEYS ${existing_index}
            existing_source_key)
        list(GET XEN_AUTHORIZED_SOURCES ${existing_index}
            existing_source)
        if(NOT existing_source_key STREQUAL runtime_source_key)
            message(FATAL_ERROR
                "Two authorized runtime sources have the same output name ${runtime_name}:\n"
                "  ${existing_source}\n"
                "  ${runtime_source_real}")
        endif()
        continue()
    endif()

    list(APPEND XEN_AUTHORIZED_NAMES "${runtime_name}")
    list(APPEND XEN_AUTHORIZED_KEYS "${runtime_key}")
    list(APPEND XEN_AUTHORIZED_SOURCES "${runtime_source_real}")
    list(APPEND XEN_AUTHORIZED_SOURCE_KEYS "${runtime_source_key}")
endforeach()

set(XEN_RUNTIME_STATE
    "${XEN_RUNTIME_OUTPUT}/xen-runtime-deployment-state.cmake")
set(XEN_RUNTIME_REPORT
    "${XEN_RUNTIME_OUTPUT}/xen-runtime-deployment.json")

# 新版状态文件精确登记 Xen 上轮部署的文件名，可清理未来新增且不符合旧命名
# 规则的运行库。状态文件使用原子替换发布，损坏时失败关闭而不是猜测。
set(XEN_DEPLOYED_RUNTIME_FILES)
if(EXISTS "${XEN_RUNTIME_STATE}")
    include("${XEN_RUNTIME_STATE}")
    if(NOT DEFINED XEN_DEPLOYED_RUNTIME_FILES)
        message(FATAL_ERROR
            "Runtime deployment state is invalid: ${XEN_RUNTIME_STATE}")
    endif()
endif()
foreach(previous_name IN LISTS XEN_DEPLOYED_RUNTIME_FILES)
    get_filename_component(previous_basename "${previous_name}" NAME)
    if(NOT previous_basename STREQUAL previous_name)
        message(FATAL_ERROR
            "Runtime deployment state contains an invalid file name: ${previous_name}")
    endif()
    string(TOLOWER "${previous_name}" previous_key)
    list(FIND XEN_AUTHORIZED_KEYS "${previous_key}" authorized_index)
    if(authorized_index EQUAL -1)
        set(stale_path "${XEN_RUNTIME_OUTPUT}/${previous_name}")
        file(REMOVE "${stale_path}")
        if(EXISTS "${stale_path}")
            message(FATAL_ERROR
                "Failed to remove stale deployed runtime: ${stale_path}")
        endif()
    endif()
endforeach()

# 首次升级没有状态文件，需迁移清理旧 POST_BUILD 规则曾部署的已知家族。
# 只匹配 Xen 明确管理过的名称；输出目录中其他用户文件不在清理范围内。
set(XEN_LEGACY_RUNTIME_PATTERNS
    "onnxruntime*.dll"
    "nvinfer*.dll"
    "nvonnxparser*.dll"
    "cudnn*.dll"
    "cublas*.dll"
    "cufft*.dll"
    "cudart*.dll"
    "DirectML.dll"
    "openvino*.dll"
    "tbb12.dll"
    "opencv_world*.dll"
    "opencv_videoio_ffmpeg*_64.dll"
    "Processing.NDI.Lib.x64.dll"
    "Processing.NDI.Lib.Licenses.txt"
    "cudart.lib")
foreach(runtime_pattern IN LISTS XEN_LEGACY_RUNTIME_PATTERNS)
    file(GLOB legacy_paths LIST_DIRECTORIES FALSE
        "${XEN_RUNTIME_OUTPUT}/${runtime_pattern}")
    foreach(legacy_path IN LISTS legacy_paths)
        get_filename_component(legacy_name "${legacy_path}" NAME)
        string(TOLOWER "${legacy_name}" legacy_key)
        list(FIND XEN_AUTHORIZED_KEYS "${legacy_key}" authorized_index)
        if(authorized_index EQUAL -1)
            file(REMOVE "${legacy_path}")
            if(EXISTS "${legacy_path}")
                message(FATAL_ERROR
                    "Failed to remove unauthorized legacy runtime: ${legacy_path}")
            endif()
        endif()
    endforeach()
endforeach()

set(XEN_AUTHORIZED_HASHES)
list(LENGTH XEN_AUTHORIZED_NAMES authorized_count)
math(EXPR authorized_last "${authorized_count} - 1")
foreach(index RANGE 0 ${authorized_last})
    list(GET XEN_AUTHORIZED_NAMES ${index} runtime_name)
    list(GET XEN_AUTHORIZED_SOURCES ${index} runtime_source)
    set(runtime_output "${XEN_RUNTIME_OUTPUT}/${runtime_name}")

    file(SHA256 "${runtime_source}" source_sha256)
    set(copy_required TRUE)
    if(EXISTS "${runtime_output}")
        file(SHA256 "${runtime_output}" output_sha256)
        if(output_sha256 STREQUAL source_sha256)
            set(copy_required FALSE)
        endif()
    endif()

    if(copy_required)
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${runtime_source}" "${runtime_output}"
            RESULT_VARIABLE copy_result
            ERROR_VARIABLE copy_error)
        if(NOT copy_result EQUAL 0)
            message(FATAL_ERROR
                "Runtime deployment copy failed (${copy_result}): "
                "${runtime_source} -> ${runtime_output}\n${copy_error}")
        endif()
    endif()

    if(NOT EXISTS "${runtime_output}")
        message(FATAL_ERROR
            "Runtime deployment output is missing after copy: ${runtime_output}")
    endif()
    file(SHA256 "${runtime_output}" output_sha256)
    if(NOT output_sha256 STREQUAL source_sha256)
        message(FATAL_ERROR
            "Runtime deployment SHA-256 mismatch: ${runtime_output}")
    endif()
    list(APPEND XEN_AUTHORIZED_HASHES "${source_sha256}")
endforeach()

set(state_content "set(XEN_DEPLOYED_RUNTIME_FILES\n")
foreach(runtime_name IN LISTS XEN_AUTHORIZED_NAMES)
    string(APPEND state_content "    [==[${runtime_name}]==]\n")
endforeach()
string(APPEND state_content ")\n")
set(state_pending "${XEN_RUNTIME_STATE}.pending")
file(WRITE "${state_pending}" "${state_content}")
file(RENAME "${state_pending}" "${XEN_RUNTIME_STATE}")

if(NOT DEFINED XEN_RUNTIME_CONFIGURATION)
    set(XEN_RUNTIME_CONFIGURATION "")
endif()
file(TO_CMAKE_PATH "${XEN_RUNTIME_OUTPUT}" report_output)
file(TO_CMAKE_PATH "${XEN_RUNTIME_MANIFEST}" report_manifest)
xen_json_escape("${XEN_RUNTIME_CONFIGURATION}" report_configuration_json)
xen_json_escape("${report_output}" report_output_json)
xen_json_escape("${report_manifest}" report_manifest_json)
string(CONCAT report_content
    "{\n"
    "  \"schema\": 1,\n"
    "  \"configuration\": \"${report_configuration_json}\",\n"
    "  \"output_directory\": \"${report_output_json}\",\n"
    "  \"authorized_manifest\": \"${report_manifest_json}\",\n"
    "  \"files\": [")
set(report_first TRUE)
foreach(index RANGE 0 ${authorized_last})
    list(GET XEN_AUTHORIZED_NAMES ${index} runtime_name)
    list(GET XEN_AUTHORIZED_SOURCES ${index} runtime_source)
    list(GET XEN_AUTHORIZED_HASHES ${index} runtime_sha256)
    xen_json_escape("${runtime_name}" runtime_name_json)
    xen_json_escape("${runtime_source}" runtime_source_json)
    if(report_first)
        set(report_first FALSE)
        string(APPEND report_content "\n")
    else()
        string(APPEND report_content ",\n")
    endif()
    string(APPEND report_content
        "    {\"name\": \"${runtime_name_json}\", "
        "\"source\": \"${runtime_source_json}\", "
        "\"sha256\": \"${runtime_sha256}\"}")
endforeach()
string(APPEND report_content "\n  ]\n}\n")

set(report_pending "${XEN_RUNTIME_REPORT}.pending")
file(WRITE "${report_pending}" "${report_content}")
file(RENAME "${report_pending}" "${XEN_RUNTIME_REPORT}")
