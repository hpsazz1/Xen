cmake_minimum_required(VERSION 3.18)

if(NOT DEFINED XEN_RUNTIME_SOURCE OR
   NOT DEFINED XEN_RUNTIME_OUTPUT OR
   NOT DEFINED XEN_RUNTIME_LOCK)
    message(FATAL_ERROR
        "Runtime deployment requires source, output and lock paths.")
endif()
if(NOT EXISTS "${XEN_RUNTIME_SOURCE}")
    message(FATAL_ERROR
        "Runtime deployment source does not exist: ${XEN_RUNTIME_SOURCE}")
endif()

file(MAKE_DIRECTORY "${XEN_RUNTIME_OUTPUT}")
# GUARD PROCESS 确保脚本无论成功还是失败退出都会释放锁。锁只串行化同一
# 构建树的部署命令，不改变各目标的编译和链接并行度。
file(LOCK "${XEN_RUNTIME_LOCK}" GUARD PROCESS TIMEOUT 300
    RESULT_VARIABLE XEN_RUNTIME_LOCK_RESULT)
if(NOT XEN_RUNTIME_LOCK_RESULT STREQUAL "0")
    message(FATAL_ERROR
        "Runtime deployment lock failed: ${XEN_RUNTIME_LOCK_RESULT}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${XEN_RUNTIME_SOURCE}" "${XEN_RUNTIME_OUTPUT}"
    RESULT_VARIABLE XEN_RUNTIME_COPY_RESULT
    ERROR_VARIABLE XEN_RUNTIME_COPY_ERROR)
if(NOT XEN_RUNTIME_COPY_RESULT EQUAL 0)
    message(FATAL_ERROR
        "Runtime deployment copy failed (${XEN_RUNTIME_COPY_RESULT}): "
        "${XEN_RUNTIME_SOURCE} -> ${XEN_RUNTIME_OUTPUT}\n"
        "${XEN_RUNTIME_COPY_ERROR}")
endif()
