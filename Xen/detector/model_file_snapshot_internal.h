#ifndef XEN_DETECTOR_MODEL_FILE_SNAPSHOT_INTERNAL_H
#define XEN_DETECTOR_MODEL_FILE_SNAPSHOT_INTERNAL_H
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <bcrypt.h>
#ifdef ERROR
#undef ERROR
#endif
#include "platform/filesystem_path_internal.h"
#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <vector>

namespace detector::detail {
// 加载期独占字节副本：同一组字节供SHA和ORT解析，不在推理热路径保留文件或重算摘要。
// SHA仅描述ONNX容器；外置initializer文件仍是独立资源。
class ModelFileSnapshot final {
public:
    explicit ModelFileSnapshot(const std::filesystem::path& path) {
        try {
            file_ = CreateFileW(xen::filesystem_detail::windows_io_path(path).c_str(),
                GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file_ == INVALID_HANDLE_VALUE) throw std::runtime_error("无法冻结模型文件进行加载");
            LARGE_INTEGER size{};
            if (!GetFileSizeEx(file_, &size) || size.QuadPart <= 0 ||
                static_cast<unsigned long long>(size.QuadPart) > std::numeric_limits<std::size_t>::max())
                throw std::runtime_error("模型文件尺寸非法");
            bytes_.resize(static_cast<std::size_t>(size.QuadPart));
            for (std::size_t offset = 0; offset < bytes_.size();) {
                DWORD read = 0;
                const auto count = static_cast<DWORD>(std::min<std::size_t>(bytes_.size() - offset, 65536));
                if (!ReadFile(file_, bytes_.data() + offset, count, &read, nullptr) || read == 0)
                    throw std::runtime_error("读取模型文件失败");
                offset += read;
            }
            check(BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0));
            check(BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0));
            auto* cursor = bytes_.data();
            for (std::size_t remaining = bytes_.size(); remaining;) {
                const auto count = static_cast<ULONG>(std::min<std::size_t>(remaining, 65536));
                check(BCryptHashData(hash_, cursor, count, 0));
                cursor += count; remaining -= count;
            }
            std::array<unsigned char, 32> digest{};
            check(BCryptFinishHash(hash_, digest.data(), static_cast<ULONG>(digest.size()), 0));
            for (const auto byte : digest) {
                sha256_ += "0123456789abcdef"[byte >> 4];
                sha256_ += "0123456789abcdef"[byte & 15];
            }
        } catch (...) { release(); throw; }
    }
    ~ModelFileSnapshot() { release(); }
    ModelFileSnapshot(const ModelFileSnapshot&) = delete;
    ModelFileSnapshot& operator=(const ModelFileSnapshot&) = delete;
    const void* data() const noexcept { return bytes_.data(); }
    std::size_t size() const noexcept { return bytes_.size(); }
    const std::string& sha256() const noexcept { return sha256_; }
private:
    static void check(NTSTATUS status) {
        if (status < 0) throw std::runtime_error("模型SHA-256计算失败");
    }
    void release() noexcept {
        if (hash_) BCryptDestroyHash(hash_);
        if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0);
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    }
    HANDLE file_ = INVALID_HANDLE_VALUE;
    std::vector<unsigned char> bytes_;
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    std::string sha256_;
};
} // namespace detector::detail
#endif
