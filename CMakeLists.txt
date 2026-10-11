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

include("${PROJECT_SOURCE_DIR}/scripts/resolve_onnxruntime.cmake")

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

# 编译器检查属于真实工程，元数据夹具不能关闭或覆盖这些检查。
if(XEN_ONNXRUNTIME_HAS_CUDA)
    if(XEN_CUDA_ROOT)
        set(CUDAToolkit_ROOT "${XEN_CUDA_ROOT}")
    endif()
    find_package(CUDAToolkit REQUIRED)
    # 仅 CUDA/TensorRT ORT 构建启用 CUDA 编译器。DirectML 独立构建不探测
    # nvcc，也不会把 CUDA kernel 或运行库带入产物。
    enable_language(CUDA)
    set(XEN_CUDA_ARCHITECTURES "75;80;86;89;90;100;120" CACHE STRING
        "CUDA architectures for Xen detector preprocessing kernels")
endif()

# 只从当前 VS 的官方 redist 目录部署 Release CRT，不复制 System32 或调试库。
# 使用 3.18 已有的模块接口；模块无法识别当前 toolset 时明确失败。
set(XEN_MSVC_REDIST_ROOT "" CACHE PATH "Official Visual C++ x64 CRT redist directory")
set(XEN_MSVC_RUNTIME_DLLS)
if(MSVC)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
    set(CMAKE_INSTALL_DEBUG_LIBRARIES FALSE)
    set(CMAKE_INSTALL_DEBUG_LIBRARIES_ONLY FALSE)
    set(CMAKE_INSTALL_UCRT_LIBRARIES FALSE)
    set(CMAKE_INSTALL_MFC_LIBRARIES FALSE)
    set(CMAKE_INSTALL_OPENMP_LIBRARIES FALSE)
    include(InstallRequiredSystemLibraries)
    if(NOT XEN_MSVC_REDIST_ROOT)
        foreach(runtime_file IN LISTS CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
            get_filename_component(runtime_name "${runtime_file}" NAME)
            if(runtime_name STREQUAL "msvcp140.dll")
                get_filename_component(XEN_MSVC_REDIST_ROOT "${runtime_file}" DIRECTORY)
                set(XEN_MSVC_REDIST_ROOT "${XEN_MSVC_REDIST_ROOT}" CACHE PATH
                    "Official Visual C++ x64 CRT redist directory" FORCE)
                break()
            endif()
        endforeach()
    endif()
endif()

set(XEN_RUNTIME_USES_MSVC "${MSVC}")
set(XEN_RUNTIME_LINKER "${CMAKE_LINKER}")
include("${PROJECT_SOURCE_DIR}/scripts/runtime_metadata.cmake")

function(xen_record_artifact_identity runtime_target)
    add_custom_command(TARGET ${runtime_target} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            "-DXEN_ARTIFACT=$<TARGET_FILE:${runtime_target}>"
            "-DXEN_IDENTITY=${CMAKE_BINARY_DIR}/xen-build-identity.json"
            "-DXEN_GIT=${GIT_EXECUTABLE}"
            "-DXEN_CONFIGURATION=$<CONFIG>"
            -P "${PROJECT_SOURCE_DIR}/scripts/record_artifact_identity.cmake"
        COMMENT "Record linked artifact source and SHA-256 identity"
        VERBATIM)
endfunction()

function(xen_finalize_runtime_deployment)
    xen_generate_runtime_metadata(runtime_targets runtime_manifest)
    if(NOT runtime_targets)
        return()
    endif()
    # 项目没有重写 RUNTIME_OUTPUT_DIRECTORY；Visual Studio 多配置生成器的
    # 共享目录固定为 <build>/<config>，与 scripts/build.ps1 的验收路径一致。
    add_custom_target(xen_runtime_deployment
        COMMAND ${CMAKE_COMMAND}
            "-DXEN_RUNTIME_MANIFEST=${runtime_manifest}"
            "-DXEN_RUNTIME_OUTPUT=${CMAKE_BINARY_DIR}/$<CONFIG>"
            "-DXEN_RUNTIME_LOCK=${CMAKE_BINARY_DIR}/xen-runtime-deploy.lock"
            "-DXEN_RUNTIME_CONFIGURATION=$<CONFIG>"
            -P "${PROJECT_SOURCE_DIR}/scripts/deploy_runtime_file.cmake"
        COMMENT "检查当前配置运行库变更并增量部署"
        VERBATIM)
    foreach(runtime_target IN LISTS runtime_targets)
        add_dependencies(${runtime_target} xen_runtime_deployment)
        xen_record_artifact_identity(${runtime_target})
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
if(WIN32)
    set(SPDLOG_WCHAR_FILENAMES ON CACHE BOOL "Use native Windows log file paths" FORCE)
endif()
FetchContent_MakeAvailable(spdlog)

# SimpleIni 当前可验证的最新稳定 tag 为 4.19。仅使用其单头文件实现，禁止
# FetchContent 把上游示例或测试加入 Xen 构建图。
FetchContent_Declare(simpleini
    GIT_REPOSITORY https://github.com/brofield/simpleini.git
    GIT_TAG        4.19
    SOURCE_SUBDIR  _xen_no_cmake)
FetchContent_MakeAvailable(simpleini)

# 发布入口使用严格 JSON schema 解析运行时与后端路由，避免用字符串扫描处理
# 路径安全边界。完整文件哈希只由发布/传输脚本执行，不进入日常 Launcher。
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.12.0)
FetchContent_MakeAvailable(nlohmann_json)

# Dear ImGui 上游不提供正式 CMake target，由 Xen 明确列出 Win32/D3D11 后端
# 与 std::string 适配源码，避免隐式编译无关示例和平台后端。
set(XEN_IMGUI_VERSION "1.92.9b")
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v${XEN_IMGUI_VERSION})
FetchContent_MakeAvailable(imgui)
# 本地 SOURCE_DIR 覆盖不会跟随 GIT_TAG 更新，必须核对实际参与编译的头文件。
file(STRINGS "${imgui_SOURCE_DIR}/imgui.h" XEN_IMGUI_VERSION_LINE
    REGEX "^#define IMGUI_VERSION[ \t]+\"")
string(REGEX REPLACE "^[^\"]*\"([^\"]+)\".*$" "\\1"
    XEN_CONFIGURED_IMGUI_VERSION "${XEN_IMGUI_VERSION_LINE}")
if(NOT XEN_CONFIGURED_IMGUI_VERSION STREQUAL XEN_IMGUI_VERSION)
    message(FATAL_ERROR "ImGui source version ${XEN_CONFIGURED_IMGUI_VERSION} does not match required ${XEN_IMGUI_VERSION}; update FETCHCONTENT_SOURCE_DIR_IMGUI")
endif()

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

add_library(clock_sync STATIC
    Xen/clock_sync/clock_sync.cpp)
target_include_directories(clock_sync PUBLIC ${PROJECT_SOURCE_DIR}/Xen)
target_link_libraries(clock_sync
    PRIVATE Threads::Threads bcrypt ws2_32)

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
    PRIVATE clock_sync bcrypt d3d11 dxgi ws2_32)
if(XEN_HAS_NDI)
    target_include_directories(capture PUBLIC ${XEN_NDI_INCLUDE_DIR})
    target_link_libraries(capture PRIVATE ${XEN_NDI_LIB})
    target_compile_definitions(capture PUBLIC XEN_HAS_NDI=1)
else()
    target_compile_definitions(capture PUBLIC XEN_HAS_NDI=0)
endif()

add_library(capture_evidence STATIC
    Xen/capture_evidence/capture_evidence.cpp)
target_include_directories(capture_evidence PUBLIC
    ${PROJECT_SOURCE_DIR}/Xen
    ${OpenCV_INCLUDE_DIRS})
target_link_libraries(capture_evidence
    PUBLIC capture ${OpenCV_LIBS}
    PRIVATE nlohmann_json::nlohmann_json bcrypt)

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

# Production red producer 只运行公开 Aim 接口和确定性模拟 backend/plant；
# 构建图刻意不链接 mouse，保证工具本身不具备物理输出能力。
add_library(aim_production_red STATIC
    Xen/aim_production_red/aim_production_red.cpp)
target_include_directories(aim_production_red PUBLIC
    ${PROJECT_SOURCE_DIR}/Xen)
target_link_libraries(aim_production_red
    PUBLIC aim
    PRIVATE config nlohmann_json::nlohmann_json bcrypt)

add_executable(xen_aim_production_red_producer
    Xen/aim_production_red/main.cpp)
target_link_libraries(xen_aim_production_red_producer PRIVATE
    aim_production_red)
xen_deploy_runtime_dlls(xen_aim_production_red_producer OPENCV)

add_library(aim_landmark STATIC
    Xen/aim_landmark/aim_landmark.cpp)
target_include_directories(aim_landmark PUBLIC ${PROJECT_SOURCE_DIR}/Xen)
target_link_libraries(aim_landmark PUBLIC aim)

add_library(auto_stop STATIC Xen/auto_stop/auto_stop.cpp)
target_include_directories(auto_stop PUBLIC ${PROJECT_SOURCE_DIR}/Xen)

# 独立离线比较工具不链接 mouse、worker 或 runtime，参考速度评分不能进入控制链。
add_library(reference_assessment STATIC Xen/reference_assessment/reference_assessment.cpp)
target_include_directories(reference_assessment PUBLIC ${PROJECT_SOURCE_DIR}/Xen)
add_executable(xen_reference_compare Xen/reference_assessment/main.cpp)
set_target_properties(xen_reference_compare PROPERTIES OUTPUT_NAME XenReferenceCompare)
target_link_libraries(xen_reference_compare PRIVATE reference_assessment input_training auto_stop nlohmann_json::nlohmann_json)
xen_deploy_runtime_dlls(xen_reference_compare)

add_library(mouse STATIC
    Xen/mouse/mouse.cpp
    Xen/mouse/kmbox_net.cpp
    Xen/mouse/makcu.cpp)
target_include_directories(mouse PUBLIC ${PROJECT_SOURCE_DIR}/Xen)
target_link_libraries(mouse PUBLIC log PRIVATE user32 ws2_32 shell32 ole32 uuid)

add_library(movement STATIC Xen/movement/movement.cpp)
target_include_directories(movement PUBLIC ${PROJECT_SOURCE_DIR}/Xen)
target_link_libraries(movement PUBLIC mouse PRIVATE log Threads::Threads)

add_library(auto_stop_worker STATIC Xen/auto_stop/auto_stop_worker.cpp)
target_include_directories(auto_stop_worker PUBLIC ${PROJECT_SOURCE_DIR}/Xen)
target_link_libraries(auto_stop_worker PUBLIC auto_stop mouse PRIVATE Threads::Threads)
add_library(hud_stop STATIC Xen/auto_stop/hud_stop.cpp)
target_link_libraries(hud_stop PUBLIC auto_stop)
target_link_libraries(auto_stop_worker PRIVATE hud_stop)

add_library(trigger STATIC Xen/trigger/trigger.cpp)
target_include_directories(trigger PUBLIC ${PROJECT_SOURCE_DIR}/Xen ${OpenCV_INCLUDE_DIRS})
add_library(trigger_worker STATIC Xen/trigger/trigger_worker.cpp)
target_link_libraries(trigger_worker PUBLIC trigger auto_stop_worker mouse log PRIVATE Threads::Threads)
add_library(source_context STATIC Xen/source_context/source_context.cpp)
target_include_directories(source_context PUBLIC ${PROJECT_SOURCE_DIR}/Xen)
target_link_libraries(source_context PRIVATE ws2_32 bcrypt user32 Threads::Threads)
add_executable(xen_source_context Xen/source_context_host/main.cpp)
target_link_libraries(xen_source_context PRIVATE source_context)
xen_deploy_runtime_dlls(xen_source_context)
add_library(weapon STATIC Xen/weapon/weapon.cpp Xen/weapon/weapon_timing.cpp)
target_include_directories(weapon PUBLIC ${PROJECT_SOURCE_DIR}/Xen)
target_link_libraries(weapon PUBLIC nlohmann_json::nlohmann_json PRIVATE ws2_32 advapi32 Threads::Threads)
add_library(recoil STATIC Xen/recoil/recoil.cpp Xen/recoil/recoil_store.cpp Xen/recoil/motion_ledger.cpp Xen/recoil/recoil_calibration.cpp)
target_include_directories(recoil PUBLIC ${PROJECT_SOURCE_DIR}/Xen)
target_link_libraries(recoil PUBLIC nlohmann_json::nlohmann_json PRIVATE bcrypt)
add_library(recoil_worker STATIC Xen/recoil/recoil_worker.cpp)
target_link_libraries(recoil_worker PUBLIC recoil trigger_worker mouse PRIVATE Threads::Threads)
add_library(recoil_tuner STATIC Xen/recoil_tuner/recoil_tuner.cpp Xen/recoil_tuner/recoil_tuner_io.cpp Xen/recoil_tuner/image_measurement.cpp
    Xen/recoil_tuner/wall_capture_analysis.cpp)
target_include_directories(recoil_tuner PUBLIC ${PROJECT_SOURCE_DIR}/Xen ${OpenCV_INCLUDE_DIRS})
target_link_libraries(recoil_tuner PUBLIC recoil nlohmann_json::nlohmann_json ${OpenCV_LIBS})
add_executable(xen_recoil_tuner Xen/recoil_tuner_cli/main.cpp)
target_link_libraries(xen_recoil_tuner PRIVATE recoil_tuner recoil weapon)
xen_deploy_runtime_dlls(xen_recoil_tuner OPENCV)
add_library(recoil_archive STATIC Xen/recoil/recoil_archive.cpp)
target_link_libraries(recoil_archive PUBLIC recoil_worker PRIVATE recoil_tuner Threads::Threads)
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
target_link_libraries(config PUBLIC log trigger source_context weapon recoil movement)
add_library(recoil_calibration_io STATIC Xen/recoil/recoil_calibration_io.cpp)
target_link_libraries(recoil_calibration_io PUBLIC recoil config)
add_executable(xen_recoil_calibration Xen/recoil_calibration_cli/main.cpp)
target_link_libraries(xen_recoil_calibration PRIVATE recoil_calibration_io recoil_worker recoil_archive weapon source_context)
xen_deploy_runtime_dlls(xen_recoil_calibration OPENCV)

add_library(app_release STATIC
    Xen/app/release_contract.cpp)
target_include_directories(app_release PUBLIC
    ${PROJECT_SOURCE_DIR}/Xen
    ${OpenCV_INCLUDE_DIRS})
target_link_libraries(app_release
    PUBLIC nlohmann_json::nlohmann_json)

add_library(app_launcher STATIC
    Xen/app/launcher_decision.cpp)
target_include_directories(app_launcher PUBLIC ${PROJECT_SOURCE_DIR}/Xen)
target_link_libraries(app_launcher
    PUBLIC app_release
    PRIVATE config)

add_library(app_startup STATIC
    Xen/app/startup.cpp)
target_include_directories(app_startup PUBLIC ${PROJECT_SOURCE_DIR}/Xen)

add_library(data_collection STATIC
    Xen/data_collection/data_collection.cpp)
target_include_directories(data_collection PUBLIC
    ${PROJECT_SOURCE_DIR}/Xen ${OpenCV_INCLUDE_DIRS})
target_link_libraries(data_collection PUBLIC ${OpenCV_LIBS}
    PRIVATE nlohmann_json::nlohmann_json bcrypt Threads::Threads log)

add_library(model_workspace STATIC
    Xen/model_workspace/model_workspace.cpp)
target_include_directories(model_workspace PUBLIC ${PROJECT_SOURCE_DIR}/Xen)
target_link_libraries(model_workspace PUBLIC data_collection
    PRIVATE nlohmann_json::nlohmann_json bcrypt shell32 comdlg32 log)

