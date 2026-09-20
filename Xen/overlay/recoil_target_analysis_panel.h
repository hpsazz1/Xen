#ifndef RECOIL_TARGET_ANALYSIS_PANEL_H
#define RECOIL_TARGET_ANALYSIS_PANEL_H
#include <memory>

// 独立离线分析界面；后台仅调用标准库Python工具，不持有设备。
class RecoilTargetAnalysisPanel {
public:
    RecoilTargetAnalysisPanel();
    ~RecoilTargetAnalysisPanel();
    void render(bool can_edit);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
#endif
