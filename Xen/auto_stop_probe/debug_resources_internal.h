#ifndef AUTO_STOP_DEBUG_RESOURCES_INTERNAL_H
#define AUTO_STOP_DEBUG_RESOURCES_INTERNAL_H
#include "auto_stop_probe/debug_runner.h"
#include "auto_stop_probe/training_evaluation_internal.h"
#include "runtime/input_training_internal.h"

namespace auto_stop_probe_detail {
// 调用方已取得唯一调试职责。每组重新订阅建立新epoch，不读取组间旧历史。
class DebugWasdSubscription {
public:
    explicit DebugWasdSubscription(IMouseController& mouse) : mouse_(mouse) {}
    ~DebugWasdSubscription() { (void)finish(); }
    DebugWasdSubscription(const DebugWasdSubscription&) = delete;
    DebugWasdSubscription& operator=(const DebugWasdSubscription&) = delete;
    bool start() noexcept {
        if (started_) return false;
        started_ = true;
        return mouse_.set_wasd_event_subscription(false) && mouse_.set_wasd_event_subscription(true);
    }
    bool finish() noexcept {
        if (!started_ || finished_) return true;
        finished_ = true;
        return mouse_.set_wasd_event_subscription(false);
    }
private:
    IMouseController& mouse_;
    bool started_ = false, finished_ = false;
};

class MonitorTraining {
public:
    MonitorTraining(std::shared_ptr<IMouseController> mouse, const std::filesystem::path& directory,
        input_training::Limits limits = {}) : mouse_(std::move(mouse)) {
        if (!mouse_->set_input_report_subscription(true)) throw std::runtime_error("输入报告订阅失败");
        try {
            auto source = std::make_shared<runtime::detail::InputTrainingSource>(mouse_);
            if (!session_.start(directory, limits, [source] { return source->read(); }))
                throw std::runtime_error("输入报告归档启动失败");
        } catch (...) { mouse_->freeze_input_reports(); throw; }
    }
    ~MonitorTraining() { (void)stop(); }
    input_training::Status stop() noexcept {
        if (!stopped_) {
            stopped_ = true;
            mouse_->freeze_input_reports();
            session_.stop();
        }
        const auto state = session_.snapshot();
        return state ? state->status : input_training::Status::STOP_TIMEOUT;
    }
    bool recording() const noexcept {
        const auto state = session_.snapshot();
        return state && state->status == input_training::Status::RECORDING;
    }
    Json report() const { return training_snapshot_json(*session_.snapshot(), "KMBOX_MONITOR"); }
private:
    std::shared_ptr<IMouseController> mouse_;
    input_training::Session session_;
    bool stopped_ = false;
};

// wait_for超时不证明Reader已退出；禁止把尚存后台的冻结水位交给下一组重置。
inline void finish_monitor_training(MonitorTraining& training) {
    if (training.stop() == input_training::Status::STOP_TIMEOUT)
        throw DebugRunFailure("输入报告归档停止超时：MONITOR_TRAINING_STOP_TIMEOUT", false);
}
}
#endif
