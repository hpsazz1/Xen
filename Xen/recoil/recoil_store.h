#ifndef RECOIL_STORE_H
#define RECOIL_STORE_H
#include "recoil/recoil.h"
#include "recoil/recoil_config.h"
#include <filesystem>
struct RecoilStoredProfile {
    std::string file;
    std::shared_ptr<const RecoilProfile> profile;
};
// 冷路径文件库：只认明确活动引用；目录中的其他候选不会自动成为活动弹道。
class RecoilStore {
public:
    explicit RecoilStore(std::filesystem::path directory);
    bool list(std::vector<RecoilStoredProfile>& profiles,std::string& error) const noexcept;
    bool load(const std::string& file,RecoilProfile& profile,std::string& error) const noexcept;
    bool save_new(const RecoilProfile& profile,std::string& file,std::string& error,
        bool preserve_calibration=false) const noexcept;
    bool set_active(const std::string& weapon,const std::string& file,std::string& error) const noexcept;
    bool rollback(const std::string& weapon,std::string& error) const noexcept;
    std::shared_ptr<const RecoilProfile> resolve(const RecoilConfig& config,
        const std::string& weapon_id,std::string& error) const noexcept;
private:
    std::filesystem::path directory_;
};
#endif
