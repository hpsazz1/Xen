#pragma once

#include <string>

namespace BuildIdentity
{
    const char* backend();
    const char* revision();
    const char* timestampUtc();
    int controllerRevision();

    // UI 使用短提交号并保留 dirty 标记，例如 "DML b4ad5b0* r4"；
    // CSV 仍输出完整 12 位提交，兼顾可读性和可审计性。
    std::string displayLabel();
}