add_library(input_training STATIC Xen/input_training/input_training.cpp)
target_include_directories(input_training PUBLIC ${PROJECT_SOURCE_DIR}/Xen)
target_link_libraries(input_training PRIVATE nlohmann_json::nlohmann_json Threads::Threads)

add_library(runtime STATIC
    Xen/runtime/runtime.cpp
    Xen/runtime/runtime_queue.cpp
    Xen/runtime/camera_motion.cpp)
target_include_directories(runtime PUBLIC ${PROJECT_SOURCE_DIR}/Xen)
target_link_libraries(runtime
    PUBLIC detector capture aim aim_landmark auto_stop auto_stop_worker trigger_worker source_context recoil_worker recoil_archive weapon mouse keyboard config log data_collection input_training movement
    PRIVATE Threads::Threads)

add_library(app_input STATIC
    Xen/app/input_router.cpp)
target_include_directories(app_input PUBLIC ${PROJECT_SOURCE_DIR}/Xen)
target_link_libraries(app_input PUBLIC runtime)

add_library(xen_debug STATIC
    Xen/debug/debug.cpp Xen/debug/recoil_report.cpp Xen/debug/auxiliary_report.cpp
    Xen/debug/session_archive.cpp)
target_include_directories(xen_debug PUBLIC
    ${PROJECT_SOURCE_DIR}/Xen
    ${OpenCV_INCLUDE_DIRS})
target_link_libraries(xen_debug PUBLIC log aim aim_landmark mouse auto_stop trigger recoil weapon recoil_tuner)

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

add_library(mouse_effect_probe_report_validation STATIC
    Xen/mouse_effect_probe/report_validation.cpp)
target_include_directories(mouse_effect_probe_report_validation PUBLIC ${PROJECT_SOURCE_DIR}/Xen)
target_link_libraries(mouse_effect_probe_report_validation PUBLIC nlohmann_json::nlohmann_json)

add_library(mouse_effect_probe STATIC
    Xen/mouse_effect_probe/mouse_effect_probe.cpp)
target_include_directories(mouse_effect_probe PUBLIC ${PROJECT_SOURCE_DIR}/Xen)
target_link_libraries(mouse_effect_probe
    PUBLIC mouse
    PRIVATE mouse_effect_probe_report_validation nlohmann_json::nlohmann_json bcrypt)

add_library(mouse_effect_probe_runner STATIC
    Xen/mouse_effect_probe_runner/mouse_effect_probe_runner.cpp)
target_include_directories(mouse_effect_probe_runner
    PUBLIC ${PROJECT_SOURCE_DIR}/Xen ${OpenCV_INCLUDE_DIRS})
target_link_libraries(mouse_effect_probe_runner
    PUBLIC mouse_effect_probe capture
    PRIVATE config nlohmann_json::nlohmann_json bcrypt)

add_library(debug_runner STATIC Xen/auto_stop_probe/debug_runner.cpp
    Xen/auto_stop_probe/counterpulse_hud.cpp)
target_include_directories(debug_runner PUBLIC ${PROJECT_SOURCE_DIR}/Xen)
target_link_libraries(debug_runner PUBLIC config mouse capture source_context input_training nlohmann_json::nlohmann_json
    PRIVATE user32 gdi32)
add_library(debug_session STATIC Xen/debug_session/debug_session.cpp Xen/recoil/recoil_debug_run.cpp
    Xen/recoil_tuner/wall_capture_run.cpp)
target_include_directories(debug_session PUBLIC ${PROJECT_SOURCE_DIR}/Xen)
target_link_libraries(debug_session PUBLIC runtime weapon nlohmann_json::nlohmann_json
    PRIVATE debug_runner recoil_archive recoil_calibration_io recoil_tuner capture log Threads::Threads)
target_compile_definitions(debug_session PRIVATE
    XEN_DEBUG_BUILD_COMMIT="${XEN_BUILD_GIT_COMMIT}"
    XEN_DEBUG_BUILD_DIRTY=${XEN_BUILD_GIT_DIRTY}
    XEN_DEBUG_RUNTIME_ID="${XEN_BUILD_RUNTIME_ID}")

add_library(overlay STATIC
    Xen/overlay/overlay.cpp Xen/overlay/recoil_panel.cpp Xen/overlay/input_training_panel.cpp Xen/overlay/debug_panel.cpp)
target_include_directories(overlay PUBLIC ${PROJECT_SOURCE_DIR}/Xen)
target_link_libraries(overlay
    PUBLIC runtime config model_workspace debug_session
    PRIVATE imgui recoil_tuner recoil recoil_calibration_io d3d11 dxgi dwmapi shell32)

add_executable(xen_app WIN32
    Xen/app/main.cpp
    Xen/app/model_catalog.cpp
    Xen/app/xen.rc)
set_target_properties(xen_app PROPERTIES OUTPUT_NAME Xen)
add_dependencies(xen_app xen_recoil_calibration xen_recoil_tuner)
set(XEN_MODEL_TOOLS_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}")
if(CMAKE_CONFIGURATION_TYPES)
    string(APPEND XEN_MODEL_TOOLS_OUTPUT "/$<CONFIG>")
endif()
# 与现有运行库部署相同：本项目未重写RUNTIME_OUTPUT_DIRECTORY。
# 不用TARGET_FILE_DIR查询，避免CMake 3.18的隐式反向依赖形成循环。
add_custom_target(model_workspace_tools
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${XEN_MODEL_TOOLS_OUTPUT}/tools/model-data"
        "${XEN_MODEL_TOOLS_OUTPUT}/tools/recoil"
        "${XEN_MODEL_TOOLS_OUTPUT}/tools/source"
        "${XEN_MODEL_TOOLS_OUTPUT}/tools/review"
        "${XEN_MODEL_TOOLS_OUTPUT}/assets/recoil"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PROJECT_SOURCE_DIR}/assets/weapon_catalog.inc"
        "${XEN_MODEL_TOOLS_OUTPUT}/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PROJECT_SOURCE_DIR}/scripts/start_source_context_session.ps1"
        "${XEN_MODEL_TOOLS_OUTPUT}/tools/source"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PROJECT_SOURCE_DIR}/scripts/review_run.py"
        "${XEN_MODEL_TOOLS_OUTPUT}/tools/review"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PROJECT_SOURCE_DIR}/scripts/model_data_pipeline.py"
        "${PROJECT_SOURCE_DIR}/scripts/model_data_review.html"
        "${PROJECT_SOURCE_DIR}/scripts/model_training_environment.py"
        "${PROJECT_SOURCE_DIR}/scripts/model_training_requirements.txt"
        "${XEN_MODEL_TOOLS_OUTPUT}/tools/model-data"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PROJECT_SOURCE_DIR}/scripts/import_recoil_profiles.py"
        "${PROJECT_SOURCE_DIR}/scripts/migrate_legacy_recoil_profiles.py"
        "${PROJECT_SOURCE_DIR}/scripts/invoke_recoil_legacy_acceptance.ps1"
        "${PROJECT_SOURCE_DIR}/scripts/build_recoil_dataset.py"
        "${XEN_MODEL_TOOLS_OUTPUT}/tools/recoil"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PROJECT_SOURCE_DIR}/assets/recoil/legacy_manifest.json"
        "${PROJECT_SOURCE_DIR}/assets/recoil/README.md"
        "${XEN_MODEL_TOOLS_OUTPUT}/assets/recoil"
    VERBATIM)
add_dependencies(xen_app model_workspace_tools)
target_link_libraries(xen_app PRIVATE
    overlay app_input app_startup runtime xen_debug config crash log app_release)
if(WIN32)
    xen_deploy_runtime_dlls(xen_app OPENCV ORT NDI)
endif()

add_executable(xen_launcher WIN32
    Xen/app/launcher.cpp
    Xen/app/model_catalog.cpp
    Xen/app/xen.rc)
set_target_properties(xen_launcher PROPERTIES OUTPUT_NAME XenLauncher)
target_link_libraries(xen_launcher PRIVATE app_release config)
target_link_libraries(xen_launcher PRIVATE app_launcher)
xen_deploy_runtime_dlls(xen_launcher)

add_executable(xen_sender
    Xen/sender/main.cpp)
set_target_properties(xen_sender PROPERTIES OUTPUT_NAME XenSender)
target_link_libraries(xen_sender PRIVATE sender capture crash log)
if(WIN32)
    xen_deploy_runtime_dlls(xen_sender OPENCV NDI)
endif()

add_executable(xen_clock_source
    Xen/clock_source/main.cpp)
set_target_properties(xen_clock_source PROPERTIES OUTPUT_NAME XenClockSource)
target_link_libraries(xen_clock_source PRIVATE clock_sync)
xen_deploy_runtime_dlls(xen_clock_source)

# 证据入口只链接 Capture 链；不链接 Runtime、Aim、Mouse 或 Keyboard，因而
# 不具备武装或发送真实输入的能力。
add_executable(xen_capture_evidence
    Xen/capture_evidence/main.cpp)
set_target_properties(
    xen_capture_evidence PROPERTIES OUTPUT_NAME XenCaptureEvidence)
target_link_libraries(xen_capture_evidence PRIVATE capture_evidence)
if(WIN32)
    xen_deploy_runtime_dlls(xen_capture_evidence OPENCV NDI)
endif()

add_executable(xen_benchmark
    Xen/benchmark/main.cpp)

# 原图离线验证不实例化 Capture/Mouse；复用实际 Detector、背景生产者和 Aim。
add_executable(camera_motion_benchmark
    Xen/benchmark/camera_motion_benchmark.cpp
    Xen/runtime/camera_motion.cpp)
target_include_directories(camera_motion_benchmark PRIVATE ${PROJECT_SOURCE_DIR}/Xen)
target_link_libraries(camera_motion_benchmark PRIVATE
    detector aim config nlohmann_json::nlohmann_json)
xen_deploy_runtime_dlls(camera_motion_benchmark OPENCV ORT)
set_target_properties(xen_benchmark PROPERTIES OUTPUT_NAME XenBenchmark)
target_link_libraries(xen_benchmark PRIVATE benchmark)
if(WIN32)
    xen_deploy_runtime_dlls(xen_benchmark OPENCV ORT NDI)
endif()

add_executable(xen_mouse_benchmark
    Xen/mouse_benchmark/main.cpp)

# 有界 WASD 设备试验独立于生产 Runtime；真实运行须显式确认。
add_executable(auto_stop_probe Xen/auto_stop_probe/main.cpp)
target_include_directories(auto_stop_probe PRIVATE ${PROJECT_SOURCE_DIR}/Xen)
target_link_libraries(auto_stop_probe PRIVATE config mouse nlohmann_json::nlohmann_json)

# 单组反向脉冲点射标定：与生产Runtime互斥，显式前台双授权。
add_executable(auto_stop_counterpulse Xen/auto_stop_probe/counterpulse_main.cpp)
target_include_directories(auto_stop_counterpulse PRIVATE ${PROJECT_SOURCE_DIR}/Xen)
target_link_libraries(auto_stop_counterpulse PRIVATE debug_runner)
if(WIN32)
    target_link_libraries(auto_stop_counterpulse PRIVATE user32 gdi32)
    xen_deploy_runtime_dlls(auto_stop_counterpulse OPENCV NDI)
endif()

# 本机 HUD 观测入口只连接 DXGI，不具备任何输入能力。
add_executable(xen_auto_stop_capture Xen/auto_stop_capture/main.cpp)
set_target_properties(xen_auto_stop_capture PROPERTIES OUTPUT_NAME XenAutoStopCapture)
target_link_libraries(xen_auto_stop_capture PRIVATE capture)
if(WIN32)
    xen_deploy_runtime_dlls(xen_auto_stop_capture OPENCV NDI)
endif()
set_target_properties(
    xen_mouse_benchmark PROPERTIES OUTPUT_NAME XenMouseBenchmark)
target_link_libraries(xen_mouse_benchmark PRIVATE mouse_benchmark)

add_executable(xen_mouse_effect_probe
    Xen/mouse_effect_probe_runner/main.cpp)
set_target_properties(
    xen_mouse_effect_probe PROPERTIES OUTPUT_NAME XenMouseEffectProbe)
target_link_libraries(xen_mouse_effect_probe
    PRIVATE mouse_effect_probe_runner)
if(WIN32)
    xen_deploy_runtime_dlls(xen_mouse_effect_probe OPENCV NDI)
endif()

add_executable(xen_mouse_effect_probe_sequence
    Xen/mouse_effect_probe_sequence/main.cpp)
set_target_properties(xen_mouse_effect_probe_sequence
    PROPERTIES OUTPUT_NAME XenMouseEffectProbeSequence)
target_link_libraries(xen_mouse_effect_probe_sequence
    PRIVATE mouse_effect_probe)

# 只执行 QPC/one-shot timer preflight 并封存 plan；不链接 Capture、
# Runtime、Aim、Mouse 或 Keyboard，因而不具备发送真实输入的能力。
add_executable(xen_mouse_effect_probe_composite_seal
    Xen/mouse_effect_probe_composite_seal/main.cpp)
set_target_properties(xen_mouse_effect_probe_composite_seal PROPERTIES
    OUTPUT_NAME XenMouseEffectProbeCompositeSeal)
target_link_libraries(xen_mouse_effect_probe_composite_seal PRIVATE
    mouse_effect_probe_report_validation nlohmann_json::nlohmann_json bcrypt)

# ─── 测试 ───────────────────────────────────────────────

