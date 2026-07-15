cmake_minimum_required(VERSION 3.24)

project(Xen LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Xen currently supports Windows builds only.")
endif()

if(NOT CMAKE_GENERATOR MATCHES "^Ninja")
    message(FATAL_ERROR "Only Ninja generators are supported by the automated build. Current generator: ${CMAKE_GENERATOR}")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(AIMBOT_USE_CUDA "Enable CUDA backend (USE_CUDA)" OFF)
option(AIMBOT_COPY_RUNTIME_DLLS "Copy required runtime DLLs next to Xen.exe after build" ON)

# 每次正式配置都生成可写入诊断 CSV 的构建身份。Git 工作区有未提交修改时追加
# -dirty，避免现场数据只凭目录名猜测实际运行版本。
find_package(Git QUIET)
set(AIMBOT_BUILD_REVISION "unknown")
if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE AIMBOT_BUILD_REVISION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" status --porcelain
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE _aimbot_git_status
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(NOT "${_aimbot_git_status}" STREQUAL "")
        string(APPEND AIMBOT_BUILD_REVISION "-dirty")
    endif()
endif()
string(TIMESTAMP AIMBOT_BUILD_TIMESTAMP_UTC "%Y%m%dT%H%M%SZ" UTC)
if(AIMBOT_USE_CUDA)
    set(AIMBOT_BUILD_BACKEND "CUDA")
else()
    set(AIMBOT_BUILD_BACKEND "DML")
endif()

if(AIMBOT_USE_CUDA)
    unset(AIMBOT_PACKAGES_DIR CACHE)
    unset(AIMBOT_OPENCV_DML_ROOT CACHE)
    unset(AIMBOT_ONNXRUNTIME_DIR CACHE)
    unset(AIMBOT_DIRECTML_DIR CACHE)
    set(AIMBOT_OPENCV_CUDA_ROOT
        "${CMAKE_SOURCE_DIR}/Xen/modules/opencv/build/cuda"
        CACHE PATH "Path to CUDA OpenCV install root")
    set(AIMBOT_TENSORRT_ROOT "" CACHE PATH "Path to TensorRT root directory")
    set(AIMBOT_CUDNN_ROOT "$ENV{ProgramW6432}/NVIDIA/CUDNN/v9.17" CACHE PATH "Path to cuDNN root directory (optional)")
else()
    unset(AIMBOT_OPENCV_CUDA_ROOT CACHE)
    unset(AIMBOT_TENSORRT_ROOT CACHE)
    unset(AIMBOT_CUDNN_ROOT CACHE)
    set(AIMBOT_PACKAGES_DIR "${CMAKE_SOURCE_DIR}/packages" CACHE PATH "Directory with restored NuGet packages")
    set(AIMBOT_OPENCV_DML_ROOT
        "${CMAKE_SOURCE_DIR}/Xen/modules/opencv/build/dml"
        CACHE PATH "Path to DML OpenCV build root")
endif()
set(AIMBOT_OPENCV_INCLUDE_DIR "" CACHE PATH "Path to OpenCV include directory")
set(AIMBOT_OPENCV_LIBRARY "" CACHE FILEPATH "Path to opencv_world*.lib")
set(AIMBOT_OPENCV_DLL "" CACHE FILEPATH "Path to opencv_world*.dll")
set(AIMBOT_GHUB_MOUSE_DLL "${CMAKE_SOURCE_DIR}/Xen/ghub_mouse.dll" CACHE FILEPATH "Path to ghub_mouse.dll")
set(AIMBOT_RZCTL_DLL "${CMAKE_SOURCE_DIR}/Xen/rzctl.dll" CACHE FILEPATH "Path to rzctl.dll")
set(AIMBOT_CPPWINRT_INCLUDE_DIR "" CACHE PATH "Path to Windows SDK cppwinrt include directory")
set(AIMBOT_NDI_ROOT "$ENV{ProgramW6432}/NDI/NDI 6 SDK" CACHE PATH "Path to NDI SDK root directory")

# Add NDI SDK include and lib paths if available
if(EXISTS "${AIMBOT_NDI_ROOT}/Include/Processing.NDI.Lib.h")
    list(APPEND AIMBOT_EXTRA_INCLUDE_DIRS "${AIMBOT_NDI_ROOT}/Include")
    list(APPEND AIMBOT_EXTRA_LINK_DIRS "${AIMBOT_NDI_ROOT}/Lib/x64")
    add_compile_definitions(AIMBOT_HAS_NDI)
endif()

function(aimbot_pick_latest_dir out_var glob_pattern)
    file(GLOB _candidates LIST_DIRECTORIES true "${glob_pattern}")
    set(_dirs "")
    foreach(_candidate IN LISTS _candidates)
        if(IS_DIRECTORY "${_candidate}")
            list(APPEND _dirs "${_candidate}")
        endif()
    endforeach()
    if(_dirs)
        list(SORT _dirs COMPARE NATURAL ORDER DESCENDING)
        list(GET _dirs 0 _latest)
    else()
        set(_latest "")
    endif()
    set(${out_var} "${_latest}" PARENT_SCOPE)
endfunction()

function(aimbot_find_opencv out_include out_library out_dll)
    set(_found_include "")
    set(_found_library "")
    set(_found_dll "")

    foreach(_root IN LISTS ARGN)
        if(_found_library)
            break()
        endif()
        if(NOT EXISTS "${_root}/include/opencv2/opencv.hpp")
            continue()
        endif()

        file(GLOB _vc_dirs LIST_DIRECTORIES true "${_root}/x64/vc*")
        if(NOT _vc_dirs)
            continue()
        endif()
        list(SORT _vc_dirs COMPARE NATURAL ORDER DESCENDING)

        foreach(_vc_dir IN LISTS _vc_dirs)
            file(GLOB _opencv_world_libs "${_vc_dir}/lib/opencv_world*.lib")
            if(NOT _opencv_world_libs)
                continue()
            endif()
            list(SORT _opencv_world_libs COMPARE NATURAL ORDER DESCENDING)
            foreach(_lib IN LISTS _opencv_world_libs)
                get_filename_component(_stem "${_lib}" NAME_WE)
                if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT CMAKE_CONFIGURATION_TYPES)
                    if(_stem MATCHES "d$")
                        continue()
                    endif()
                endif()
                if(CMAKE_CONFIGURATION_TYPES)
                    if(_stem MATCHES "d$")
                        continue()
                    endif()
                endif()
                set(_dll "${_vc_dir}/bin/${_stem}.dll")
                if(EXISTS "${_dll}")
                    set(_found_include "${_root}/include")
                    set(_found_library "${_lib}")
                    set(_found_dll "${_dll}")
                    break()
                endif()
            endforeach()
            if(_found_library)
                break()
            endif()
        endforeach()
    endforeach()

    set(${out_include} "${_found_include}" PARENT_SCOPE)
    set(${out_library} "${_found_library}" PARENT_SCOPE)
    set(${out_dll} "${_found_dll}" PARENT_SCOPE)
