// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef XEN_RECORDER_DIAGNOSTICS_H
#define XEN_RECORDER_DIAGNOSTICS_H
#include <filesystem>
#include <memory>
namespace xen::recorder {
class Diagnostics final {
public:
    explicit Diagnostics(const std::filesystem::path& directory);
    ~Diagnostics();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
#endif
