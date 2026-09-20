#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "overlay/recoil_target_panel.h"
#include "overlay/overlay.h"
#include <d3d11.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
void help(const char* text) {
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) return;
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}
std::string path_text(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
struct PreviewLoad {
    std::atomic<bool> done{false};
    std::string path, error;
    cv::Mat bgra;
};
// 单槽后台文件解码；任务只持有自己的shared_ptr，关闭面板不访问悬空UI或等待磁盘。
std::shared_ptr<PreviewLoad> load_preview(const std::string& path) {
    auto job = std::make_shared<PreviewLoad>();
    job->path = path;
    std::thread([job] {
        try {
            std::ifstream input(std::filesystem::u8path(job->path), std::ios::binary | std::ios::ate);
            const auto size = input.tellg();
            if (!input || size <= 0 || size > 128 * 1024 * 1024)
                throw std::runtime_error("预览图不存在或超过128MiB。");
            std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
            input.seekg(0);
            if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
                throw std::runtime_error("读取预览图失败。");
            // 本流程只读取自己归档的PNG；先核对IHDR尺寸，避免解码后才发现异常分配。
            const std::array<unsigned char, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
            if (bytes.size() < 24 || !std::equal(signature.begin(), signature.end(), bytes.begin()) ||
                bytes[12] != 'I' || bytes[13] != 'H' || bytes[14] != 'D' || bytes[15] != 'R')
                throw std::runtime_error("预览必须是本流程归档的PNG原图。");
            const auto dimension = [&](int offset) {
                return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
                    (static_cast<std::uint32_t>(bytes[offset+1]) << 16) |
                    (static_cast<std::uint32_t>(bytes[offset+2]) << 8) | bytes[offset+3];
            };
            const auto width = dimension(16), height = dimension(20);
            if (!width || !height || width > 16384 || height > 16384 ||
                static_cast<std::uint64_t>(width) * height > 33554432)
                throw std::runtime_error("预览图原始尺寸超过有界图像预算。");
            const auto image = cv::imdecode(bytes, cv::IMREAD_COLOR);
            if (image.empty() || image.total() > 33554432)
                throw std::runtime_error("预览图无效或像素数量过大。");
            cv::cvtColor(image, job->bgra, cv::COLOR_BGR2BGRA);
        } catch (const std::exception& error) { job->error = error.what(); }
        catch (...) { job->error = "预览图解码失败。"; }
        job->done.store(true, std::memory_order_release);
    }).detach();
    return job;
}
bool roi_valid(const std::array<int, 4>& value, int width, int height) {
    return value[0] >= 1 && value[1] >= 1 && value[2] >= 32 && value[3] >= 32 &&
        static_cast<std::int64_t>(value[0]) + value[2] < width &&
        static_cast<std::int64_t>(value[1]) + value[3] < height;
}
}

struct RecoilTargetPanel::Impl {
    std::string weapon = "ak47", profile_path, calibration_path, control_path;
    std::string session_id = "target-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    std::string status, report_directory, desired_preview, loaded_preview;
    std::array<int, 4> target{}, background{};
    int duration_ms = 1000, shots = 5, split = 0, selection = 0;
    bool ammo_confirmed = false, stable_confirmed = false, initialized = false, dragging = false;
    ImVec2 drag_origin{};
    std::uint64_t revision = 0, submitted_revision = 0, result_generation = 0;
    bool awaiting_result = false, submitted_fire = false;
    std::string submitted_mode;
    std::shared_ptr<const debug_session::Json> last_result;
    std::shared_ptr<PreviewLoad> preview_job;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> preview_view;
    int image_width = 0, image_height = 0;

