#ifndef AUXILIARY_REPORT_H
#define AUXILIARY_REPORT_H
#include "runtime/runtime.h"
#include <string>

// 独立执行证据；不依赖普通Log等级或访问认证环境。
std::string trigger_execution_json(const RuntimeSnapshot& snapshot);
std::string output_arbitration_json(const RuntimeSnapshot& snapshot);
#endif
