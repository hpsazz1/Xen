#ifndef RUNTIME_SOURCE_FOCUS_INTERNAL_H
#define RUNTIME_SOURCE_FOCUS_INTERNAL_H

#include "source_context/source_context.h"
#include <functional>
#include <utility>

namespace runtime::detail {
// Runtime与双worker回归共用真实来源适配入口。
inline std::function<source_context::SourceContextSnapshot()> auto_stop_source_reader(
        std::function<source_context::SourceContextSnapshot()> read) {
    // 可被AutoStop和Trigger同时调用；断点状态由worker的既有mutex统一管理。
    return read;
}
} // namespace runtime::detail
#endif