    void edited(OverlayActions& actions, bool geometry = false) {
        ++revision;
        actions.debug_plan_edited = true;
        stable_confirmed = false;
        control_path.clear();
        if (geometry) calibration_path.clear();
    }
    bool regions_ready() const {
        if (!roi_valid(target, image_width, image_height) || !roi_valid(background, image_width, image_height)) return false;
        const cv::Rect a(target[0]-1, target[1]-1, target[2]+2, target[3]+2);
        const cv::Rect b(background[0]-1, background[1]-1, background[2]+2, background[3]+2);
        return (a & b).area() == 0;
    }
    void prepare(const char* mode, bool fire, OverlayActions& actions) {
        actions.debug_plan_edited = true;
        actions.debug_action = debug_session::Action::PREPARE;
        auto& request = actions.debug_request;
        request = {};
        request.mode = debug_session::Mode::RECOIL_TARGET;
        request.weapon_id = weapon;
        request.output_root = "cache/recoil/target";
        request.show_hud = false;
        const char* splits[] = {"fit", "holdout", "test"};
        request.recoil_target_options = {{"mode", mode}, {"fire", fire}, {"weapon_id", weapon},
            {"duration_ms", duration_ms}, {"target_shots", shots}, {"roi", target},
            {"background_roi", background}, {"calibration_path", calibration_path},
            {"profile_path", profile_path}, {"ammo_limit_confirmed", ammo_confirmed},
            {"split", splits[split]}, {"session_id", session_id},
            {"control_reference_path", control_path}, {"control_observed_stable_confirmed", stable_confirmed}};
        submitted_mode = mode;
        submitted_fire = fire;
        submitted_revision = revision;
        awaiting_result = true;
        status = "已提交准备；本轮仍需在调试会话中由你明确启动。结束后不会自动续跑。";
    }
    void consume(const debug_session::Snapshot* snapshot) {
        if (!snapshot || !snapshot->result || snapshot->busy ||
            (snapshot->generation == result_generation && snapshot->result == last_result)) return;
        result_generation = snapshot->generation;
        last_result = snapshot->result;
        if (!awaiting_result || submitted_revision != revision) return;
        const auto& result = *snapshot->result;
        if (!result.is_object() || result.value("weapon_id", std::string{}) != weapon ||
            result.value("mode", std::string{}) != "recoil_target" ||
            result.value("target_mode", std::string{}) != submitted_mode ||
            result.value("fire", false) != submitted_fire) return;
        awaiting_result = false;
        report_directory = result.value("capture_path", snapshot->report_directory);
        const auto preview = result.value("preview_path", std::string{});
        if (!preview.empty()) desired_preview = preview;
        status = snapshot->message;
        const bool success = snapshot->state == debug_session::State::COMPLETED &&
            result.value("success", false) && result.value("cleanup_known", false);
        if (!success) { stable_confirmed = false; return; }
        if (submitted_mode == "calibrate") {
            calibration_path = result.value("calibration_path", std::string{});
            control_path.clear();
            stable_confirmed = false;
        }
        if (submitted_mode == "control" && !submitted_fire && !report_directory.empty()) {
            control_path = path_text(std::filesystem::u8path(report_directory) / "result.json");
            stable_confirmed = false;
            status = "无射击对照完成。请核对实际画面是否稳定，再勾选人工确认。";
        }
    }
    void poll_preview(ID3D11Device* device) {
        if (preview_job && preview_job->done.load(std::memory_order_acquire)) {
            auto job = std::move(preview_job);
            if (job->path == desired_preview) {
                loaded_preview = job->path;
                preview_view.Reset();
                image_width = image_height = 0;
                if (!job->error.empty()) status = job->error;
                else if (device) {
                    D3D11_TEXTURE2D_DESC description{};
                    description.Width = job->bgra.cols;
                    description.Height = job->bgra.rows;
                    description.MipLevels = description.ArraySize = 1;
                    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                    description.SampleDesc.Count = 1;
                    description.Usage = D3D11_USAGE_IMMUTABLE;
                    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    D3D11_SUBRESOURCE_DATA initial{};
                    initial.pSysMem = job->bgra.data;
                    initial.SysMemPitch = static_cast<UINT>(job->bgra.step);
                    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
                    if (SUCCEEDED(device->CreateTexture2D(&description, &initial, &texture)) &&
                        SUCCEEDED(device->CreateShaderResourceView(texture.Get(), nullptr, &preview_view))) {
                        image_width = job->bgra.cols;
                        image_height = job->bgra.rows;
                    } else status = "预览纹理创建失败。";
                } else status = "预览设备不可用。";
            }
        }
        if (!preview_job && !desired_preview.empty() && desired_preview != loaded_preview)
            preview_job = load_preview(desired_preview);
    }
    void image(OverlayActions& actions, bool can_edit) {
        if (!preview_view) {
            ImGui::TextDisabled(preview_job ? "正在后台载入预览……" : "先准备短录像并启动，完成后在原图选择锚点。" );
            return;
        }
        ImGui::RadioButton("选择目标纹理", &selection, 0); help("在图中拖选固定靶面纹理；原图至少32×32像素，四周留出支撑。");
        ImGui::SameLine();
        ImGui::RadioButton("选择独立背景", &selection, 1); help("选择另一块静态背景，与目标区域及其边缘不重叠。背景仅核对目标独立运动。");
        const float scale = std::min({1.0f, std::max(64.0f, ImGui::GetContentRegionAvail().x) / image_width,
            360.0f / image_height});
        const ImVec2 size(image_width * scale, image_height * scale);
        ImGui::Image(ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(preview_view.Get()))), size);
        const auto origin = ImGui::GetItemRectMin();
        const auto mouse = ImGui::GetIO().MousePos;
        const ImVec2 pixel(std::clamp((mouse.x-origin.x)/scale, 0.0f, static_cast<float>(image_width)),
            std::clamp((mouse.y-origin.y)/scale, 0.0f, static_cast<float>(image_height)));
        if (can_edit && ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            dragging = true; drag_origin = pixel;
        }
        if (!can_edit) dragging = false;
        auto* draw = ImGui::GetWindowDrawList();
        const auto rectangle = [&](const std::array<int, 4>& box, ImU32 color) {
            draw->AddRect({origin.x + box[0]*scale, origin.y + box[1]*scale},
                {origin.x + (static_cast<float>(box[0])+box[2])*scale,
                 origin.y + (static_cast<float>(box[1])+box[3])*scale}, color, 0.0f, 2.0f, ImDrawFlags_None);
        };
        rectangle(target, IM_COL32(60, 230, 110, 255));
        rectangle(background, IM_COL32(60, 180, 250, 255));
        if (dragging) {
            std::array<int,4> box{static_cast<int>(std::floor(std::min(pixel.x,drag_origin.x))),
                static_cast<int>(std::floor(std::min(pixel.y,drag_origin.y))),
                static_cast<int>(std::ceil(std::abs(pixel.x-drag_origin.x))),
                static_cast<int>(std::ceil(std::abs(pixel.y-drag_origin.y)))};
            rectangle(box, IM_COL32(255, 210, 70, 255));
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                dragging = false;
                (selection == 0 ? target : background) = box;
                edited(actions, true);
            }
        }
        ImGui::Text("原图 %d × %d；绿框目标，蓝框背景。坐标单位为原图像素。", image_width, image_height);
    }
};

RecoilTargetPanel::RecoilTargetPanel() : impl_(std::make_unique<Impl>()) {}
RecoilTargetPanel::~RecoilTargetPanel() = default;

void RecoilTargetPanel::render(AppConfig& config, OverlayActions& actions,
    const debug_session::Snapshot* snapshot, ID3D11Device* device, bool can_edit) noexcept {
    try {
        auto& p = *impl_;
        if (!p.initialized) {
            p.profile_path = path_text(std::filesystem::u8path(config.recoil.profile_directory) / "weapon_ak47-r1.json");
            p.initialized = true;
        }
        p.consume(snapshot);
        p.poll_preview(device);
        ImGui::PushID("recoil_target_panel");
        ImGui::TextWrapped("固定目标迭代：先短录像选两块纹理，再标定与无射击对照。原30发曲线保持原样，候选仅覆盖独立验证的短段。");
        ImGui::TextWrapped("准备后回到游戏，按住测试键直到本轮完成；松开测试键或按End立即终止。每轮重新准备，不自动续打。");
        ImGui::Text("当前灵敏度 %.3f；实验 X / Y 强度固定 100%%。", config.recoil.sensitivity);
        const bool editable = can_edit && !(snapshot && snapshot->busy);
        ImGui::BeginDisabled(!editable);
        ImGui::PushItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x - 210.0f));
        if (ImGui::InputText("武器 ID", &p.weapon)) p.edited(actions, true);
        help("与GSI、父曲线及标定的武器身份一致；更改后原标定失效。");
        if (ImGui::InputText("父曲线 JSON", &p.profile_path)) p.edited(actions);
        help("输入认可曲线的完整或工作目录相对路径。不会导入、活动或覆盖该曲线。");
        if (ImGui::InputText("批次 / Session", &p.session_id)) p.edited(actions);
        help("同一父基线的独立重复轮使用同一批次标识；fit和验证轮用途单独冻结。");
        if (ImGui::SliderInt("本轮时长 (ms)", &p.duration_ms, 100, 3000)) p.edited(actions);
        help("有限实验窗口，单位毫秒；不能凭时长或GSI延迟保证实际发数。");
        if (ImGui::SliderInt("弹数上限", &p.shots, 1, 5)) p.edited(actions);
        help("首轮最多五发。开火前须手动预置不超过此数的可用弹药且不换弹。");
        if (ImGui::Combo("数据用途", &p.split, "fit\0holdout\0test\0")) p.edited(actions);
        help("本轮开始前冻结用途。拟合轮不得冒充独立验证轮；前馈单独验证使用test。");
        if (ImGui::Button("1. 准备短录像（无输出）")) p.prepare("observe", false, actions);
        help("允许尚未选ROI；只录制有限短片。准备后由你在调试会话中启动，不自动续录。");
        p.image(actions, editable);
        if (ImGui::InputInt4("目标 ROI (x,y,w,h)", p.target.data())) p.edited(actions, true);
        help("原图整数像素，最小32×32。调整后标定和无射击对照失效。");
        if (ImGui::InputInt4("背景 ROI (x,y,w,h)", p.background.data())) p.edited(actions, true);
        help("独立静态纹理，不能与目标及1像素边缘重叠。不是从目标误差中扣除背景位移。");
        const bool regions = p.regions_ready();
        if (!regions) ImGui::TextDisabled("请在原图内选择两块至少32×32且边缘不重叠的区域。");
        ImGui::BeginDisabled(!regions);
        if (ImGui::Button("2. 准备两轴标定（不射击）")) p.prepare("calibrate", false, actions);
        help("会在用户启动后产生有界鼠标位移；测量方向、交叉轴和响应。完成后自动带入标定文件，不自动开始下一步。");
        ImGui::EndDisabled();
        if (ImGui::InputText("标定文件", &p.calibration_path)) p.edited(actions);
        help("自动带入本轮成功标定，也可输入既有文件；后端核对模板、环境与父版本身份。");
        ImGui::BeginDisabled(!regions || p.calibration_path.empty() || p.profile_path.empty());
        if (ImGui::Button("3. 准备无射击闭环对照")) p.prepare("control", false, actions);
        help("用户启动后仅产生鼠标补偿，不按下左键。静止且全程零反馈时，仅验证噪声与漂移，不能认定动态纠偏、符号或延迟振荡已验收。");
        ImGui::EndDisabled();
        ImGui::TextWrapped("对照限制：静止且零反馈只支持噪声与漂移检查，不代表动态纠偏已通过。");
        ImGui::TextWrapped("无射击对照：%s", p.control_path.empty() ? "待完成" : p.control_path.c_str());
        ImGui::BeginDisabled(p.control_path.empty());
        if (ImGui::Checkbox("我已观察本次无射击对照，确认画面稳定", &p.stable_confirmed)) actions.debug_plan_edited = true;
        help("只表示你核对了该次对照的可见稳定；自动报告不能替代此确认，也不代表实际射向合格。");
        ImGui::EndDisabled();
        if (ImGui::Checkbox("已限弹、关闭自动补弹，且本轮不换弹", &p.ammo_confirmed)) actions.debug_plan_edited = true;
        help("确认已将可用弹药预置为不超过本轮上限、关闭自动补弹且不换弹。物理发数限制由现场条件落实；按钮回执、预计射速和延迟GSI不能代替。");
        const bool fire_ready = regions && !p.calibration_path.empty() && !p.profile_path.empty() &&
            !p.control_path.empty() && p.stable_confirmed && p.ammo_confirmed;
        ImGui::BeginDisabled(!fire_ready);
        if (ImGui::Button("4. 准备单轮基线 + 反馈")) p.prepare("control", true, actions);
        help("准备最多五发的单轮试验；仍须用户明确启动。松键/End、失配或清理异常终止，不自动重试。");
        ImGui::BeginDisabled(p.split != 2);
        if (ImGui::Button("准备前馈独立验证")) p.prepare("test", true, actions);
        help("选择test用途后可用。单独回放指定父/候选曲线，反馈不替曲线兜底，结果仍须人工物理确认。");
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::PopItemWidth();
        ImGui::EndDisabled();
        if (snapshot && snapshot->busy) {
            if (ImGui::Button("停止本轮")) {
                actions.debug_action = debug_session::Action::CANCEL;
                p.stable_confirmed = false;
            }
            help("撤销新输出并进入清理；清理未知不能认定安全完成。");
        }
        if (!p.status.empty()) ImGui::TextWrapped("%s", p.status.c_str());
        if (!p.report_directory.empty()) {
            ImGui::TextWrapped("本轮目录：%s", p.report_directory.c_str());
            if (ImGui::Button("查看本轮目录")) {
                const auto directory = std::filesystem::u8path(p.report_directory);
                ShellExecuteW(nullptr, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            help("打开原始图像、命令和结果归档；不会活动曲线或开始下一轮。");
        }
        ImGui::PopID();
    } catch (...) { impl_->status = "固定目标面板操作失败，请检查路径与本轮报告。"; }
}
