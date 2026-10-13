#ifndef MODEL_WORKSPACE_ACTIONS_INTERNAL_H
#define MODEL_WORKSPACE_ACTIONS_INTERNAL_H

#include "model_workspace/model_workspace.h"
#include <functional>

namespace model_workspace::detail {
// 每实例测试适配：保留生产校验，只替换外部打开动作或设置采集提交屏障。
struct ActionAdapter {
    std::function<void()> before_collection_start;
    std::function<bool(const std::filesystem::path&)> open_review;
};
}
#endif