include(CTest)
if(BUILD_TESTING)
    # 测试解释器必须先于任何命令登记解析，不能依赖第二次配置留下的缓存。
    find_program(XEN_POWERSHELL7_EXECUTABLE NAMES pwsh)
    if(NOT XEN_POWERSHELL7_EXECUTABLE)
        message(FATAL_ERROR "PowerShell 7 executable not found: pwsh")
    endif()
    set(XEN_WINDOWS_POWERSHELL
        "$ENV{SystemRoot}/System32/WindowsPowerShell/v1.0/powershell.exe")
    if(NOT EXISTS "${XEN_WINDOWS_POWERSHELL}")
        message(FATAL_ERROR
            "Windows PowerShell 5 executable not found: ${XEN_WINDOWS_POWERSHELL}")
    endif()
    find_program(XEN_PYTHON_EXECUTABLE NAMES python python3)
    if(NOT XEN_PYTHON_EXECUTABLE)
        message(FATAL_ERROR "Python executable not found: python or python3")
    endif()
    add_executable(weapon_tests tests/weapon_tests.cpp)
    add_executable(gsi_accept_recovery_tests tests/gsi_accept_recovery_tests.cpp)
    target_include_directories(gsi_accept_recovery_tests PRIVATE ${PROJECT_SOURCE_DIR}/Xen)
    target_link_libraries(gsi_accept_recovery_tests PRIVATE nlohmann_json::nlohmann_json ws2_32 advapi32 Threads::Threads)
    add_test(NAME gsi_accept_recovery_tests COMMAND gsi_accept_recovery_tests)
    set_tests_properties(gsi_accept_recovery_tests PROPERTIES TIMEOUT 30)
    add_executable(weapon_timing_tests tests/weapon_timing_tests.cpp)
    target_link_libraries(weapon_timing_tests PRIVATE weapon)
    add_test(NAME weapon_timing_tests COMMAND weapon_timing_tests)
    add_executable(weapon_stop_integration_tests tests/weapon_stop_integration_tests.cpp)
    target_link_libraries(weapon_stop_integration_tests PRIVATE weapon trigger_worker auto_stop_worker aim)
    add_test(NAME weapon_stop_integration_tests COMMAND weapon_stop_integration_tests)
    target_link_libraries(weapon_tests PRIVATE weapon)
    add_test(NAME weapon_tests COMMAND weapon_tests)
    foreach(test_name recoil_tests recoil_store_tests motion_ledger_tests)
        add_executable(${test_name} tests/${test_name}.cpp)
        target_link_libraries(${test_name} PRIVATE recoil)
        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()
    add_executable(recoil_worker_tests tests/recoil_worker_tests.cpp)
    # 真实旧数据仅本地提供参数，不分发到默认测试夹具。
    add_executable(recoil_legacy_parity_tests tests/recoil_legacy_parity_tests.cpp)
    target_link_libraries(recoil_legacy_parity_tests PRIVATE recoil nlohmann_json::nlohmann_json)
    add_test(NAME import_recoil_profiles_tests COMMAND "${XEN_PYTHON_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/import_recoil_profiles_tests.py")
    add_test(NAME migrate_legacy_recoil_profiles_tests COMMAND "${XEN_PYTHON_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/migrate_legacy_recoil_profiles_tests.py"
        --store-test "$<TARGET_FILE:recoil_store_tests>")
    target_link_libraries(recoil_worker_tests PRIVATE recoil_worker)
    add_test(NAME recoil_worker_tests COMMAND recoil_worker_tests)
    add_executable(recoil_calibration_tests tests/recoil_calibration_tests.cpp)
    target_link_libraries(recoil_calibration_tests PRIVATE recoil_calibration_io)
    xen_deploy_runtime_dlls(recoil_calibration_tests OPENCV)
    add_test(NAME recoil_calibration_tests COMMAND recoil_calibration_tests)
    add_executable(recoil_archive_tests tests/recoil_archive_tests.cpp)
    target_link_libraries(recoil_archive_tests PRIVATE recoil_archive)
    xen_deploy_runtime_dlls(recoil_archive_tests OPENCV)
    add_test(NAME recoil_archive_tests COMMAND recoil_archive_tests)
    add_executable(recoil_tuner_tests tests/recoil_tuner_tests.cpp)
    target_link_libraries(recoil_tuner_tests PRIVATE recoil_tuner recoil)
    xen_deploy_runtime_dlls(recoil_tuner_tests OPENCV)
    add_test(NAME recoil_tuner_tests COMMAND recoil_tuner_tests)
    # 合成热点计时需显式运行，不将机器速度作为 CTest 门禁。
    add_executable(recoil_tuner_phase_benchmark tests/recoil_tuner_phase_benchmark.cpp)
    target_link_libraries(recoil_tuner_phase_benchmark PRIVATE recoil_tuner recoil)
    xen_deploy_runtime_dlls(recoil_tuner_phase_benchmark OPENCV)
    add_test(NAME recoil_tuner_phase_compare_tests
        COMMAND "${XEN_PYTHON_EXECUTABLE}" -B -X utf8
            "${PROJECT_SOURCE_DIR}/tests/recoil_tuner_phase_compare.py" --self-test)
    add_executable(motion_ledger_benchmark
        tests/motion_ledger_benchmark.cpp Xen/recoil/motion_ledger.cpp)
    target_include_directories(motion_ledger_benchmark PRIVATE Xen)
    add_executable(recoil_wall_analysis_tests tests/recoil_wall_analysis_tests.cpp)
    target_link_libraries(recoil_wall_analysis_tests PRIVATE recoil_tuner)
    xen_deploy_runtime_dlls(recoil_wall_analysis_tests OPENCV)
    add_test(NAME recoil_wall_analysis_tests COMMAND recoil_wall_analysis_tests)
    add_executable(recoil_config_tests tests/recoil_config_tests.cpp)
    target_link_libraries(recoil_config_tests PRIVATE config)
    xen_deploy_runtime_dlls(recoil_config_tests OPENCV)
    add_test(NAME recoil_config_tests COMMAND recoil_config_tests)
    add_executable(aim_external_motion_tests tests/aim_external_motion_tests.cpp)
    target_link_libraries(aim_external_motion_tests PRIVATE aim log)
    xen_deploy_runtime_dlls(aim_external_motion_tests OPENCV)
    add_test(NAME aim_external_motion_tests COMMAND aim_external_motion_tests)
    add_executable(aim_recoil_integration_tests tests/aim_recoil_integration_tests.cpp)
    target_link_libraries(aim_recoil_integration_tests PRIVATE aim log recoil_worker)
    xen_deploy_runtime_dlls(aim_recoil_integration_tests OPENCV)
    add_test(NAME aim_recoil_integration_tests COMMAND aim_recoil_integration_tests)
    add_executable(recoil_report_tests tests/recoil_report_tests.cpp)
    target_link_libraries(recoil_report_tests PRIVATE xen_debug)
    xen_deploy_runtime_dlls(recoil_report_tests OPENCV)
    add_test(NAME recoil_report_tests COMMAND recoil_report_tests)
    add_executable(app_report_lifecycle_tests tests/app_report_lifecycle_tests.cpp)
    target_link_libraries(app_report_lifecycle_tests PRIVATE xen_debug recoil_archive)
    xen_deploy_runtime_dlls(app_report_lifecycle_tests OPENCV)
    add_test(NAME app_report_lifecycle_tests COMMAND app_report_lifecycle_tests)
    set_tests_properties(app_report_lifecycle_tests PROPERTIES TIMEOUT 30)
    add_executable(app_report_interleave_tests tests/app_report_interleave_tests.cpp)
    target_link_libraries(app_report_interleave_tests PRIVATE xen_debug recoil_archive)
    xen_deploy_runtime_dlls(app_report_interleave_tests OPENCV)
    add_test(NAME app_report_interleave_tests COMMAND app_report_interleave_tests)
    set_tests_properties(app_report_interleave_tests PROPERTIES TIMEOUT 60)
    add_test(NAME review_run_tests COMMAND "${XEN_PYTHON_EXECUTABLE}" -B -X utf8
        "${PROJECT_SOURCE_DIR}/tests/review_run_tests.py")
    # 离线像素专项依赖可加载的本地扩展；模块存在不等于同一解释器可以导入。
    execute_process(
        COMMAND "${XEN_PYTHON_EXECUTABLE}" -B -X utf8 -c
            "import sys, numpy, cv2; print('Python ' + sys.version.split()[0] + ', NumPy ' + numpy.__version__ + ', OpenCV ' + cv2.__version__)"
        RESULT_VARIABLE XEN_PYTHON_MODULES_RESULT
        OUTPUT_VARIABLE XEN_PYTHON_MODULES_OUTPUT
        ERROR_VARIABLE XEN_PYTHON_MODULES_ERROR
        TIMEOUT 30
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE)
    if(NOT XEN_PYTHON_MODULES_RESULT STREQUAL "0")
        message(FATAL_ERROR
            "测试 Python 无法加载 NumPy/OpenCV：${XEN_PYTHON_EXECUTABLE}\n"
            "退出结果：${XEN_PYTHON_MODULES_RESULT}\n${XEN_PYTHON_MODULES_ERROR}")
    endif()
    message(STATUS "测试 Python：${XEN_PYTHON_EXECUTABLE} (${XEN_PYTHON_MODULES_OUTPUT})")
    add_test(NAME recoil_dataset_tests
        COMMAND "${XEN_PYTHON_EXECUTABLE}" -B -X utf8 "${PROJECT_SOURCE_DIR}/tests/recoil_dataset_tests.py")
    add_test(NAME recoil_batch_dataset_tests
        COMMAND "${XEN_PYTHON_EXECUTABLE}" -B -X utf8 "${PROJECT_SOURCE_DIR}/tests/recoil_batch_dataset_tests.py")
    add_test(NAME recoil_archive_dataset_tests
        COMMAND "${XEN_PYTHON_EXECUTABLE}" -B -X utf8 "${PROJECT_SOURCE_DIR}/tests/recoil_archive_dataset_tests.py" "$<TARGET_FILE:recoil_archive_tests>")
    add_test(NAME recoil_calibration_cli_tests
        COMMAND "${XEN_PYTHON_EXECUTABLE}" -B -X utf8 "${PROJECT_SOURCE_DIR}/tests/recoil_calibration_cli_tests.py" "$<TARGET_FILE:xen_recoil_calibration>")
    add_test(NAME recoil_cli_tests
        COMMAND "${XEN_PYTHON_EXECUTABLE}" -B -X utf8 "${PROJECT_SOURCE_DIR}/tests/recoil_cli_tests.py" "$<TARGET_FILE:xen_recoil_tuner>")
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

    if(WIN32 AND XEN_ONNXRUNTIME_HAS_TENSORRT)
        # 元数据夹具调用生产共用的解析/部署规则，在原 owned 深根验证全部合同。
        # 真实根工程仍启用 CXX/CUDA；夹具不重复依赖深路径编译器中间文件。
        # Visual Studio 即使 LANGUAGES NONE 也先运行 MSBuild 查询 VCTargetsPath；
        # 元数据工程显式采用 Ninja Multi-Config，避免引入无关的编译器/项目探测。
        get_filename_component(xen_cmake_bin "${CMAKE_COMMAND}" DIRECTORY)
        find_program(XEN_METADATA_NINJA_EXECUTABLE NAMES ninja
            HINTS "${xen_cmake_bin}/../../Ninja")
        add_test(NAME runtime_component_producer_tests
            COMMAND ${CMAKE_COMMAND}
                "-DXEN_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
                "-DXEN_COMPONENT_TEST_ROOT=${CMAKE_BINARY_DIR}/runtime-component-producer-tests"
                "-DXEN_GENERATOR=Ninja Multi-Config"
                "-DXEN_METADATA_NINJA_EXECUTABLE=${XEN_METADATA_NINJA_EXECUTABLE}"
                "-DXEN_OPENCV_DIR=${OpenCV_DIR}"
                "-DXEN_CUDA_ROOT=${XEN_CUDA_ROOT}"
                "-DXEN_TENSORRT_PROVIDER_DLL=${ONNXRUNTIME_RUNTIME_DIR}/onnxruntime_providers_tensorrt.dll"
                "-DXEN_TENSORRT_MAJOR=${XEN_TENSORRT_MAJOR}"
                "-DXEN_SPDLOG_SOURCE_DIR=${spdlog_SOURCE_DIR}"
                "-DXEN_SIMPLEINI_SOURCE_DIR=${simpleini_SOURCE_DIR}"
                "-DXEN_NLOHMANN_JSON_SOURCE_DIR=${nlohmann_json_SOURCE_DIR}"
                "-DXEN_IMGUI_SOURCE_DIR=${imgui_SOURCE_DIR}"
                "-DXEN_RUNTIME_LINKER=${CMAKE_LINKER}"
                "-DXEN_MSVC_REDIST_ROOT=${XEN_MSVC_REDIST_ROOT}"
                -P "${PROJECT_SOURCE_DIR}/tests/runtime_component_producer_tests.cmake")
    endif()

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

    # 公有加载入口仍使用真实 ORT；仅在独立测试进程观察 SDK 借用生命周期。
    # MANUAL_INIT 必须覆盖所有消费 ORT C++ API 的 TU，不能混链普通 detector。
    add_executable(detector_typeinfo_lifetime_tests
        tests/detector_typeinfo_lifetime_tests.cpp
        Xen/detector/detector.cpp
        Xen/detector/session.cpp
        Xen/detector/preprocess.cpp
        Xen/detector/postprocess.cpp)
    target_include_directories(detector_typeinfo_lifetime_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/Xen
        ${ONNXRUNTIME_INCLUDE_DIR}
        ${OpenCV_INCLUDE_DIRS})
    target_compile_definitions(detector_typeinfo_lifetime_tests PRIVATE
        ORT_API_MANUAL_INIT
        XEN_HAS_CUDA_RUNTIME=0
        XEN_HAS_DIRECTML_INTEROP=0
        XEN_HAS_OPENVINO_PROVIDER=0)
    target_link_libraries(detector_typeinfo_lifetime_tests PRIVATE
        ${ONNXRUNTIME_LIB} ${OpenCV_LIBS} ${CMAKE_DL_LIBS} log Threads::Threads)
    xen_deploy_runtime_dlls(detector_typeinfo_lifetime_tests OPENCV ORT)
    add_test(NAME detector_typeinfo_lifetime_tests
        COMMAND detector_typeinfo_lifetime_tests
            "${PROJECT_SOURCE_DIR}/tests/fixtures/detector_typeinfo_segmentation.onnx")

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

    add_executable(capture_evidence_tests
        tests/capture_evidence_tests.cpp)
    target_link_libraries(capture_evidence_tests PRIVATE capture_evidence)
    xen_deploy_runtime_dlls(capture_evidence_tests OPENCV NDI)
    add_test(NAME capture_evidence_tests COMMAND capture_evidence_tests)
    set_tests_properties(capture_evidence_tests PROPERTIES RUN_SERIAL TRUE)
    add_executable(capture_evidence_producer_fixture tests/capture_evidence_producer_fixture.cpp)
    target_link_libraries(capture_evidence_producer_fixture PRIVATE capture_evidence nlohmann_json::nlohmann_json)
    xen_deploy_runtime_dlls(capture_evidence_producer_fixture OPENCV NDI)

    add_executable(aim_tests
        tests/aim_tests.cpp
        tests/aim_feedback_budget_tests.cpp
        tests/aim_width_observation_tests.cpp)
    target_link_libraries(aim_tests PRIVATE aim log)
    xen_deploy_runtime_dlls(aim_tests OPENCV)
    add_test(NAME aim_tests COMMAND aim_tests)

    add_executable(aim_transition_smoothing_tests tests/aim_transition_smoothing_tests.cpp)
    target_link_libraries(aim_transition_smoothing_tests PRIVATE aim log)
    xen_deploy_runtime_dlls(aim_transition_smoothing_tests OPENCV)
    add_test(NAME aim_transition_smoothing_tests COMMAND aim_transition_smoothing_tests)

    add_executable(aim_production_red_producer_tests
        tests/aim_production_red_producer_tests.cpp)
    target_link_libraries(aim_production_red_producer_tests PRIVATE
        aim_production_red config nlohmann_json::nlohmann_json)
    xen_deploy_runtime_dlls(aim_production_red_producer_tests OPENCV)
    add_test(NAME aim_production_red_producer_tests
        COMMAND aim_production_red_producer_tests)
    add_test(NAME aim_production_red_producer_integration_tests
        COMMAND "${XEN_PYTHON_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/aim_production_red_producer_integration_tests.py"
            --producer "$<TARGET_FILE:xen_aim_production_red_producer>"
            --evaluator "${PROJECT_SOURCE_DIR}/scripts/evaluate_aim_production_red.py"
            --powershell-executable "${XEN_POWERSHELL7_EXECUTABLE}"
            --test-root "${CMAKE_BINARY_DIR}/aim-production-red-producer-integration")
    add_test(NAME producer_integer_bounds_tests
        COMMAND "${XEN_PYTHON_EXECUTABLE}" -B -X utf8
            "${PROJECT_SOURCE_DIR}/tests/producer_integer_bounds_tests.py"
            --producer "$<TARGET_FILE:xen_aim_production_red_producer>"
            --output-root "${CMAKE_BINARY_DIR}/producer-integer-fixtures")
    add_test(NAME windows_long_path_cli_tests
        COMMAND "${XEN_PYTHON_EXECUTABLE}" -B -X utf8
            "${PROJECT_SOURCE_DIR}/tests/windows_long_path_cli_tests.py"
            --producer "$<TARGET_FILE:xen_aim_production_red_producer>"
            --sequence "$<TARGET_FILE:xen_mouse_effect_probe_sequence>"
            --evaluator "${PROJECT_SOURCE_DIR}/scripts/evaluate_aim_production_red.py"
            --powershell-executable "${XEN_POWERSHELL7_EXECUTABLE}"
            --test-root "${CMAKE_BINARY_DIR}/windows-long-path-cli-tests")
    set_tests_properties(windows_long_path_cli_tests PROPERTIES TIMEOUT 120)
    set_tests_properties(producer_integer_bounds_tests PROPERTIES TIMEOUT 90)
    add_test(NAME producer_completion_overflow_tests
        COMMAND "${XEN_PYTHON_EXECUTABLE}" -B -X utf8
            "${PROJECT_SOURCE_DIR}/tests/producer_completion_overflow_tests.py"
            --producer "$<TARGET_FILE:xen_aim_production_red_producer>"
            --output-root "${CMAKE_BINARY_DIR}/producer-completion-fixtures")
    set_tests_properties(producer_completion_overflow_tests PROPERTIES TIMEOUT 60)

    add_executable(aim_landmark_tests
        tests/aim_landmark_tests.cpp)
    target_link_libraries(aim_landmark_tests PRIVATE aim_landmark)
    add_test(NAME aim_landmark_tests COMMAND aim_landmark_tests)

    # Aim 真实视频真值解析和追踪指标保持为离线纯算法测试，不加载 ORT Session。
    add_executable(aim_evaluation_tests
        tests/aim_evaluation_tests.cpp)
    target_link_libraries(aim_evaluation_tests PRIVATE aim log)
    xen_deploy_runtime_dlls(aim_evaluation_tests OPENCV)
    add_test(NAME aim_evaluation_tests COMMAND aim_evaluation_tests)

    add_executable(aim_background_motion_tests tests/aim_background_motion_tests.cpp)
    target_link_libraries(aim_background_motion_tests PRIVATE aim log)
    xen_deploy_runtime_dlls(aim_background_motion_tests OPENCV)
    add_test(NAME aim_background_motion_tests COMMAND aim_background_motion_tests)

    add_executable(aim_prediction_width_tests tests/aim_prediction_width_tests.cpp)
    target_link_libraries(aim_prediction_width_tests PRIVATE aim log)
    xen_deploy_runtime_dlls(aim_prediction_width_tests OPENCV)
    add_test(NAME aim_prediction_width_tests COMMAND aim_prediction_width_tests)

    add_executable(aim_delay_origin_tests tests/aim_delay_origin_tests.cpp)
    target_link_libraries(aim_delay_origin_tests PRIVATE aim recoil log)
    xen_deploy_runtime_dlls(aim_delay_origin_tests OPENCV)
    add_test(NAME aim_delay_origin_tests COMMAND aim_delay_origin_tests)

    add_executable(aim_background_observation_time_tests
        tests/aim_background_observation_time_tests.cpp)
    target_link_libraries(aim_background_observation_time_tests PRIVATE aim log)
    xen_deploy_runtime_dlls(aim_background_observation_time_tests OPENCV)
    add_test(NAME aim_background_observation_time_tests
        COMMAND aim_background_observation_time_tests)

    add_executable(aim_observer_numeric_tests tests/aim_observer_numeric_tests.cpp)
    target_link_libraries(aim_observer_numeric_tests PRIVATE aim log)
    xen_deploy_runtime_dlls(aim_observer_numeric_tests OPENCV)
    add_test(NAME aim_observer_numeric_tests COMMAND aim_observer_numeric_tests)

    add_executable(aim_prediction_recovery_tests tests/aim_prediction_recovery_tests.cpp)
    target_link_libraries(aim_prediction_recovery_tests PRIVATE aim log nlohmann_json::nlohmann_json)
    target_compile_definitions(aim_prediction_recovery_tests PRIVATE
        XEN_AIM_PREDICTION_RECOVERY_FIXTURE="${PROJECT_SOURCE_DIR}/tests/fixtures/aim_combo_prediction_recovery.json")
    xen_deploy_runtime_dlls(aim_prediction_recovery_tests OPENCV)
    add_test(NAME aim_prediction_recovery_tests COMMAND aim_prediction_recovery_tests)

    add_executable(aim_prediction_source_loss_tests tests/aim_prediction_source_loss_tests.cpp)
    target_link_libraries(aim_prediction_source_loss_tests PRIVATE aim log nlohmann_json::nlohmann_json)
    target_compile_definitions(aim_prediction_source_loss_tests PRIVATE
        XEN_AIM_PREDICTION_SOURCE_LOSS_FIXTURE="${PROJECT_SOURCE_DIR}/tests/fixtures/aim_prediction_source_loss.json"
        XEN_AIM_PREDICTION_INDEPENDENT_FIXTURE="${PROJECT_SOURCE_DIR}/tests/fixtures/aim_prediction_independent.json")
    xen_deploy_runtime_dlls(aim_prediction_source_loss_tests OPENCV)
    add_test(NAME aim_prediction_source_loss_tests COMMAND aim_prediction_source_loss_tests)

    add_executable(aim_combo_dynamics_tests tests/aim_combo_dynamics_tests.cpp)
    target_link_libraries(aim_combo_dynamics_tests PRIVATE aim log)
    xen_deploy_runtime_dlls(aim_combo_dynamics_tests OPENCV)
    add_test(NAME aim_combo_dynamics_tests COMMAND aim_combo_dynamics_tests)
    add_test(NAME aim_long_delay_tests COMMAND aim_combo_dynamics_tests --long-delay)

    add_executable(aim_prediction_soft_allocation_tests tests/aim_prediction_soft_allocation_tests.cpp)
    target_link_libraries(aim_prediction_soft_allocation_tests PRIVATE aim log)
    xen_deploy_runtime_dlls(aim_prediction_soft_allocation_tests OPENCV)
    add_test(NAME aim_prediction_soft_allocation_tests COMMAND aim_prediction_soft_allocation_tests)

    add_executable(aim_prediction_gap_continuity_tests tests/aim_prediction_gap_continuity_tests.cpp)
    target_link_libraries(aim_prediction_gap_continuity_tests PRIVATE aim log)
    xen_deploy_runtime_dlls(aim_prediction_gap_continuity_tests OPENCV)
    add_test(NAME aim_prediction_gap_continuity_tests COMMAND aim_prediction_gap_continuity_tests)

    add_executable(aim_x_filter_reset_tests tests/aim_x_filter_reset_tests.cpp)
    target_link_libraries(aim_x_filter_reset_tests PRIVATE aim log)
    xen_deploy_runtime_dlls(aim_x_filter_reset_tests OPENCV)
    add_test(NAME aim_x_filter_reset_tests COMMAND aim_x_filter_reset_tests)

    add_executable(aim_integral_motion_support_tests
        tests/aim_integral_motion_support_tests.cpp)
    target_link_libraries(aim_integral_motion_support_tests PRIVATE aim log)
    xen_deploy_runtime_dlls(aim_integral_motion_support_tests OPENCV)
    add_test(NAME aim_integral_motion_support_tests
        COMMAND aim_integral_motion_support_tests)

    add_executable(aim_motion_inventory_tests tests/aim_motion_inventory_tests.cpp)
    target_link_libraries(aim_motion_inventory_tests PRIVATE aim log)
    xen_deploy_runtime_dlls(aim_motion_inventory_tests OPENCV)
    add_test(NAME aim_motion_inventory_tests COMMAND aim_motion_inventory_tests)
    add_executable(aim_opening_maintenance_tests tests/aim_opening_maintenance_tests.cpp)
    target_link_libraries(aim_opening_maintenance_tests PRIVATE aim log)
    xen_deploy_runtime_dlls(aim_opening_maintenance_tests OPENCV)
    add_test(NAME aim_opening_maintenance_tests COMMAND aim_opening_maintenance_tests)
    add_executable(aim_cross_side_maintenance_tests tests/aim_cross_side_maintenance_tests.cpp)
    target_link_libraries(aim_cross_side_maintenance_tests PRIVATE aim log)
    xen_deploy_runtime_dlls(aim_cross_side_maintenance_tests OPENCV)
    add_test(NAME aim_cross_side_maintenance_tests COMMAND aim_cross_side_maintenance_tests)
    add_executable(aim_unified_maintenance_tests tests/aim_unified_maintenance_tests.cpp)
    target_link_libraries(aim_unified_maintenance_tests PRIVATE aim log)
    xen_deploy_runtime_dlls(aim_unified_maintenance_tests OPENCV)
    add_test(NAME aim_unified_maintenance_tests COMMAND aim_unified_maintenance_tests)
    add_executable(aim_observed_feedback_tests tests/aim_observed_feedback_tests.cpp)
    target_link_libraries(aim_observed_feedback_tests PRIVATE aim log)
    xen_deploy_runtime_dlls(aim_observed_feedback_tests OPENCV)
    add_test(NAME aim_observed_feedback_tests COMMAND aim_observed_feedback_tests)
    add_executable(aim_world_observation_gap_tests tests/aim_world_observation_gap_tests.cpp)
    target_link_libraries(aim_world_observation_gap_tests PRIVATE aim log)
    xen_deploy_runtime_dlls(aim_world_observation_gap_tests OPENCV)
    add_test(NAME aim_world_observation_gap_tests COMMAND aim_world_observation_gap_tests)

    add_executable(aim_background_camera_translation_tests
        tests/aim_background_camera_translation_tests.cpp)
    target_link_libraries(aim_background_camera_translation_tests PRIVATE aim log)
    xen_deploy_runtime_dlls(aim_background_camera_translation_tests OPENCV)
    add_test(NAME aim_background_camera_translation_tests
        COMMAND aim_background_camera_translation_tests)

    add_executable(aim_feature_combinations_tests tests/aim_feature_combinations_tests.cpp)
    target_link_libraries(aim_feature_combinations_tests PRIVATE aim log)
    xen_deploy_runtime_dlls(aim_feature_combinations_tests OPENCV)
    add_test(NAME aim_feature_combinations_tests COMMAND aim_feature_combinations_tests)

    add_executable(runtime_camera_motion_tests
        tests/runtime_camera_motion_tests.cpp
        Xen/runtime/camera_motion.cpp)
    target_include_directories(runtime_camera_motion_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/Xen ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(runtime_camera_motion_tests PRIVATE aim ${OpenCV_LIBS})
    xen_deploy_runtime_dlls(runtime_camera_motion_tests OPENCV)
    add_test(NAME runtime_camera_motion_tests COMMAND runtime_camera_motion_tests)

    add_executable(runtime_aim_frame_tests
        tests/runtime_aim_frame_tests.cpp Xen/runtime/camera_motion.cpp Xen/runtime/runtime_queue.cpp)
    target_include_directories(runtime_aim_frame_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/Xen ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(runtime_aim_frame_tests PRIVATE aim weapon auto_stop_worker source_context trigger recoil ${OpenCV_LIBS})
    xen_deploy_runtime_dlls(runtime_aim_frame_tests OPENCV)
    add_test(NAME runtime_aim_frame_tests COMMAND runtime_aim_frame_tests)

    add_executable(aim_image_prediction_tests
        tests/aim_image_prediction_tests.cpp Xen/runtime/camera_motion.cpp Xen/runtime/runtime_queue.cpp)
    target_include_directories(aim_image_prediction_tests PRIVATE ${PROJECT_SOURCE_DIR}/Xen ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(aim_image_prediction_tests PRIVATE aim weapon auto_stop_worker ${OpenCV_LIBS})
    xen_deploy_runtime_dlls(aim_image_prediction_tests OPENCV)
    add_test(NAME aim_image_prediction_tests COMMAND aim_image_prediction_tests)

    add_executable(mouse_tests
        tests/mouse_tests.cpp)
    target_link_libraries(mouse_tests PRIVATE mouse log ws2_32)
    add_test(NAME mouse_tests COMMAND mouse_tests)

    add_executable(trigger_tests tests/trigger_tests.cpp)
    target_link_libraries(trigger_tests PRIVATE trigger)
    add_test(NAME trigger_tests COMMAND trigger_tests)
    add_executable(trigger_worker_tests tests/trigger_worker_tests.cpp)
    target_link_libraries(trigger_worker_tests PRIVATE trigger_worker)
    add_test(NAME trigger_worker_tests COMMAND trigger_worker_tests)
    add_executable(trigger_observation_clock_tests tests/trigger_observation_clock_tests.cpp)
    target_link_libraries(trigger_observation_clock_tests PRIVATE trigger auto_stop_worker mouse log Threads::Threads)
    target_compile_definitions(trigger_observation_clock_tests PRIVATE
        XEN_TRIGGER_WORKER_SOURCE="${PROJECT_SOURCE_DIR}/Xen/trigger/trigger_worker.cpp")
    add_test(NAME trigger_observation_clock_tests COMMAND trigger_observation_clock_tests)
    set_tests_properties(trigger_observation_clock_tests PROPERTIES TIMEOUT 15)
    add_executable(imgui_hotfix_tests tests/imgui_hotfix_tests.cpp)
    target_link_libraries(imgui_hotfix_tests PRIVATE imgui)
    add_test(NAME imgui_hotfix_tests COMMAND imgui_hotfix_tests)
    add_executable(kmbox_button_tests tests/kmbox_button_tests.cpp)
    target_link_libraries(kmbox_button_tests PRIVATE mouse ws2_32)
    add_test(NAME kmbox_button_tests COMMAND kmbox_button_tests)
    add_test(NAME kmbox_delayed_close_observer_tests COMMAND kmbox_button_tests --delayed-close-observer)
    add_test(NAME kmbox_duplicate_close_observer_tests COMMAND kmbox_button_tests --duplicate-close-observer)
    add_executable(source_context_tests tests/source_context_tests.cpp)
    target_link_libraries(source_context_tests PRIVATE source_context bcrypt ws2_32)
    add_test(NAME source_context_tests COMMAND source_context_tests)
    add_executable(auto_stop_tests tests/auto_stop_tests.cpp)
    target_link_libraries(auto_stop_tests PRIVATE auto_stop)
    add_test(NAME auto_stop_tests COMMAND auto_stop_tests)

    add_executable(reference_assessment_tests tests/reference_assessment_tests.cpp)
    target_link_libraries(reference_assessment_tests PRIVATE reference_assessment)
    add_test(NAME reference_assessment_tests COMMAND reference_assessment_tests)
    add_executable(hud_stop_tests tests/hud_stop_tests.cpp)
    target_link_libraries(hud_stop_tests PRIVATE hud_stop)
    add_test(NAME hud_stop_tests COMMAND hud_stop_tests)
    add_test(NAME hud_stop_worker_tests COMMAND auto_stop_worker_tests --hud-reference)
    add_test(NAME hud_weapon_stop_integration_tests COMMAND weapon_stop_integration_tests --hud-reference)
    add_test(NAME hud_aim_random_integration_tests COMMAND weapon_stop_integration_tests --aim-random)
    add_test(NAME live_game_data_root_tests COMMAND "${XEN_PYTHON_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/live_game_data_root_tests.py")
    add_test(NAME hud_stop_acceptance_tests COMMAND "${XEN_PYTHON_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/hud_stop_acceptance_tests.py")
    add_test(NAME reference_compare_cli_tests COMMAND "${XEN_PYTHON_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/reference_compare_cli_tests.py"
        "$<TARGET_FILE:xen_reference_compare>" "${PROJECT_SOURCE_DIR}/assets/reference_assessment/example.json")

    add_executable(auto_stop_worker_tests tests/auto_stop_worker_tests.cpp)
    target_link_libraries(auto_stop_worker_tests PRIVATE auto_stop_worker movement)
    add_test(NAME auto_stop_worker_tests COMMAND auto_stop_worker_tests)

    add_executable(auto_stop_probe_tests tests/auto_stop_probe_tests.cpp)
    target_include_directories(auto_stop_probe_tests PRIVATE ${PROJECT_SOURCE_DIR}/Xen)
    target_link_libraries(auto_stop_probe_tests PRIVATE mouse nlohmann_json::nlohmann_json)
    add_test(NAME auto_stop_probe_tests COMMAND auto_stop_probe_tests)
    add_executable(auto_stop_counterpulse_tests tests/auto_stop_counterpulse_tests.cpp)
    target_include_directories(auto_stop_counterpulse_tests PRIVATE ${PROJECT_SOURCE_DIR}/Xen)
    target_link_libraries(auto_stop_counterpulse_tests PRIVATE mouse nlohmann_json::nlohmann_json)
    add_test(NAME auto_stop_counterpulse_tests COMMAND auto_stop_counterpulse_tests)
    add_executable(auto_stop_sampling_analysis_tests tests/auto_stop_sampling_analysis_tests.cpp)
    target_include_directories(auto_stop_sampling_analysis_tests PRIVATE ${PROJECT_SOURCE_DIR}/Xen)
    target_link_libraries(auto_stop_sampling_analysis_tests PRIVATE mouse nlohmann_json::nlohmann_json)
    add_test(NAME auto_stop_sampling_analysis_tests COMMAND auto_stop_sampling_analysis_tests)
    add_executable(auto_stop_manual_sampling_tests tests/auto_stop_manual_sampling_tests.cpp)
    target_include_directories(auto_stop_manual_sampling_tests PRIVATE ${PROJECT_SOURCE_DIR}/Xen)
    target_link_libraries(auto_stop_manual_sampling_tests PRIVATE mouse nlohmann_json::nlohmann_json)
    add_test(NAME auto_stop_manual_sampling_tests COMMAND auto_stop_manual_sampling_tests)
    add_executable(auto_stop_manual_monitor_tests tests/auto_stop_manual_monitor_tests.cpp)
    target_include_directories(auto_stop_manual_monitor_tests PRIVATE ${PROJECT_SOURCE_DIR}/Xen)
    target_link_libraries(auto_stop_manual_monitor_tests PRIVATE input_training mouse nlohmann_json::nlohmann_json ws2_32)
    add_test(NAME auto_stop_manual_monitor_tests COMMAND auto_stop_manual_monitor_tests)
    add_executable(auto_stop_manual_finalize_tests tests/auto_stop_manual_finalize_tests.cpp)
    target_include_directories(auto_stop_manual_finalize_tests PRIVATE ${PROJECT_SOURCE_DIR}/Xen)
    target_link_libraries(auto_stop_manual_finalize_tests PRIVATE input_training mouse nlohmann_json::nlohmann_json)
    add_test(NAME auto_stop_manual_finalize_tests COMMAND auto_stop_manual_finalize_tests)
    add_executable(auto_stop_manual_labels_tests tests/auto_stop_manual_labels_tests.cpp)
    target_include_directories(auto_stop_manual_labels_tests PRIVATE ${PROJECT_SOURCE_DIR}/Xen)
    target_link_libraries(auto_stop_manual_labels_tests PRIVATE mouse nlohmann_json::nlohmann_json)
    add_test(NAME auto_stop_manual_labels_tests COMMAND auto_stop_manual_labels_tests)
    add_executable(auto_stop_hud_feedback_tests tests/auto_stop_hud_feedback_tests.cpp)
    target_include_directories(auto_stop_hud_feedback_tests PRIVATE ${PROJECT_SOURCE_DIR}/Xen)
    target_link_libraries(auto_stop_hud_feedback_tests PRIVATE mouse nlohmann_json::nlohmann_json)
    add_test(NAME auto_stop_hud_feedback_tests COMMAND auto_stop_hud_feedback_tests)
    add_executable(auto_stop_manual_intervals_tests tests/auto_stop_manual_intervals_tests.cpp)
    target_include_directories(auto_stop_manual_intervals_tests PRIVATE ${PROJECT_SOURCE_DIR}/Xen)
    target_link_libraries(auto_stop_manual_intervals_tests PRIVATE mouse nlohmann_json::nlohmann_json)
    add_test(NAME auto_stop_manual_intervals_tests COMMAND auto_stop_manual_intervals_tests)
    add_executable(auto_stop_default_baseline_tests tests/auto_stop_default_baseline_tests.cpp)
    target_include_directories(auto_stop_default_baseline_tests PRIVATE ${PROJECT_SOURCE_DIR}/Xen)
    target_link_libraries(auto_stop_default_baseline_tests PRIVATE input_training mouse nlohmann_json::nlohmann_json)
    add_test(NAME auto_stop_default_baseline_tests COMMAND auto_stop_default_baseline_tests)
    add_executable(auto_stop_manual_plan_tests tests/auto_stop_manual_plan_tests.cpp)
    target_include_directories(auto_stop_manual_plan_tests PRIVATE ${PROJECT_SOURCE_DIR}/Xen)
    target_link_libraries(auto_stop_manual_plan_tests PRIVATE input_training mouse nlohmann_json::nlohmann_json)
    add_test(NAME auto_stop_manual_plan_tests COMMAND auto_stop_manual_plan_tests)
    add_test(NAME auto_stop_manual_cli_tests COMMAND "${XEN_PYTHON_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/auto_stop_manual_cli_tests.py" --executable "$<TARGET_FILE:auto_stop_counterpulse>")
    add_executable(auto_stop_counterpulse_hud_tests tests/auto_stop_counterpulse_hud_tests.cpp
        Xen/auto_stop_probe/counterpulse_hud.cpp)
    target_include_directories(auto_stop_counterpulse_hud_tests PRIVATE ${PROJECT_SOURCE_DIR}/Xen)
    target_link_libraries(auto_stop_counterpulse_hud_tests PRIVATE mouse nlohmann_json::nlohmann_json user32 gdi32)
    add_test(NAME auto_stop_counterpulse_hud_tests COMMAND auto_stop_counterpulse_hud_tests)
    add_executable(auto_stop_training_evaluation_tests tests/auto_stop_training_evaluation_tests.cpp)
    target_include_directories(auto_stop_training_evaluation_tests PRIVATE ${PROJECT_SOURCE_DIR}/Xen)
    target_link_libraries(auto_stop_training_evaluation_tests PRIVATE input_training mouse nlohmann_json::nlohmann_json)
    add_test(NAME auto_stop_training_evaluation_tests COMMAND auto_stop_training_evaluation_tests)
    add_executable(auto_stop_readiness_tests tests/auto_stop_readiness_tests.cpp)
    target_include_directories(auto_stop_readiness_tests PRIVATE ${PROJECT_SOURCE_DIR}/Xen)
    target_link_libraries(auto_stop_readiness_tests PRIVATE mouse source_context nlohmann_json::nlohmann_json)
    add_test(NAME auto_stop_readiness_tests COMMAND auto_stop_readiness_tests)
    add_executable(auto_stop_preroll_tests tests/auto_stop_preroll_tests.cpp)
    target_include_directories(auto_stop_preroll_tests PRIVATE ${PROJECT_SOURCE_DIR}/Xen)
    add_test(NAME auto_stop_preroll_tests COMMAND auto_stop_preroll_tests)
    add_test(NAME auto_stop_counterpulse_cli_tests COMMAND "${XEN_PYTHON_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/auto_stop_counterpulse_cli_tests.py" --executable "$<TARGET_FILE:auto_stop_counterpulse>"
        --powershell-executable "${XEN_POWERSHELL7_EXECUTABLE}"
        --windows-powershell-executable "${XEN_WINDOWS_POWERSHELL}")
    add_test(NAME auto_stop_impacts_tests COMMAND "${XEN_PYTHON_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/auto_stop_impacts_tests.py")
    add_test(NAME auto_stop_hud_analysis_tests
        COMMAND "${XEN_PYTHON_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/auto_stop_hud_analysis_tests.py")

    add_executable(kmbox_keyboard_tests tests/kmbox_keyboard_tests.cpp)
    target_link_libraries(kmbox_keyboard_tests PRIVATE mouse ws2_32)
    add_test(NAME kmbox_keyboard_tests COMMAND kmbox_keyboard_tests)

    # 子进程仅调用公开 lease，不创建后端；分别设置 TMP/TEMP 验证生产独占。
    add_executable(mouse_output_owner_lease_tests
        tests/mouse_output_owner_lease_tests.cpp)
    target_link_libraries(mouse_output_owner_lease_tests PRIVATE mouse)
    add_test(NAME mouse_output_owner_lease_tests
        COMMAND "${CMAKE_COMMAND}" -E env --unset=PSModulePath --
            "${XEN_POWERSHELL7_EXECUTABLE}"
            -NoProfile -ExecutionPolicy Bypass
            -File "${PROJECT_SOURCE_DIR}/tests/mouse_output_owner_lease_tests.ps1"
            -LeaseExecutable "$<TARGET_FILE:mouse_output_owner_lease_tests>"
            -TestRoot "${CMAKE_BINARY_DIR}/mouse-output-owner-lease-tests")

    add_test(NAME source_context_session_tests
        COMMAND "${CMAKE_COMMAND}" -E env --unset=PSModulePath --
            "${XEN_POWERSHELL7_EXECUTABLE}" -NoProfile -ExecutionPolicy Bypass
            -File "${PROJECT_SOURCE_DIR}/tests/source_context_session_tests.ps1")

    add_executable(mouse_benchmark_tests
        tests/mouse_benchmark_tests.cpp)
    target_link_libraries(mouse_benchmark_tests PRIVATE
        mouse_benchmark ws2_32)
    add_test(NAME mouse_benchmark_tests COMMAND mouse_benchmark_tests)

    add_executable(mouse_effect_probe_tests
        tests/mouse_effect_probe_tests.cpp)
    target_link_libraries(mouse_effect_probe_tests PRIVATE mouse_effect_probe nlohmann_json::nlohmann_json)
    add_test(NAME mouse_effect_probe_tests COMMAND mouse_effect_probe_tests)
    add_executable(sequence_file_budget_tests tests/sequence_file_budget_tests.cpp)
    target_link_libraries(sequence_file_budget_tests PRIVATE mouse_effect_probe)
    add_test(NAME sequence_file_budget_tests COMMAND sequence_file_budget_tests)
    set_tests_properties(sequence_file_budget_tests PROPERTIES RUN_SERIAL TRUE)

    add_executable(mouse_effect_probe_runner_tests
        tests/mouse_effect_probe_runner_tests.cpp)
    target_link_libraries(mouse_effect_probe_runner_tests
        PRIVATE mouse_effect_probe_runner)
    add_test(NAME mouse_effect_probe_runner_tests
        COMMAND mouse_effect_probe_runner_tests)

    add_executable(debug_runner_tests tests/debug_runner_tests.cpp)
    add_executable(debug_source_epoch_tests tests/debug_source_epoch_tests.cpp)
    target_link_libraries(debug_source_epoch_tests PRIVATE debug_runner ws2_32)
    xen_deploy_runtime_dlls(debug_source_epoch_tests OPENCV NDI)
    add_test(NAME debug_source_epoch_tests COMMAND debug_source_epoch_tests)
    set_tests_properties(debug_source_epoch_tests PROPERTIES TIMEOUT 30)
    target_link_libraries(debug_runner_tests PRIVATE debug_runner)
    add_test(NAME debug_runner_tests COMMAND debug_runner_tests)
    xen_deploy_runtime_dlls(debug_runner_tests OPENCV NDI)
    add_executable(debug_session_tests tests/debug_session_tests.cpp)
    add_executable(debug_cleanup_result_tests tests/debug_cleanup_result_tests.cpp)
    target_link_libraries(debug_cleanup_result_tests PRIVATE debug_session ws2_32)
    xen_deploy_runtime_dlls(debug_cleanup_result_tests OPENCV ORT NDI)
    add_test(NAME debug_cleanup_result_tests COMMAND debug_cleanup_result_tests "${CMAKE_BINARY_DIR}/debug-cleanup-fixtures")
    set_tests_properties(debug_cleanup_result_tests PROPERTIES TIMEOUT 30)
    target_link_libraries(debug_session_tests PRIVATE debug_session)
    add_test(NAME debug_session_tests COMMAND debug_session_tests)
    add_executable(recoil_source_epoch_tests tests/recoil_source_epoch_tests.cpp)
    target_link_libraries(recoil_source_epoch_tests PRIVATE debug_session)
    xen_deploy_runtime_dlls(recoil_source_epoch_tests OPENCV ORT NDI)
    add_test(NAME recoil_source_epoch_tests COMMAND recoil_source_epoch_tests)
    set_tests_properties(recoil_source_epoch_tests PROPERTIES TIMEOUT 30)
    add_executable(wall_capture_run_tests tests/wall_capture_run_tests.cpp)
    target_link_libraries(wall_capture_run_tests PRIVATE debug_session)
    xen_deploy_runtime_dlls(wall_capture_run_tests OPENCV ORT NDI)
    add_test(NAME wall_capture_run_tests COMMAND wall_capture_run_tests)
    xen_deploy_runtime_dlls(debug_session_tests OPENCV ORT NDI)
    set_tests_properties(debug_session_tests PROPERTIES TIMEOUT 30)
    add_executable(debug_panel_tests tests/debug_panel_tests.cpp Xen/overlay/debug_panel.cpp)
    target_link_libraries(debug_panel_tests PRIVATE debug_session imgui)
    add_test(NAME debug_panel_tests COMMAND debug_panel_tests)
    xen_deploy_runtime_dlls(debug_panel_tests OPENCV ORT NDI)
    add_executable(recoil_editor_job_tests tests/recoil_editor_job_tests.cpp)
    target_include_directories(recoil_editor_job_tests PRIVATE ${PROJECT_SOURCE_DIR}/Xen)
    add_test(NAME recoil_editor_job_tests COMMAND recoil_editor_job_tests)
    set_tests_properties(recoil_editor_job_tests PROPERTIES TIMEOUT 10)

    add_executable(input_training_tests tests/input_training_tests.cpp)
    target_link_libraries(input_training_tests PRIVATE input_training)
    add_test(NAME input_training_tests COMMAND input_training_tests)
    add_executable(input_training_start_failure_tests tests/input_training_start_failure_tests.cpp)
    target_link_libraries(input_training_start_failure_tests PRIVATE input_training)
    add_test(NAME input_training_start_failure_tests COMMAND input_training_start_failure_tests)
    add_executable(input_training_commit_tests tests/input_training_commit_tests.cpp)
    target_link_libraries(input_training_commit_tests PRIVATE input_training)
    add_test(NAME input_training_commit_tests COMMAND input_training_commit_tests)

    add_executable(test_mouse_input_reports tests/test_mouse_input_reports.cpp)
    target_include_directories(test_mouse_input_reports PRIVATE ${PROJECT_SOURCE_DIR}/Xen)
    add_test(NAME mouse_input_report_tests COMMAND test_mouse_input_reports)

    add_executable(movement_tests tests/movement_tests.cpp)
    target_link_libraries(movement_tests PRIVATE movement)
    add_test(NAME movement_tests COMMAND movement_tests)
    xen_deploy_runtime_dlls(movement_tests)

    add_executable(input_training_panel_tests tests/input_training_panel_tests.cpp
        Xen/overlay/input_training_panel.cpp)
    target_link_libraries(input_training_panel_tests PRIVATE input_training imgui runtime)
    add_test(NAME input_training_panel_tests COMMAND input_training_panel_tests)

    add_executable(runtime_input_training_tests tests/runtime_input_training_tests.cpp)
    target_link_libraries(runtime_input_training_tests PRIVATE runtime)
    xen_deploy_runtime_dlls(runtime_input_training_tests OPENCV ORT NDI)
    add_test(NAME runtime_input_training_tests COMMAND runtime_input_training_tests)

    add_executable(runtime_tests
        tests/runtime_tests.cpp
        Xen/runtime/runtime_queue.cpp)
    target_include_directories(runtime_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/Xen
        ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(runtime_tests PRIVATE aim weapon ${OpenCV_LIBS} Threads::Threads)
    xen_deploy_runtime_dlls(runtime_tests OPENCV)
    add_test(NAME runtime_tests COMMAND runtime_tests)

    # 输入健康专项串联公开 KeyboardListener 与 Runtime intent；不启动
    # Capture、Detector 或真实鼠标后端，也不触发任何物理输出。
    add_executable(input_safety_tests
        tests/input_safety_tests.cpp)
    target_link_libraries(input_safety_tests PRIVATE app_input)
    xen_deploy_runtime_dlls(input_safety_tests OPENCV ORT NDI)
    add_test(NAME input_safety_tests COMMAND input_safety_tests)

    add_executable(data_collection_tests tests/data_collection_tests.cpp)
    target_link_libraries(data_collection_tests PRIVATE data_collection nlohmann_json::nlohmann_json)
    xen_deploy_runtime_dlls(data_collection_tests OPENCV)
    add_test(NAME data_collection_tests COMMAND data_collection_tests)

    add_executable(model_workspace_tests tests/model_workspace_tests.cpp)
    target_link_libraries(model_workspace_tests PRIVATE model_workspace nlohmann_json::nlohmann_json)
    xen_deploy_runtime_dlls(model_workspace_tests OPENCV)
    add_test(NAME model_workspace_tests COMMAND model_workspace_tests
        "${XEN_PYTHON_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/model_workspace_worker_fixture.py")
    add_test(NAME evidence_publication_tests
        COMMAND "${XEN_PYTHON_EXECUTABLE}" -B -X utf8
        "${PROJECT_SOURCE_DIR}/tests/evidence_publication_tests.py")
    add_test(NAME publication_script_tests
        COMMAND "${XEN_POWERSHELL7_EXECUTABLE}" -NoProfile -ExecutionPolicy Bypass
        -File "${PROJECT_SOURCE_DIR}/tests/publication_script_tests.ps1")
    add_test(NAME reference_compare_package_tests
        COMMAND "${XEN_POWERSHELL7_EXECUTABLE}" -NoProfile -ExecutionPolicy Bypass
        -File "${PROJECT_SOURCE_DIR}/tests/reference_compare_package_tests.ps1")
    add_test(NAME annotation_roi_tests
        COMMAND "${XEN_PYTHON_EXECUTABLE}" -B -X utf8
        "${PROJECT_SOURCE_DIR}/tests/annotation_roi_tests.py")
    add_test(NAME model_reference_compare_tests
        COMMAND "${XEN_PYTHON_EXECUTABLE}" -B -X utf8
        "${PROJECT_SOURCE_DIR}/tests/model_reference_compare_tests.py")
    add_test(NAME model_data_pipeline_tests
        COMMAND "${XEN_PYTHON_EXECUTABLE}" -B -X utf8
        "${PROJECT_SOURCE_DIR}/tests/model_data_pipeline_tests.py")
    add_test(NAME dataset_snapshot_tests
        COMMAND "${XEN_PYTHON_EXECUTABLE}" -B -X utf8
        "${PROJECT_SOURCE_DIR}/tests/dataset_snapshot_tests.py"
        --output-root "${CMAKE_BINARY_DIR}/dataset-snapshot-fixtures")
    add_test(NAME model_data_review_tests
        COMMAND "${XEN_PYTHON_EXECUTABLE}" -B -X utf8
        "${PROJECT_SOURCE_DIR}/tests/model_data_review_tests.py")
    add_test(NAME model_training_environment_tests
        COMMAND "${XEN_PYTHON_EXECUTABLE}" -B -X utf8
        "${PROJECT_SOURCE_DIR}/tests/model_training_environment_tests.py")
    add_test(NAME training_version_contract_tests
        COMMAND "${XEN_PYTHON_EXECUTABLE}" -B -X utf8
        "${PROJECT_SOURCE_DIR}/tests/training_version_contract_tests.py"
        --output-root "${CMAKE_BINARY_DIR}/training-version-fixtures")
    set_tests_properties(training_version_contract_tests PROPERTIES TIMEOUT 30)
    add_test(NAME model_data_integration_tests
        COMMAND "${XEN_PYTHON_EXECUTABLE}" -B -X utf8
        "${PROJECT_SOURCE_DIR}/tests/model_data_integration_tests.py"
        "$<TARGET_FILE:data_collection_tests>"
        "${PROJECT_SOURCE_DIR}/scripts/model_data_pipeline.py")

    # 独立窗口验收host不创建Runtime或任何设备；仅显式运行时保存自身窗口。
    add_executable(model_workspace_ui_preview tests/model_workspace_ui_preview.cpp)
    target_link_libraries(model_workspace_ui_preview PRIVATE overlay imgui ${OpenCV_LIBS} log)
    xen_deploy_runtime_dlls(model_workspace_ui_preview OPENCV ORT NDI)
    add_executable(debug_workspace_ui_preview tests/debug_workspace_ui_preview.cpp)
    target_link_libraries(debug_workspace_ui_preview PRIVATE overlay imgui ${OpenCV_LIBS} log)
    xen_deploy_runtime_dlls(debug_workspace_ui_preview OPENCV ORT NDI)

    # 通过生产 wWinMain 验证完整退出屏障；测试入口只驱动界面并观察回收。
    add_executable(app_file_dialog_shutdown_tests
        tests/app_file_dialog_shutdown_tests.cpp
        Xen/app/main.cpp
        Xen/app/model_catalog.cpp
        Xen/app/xen.rc)
    add_dependencies(app_file_dialog_shutdown_tests
        xen_recoil_calibration xen_recoil_tuner model_workspace_tools)
    target_link_libraries(app_file_dialog_shutdown_tests PRIVATE
        overlay app_input app_startup runtime xen_debug config crash log app_release imgui)
    xen_deploy_runtime_dlls(app_file_dialog_shutdown_tests OPENCV ORT NDI)
    foreach(dialog_state picker error)
        add_test(NAME app_file_dialog_shutdown_${dialog_state}
            COMMAND app_file_dialog_shutdown_tests
                "${CMAKE_BINARY_DIR}/app-file-dialog-shutdown-tests" ${dialog_state})
        set_tests_properties(app_file_dialog_shutdown_${dialog_state} PROPERTIES
            TIMEOUT 45 RUN_SERIAL TRUE LABELS "app;ui;shutdown")
    endforeach()

    # 显式桌面验收：启动实际 Xen.exe，界面操作与进程外回收见证分离。
    add_executable(xen_exe_shutdown_probe tests/xen_exe_shutdown_probe.cpp)
    target_link_libraries(xen_exe_shutdown_probe PRIVATE config recoil nlohmann_json::nlohmann_json ${OpenCV_LIBS} ws2_32)
    add_dependencies(xen_exe_shutdown_probe xen_app)
    xen_deploy_runtime_dlls(xen_exe_shutdown_probe OPENCV)

    # 测试宿主构造 Runtime 与弹窗共存；保留实际应用的运行时编辑限制。
    add_executable(runtime_file_dialog_shutdown_tests tests/runtime_file_dialog_shutdown_tests.cpp Xen/app/xen.rc)
    target_link_libraries(runtime_file_dialog_shutdown_tests PRIVATE
        overlay runtime xen_debug config recoil log imgui ${OpenCV_LIBS} ws2_32)
    xen_deploy_runtime_dlls(runtime_file_dialog_shutdown_tests OPENCV ORT NDI)
    foreach(dialog_state picker error)
        add_test(NAME runtime_file_dialog_shutdown_${dialog_state}
            COMMAND runtime_file_dialog_shutdown_tests
                "${CMAKE_BINARY_DIR}/runtime-file-dialog-shutdown-tests" ${dialog_state}
                "${PROJECT_SOURCE_DIR}/tests/fixtures/detector_typeinfo_segmentation.onnx")
        set_tests_properties(runtime_file_dialog_shutdown_${dialog_state} PROPERTIES
            TIMEOUT 60 RUN_SERIAL TRUE LABELS "runtime;ui;shutdown")
    endforeach()

    # Overlay Present 系统 seam 与标量 helper 均不创建 Win32/D3D11 窗口，
    # 测试只注入 HRESULT，也不链接 ImGui。
    add_executable(overlay_tests
        tests/overlay_tests.cpp)
    target_include_directories(overlay_tests PRIVATE ${PROJECT_SOURCE_DIR}/Xen)
    target_link_libraries(overlay_tests PRIVATE config)
    add_test(NAME overlay_tests COMMAND overlay_tests)

    add_executable(hotkey_device_capture_tests tests/hotkey_device_capture_tests.cpp)
    # overlay_internal.h 的状态摘要使用 Trigger 公有类型，传递其头文件依赖。
    target_link_libraries(hotkey_device_capture_tests PRIVATE keyboard trigger)
    add_test(NAME hotkey_device_capture_tests COMMAND hotkey_device_capture_tests)

    # 只初始化 Overlay；没有设备/Runtime 实例，供实际窗口布局复核。
    add_executable(auxiliary_ui_preview tests/auxiliary_ui_preview.cpp)
    target_link_libraries(auxiliary_ui_preview PRIVATE overlay)
    xen_deploy_runtime_dlls(auxiliary_ui_preview OPENCV ORT NDI)

    # 模型目录测试只验证 App 的路径、枚举和安全边界，不创建 Overlay 或 ORT Session。
    add_executable(model_catalog_tests
        tests/model_catalog_tests.cpp
        Xen/app/model_catalog.cpp)
    target_include_directories(
        model_catalog_tests PRIVATE ${PROJECT_SOURCE_DIR}/Xen)
    add_test(NAME model_catalog_tests COMMAND model_catalog_tests)

    add_executable(release_contract_tests
        tests/release_contract_tests.cpp)
    target_link_libraries(release_contract_tests PRIVATE app_release)
    add_test(NAME release_contract_tests COMMAND release_contract_tests)

    # Launcher 决策专项只解析隔离临时配置并选择清单 Worker；不调用
    # CreateProcessW，也不启动 GUI、Runtime 或任何物理输出。
    add_executable(launcher_decision_tests
        tests/launcher_decision_tests.cpp)
    target_link_libraries(launcher_decision_tests PRIVATE app_launcher)
    add_test(NAME launcher_decision_tests COMMAND launcher_decision_tests)

    # GUI 启动失败专项只注入 Log/Overlay 结果与错误展示 adapter；不创建
    # Win32/D3D11 窗口，也不启动 Runtime、Worker 或任何物理输出。
    add_executable(app_startup_tests
        tests/app_startup_tests.cpp)
    target_link_libraries(app_startup_tests PRIVATE app_startup app_release)
    add_test(NAME app_startup_tests COMMAND app_startup_tests)

    add_test(NAME mouse_effect_probe_prbs_design_tests
        COMMAND "${XEN_PYTHON_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/mouse_effect_probe_prbs_design_tests.py")
    add_test(NAME mouse_effect_probe_b_analysis_tests
        COMMAND "${XEN_PYTHON_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/mouse_effect_probe_b_analysis_tests.py")
    add_test(NAME mouse_effect_probe_b_holdout_analysis_tests
        COMMAND "${XEN_PYTHON_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/mouse_effect_probe_b_holdout_analysis_tests.py")
    # 只分析合成数组和本轮 fixture；禁写源码缓存，统一 CTest 管道中的中文编码。
    add_test(NAME mouse_effect_probe_a2_calibration_tests
        COMMAND "${XEN_PYTHON_EXECUTABLE}" -B -X utf8
            "${PROJECT_SOURCE_DIR}/tests/mouse_effect_probe_a2_calibration_tests.py")
    add_test(NAME mouse_effect_probe_a2_physical_analysis_tests
        COMMAND "${XEN_PYTHON_EXECUTABLE}" -B -X utf8
            "${PROJECT_SOURCE_DIR}/tests/mouse_effect_probe_a2_physical_analysis_tests.py")
    add_test(NAME mouse_effect_probe_physical_analysis_tests
        COMMAND "${XEN_PYTHON_EXECUTABLE}" -B -X utf8
            "${PROJECT_SOURCE_DIR}/tests/mouse_effect_probe_physical_analysis_tests.py")
    add_test(NAME mouse_effect_probe_pixel_analysis_tests
        COMMAND "${XEN_PYTHON_EXECUTABLE}" -B -X utf8
            "${PROJECT_SOURCE_DIR}/tests/mouse_effect_probe_pixel_analysis_tests.py")
    # Production red evaluator 只消费 hash-bound 离线 trace；不加载 Aim、
    # Mouse 或任何真实输出后端。
    add_test(NAME aim_production_red_evaluator_tests
        COMMAND "${XEN_PYTHON_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/aim_production_red_evaluator_tests.py")
    add_test(NAME evaluator_time_contract_tests
        COMMAND "${XEN_PYTHON_EXECUTABLE}" -B -X utf8
            "${PROJECT_SOURCE_DIR}/tests/evaluator_time_contract_tests.py"
            --output-root "${CMAKE_BINARY_DIR}/evaluator-time-fixtures")
    set_tests_properties(evaluator_time_contract_tests PROPERTIES TIMEOUT 30)
    add_test(NAME aim_production_red_plan_builder_tests
        COMMAND "${XEN_PYTHON_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/aim_production_red_plan_builder_tests.py"
            --builder
                "${PROJECT_SOURCE_DIR}/scripts/build_aim_production_red_dbed788b_plan.py"
            --fixture-header
                "${PROJECT_SOURCE_DIR}/tests/aim_x_latest_pixel_holdout_fixture.h"
            --powershell-executable "${XEN_POWERSHELL7_EXECUTABLE}"
            --test-root
                "${CMAKE_BINARY_DIR}/aim-production-red-plan-builder-tests")
    add_test(NAME aim_production_red_fidelity_plan_builder_tests
        COMMAND "${XEN_PYTHON_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/aim_production_red_fidelity_plan_builder_tests.py"
            --builder
                "${PROJECT_SOURCE_DIR}/scripts/build_aim_production_red_dbed788b_fidelity_plan.py"
            --powershell-executable "${XEN_POWERSHELL7_EXECUTABLE}"
            --test-root
                "${CMAKE_BINARY_DIR}/aim-production-red-fidelity-plan-builder-tests")
    add_test(NAME mouse_effect_probe_a2_magnitude_domain_tests
        COMMAND "${XEN_PYTHON_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/mouse_effect_probe_a2_magnitude_domain_tests.py"
            --analyzer
                "${PROJECT_SOURCE_DIR}/scripts/analyze_mouse_effect_probe_a2_magnitude_domain.py")
    add_test(NAME mouse_effect_probe_b_command_magnitude_analysis_tests
        COMMAND "${XEN_PYTHON_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/mouse_effect_probe_b_command_magnitude_analysis_tests.py"
            --analyzer
                "${PROJECT_SOURCE_DIR}/scripts/analyze_mouse_effect_probe_b_command_magnitude.py")
    add_test(NAME mouse_effect_probe_b_completion_phase_analysis_tests
        COMMAND "${XEN_PYTHON_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/mouse_effect_probe_b_completion_phase_analysis_tests.py"
            --analyzer
                "${PROJECT_SOURCE_DIR}/scripts/analyze_mouse_effect_probe_b_completion_phase.py")
    add_test(NAME mouse_effect_probe_b_composite_phase_evaluator_tests
        COMMAND "${XEN_PYTHON_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/mouse_effect_probe_b_composite_phase_evaluator_tests.py"
            --evaluator
                "${PROJECT_SOURCE_DIR}/scripts/evaluate_mouse_effect_probe_b_composite_phase.py")
    add_test(NAME mouse_effect_probe_b_composite_phase_binder_tests
        COMMAND "${XEN_PYTHON_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/mouse_effect_probe_b_composite_phase_binder_tests.py"
            --binder
                "${PROJECT_SOURCE_DIR}/scripts/bind_mouse_effect_probe_b_composite_phase_calibration.py"
            --evaluator
                "${PROJECT_SOURCE_DIR}/scripts/evaluate_mouse_effect_probe_b_composite_phase.py")
    add_test(NAME mouse_effect_probe_b_composite_phase_plan_tests
        COMMAND "${XEN_PYTHON_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/mouse_effect_probe_b_composite_phase_plan_tests.py"
            --generator
                "${PROJECT_SOURCE_DIR}/scripts/freeze_mouse_effect_probe_b_composite_phase_plan.py"
            --sequence-executable
                "$<TARGET_FILE:xen_mouse_effect_probe_sequence>"
            --binder
                "${PROJECT_SOURCE_DIR}/scripts/bind_mouse_effect_probe_b_composite_phase_calibration.py"
            --evaluator
                "${PROJECT_SOURCE_DIR}/scripts/evaluate_mouse_effect_probe_b_composite_phase.py"
            --producer
                "${PROJECT_SOURCE_DIR}/scripts/produce_mouse_effect_probe_b_composite_phase_ledgers.py"
            --seal-executable
                "$<TARGET_FILE:xen_mouse_effect_probe_composite_seal>")
    add_executable(mouse_effect_probe_scheduler_study_tests
        tests/mouse_effect_probe_scheduler_study_tests.cpp)
    target_include_directories(mouse_effect_probe_scheduler_study_tests PRIVATE
        "${PROJECT_SOURCE_DIR}/Xen")
    target_link_libraries(mouse_effect_probe_scheduler_study_tests PRIVATE
        nlohmann_json::nlohmann_json)
    add_test(NAME mouse_effect_probe_scheduler_study_tests
        COMMAND mouse_effect_probe_scheduler_study_tests)
    add_executable(mouse_effect_probe_scheduler_study_driver_tests
        tests/mouse_effect_probe_scheduler_study_driver_tests.cpp)
    target_include_directories(mouse_effect_probe_scheduler_study_driver_tests PRIVATE
        "${PROJECT_SOURCE_DIR}/Xen")
    # 测试包含真实 Seal 入口，必须同步链接其无设备报告校验依赖。
    target_link_libraries(mouse_effect_probe_scheduler_study_driver_tests PRIVATE
        mouse_effect_probe_report_validation nlohmann_json::nlohmann_json bcrypt)
    add_test(NAME mouse_effect_probe_scheduler_study_driver_tests
        COMMAND mouse_effect_probe_scheduler_study_driver_tests)
    add_test(NAME mouse_effect_probe_scheduler_study_cli_tests
        COMMAND "${XEN_PYTHON_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/mouse_effect_probe_scheduler_study_cli_tests.py"
            --seal-executable "$<TARGET_FILE:xen_mouse_effect_probe_composite_seal>"
            --selector-executable "$<TARGET_FILE:mouse_effect_probe_scheduler_study_tests>"
            --evidence-root "${CMAKE_BINARY_DIR}/scheduler-study-test-evidence")
    foreach(report_case semantics frozen-report)
        add_test(NAME composite_report_${report_case}
            COMMAND "${XEN_PYTHON_EXECUTABLE}" -B -X utf8
                "${PROJECT_SOURCE_DIR}/tests/composite_report_validation_tests.py"
                --seal-executable "$<TARGET_FILE:xen_mouse_effect_probe_composite_seal>"
                --case "${report_case}")
        set_tests_properties(composite_report_${report_case} PROPERTIES TIMEOUT 60)
    endforeach()
    add_test(NAME composite_sidecar_binding_tests
        COMMAND "${XEN_PYTHON_EXECUTABLE}" -B -X utf8
            "${PROJECT_SOURCE_DIR}/tests/composite_sidecar_binding_tests.py"
            --seal-executable "$<TARGET_FILE:xen_mouse_effect_probe_composite_seal>"
            --recorder-fixture "$<TARGET_FILE:capture_evidence_producer_fixture>")
    set_tests_properties(composite_sidecar_binding_tests PROPERTIES TIMEOUT 60)
    add_test(NAME mouse_effect_probe_b_composite_phase_producer_tests
        COMMAND "${XEN_PYTHON_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/mouse_effect_probe_b_composite_phase_producer_tests.py"
            --producer
                "${PROJECT_SOURCE_DIR}/scripts/produce_mouse_effect_probe_b_composite_phase_ledgers.py"
            --binder
                "${PROJECT_SOURCE_DIR}/scripts/bind_mouse_effect_probe_b_composite_phase_calibration.py"
            --evaluator
                "${PROJECT_SOURCE_DIR}/scripts/evaluate_mouse_effect_probe_b_composite_phase.py"
            --report-verifier
                "$<TARGET_FILE:xen_mouse_effect_probe_composite_seal>")
    # 固定 powershell.exe 仍会继承调用方的 PSModulePath。统一清除该变量，
    # 让 Windows PowerShell 5 按自身 PSHOME 重建系统模块路径；cmake -E env
    # 只改变测试子进程，不修改调用 CTest 的父环境。
    function(xen_add_windows_powershell_test test_name script_path)
        add_test(NAME ${test_name}
            COMMAND "${CMAKE_COMMAND}" -E env --unset=PSModulePath --
                "${XEN_WINDOWS_POWERSHELL}"
                -NoProfile -ExecutionPolicy Bypass
                -File "${script_path}" ${ARGN})
    endfunction()

    xen_add_windows_powershell_test(path_safety_tests
        "${PROJECT_SOURCE_DIR}/tests/path_safety_tests.ps1"
        -PathSafetyScript "${PROJECT_SOURCE_DIR}/scripts/invoke_path_safety.ps1"
        -RepositoryRoot "${PROJECT_SOURCE_DIR}")

    add_test(NAME team_filter_config_tests
        COMMAND "${XEN_POWERSHELL7_EXECUTABLE}" -NoProfile -ExecutionPolicy Bypass
            -File "${PROJECT_SOURCE_DIR}/tests/team_filter_config_tests.ps1")

    add_test(NAME gsi_realtime_config_tests
        COMMAND "${XEN_POWERSHELL7_EXECUTABLE}" -NoProfile -ExecutionPolicy Bypass
            -File "${PROJECT_SOURCE_DIR}/tests/gsi_realtime_config_tests.ps1")

    add_test(NAME test_root_owner_cli_tests
        COMMAND "${CMAKE_COMMAND}" -E env --unset=PSModulePath --
            "${XEN_POWERSHELL7_EXECUTABLE}"
            -NoProfile -ExecutionPolicy Bypass
            -File "${PROJECT_SOURCE_DIR}/tests/test_root_owner_cli_tests.ps1"
            -PythonExecutable "${XEN_PYTHON_EXECUTABLE}"
            -PowerShellExecutable "${XEN_POWERSHELL7_EXECUTABLE}"
            -ProducerExecutable "$<TARGET_FILE:xen_aim_production_red_producer>"
            -SequenceExecutable "$<TARGET_FILE:xen_mouse_effect_probe_sequence>"
            -GitExecutable "${GIT_EXECUTABLE}"
            -PublisherTestsScript
                "${PROJECT_SOURCE_DIR}/tests/mouse_effect_probe_b_publish_tests.ps1"
            -PublishScript "${PROJECT_SOURCE_DIR}/scripts/publish_mouse_effect_probe_b.ps1"
            -TestRoot "${CMAKE_BINARY_DIR}/test-root-owner-cli-tests")

    xen_add_windows_powershell_test(probe_native_process_tests
        "${PROJECT_SOURCE_DIR}/tests/probe_native_process_tests.ps1"
        -SequenceExecutable "$<TARGET_FILE:xen_mouse_effect_probe_sequence>"
        -TestRoot "${CMAKE_BINARY_DIR}/probe-native-process-tests")
    xen_add_windows_powershell_test(powershell_module_path_tests
        "${PROJECT_SOURCE_DIR}/tests/powershell_module_path_tests.ps1"
        -CMakeCommand "${CMAKE_COMMAND}"
        -CTestCommand "${CMAKE_CTEST_COMMAND}"
        -PowerShell7Executable "${XEN_POWERSHELL7_EXECUTABLE}"
        -BuildDirectory "${CMAKE_BINARY_DIR}"
        -Configuration "$<CONFIG>")
    xen_add_windows_powershell_test(benchmark_xudp_snapshot_tests
        "${PROJECT_SOURCE_DIR}/tests/benchmark_xudp_snapshot_tests.ps1"
        -GitExecutable "${GIT_EXECUTABLE}")
    xen_add_windows_powershell_test(benchmark_output_ownership_tests
        "${PROJECT_SOURCE_DIR}/tests/benchmark_output_ownership_tests.ps1")
    xen_add_windows_powershell_test(transfer_directory_ownership_tests
        "${PROJECT_SOURCE_DIR}/tests/transfer_directory_ownership_tests.ps1")
    xen_add_windows_powershell_test(receiver_run_id_tests
        "${PROJECT_SOURCE_DIR}/tests/receiver_run_id_tests.ps1")
    xen_add_windows_powershell_test(prepare_commit_accounting_tests
        "${PROJECT_SOURCE_DIR}/tests/prepare_commit_accounting_tests.ps1")
    xen_add_windows_powershell_test(prepare_activation_pair_tests
        "${PROJECT_SOURCE_DIR}/tests/prepare_activation_pair_tests.ps1")
    xen_add_windows_powershell_test(benchmark_mouse_script_tests
        "${PROJECT_SOURCE_DIR}/tests/benchmark_mouse_script_tests.ps1"
        -BuildDirectory "${CMAKE_BINARY_DIR}"
        -GitExecutable "${GIT_EXECUTABLE}")
    xen_add_windows_powershell_test(mouse_effect_probe_script_contract_tests
        "${PROJECT_SOURCE_DIR}/tests/mouse_effect_probe_script_contract_tests.ps1"
        -PrepareScript
            "${PROJECT_SOURCE_DIR}/scripts/prepare_mouse_effect_probe_a.ps1"
        -LaunchScript
            "${PROJECT_SOURCE_DIR}/scripts/launch_mouse_effect_probe_a.ps1")
    xen_add_windows_powershell_test(
        mouse_effect_probe_a2_s1_script_contract_tests
        "${PROJECT_SOURCE_DIR}/tests/mouse_effect_probe_a2_s1_script_contract_tests.ps1"
        -CaptureScript
            "${PROJECT_SOURCE_DIR}/scripts/capture_mouse_effect_probe_a2_s1.ps1")
    xen_add_windows_powershell_test(
        mouse_effect_probe_a2_s1_prepare_contract_tests
        "${PROJECT_SOURCE_DIR}/tests/mouse_effect_probe_a2_s1_prepare_contract_tests.ps1"
        -PrepareScript
            "${PROJECT_SOURCE_DIR}/scripts/prepare_mouse_effect_probe_a2_s1.ps1"
        -LaunchScript
            "${PROJECT_SOURCE_DIR}/scripts/launch_mouse_effect_probe_a.ps1")
    xen_add_windows_powershell_test(
        mouse_effect_probe_a2_s1_prepare_integration_tests
        "${PROJECT_SOURCE_DIR}/tests/mouse_effect_probe_a2_s1_prepare_integration_tests.ps1"
        -PrepareScript
            "${PROJECT_SOURCE_DIR}/scripts/prepare_mouse_effect_probe_a2_s1.ps1"
        -ToolRoot "$<TARGET_FILE_DIR:xen_mouse_effect_probe>"
        -TestRoot
            "${CMAKE_BINARY_DIR}/mouse-effect-probe-a2-s1-prepare-integration-tests")
    xen_add_windows_powershell_test(
        mouse_effect_probe_a2_prepare_contract_tests
        "${PROJECT_SOURCE_DIR}/tests/mouse_effect_probe_a2_prepare_contract_tests.ps1"
        -PrepareScript
            "${PROJECT_SOURCE_DIR}/scripts/prepare_mouse_effect_probe_a2.ps1"
        -LaunchScript
            "${PROJECT_SOURCE_DIR}/scripts/launch_mouse_effect_probe_a.ps1")
    xen_add_windows_powershell_test(
        mouse_effect_probe_a2_prepare_integration_tests
        "${PROJECT_SOURCE_DIR}/tests/mouse_effect_probe_a2_prepare_integration_tests.ps1"
        -PrepareScript
            "${PROJECT_SOURCE_DIR}/scripts/prepare_mouse_effect_probe_a2.ps1"
        -ToolRoot "$<TARGET_FILE_DIR:xen_mouse_effect_probe>"
        -TestRoot
            "${CMAKE_BINARY_DIR}/mouse-effect-probe-a2-prepare-integration-tests")
    xen_add_windows_powershell_test(
        mouse_effect_probe_b_prepare_contract_tests
        "${PROJECT_SOURCE_DIR}/tests/mouse_effect_probe_b_prepare_contract_tests.ps1"
        -PrepareScript
            "${PROJECT_SOURCE_DIR}/scripts/prepare_mouse_effect_probe_b.ps1"
        -LaunchScript
            "${PROJECT_SOURCE_DIR}/scripts/launch_mouse_effect_probe_a.ps1")
    xen_add_windows_powershell_test(
        mouse_effect_probe_b_prepare_integration_tests
        "${PROJECT_SOURCE_DIR}/tests/mouse_effect_probe_b_prepare_integration_tests.ps1"
        -PrepareScript
            "${PROJECT_SOURCE_DIR}/scripts/prepare_mouse_effect_probe_b.ps1"
        -ToolRoot "$<TARGET_FILE_DIR:xen_mouse_effect_probe>"
        -PythonExecutable "${XEN_PYTHON_EXECUTABLE}"
        -DesignerScript
            "${PROJECT_SOURCE_DIR}/scripts/design_mouse_effect_probe_prbs.py"
        -AnalyzerScript
            "${PROJECT_SOURCE_DIR}/scripts/analyze_mouse_effect_probe_b.py"
        -TestRoot
            "${CMAKE_BINARY_DIR}/mouse-effect-probe-b-prepare-integration-tests")
    add_test(NAME mouse_effect_probe_b_holdout_prepare_integration_tests
        COMMAND "${XEN_PYTHON_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/mouse_effect_probe_b_holdout_prepare_integration_tests.py"
            --prepare-script
                "${PROJECT_SOURCE_DIR}/scripts/prepare_mouse_effect_probe_b_holdout.ps1"
            --tool-root "$<TARGET_FILE_DIR:xen_mouse_effect_probe>"
            --python "${XEN_PYTHON_EXECUTABLE}"
            --powershell "${XEN_WINDOWS_POWERSHELL}"
            --test-root
                "${CMAKE_BINARY_DIR}/mouse-effect-probe-b-holdout-prepare-integration-tests")
    xen_add_windows_powershell_test(
        mouse_effect_probe_b_holdout_script_contract_tests
        "${PROJECT_SOURCE_DIR}/tests/mouse_effect_probe_b_holdout_script_contract_tests.ps1"
        -PrepareScript
            "${PROJECT_SOURCE_DIR}/scripts/prepare_mouse_effect_probe_b_holdout.ps1"
        -LaunchScript
            "${PROJECT_SOURCE_DIR}/scripts/launch_mouse_effect_probe_a.ps1")
    xen_add_windows_powershell_test(
        mouse_effect_probe_b_command_magnitude_script_contract_tests
        "${PROJECT_SOURCE_DIR}/tests/mouse_effect_probe_b_command_magnitude_script_contract_tests.ps1"
        -PrepareScript
            "${PROJECT_SOURCE_DIR}/scripts/prepare_mouse_effect_probe_b_command_magnitude.ps1"
        -LaunchScript
            "${PROJECT_SOURCE_DIR}/scripts/launch_mouse_effect_probe_a.ps1")
    xen_add_windows_powershell_test(
        mouse_effect_probe_b_command_magnitude_prepare_integration_tests
        "${PROJECT_SOURCE_DIR}/tests/mouse_effect_probe_b_command_magnitude_prepare_integration_tests.ps1"
        -PrepareScript
            "${PROJECT_SOURCE_DIR}/scripts/prepare_mouse_effect_probe_b_command_magnitude.ps1"
        -ToolRoot "$<TARGET_FILE_DIR:xen_mouse_effect_probe>"
        -TestRoot
            "${CMAKE_BINARY_DIR}/mouse-effect-probe-b-command-magnitude-prepare-integration-tests")
    xen_add_windows_powershell_test(
        mouse_effect_probe_b_composite_phase_script_contract_tests
        "${PROJECT_SOURCE_DIR}/tests/mouse_effect_probe_b_composite_phase_script_contract_tests.ps1"
        -PrepareScript
            "${PROJECT_SOURCE_DIR}/scripts/prepare_mouse_effect_probe_b_composite_phase.ps1"
        -LaunchScript
            "${PROJECT_SOURCE_DIR}/scripts/launch_mouse_effect_probe_a.ps1")
    xen_add_windows_powershell_test(
        mouse_effect_probe_b_composite_phase_prepare_integration_tests
        "${PROJECT_SOURCE_DIR}/tests/mouse_effect_probe_b_composite_phase_prepare_integration_tests.ps1"
        -PrepareScript
            "${PROJECT_SOURCE_DIR}/scripts/prepare_mouse_effect_probe_b_composite_phase.ps1"
        -ToolRoot "$<TARGET_FILE_DIR:xen_mouse_effect_probe>"
        -PythonExecutable "${XEN_PYTHON_EXECUTABLE}"
        -TestRoot
            "${CMAKE_BINARY_DIR}/probe-b-composite-prepare")
    xen_add_windows_powershell_test(
        mouse_effect_probe_b_publish_tests
        "${PROJECT_SOURCE_DIR}/tests/mouse_effect_probe_b_publish_tests.ps1"
        -PublishScript
            "${PROJECT_SOURCE_DIR}/scripts/publish_mouse_effect_probe_b.ps1"
        -TestRoot
            "${CMAKE_BINARY_DIR}/mouse-effect-probe-b-publish-tests"
        -GitExecutable "${GIT_EXECUTABLE}")
    xen_add_windows_powershell_test(
        mouse_effect_probe_sequence_cli_tests
        "${PROJECT_SOURCE_DIR}/tests/mouse_effect_probe_sequence_cli_tests.ps1"
        -Executable "$<TARGET_FILE:xen_mouse_effect_probe_sequence>"
        -TestRoot
            "${CMAKE_BINARY_DIR}/mouse-effect-probe-sequence-cli-tests")
    xen_add_windows_powershell_test(obs_capture_binding_tests
        "${PROJECT_SOURCE_DIR}/tests/obs_capture_binding_tests.ps1"
        -BindingScript "${PROJECT_SOURCE_DIR}/scripts/export_obs_capture_binding.ps1"
        -TestRoot "${CMAKE_BINARY_DIR}/obs-capture-binding-tests")
    xen_add_windows_powershell_test(capture_evidence_cli_tests
        "${PROJECT_SOURCE_DIR}/tests/capture_evidence_cli_tests.ps1"
        -Executable "$<TARGET_FILE:xen_capture_evidence>"
        -TestRoot "${CMAKE_BINARY_DIR}/capture-evidence-cli-tests")
    xen_add_windows_powershell_test(sender_cli_tests
        "${PROJECT_SOURCE_DIR}/tests/sender_cli_tests.ps1"
        -Executable "$<TARGET_FILE:xen_sender>"
        -TestRoot "${CMAKE_BINARY_DIR}/sender-cli-tests")
    xen_add_windows_powershell_test(release_bundle_tests
        "${PROJECT_SOURCE_DIR}/tests/release_bundle_tests.ps1"
        -PublishScript "${PROJECT_SOURCE_DIR}/scripts/publish_release_bundle.ps1"
        -GitExecutable "${GIT_EXECUTABLE}"
        -TestRoot "${CMAKE_BINARY_DIR}/release-bundle-tests"
        -ActualReleaseDirectory "${XEN_MODEL_TOOLS_OUTPUT}"
        -ActualRepositoryRoot "${PROJECT_SOURCE_DIR}")
    xen_add_windows_powershell_test(publish_worker_update_tests
        "${PROJECT_SOURCE_DIR}/scripts/test_publish_worker_update.ps1"
        -GitExecutable "${GIT_EXECUTABLE}"
        -ManifestValidator "$<TARGET_FILE:release_contract_tests>"
        -TestRoot "${CMAKE_BINARY_DIR}/worker-update-tests")
    xen_add_windows_powershell_test(process_resource_sampling_tests
        "${PROJECT_SOURCE_DIR}/scripts/test_measure_process_resources.ps1"
        -EvidenceParent "${CMAKE_BINARY_DIR}/process-resource-sampling-tests")
    xen_add_windows_powershell_test(soak_acceptance_tests
        "${PROJECT_SOURCE_DIR}/scripts/test_soak_acceptance.ps1"
        -EvidenceParent "${CMAKE_BINARY_DIR}/soak-acceptance-tests")
    xen_add_windows_powershell_test(recoil_legacy_acceptance_tests
        "${PROJECT_SOURCE_DIR}/scripts/test_recoil_legacy_acceptance.ps1"
        -TestRoot "${CMAKE_BINARY_DIR}/recoil-legacy-acceptance-tests")
    xen_add_windows_powershell_test(aim_analysis_import_scope_tests
        "${PROJECT_SOURCE_DIR}/tests/aim_analysis_import_scope_tests.ps1"
        -ScriptsRoot "${PROJECT_SOURCE_DIR}/scripts")
    xen_add_windows_powershell_test(aim_marker_snapshot_tests
        "${PROJECT_SOURCE_DIR}/scripts/test_aim_marker_snapshot.ps1"
        -SourcePath "${PROJECT_SOURCE_DIR}/scripts/invoke_aim_manual_acceptance.ps1"
        -TestRoot "${CMAKE_BINARY_DIR}/aim-marker-snapshot-tests")
    xen_add_windows_powershell_test(powershell_host_contract_tests
        "${PROJECT_SOURCE_DIR}/tests/powershell_host_contract_tests.ps1"
        -PowerShell7Executable "${XEN_POWERSHELL7_EXECUTABLE}"
        -WorkflowScript
            "${PROJECT_SOURCE_DIR}/scripts/test_release_transfer_and_aim_manual.ps1")

    xen_add_windows_powershell_test(auxiliary_powershell_tests
        "${PROJECT_SOURCE_DIR}/scripts/test_auxiliary_powershell.ps1")

    add_executable(debug_tests
        tests/debug_tests.cpp)
    target_link_libraries(debug_tests PRIVATE xen_debug log)
    add_test(NAME debug_tests COMMAND debug_tests)
    add_executable(aim_diagnostic_epoch_fixture tests/aim_diagnostic_epoch_fixture.cpp)
    target_link_libraries(aim_diagnostic_epoch_fixture PRIVATE xen_debug log)
    xen_add_windows_powershell_test(aim_diagnostic_epoch_tests
        "${PROJECT_SOURCE_DIR}/tests/aim_diagnostic_epoch_tests.ps1"
        -FixtureExecutable "$<TARGET_FILE:aim_diagnostic_epoch_fixture>")
    add_executable(benchmark_report_fixture tests/benchmark_report_fixture.cpp)
    target_link_libraries(benchmark_report_fixture PRIVATE xen_debug log)
    xen_add_windows_powershell_test(benchmark_evidence_snapshot_tests
        "${PROJECT_SOURCE_DIR}/tests/benchmark_evidence_snapshot_tests.ps1"
        -FixtureExecutable "$<TARGET_FILE:benchmark_report_fixture>")
    xen_add_windows_powershell_test(network_evidence_binding_tests
        "${PROJECT_SOURCE_DIR}/tests/benchmark_evidence_snapshot_tests.ps1"
        -FixtureExecutable "$<TARGET_FILE:benchmark_report_fixture>" -NetworkOnly)
    add_executable(session_archive_tests tests/session_archive_tests.cpp)
    target_link_libraries(session_archive_tests PRIVATE xen_debug log)
    add_test(NAME session_archive_tests COMMAND session_archive_tests)
    add_test(NAME session_archive_pending_failure_tests COMMAND session_archive_tests --pending-failure-only)

    add_executable(benchmark_tests
        tests/benchmark_tests.cpp)
    target_link_libraries(benchmark_tests PRIVATE benchmark)
    if(WIN32)
        xen_deploy_runtime_dlls(benchmark_tests OPENCV ORT NDI)
    endif()
    add_test(NAME benchmark_tests COMMAND benchmark_tests)
    add_executable(report_close_tests tests/report_close_tests.cpp)
    target_link_libraries(report_close_tests PRIVATE benchmark sender mouse_effect_probe)
    xen_deploy_runtime_dlls(report_close_tests OPENCV ORT NDI)
    add_test(NAME report_close_tests COMMAND report_close_tests)

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
    add_test(NAME xudp_stream_validation_tests COMMAND io_tests --xudp-only)

    if(XEN_HAS_NDI)
        # 注入 SDK/外部时钟，仍经真实接收线程、发布与公开 grab；不初始化 NDI 实网。
        add_executable(ndi_capture_contract_tests
            tests/ndi_capture_contract_tests.cpp)
        target_link_libraries(ndi_capture_contract_tests PRIVATE capture)
        xen_deploy_runtime_dlls(ndi_capture_contract_tests OPENCV NDI)
        add_test(NAME ndi_capture_contract_tests COMMAND ndi_capture_contract_tests)

        # 同一 NDI 系统夹具继续进入 Runtime 生产队列/时钟和 Aim；不启动 Detector/Mouse。
        add_executable(ndi_runtime_timing_tests
            tests/ndi_runtime_timing_tests.cpp
            Xen/runtime/runtime_queue.cpp)
        target_link_libraries(ndi_runtime_timing_tests PRIVATE capture aim)
        xen_deploy_runtime_dlls(ndi_runtime_timing_tests OPENCV NDI)
        add_test(NAME ndi_runtime_timing_tests COMMAND ndi_runtime_timing_tests)
    endif()

    add_executable(clock_sync_tests
        tests/clock_sync_tests.cpp)
    target_link_libraries(clock_sync_tests PRIVATE clock_sync ws2_32)
    add_test(NAME clock_sync_tests COMMAND clock_sync_tests)

    # 参数化离线量化 clock mapping 误差对 Aim 公有输出的非时序
    # synthetic sequence stress。clock 与 Aim cadence 仅显式报告，不伪装对齐、
    # 真实因果或生产阈值；不启动 NDI、Runtime 或任何物理输入后端。
    add_executable(clock_quality_measurement
        tests/clock_quality_measurement.cpp)
    target_link_libraries(clock_quality_measurement
        PRIVATE clock_sync aim log)
    xen_deploy_runtime_dlls(clock_quality_measurement OPENCV)
    # 数值 oracle 只验证测量器的逐样本消费、单位、符号和 Aim 差异，
    # fixture 数值不构成生产门槛。
    xen_add_windows_powershell_test(clock_quality_measurement_numeric_oracle
        "${PROJECT_SOURCE_DIR}/tests/clock_quality_measurement_tests.ps1"
        -Executable "$<TARGET_FILE:clock_quality_measurement>"
        -TestRoot "${CMAKE_BINARY_DIR}/clock-quality-measurement-tests")

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
        target_link_libraries(detector_model_test
            PRIVATE detector aim aim_landmark config xen_debug bcrypt)
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

        # 该用例固定请求 TensorRT CUDA Graph，并不验证纯 CUDA EP。只有本轮
        # 明确配置了匹配的 TensorRT SDK 时才注册，避免 CUDA-only 矩阵被
        # 未部署的 nvinfer 运行库污染。
        if(XEN_ONNXRUNTIME_HAS_CUDA AND
           XEN_ONNXRUNTIME_HAS_TENSORRT AND
           XEN_TENSORRT_ROOT)
            add_test(NAME detector_tensorrt_model_test
                COMMAND detector_model_test "${XEN_TEST_MODEL}" tensorrt
                    "${CMAKE_BINARY_DIR}/cache/tensorrt/detector-model-test"
                    --gpu-preprocess on)
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
        target_link_libraries(runtime_model_reload_test PRIVATE runtime xen_debug ws2_32)
        if(WIN32)
            xen_deploy_runtime_dlls(
                runtime_model_reload_test OPENCV ORT NDI)
        endif()
        add_test(NAME runtime_model_reload_test
            COMMAND runtime_model_reload_test "${XEN_TEST_MODEL}")

        add_executable(collection_lifecycle_tests tests/collection_lifecycle_tests.cpp)
        target_link_libraries(collection_lifecycle_tests PRIVATE app_input model_workspace xen_debug)
        xen_deploy_runtime_dlls(collection_lifecycle_tests OPENCV ORT NDI)
        foreach(case ui-response stop-interleave writer-failure concurrent-stop
                     admission-stop admission-commit admission-restart admission-reload admission-stopping frame-identity)
            add_test(NAME collection_lifecycle_${case}
                COMMAND collection_lifecycle_tests "${XEN_TEST_MODEL}" "--${case}")
            set_tests_properties(collection_lifecycle_${case} PROPERTIES TIMEOUT 45)
        endforeach()
        add_executable(runtime_start_failure_tests tests/runtime_start_failure_tests.cpp)
        target_link_libraries(runtime_start_failure_tests PRIVATE runtime)
        xen_deploy_runtime_dlls(runtime_start_failure_tests OPENCV ORT NDI)
        add_test(NAME runtime_start_failure_tests
            COMMAND runtime_start_failure_tests "${XEN_TEST_MODEL}")
        set_tests_properties(runtime_start_failure_tests PROPERTIES TIMEOUT 60)
    endif()
endif()

if(BUILD_TESTING)
    add_test(NAME weapon_fire_test_cli_tests COMMAND "${XEN_PYTHON_EXECUTABLE}" -B -X utf8
        "${PROJECT_SOURCE_DIR}/tests/weapon_fire_test_cli_tests.py" --executable "$<TARGET_FILE:auto_stop_counterpulse>"
        --powershell-executable "${XEN_WINDOWS_POWERSHELL}")
    # 同一专项一次构建依赖图，避免逐目标重复运行运行库部署前置步骤。
    add_custom_target(debug_workspace_validation DEPENDS xen_app auto_stop_counterpulse
        debug_workspace_ui_preview debug_runner_tests debug_session_tests debug_panel_tests recoil_editor_job_tests
        overlay_tests input_training_panel_tests runtime_input_training_tests input_training_tests input_safety_tests io_tests
        auto_stop_counterpulse_tests auto_stop_sampling_analysis_tests auto_stop_counterpulse_hud_tests
        auto_stop_manual_sampling_tests auto_stop_manual_monitor_tests auto_stop_manual_finalize_tests
        auto_stop_manual_labels_tests auto_stop_hud_feedback_tests auto_stop_manual_intervals_tests
        auto_stop_default_baseline_tests auto_stop_manual_plan_tests auto_stop_training_evaluation_tests
        auto_stop_readiness_tests auto_stop_preroll_tests config_tests trigger_tests trigger_worker_tests
        auto_stop_tests auto_stop_worker_tests weapon_timing_tests weapon_tests weapon_stop_integration_tests
        recoil_worker_tests recoil_report_tests)
endif()

if(WIN32)
    xen_finalize_runtime_deployment()
    xen_record_artifact_identity(xen_mouse_benchmark)
    xen_record_artifact_identity(xen_mouse_effect_probe_sequence)
    xen_record_artifact_identity(xen_mouse_effect_probe_composite_seal)
endif()
