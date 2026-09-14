#ifndef OVERLAY_INPUT_TRAINING_PANEL_H
#define OVERLAY_INPUT_TRAINING_PANEL_H

#include <memory>

namespace input_training { struct Snapshot; }
struct OverlayActions;

// 只持有浏览位置；采集、目录创建与归档读取均交给 App/Runtime。
class InputTrainingPanel {
public:
    InputTrainingPanel();
    ~InputTrainingPanel();
    InputTrainingPanel(const InputTrainingPanel&) = delete;
    InputTrainingPanel& operator=(const InputTrainingPanel&) = delete;
    void render(const std::shared_ptr<const input_training::Snapshot>& snapshot,
                OverlayActions& actions) noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif
