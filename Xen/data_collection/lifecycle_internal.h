#ifndef DATA_COLLECTION_LIFECYCLE_INTERNAL_H
#define DATA_COLLECTION_LIFECYCLE_INTERNAL_H

#include "data_collection/data_collection.h"
#include <functional>

namespace data_collection::detail {
// 每实例屏障只观察真实写入和回收边界；默认生产路径不安装回调。
struct LifecycleAdapter {
    std::function<void()> before_write;
    std::function<void()> stop_requested;
    std::function<void()> before_join;
};
}
#endif