endfunction()

function(aimbot_require_file path description)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "${description} not found: ${path}")
    endif()
endfunction()

function(aimbot_copy_if_exists target path)
    if(EXISTS "${path}")
        add_custom_command(
            TARGET ${target}
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${path}" "$<TARGET_FILE_DIR:${target}>"
            VERBATIM
        )
    endif()
endfunction()

# 发布目录必须由可追溯的依赖清单生成，不能依赖构建目录里碰巧存在的旧 DLL。
# 这里对跨设备运行必需的文件采用强校验：源文件缺失时立即停止配置，避免产出残缺发布包。
function(aimbot_copy_required_runtime target path description)
    aimbot_require_file("${path}" "${description}")
    add_custom_command(
        TARGET ${target}
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${path}" "$<TARGET_FILE_DIR:${target}>"
        VERBATIM
    )
endfunction()

function(aimbot_remove_runtime_files target)
    set(_script "${CMAKE_CURRENT_BINARY_DIR}/${target}_remove_stale_runtime.cmake")
    file(WRITE "${_script}" "if(NOT DEFINED TARGET_DIR)\n  message(FATAL_ERROR \"TARGET_DIR is not set\")\nendif()\nset(_patterns\n")
    foreach(_pattern IN LISTS ARGN)
        file(APPEND "${_script}" "  \"${_pattern}\"\n")
    endforeach()
    file(APPEND "${_script}" [=[
)
foreach(_pattern IN LISTS _patterns)
  file(GLOB _matches "${TARGET_DIR}/${_pattern}")
  if(_matches)
    file(REMOVE ${_matches})
  endif()
endforeach()
]=])
    add_custom_command(
        TARGET ${target}
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} "-DTARGET_DIR=$<TARGET_FILE_DIR:${target}>" -P "${_script}"
        VERBATIM
    )
endfunction()

if(AIMBOT_USE_CUDA)
    aimbot_pick_latest_dir(_tensorrt_default "${CMAKE_SOURCE_DIR}/Xen/modules/TensorRT-*")
    if(AIMBOT_TENSORRT_ROOT STREQUAL "" OR
       NOT EXISTS "${AIMBOT_TENSORRT_ROOT}/include/NvInfer.h" OR
       NOT EXISTS "${AIMBOT_TENSORRT_ROOT}/lib/nvinfer_10.lib")
        set(AIMBOT_TENSORRT_ROOT "${_tensorrt_default}" CACHE PATH "Path to TensorRT root directory" FORCE)
    endif()
endif()

