cmake_minimum_required(VERSION 3.18)
project(xen_app_startup_contract LANGUAGES CXX)

# 仅编译同一生产边界与既有 fake 测试；不配置根工程或解析任何 Provider/SDK。
# 这是专项反馈工程，不替代完整 Xen 构建，也不改变根工程的严格 Provider 选择。
get_filename_component(XEN_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
add_executable(app_startup_tests
    "${XEN_SOURCE_DIR}/tests/app_startup_tests.cpp"
    "${XEN_SOURCE_DIR}/Xen/app/startup.cpp")
target_include_directories(app_startup_tests PRIVATE "${XEN_SOURCE_DIR}/Xen")
target_compile_features(app_startup_tests PRIVATE cxx_std_20)
set_target_properties(app_startup_tests PROPERTIES CXX_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(app_startup_tests PRIVATE /utf-8)
endif()

enable_testing()
add_test(NAME app_startup_tests COMMAND app_startup_tests)
set_tests_properties(app_startup_tests PROPERTIES TIMEOUT 10)
