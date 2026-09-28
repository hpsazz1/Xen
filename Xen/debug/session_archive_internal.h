#ifndef SESSION_ARCHIVE_INTERNAL_H
#define SESSION_ARCHIVE_INTERNAL_H
#include "debug/session_archive.h"
#include <functional>
namespace xen::debug::detail {
// 专项测试通过生产提交接口控制写入暂停；回调永远不持队列锁。
class SessionArchiveTestAccess {
public:
    static void before_flush(SessionArchive&, std::function<void()> callback);
};
}
#endif