if(NOT AIMBOT_USE_CUDA)
    aimbot_pick_latest_dir(_onnxruntime_default "${AIMBOT_PACKAGES_DIR}/Microsoft.ML.OnnxRuntime.DirectML.*")
    aimbot_pick_latest_dir(_directml_default "${AIMBOT_PACKAGES_DIR}/Microsoft.AI.DirectML.*")
    set(AIMBOT_ONNXRUNTIME_DIR "${_onnxruntime_default}" CACHE PATH "Path to Microsoft.ML.OnnxRuntime.DirectML.* package")
    set(AIMBOT_DIRECTML_DIR "${_directml_default}" CACHE PATH "Path to Microsoft.AI.DirectML.* package")
endif()

if(AIMBOT_USE_CUDA)
    set(_opencv_candidate_roots
        "${CMAKE_SOURCE_DIR}/Xen/modules/opencv/build/cuda")
else()
    set(_opencv_candidate_roots
        "${AIMBOT_OPENCV_DML_ROOT}"
        "${CMAKE_SOURCE_DIR}/Xen/modules/opencv/prebuilt/opencv/build")
endif()

if(AIMBOT_OPENCV_INCLUDE_DIR STREQUAL "" OR
   AIMBOT_OPENCV_LIBRARY STREQUAL "" OR
   AIMBOT_OPENCV_DLL STREQUAL "" OR
   NOT EXISTS "${AIMBOT_OPENCV_INCLUDE_DIR}/opencv2/opencv.hpp" OR
   NOT EXISTS "${AIMBOT_OPENCV_LIBRARY}" OR
   NOT EXISTS "${AIMBOT_OPENCV_DLL}")
    aimbot_find_opencv(_opencv_include_detected _opencv_library_detected _opencv_dll_detected ${_opencv_candidate_roots})
    if(NOT _opencv_library_detected STREQUAL "")
        set(AIMBOT_OPENCV_INCLUDE_DIR "${_opencv_include_detected}" CACHE PATH "Path to OpenCV include directory" FORCE)
        set(AIMBOT_OPENCV_LIBRARY "${_opencv_library_detected}" CACHE FILEPATH "Path to opencv_world*.lib" FORCE)
        set(AIMBOT_OPENCV_DLL "${_opencv_dll_detected}" CACHE FILEPATH "Path to opencv_world*.dll" FORCE)
    endif()
endif()

if(AIMBOT_CPPWINRT_INCLUDE_DIR STREQUAL "")
    if(DEFINED CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION AND DEFINED ENV{WindowsSdkDir})
        file(TO_CMAKE_PATH "$ENV{WindowsSdkDir}" _winsdk_dir)
        set(AIMBOT_CPPWINRT_INCLUDE_DIR "${_winsdk_dir}/Include/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/cppwinrt")
    endif()
endif()

