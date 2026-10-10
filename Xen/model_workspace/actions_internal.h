#ifndef MODEL_WORKSPACE_ACTIONS_INTERNAL_H
#define MODEL_WORKSPACE_ACTIONS_INTERNAL_H

#include "model_workspace/model_workspace.h"
#include <functional>

namespace model_workspace::detail {
// 每实例测试屏障位于真实模型校验之后、采集提交之前；生产默认不安装。
struct ActionAdapter {
    std::function<void()> before_collection_start;
};
}
#endif