if(AIMBOT_CPPWINRT_INCLUDE_DIR STREQUAL "")
    if(DEFINED CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
        set(_vs_sdk_candidate "C:/Program Files (x86)/Windows Kits/10/Include/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/cppwinrt")
        if(EXISTS "${_vs_sdk_candidate}/winrt/base.h")
            set(AIMBOT_CPPWINRT_INCLUDE_DIR "${_vs_sdk_candidate}")
        endif()
    endif()
endif()

if(AIMBOT_CPPWINRT_INCLUDE_DIR STREQUAL "")
    file(GLOB _winsdk_cppwinrt_candidates LIST_DIRECTORIES true "C:/Program Files (x86)/Windows Kits/10/Include/*/cppwinrt")
    if(_winsdk_cppwinrt_candidates)
        list(SORT _winsdk_cppwinrt_candidates COMPARE NATURAL ORDER DESCENDING)
        list(GET _winsdk_cppwinrt_candidates 0 AIMBOT_CPPWINRT_INCLUDE_DIR)
    endif()
endif()

if(NOT AIMBOT_USE_CUDA)
    if(AIMBOT_ONNXRUNTIME_DIR STREQUAL "")
        message(FATAL_ERROR "Microsoft.ML.OnnxRuntime.DirectML package directory was not found. Set AIMBOT_ONNXRUNTIME_DIR manually.")
    endif()
    if(AIMBOT_DIRECTML_DIR STREQUAL "")
        message(FATAL_ERROR "Microsoft.AI.DirectML package directory was not found. Set AIMBOT_DIRECTML_DIR manually.")
    endif()
endif()

set(SRC_DIR "${CMAKE_SOURCE_DIR}/Xen")
set_property(SOURCE "${SRC_DIR}/runtime/build_identity.cpp" APPEND PROPERTY COMPILE_DEFINITIONS
    AIMBOT_BUILD_BACKEND="${AIMBOT_BUILD_BACKEND}"
    AIMBOT_BUILD_REVISION="${AIMBOT_BUILD_REVISION}"
    AIMBOT_BUILD_TIMESTAMP_UTC="${AIMBOT_BUILD_TIMESTAMP_UTC}"
)

set(AIMBOT_SOURCES
    "${SRC_DIR}/benchmarks/provider_benchmark.cpp"
    "${SRC_DIR}/capture/capture.cpp"
    "${SRC_DIR}/capture/duplication_api_capture.cpp"
    "${SRC_DIR}/capture/udp_capture.cpp"
    "${SRC_DIR}/capture/virtual_camera.cpp"
    "${SRC_DIR}/capture/winrt_capture.cpp"
    "${SRC_DIR}/capture/ndi_capture.cpp"
    "${SRC_DIR}/capture/ndi_frame_geometry.cpp"
    "${SRC_DIR}/config/config.cpp"
    "${SRC_DIR}/detector/detection_buffer.cpp"
    "${SRC_DIR}/detector/detection_filters.cpp"
    "${SRC_DIR}/detector/postProcess.cpp"
    "${SRC_DIR}/imgui/imgui.cpp"
    "${SRC_DIR}/imgui/imgui_draw.cpp"
    "${SRC_DIR}/imgui/imgui_impl_dx11.cpp"
    "${SRC_DIR}/imgui/imgui_impl_win32.cpp"
    "${SRC_DIR}/imgui/imgui_tables.cpp"
    "${SRC_DIR}/imgui/imgui_widgets.cpp"
    "${SRC_DIR}/keyboard/keyboard_listener.cpp"
    "${SRC_DIR}/keyboard/keycodes.cpp"
    "${SRC_DIR}/mem/cpu_affinity_manager.cpp"
    "${SRC_DIR}/debug/pipeline_tracer.cpp"
    "${SRC_DIR}/modules/makcu/src/makcu.cpp"
    "${SRC_DIR}/modules/makcu/src/serialport.cpp"
    "${SRC_DIR}/mouse/ghub.cpp"
    "${SRC_DIR}/mouse/hid.c"
    "${SRC_DIR}/mouse/KmboxAConnection.cpp"
    "${SRC_DIR}/mouse/kmboxA.cpp"
    "${SRC_DIR}/mouse/kmboxNetConnection.cpp"
    "${SRC_DIR}/mouse/kmbox_net/kmboxNet.cpp"
    "${SRC_DIR}/mouse/Makcu.cpp"
    "${SRC_DIR}/mouse/MouseInput.cpp"
    "${SRC_DIR}/mouse/mouse.cpp"
    "${SRC_DIR}/mouse/rzctl.cpp"
    "${SRC_DIR}/mouse/AimbotTarget.cpp"
    "${SRC_DIR}/overlay/draw_ai.cpp"
    "${SRC_DIR}/overlay/draw_buttons.cpp"
    "${SRC_DIR}/overlay/draw_capture.cpp"
    "${SRC_DIR}/overlay/config_dirty.cpp"
    "${SRC_DIR}/overlay/draw_debug.cpp"
    "${SRC_DIR}/overlay/draw_depth.cpp"
    "${SRC_DIR}/overlay/draw_game_overlay.cpp"
    "${SRC_DIR}/overlay/draw_mouse.cpp"
    "${SRC_DIR}/overlay/draw_overlay.cpp"
    "${SRC_DIR}/overlay/draw_pipeline_tracer.cpp"
    "${SRC_DIR}/overlay/draw_stats.cpp"
    "${SRC_DIR}/overlay/draw_target.cpp"
    "${SRC_DIR}/overlay/Game_overlay.cpp"
    "${SRC_DIR}/overlay/overlay.cpp"
    "${SRC_DIR}/runtime/thread_loops_shared.cpp"
    "${SRC_DIR}/runtime/aim_pipeline_runtime.cpp"
    "${SRC_DIR}/runtime/applied_view_motion_model.cpp"
    "${SRC_DIR}/runtime/relative_los_kalman.cpp"
    "${SRC_DIR}/runtime/los_aim_controller.cpp"
    "${SRC_DIR}/runtime/command_trajectory_shaper.cpp"
    "${SRC_DIR}/runtime/output_scheduler.cpp"
    "${SRC_DIR}/runtime/build_identity.cpp"
    "${SRC_DIR}/runtime/application_shutdown.cpp"
    "${SRC_DIR}/runtime/application_threads.cpp"
    "${SRC_DIR}/runtime/startup_helpers.cpp"
    "${SRC_DIR}/runtime/video_replay_cli.cpp"
    "${SRC_DIR}/runtime/input_device_manager.cpp"
    "${SRC_DIR}/runtime/mouse_thread_loop.cpp"
    "${SRC_DIR}/runtime/game_overlay_loop.cpp"
    "${SRC_DIR}/scr/data_collector.cpp"
    "${SRC_DIR}/scr/other_tools.cpp"
    "${SRC_DIR}/Xen.cpp"
    "${SRC_DIR}/Xen.rc"
)

if(AIMBOT_USE_CUDA)
    if(NOT CMAKE_CUDA_FLAGS MATCHES "(^| )--allow-unsupported-compiler($| )")
        set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} --allow-unsupported-compiler")
    endif()
    if(NOT CUDA_NVCC_FLAGS MATCHES "(^| )--allow-unsupported-compiler($| )")
        set(CUDA_NVCC_FLAGS "${CUDA_NVCC_FLAGS} --allow-unsupported-compiler")
    endif()
    enable_language(CUDA)
    list(APPEND AIMBOT_SOURCES
        "${SRC_DIR}/depth/depth_anything_trt.cpp"
        "${SRC_DIR}/depth/depth_mask.cpp"
        "${SRC_DIR}/depth/depth_utils.cpp"
        "${SRC_DIR}/detector/cuda_preprocess.cu"
        "${SRC_DIR}/detector/trt_detector.cpp"
        "${SRC_DIR}/mem/gpu_resource_manager.cpp"
        "${SRC_DIR}/tensorrt/nvinf.cpp"
    )
else()
    list(APPEND AIMBOT_SOURCES
        "${SRC_DIR}/detector/dml_detector.cpp"
    )
endif()

add_executable(ai ${AIMBOT_SOURCES})

include(CTest)
if(BUILD_TESTING)
    add_test(
        NAME xen.build.cuda_architecture
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_SOURCE_DIR}/tests/build_script_tests.ps1"
    )
    add_test(
        NAME xen.analysis.nine_grid
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_SOURCE_DIR}/tests/nine_grid_analysis_tests.ps1"
    )
    add_test(
        NAME xen.analysis.moving_target
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_SOURCE_DIR}/tests/moving_target_analysis_tests.ps1"
    )
    add_test(
        NAME xen.analysis.dml_performance
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_SOURCE_DIR}/tests/dml_performance_analysis_tests.ps1"
    )
    add_test(
        NAME xen.ui.prediction_visibility
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_SOURCE_DIR}/tests/ui_visibility_tests.ps1"
    )

    add_executable(xen_algorithm_tests
        "${CMAKE_SOURCE_DIR}/tests/speed_curve_tests.cpp"
        "${SRC_DIR}/debug/pipeline_tracer.cpp"
        "${SRC_DIR}/capture/ndi_frame_geometry.cpp"
        "${SRC_DIR}/runtime/aim_pipeline_runtime.cpp"
        "${SRC_DIR}/runtime/applied_view_motion_model.cpp"
        "${SRC_DIR}/runtime/relative_los_kalman.cpp"
        "${SRC_DIR}/runtime/los_aim_controller.cpp"
        "${SRC_DIR}/runtime/command_trajectory_shaper.cpp"
        "${SRC_DIR}/runtime/output_scheduler.cpp"
        "${SRC_DIR}/runtime/build_identity.cpp"
    )
    set_target_properties(xen_algorithm_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    target_include_directories(xen_algorithm_tests PRIVATE
        "${SRC_DIR}"
        "${AIMBOT_OPENCV_INCLUDE_DIR}")
    target_link_libraries(xen_algorithm_tests PRIVATE "${AIMBOT_OPENCV_LIBRARY}")
    add_custom_command(TARGET xen_algorithm_tests POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${AIMBOT_OPENCV_DLL}"
            "$<TARGET_FILE_DIR:xen_algorithm_tests>"
        COMMENT "Copying OpenCV runtime for algorithm timing tests")
    target_compile_features(xen_algorithm_tests PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_options(xen_algorithm_tests PRIVATE /utf-8 /W4 /permissive- /EHsc)
    endif()
    add_test(NAME xen.algorithm.basic_tracking COMMAND xen_algorithm_tests)

    add_executable(xen_config_tests
        "${CMAKE_SOURCE_DIR}/tests/config_tests.cpp"
        "${SRC_DIR}/config/config.cpp"
    )
    set_target_properties(xen_config_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/$<CONFIG>")
    target_include_directories(xen_config_tests PRIVATE "${SRC_DIR}")
    target_compile_features(xen_config_tests PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_options(xen_config_tests PRIVATE /utf-8 /W4 /permissive- /EHsc)
    endif()
    add_test(NAME xen.config.auto_derive_preserves_movement COMMAND xen_config_tests)
endif()

set_target_properties(ai PROPERTIES
    OUTPUT_NAME "Xen"
    # Xen.exe 导出的导入库和 .exp 仅供链接使用，不属于可复制到其他电脑的运行目录。
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>"
)

if(MSVC)
    set_target_properties(ai PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()

if(AIMBOT_USE_CUDA)
    set_target_properties(ai PROPERTIES
        CUDA_STANDARD 17
        CUDA_STANDARD_REQUIRED ON
    )
endif()

target_include_directories(ai PRIVATE
    "${SRC_DIR}"
    "${SRC_DIR}/benchmarks"
    "${SRC_DIR}/capture"
    "${SRC_DIR}/config"
    "${SRC_DIR}/detector"
    "${SRC_DIR}/imgui"
    "${SRC_DIR}/keyboard"
    "${SRC_DIR}/include"
    "${SRC_DIR}/mem"
    "${SRC_DIR}/mouse"
    "${SRC_DIR}/overlay"
    "${SRC_DIR}/scr"
    "${SRC_DIR}/modules/makcu/include"
    "${SRC_DIR}/modules/stb"
    "${SRC_DIR}/modules/imgui-1.92.8"
    "${SRC_DIR}/modules/imgui-1.92.8/backends"
    "${SRC_DIR}/third_party/motion_control"
)

target_compile_definitions(ai PRIVATE
    UNICODE
    _UNICODE
    _CONSOLE
    # C++/WinRT still includes the experimental coroutine header in C++17 mode.
    _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS
    NOMINMAX
)

if(MSVC)
    target_compile_options(ai PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:/utf-8>
        $<$<COMPILE_LANGUAGE:CXX>:/W3>
        $<$<COMPILE_LANGUAGE:CXX>:/sdl>
        $<$<COMPILE_LANGUAGE:CXX>:/permissive->
        $<$<COMPILE_LANGUAGE:CXX>:/EHsc>
        $<$<COMPILE_LANGUAGE:CXX>:/wd4819>
    )
endif()

if(AIMBOT_USE_CUDA)
    target_compile_definitions(ai PRIVATE USE_CUDA)
    target_include_directories(ai PRIVATE
        "${SRC_DIR}/depth"
        "${SRC_DIR}/tensorrt"
    )
endif()

if(NOT AIMBOT_USE_CUDA)
    if(NOT EXISTS "${AIMBOT_OPENCV_INCLUDE_DIR}/opencv2/opencv.hpp" OR
       NOT EXISTS "${AIMBOT_OPENCV_LIBRARY}" OR
       NOT EXISTS "${AIMBOT_OPENCV_DLL}")
        message(FATAL_ERROR
            "DML build requires prebuilt OpenCV package.\n"
            "Run: powershell -ExecutionPolicy Bypass -File tools/setup_opencv_dml.ps1\n"
            "Or set custom paths via:\n"
            "  -DAIMBOT_OPENCV_INCLUDE_DIR=...\n"
            "  -DAIMBOT_OPENCV_LIBRARY=...\n"
            "  -DAIMBOT_OPENCV_DLL=...")
    endif()
endif()

aimbot_require_file("${AIMBOT_OPENCV_INCLUDE_DIR}/opencv2/opencv.hpp" "OpenCV headers")
aimbot_require_file("${AIMBOT_OPENCV_LIBRARY}" "OpenCV library")
aimbot_require_file("${AIMBOT_OPENCV_DLL}" "OpenCV runtime DLL")
if(AIMBOT_CPPWINRT_INCLUDE_DIR STREQUAL "")
    message(FATAL_ERROR "C++/WinRT include directory is not set. Set AIMBOT_CPPWINRT_INCLUDE_DIR manually.")
endif()
aimbot_require_file("${AIMBOT_CPPWINRT_INCLUDE_DIR}/winrt/base.h" "C++/WinRT headers")

target_include_directories(ai PRIVATE
    "${AIMBOT_OPENCV_INCLUDE_DIR}"
    "${AIMBOT_CPPWINRT_INCLUDE_DIR}"
)

if(AIMBOT_EXTRA_INCLUDE_DIRS)
    target_include_directories(ai PRIVATE ${AIMBOT_EXTRA_INCLUDE_DIRS})
endif()
if(AIMBOT_EXTRA_LINK_DIRS)
    foreach(_ndi_lib_dir IN LISTS AIMBOT_EXTRA_LINK_DIRS)
        target_link_directories(ai PRIVATE "${_ndi_lib_dir}")
    endforeach()
endif()
if(AIMBOT_HAS_NDI)
    target_link_libraries(ai PRIVATE "Processing.NDI.Lib.x64.lib")
endif()

if(NOT AIMBOT_USE_CUDA)
    aimbot_require_file("${AIMBOT_ONNXRUNTIME_DIR}/build/native/include/onnxruntime_cxx_api.h" "ONNX Runtime headers")
    aimbot_require_file("${AIMBOT_ONNXRUNTIME_DIR}/runtimes/win-x64/native/onnxruntime.lib" "ONNX Runtime import library")
    aimbot_require_file("${AIMBOT_DIRECTML_DIR}/bin/x64-win/DirectML.dll" "DirectML runtime DLL")
    target_include_directories(ai PRIVATE "${AIMBOT_ONNXRUNTIME_DIR}/build/native/include")
endif()

target_link_libraries(ai PRIVATE
    "${AIMBOT_OPENCV_LIBRARY}"
    d3d11
    dxgi
    dwmapi
    d2d1
    setupapi
    WindowsApp
)

if(NOT AIMBOT_USE_CUDA)
    target_link_libraries(ai PRIVATE
        "${AIMBOT_ONNXRUNTIME_DIR}/runtimes/win-x64/native/onnxruntime.lib"
    )
endif()

if(NOT AIMBOT_USE_CUDA AND
   EXISTS "${AIMBOT_DIRECTML_DIR}/include/DirectML.h" AND
   EXISTS "${AIMBOT_DIRECTML_DIR}/bin/x64-win/DirectML.lib")
    target_include_directories(ai PRIVATE "${AIMBOT_DIRECTML_DIR}/include")
    target_link_libraries(ai PRIVATE
        d3d12
        "${AIMBOT_DIRECTML_DIR}/bin/x64-win/DirectML.lib"
    )
    target_compile_definitions(ai PRIVATE AIMBOT_HAS_DIRECTML_EX)
endif()

if(AIMBOT_USE_CUDA)
    find_package(CUDAToolkit REQUIRED)

    # CUDA 13.2 将 Windows 运行库放在 bin/x64；兼容旧工具包仍直接放在 bin 的布局。
    set(_cuda_runtime_dirs
        "${CUDAToolkit_BIN_DIR}/x64"
        "${CUDAToolkit_BIN_DIR}"
        "${CUDAToolkit_LIBRARY_DIR}/../../bin/x64")
    set(_cuda_runtime_dir "")
    foreach(_candidate IN LISTS _cuda_runtime_dirs)
        if(EXISTS "${_candidate}/cublas64_13.dll")
            cmake_path(NORMAL_PATH _candidate OUTPUT_VARIABLE _cuda_runtime_dir)
            break()
        endif()
    endforeach()
    if(_cuda_runtime_dir STREQUAL "")
        message(FATAL_ERROR "CUDA runtime directory containing cublas64_13.dll was not found.")
    endif()

    # OpenCV CUDA 的 PE 依赖包含动态 MSVC CRT；应用自身使用 /MT 不代表 OpenCV 不需要这些 DLL。
    file(GLOB _msvc_redist_candidates LIST_DIRECTORIES true
        "$ENV{ProgramW6432}/Microsoft Visual Studio/*/*/VC/Redist/MSVC/*/x64/Microsoft.VC143.CRT"
        "$ENV{ProgramFiles}/Microsoft Visual Studio/*/*/VC/Redist/MSVC/*/x64/Microsoft.VC143.CRT")
    if(NOT _msvc_redist_candidates)
        message(FATAL_ERROR "Microsoft VC143 x64 redistributable directory was not found.")
    endif()
    list(SORT _msvc_redist_candidates COMPARE NATURAL ORDER DESCENDING)
    list(GET _msvc_redist_candidates 0 _msvc_redist_dir)

    aimbot_require_file("${AIMBOT_TENSORRT_ROOT}/include/NvInfer.h" "TensorRT headers")
    aimbot_require_file("${AIMBOT_TENSORRT_ROOT}/lib/nvinfer_10.lib" "TensorRT nvinfer_10.lib")
    aimbot_require_file("${AIMBOT_TENSORRT_ROOT}/lib/nvonnxparser_10.lib" "TensorRT nvonnxparser_10.lib")

    target_include_directories(ai PRIVATE "${AIMBOT_TENSORRT_ROOT}/include")
    target_link_directories(ai PRIVATE "${AIMBOT_TENSORRT_ROOT}/lib")
    target_link_libraries(ai PRIVATE
        CUDA::cuda_driver
        CUDA::cudart
        nvinfer_10
        nvonnxparser_10
    )

    if(EXISTS "${AIMBOT_CUDNN_ROOT}/include/13.1/cudnn.h")
        target_include_directories(ai PRIVATE "${AIMBOT_CUDNN_ROOT}/include/13.1")
    endif()
endif()

if(AIMBOT_COPY_RUNTIME_DLLS)
    aimbot_copy_if_exists(ai "${AIMBOT_OPENCV_DLL}")
    aimbot_copy_if_exists(ai "${AIMBOT_GHUB_MOUSE_DLL}")
    aimbot_copy_if_exists(ai "${AIMBOT_RZCTL_DLL}")

    if(NOT AIMBOT_USE_CUDA)
        aimbot_copy_if_exists(ai "${AIMBOT_ONNXRUNTIME_DIR}/runtimes/win-x64/native/onnxruntime.dll")
        aimbot_copy_if_exists(ai "${AIMBOT_ONNXRUNTIME_DIR}/runtimes/win-x64/native/onnxruntime_providers_shared.dll")
        aimbot_copy_if_exists(ai "${AIMBOT_ONNXRUNTIME_DIR}/runtimes/win-x64/native/onnxruntime_providers_dml.dll")
        aimbot_copy_if_exists(ai "${AIMBOT_DIRECTML_DIR}/bin/x64-win/DirectML.dll")
    endif()

    if(AIMBOT_USE_CUDA)
        file(GLOB _tensorrt_builder_resources "${AIMBOT_TENSORRT_ROOT}/bin/nvinfer_builder_resource_*.dll")
        if(NOT _tensorrt_builder_resources)
            message(FATAL_ERROR "TensorRT builder resource DLLs were not found in ${AIMBOT_TENSORRT_ROOT}/bin.")
        endif()
        foreach(_resource IN LISTS _tensorrt_builder_resources)
            aimbot_copy_required_runtime(ai "${_resource}" "TensorRT builder resource")
        endforeach()

        # TensorRT core DLLs (from project modules)
        aimbot_copy_if_exists(ai "${AIMBOT_TENSORRT_ROOT}/bin/nvinfer_10.dll")
        aimbot_copy_if_exists(ai "${AIMBOT_TENSORRT_ROOT}/bin/nvonnxparser_10.dll")

        # 这些文件来自 Xen.exe、OpenCV CUDA 及其二级 PE 依赖的实测闭包。
        foreach(_cuda_dll IN ITEMS
                cublas64_13.dll
                cublasLt64_13.dll
                cufft64_12.dll
                nppc64_13.dll
                nppial64_13.dll
                nppicc64_13.dll
                nppidei64_13.dll
                nppig64_13.dll
                nppist64_13.dll
                nppitc64_13.dll)
            aimbot_copy_required_runtime(ai "${_cuda_runtime_dir}/${_cuda_dll}" "CUDA/OpenCV runtime ${_cuda_dll}")
        endforeach()
        foreach(_msvc_dll IN ITEMS
                concrt140.dll
                msvcp140.dll
                vcruntime140.dll
                vcruntime140_1.dll)
            aimbot_copy_required_runtime(ai "${_msvc_redist_dir}/${_msvc_dll}" "MSVC runtime ${_msvc_dll}")
        endforeach()
    endif()
endif()

if(AIMBOT_USE_CUDA)
    aimbot_remove_runtime_files(ai
        "Xen.exp"
        "Xen.lib"
        "xen_*_tests.exe"
        "DirectML.dll"
        "onnxruntime.dll"
        "onnxruntime_providers_*.dll"
        "cpbox_mouse_dll.dll"
        "cudart*.dll"
        "cudnn*.dll"
        "cufftw*.dll"
        "nppif*.dll"
        "nppim*.dll"
        "nppisu*.dll"
        "npps*.dll"
        "nvinfer_dispatch*.dll"
        "nvinfer_lean*.dll"
        "nvinfer_plugin*.dll"
        "nvinfer_vc_plugin*.dll"
        "nvJitLink*.dll"
        "nvrtc*.dll"
        "msvcp140_1.dll"
        "msvcp140_2.dll"
        "msvcp140_atomic_wait.dll"
        "msvcp140_codecvt_ids.dll"
        "vccorlib140.dll"
        "vcruntime140_threads.dll")
else()
    aimbot_remove_runtime_files(ai
        "Xen.exp"
        "Xen.lib"
        "xen_*_tests.exe"
        "cudart*.dll"
        "cudnn*.dll"
        "nvinfer*.dll"
        "nvonnxparser*.dll"
        "onnxruntime_providers_cuda.dll")
endif()

if(BUILD_TESTING)
    if(AIMBOT_USE_CUDA)
        set(_release_backend "CUDA")
    else()
        set(_release_backend "DML")
    endif()
    add_test(
        NAME xen.release.runtime_layout
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tests/release_runtime_tests.ps1"
            -Backend "${_release_backend}"
            -ReleaseDir "$<TARGET_FILE_DIR:ai>")
endif()

if(AIMBOT_USE_CUDA)
    message(STATUS "Aimbot backend: CUDA/TensorRT")
else()
    message(STATUS "Aimbot backend: DirectML")
endif()
message(STATUS "OpenCV include: ${AIMBOT_OPENCV_INCLUDE_DIR}")
message(STATUS "OpenCV lib: ${AIMBOT_OPENCV_LIBRARY}")
if(NOT AIMBOT_USE_CUDA)
    message(STATUS "ONNX Runtime package: ${AIMBOT_ONNXRUNTIME_DIR}")
    message(STATUS "DirectML package: ${AIMBOT_DIRECTML_DIR}")
endif()
message(STATUS "C++/WinRT include: ${AIMBOT_CPPWINRT_INCLUDE_DIR}")
