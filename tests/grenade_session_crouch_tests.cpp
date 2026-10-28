#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <bcrypt.h>
#ifdef ERROR
#undef ERROR
#endif
#include "debug_session/debug_session.h"
#include "movement/timeline.h"
#include "runtime/startup_internal.h"
#include "source_context/source_context_internal.h"
#include "log/log.h"
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using namespace debug_session;
void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}
template<class Predicate> void await(Predicate predicate, const char* message) {
    const auto deadline = Clock::now() + 8s;
    while (!predicate()) {
        require(Clock::now() < deadline,message);
        std::this_thread::sleep_for(2ms);
    }
}
std::string utf8(const std::filesystem::path& path) {
    const auto text = path.u8string(); return {text.begin(),text.end()};
}
void save(const std::filesystem::path& path, const Json& value) {
    std::ofstream output(path,std::ios::binary);
    require(static_cast<bool>(output),"合成资料无法写入"); output << value.dump(2);
}
Json read(const std::filesystem::path& path) {
    std::ifstream input(path,std::ios::binary); return Json::parse(input);
}
std::string digest(const std::vector<unsigned char>& bytes) {
    BCRYPT_ALG_HANDLE handle = nullptr;
    require(BCryptOpenAlgorithmProvider(&handle,BCRYPT_SHA256_ALGORITHM,nullptr,0)==0,"测试摘要初始化失败");
    std::array<unsigned char,32> hash{};
    const auto result = BCryptHash(handle,nullptr,0,const_cast<PUCHAR>(bytes.data()),
        static_cast<ULONG>(bytes.size()),hash.data(),static_cast<ULONG>(hash.size()));
    BCryptCloseAlgorithmProvider(handle,0);
    require(result==0,"测试摘要计算失败");
    std::ostringstream output; output << std::hex << std::setfill('0');
    for (const auto byte : hash) output << std::setw(2) << static_cast<unsigned>(byte);
    return output.str();
}
cv::Mat texture(int seed) {
    cv::Mat image(320,320,CV_8UC3); cv::RNG random(seed);
    random.fill(image,cv::RNG::UNIFORM,0,255); cv::GaussianBlur(image,image,{3,3},.65);
    return image;
}
// 回环鉴权端只发布合成焦点，不查询Windows窗口，也不连接真实源机。
class FocusFixture final {
public:
    source_context::SourceContextConfig config;
    std::atomic_bool focused{true}, failed{false};
    FocusFixture() {
        config.enabled = true; config.host = "127.0.0.1"; config.process_name = "synthetic.exe";
        config.token = std::string(32,'t'); config.ttl_ms = 300;
        socket_ = socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
        require(socket_!=INVALID_SOCKET,"回环焦点socket失败");
        sockaddr_in address{}; address.sin_family=AF_INET; address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        int length=sizeof(address);
        if (bind(socket_,reinterpret_cast<sockaddr*>(&address),sizeof(address))!=0 ||
            getsockname(socket_,reinterpret_cast<sockaddr*>(&address),&length)!=0) {
            closesocket(socket_); throw std::runtime_error("回环焦点绑定失败");
        }
        config.port=ntohs(address.sin_port); worker_=std::thread([this]{serve();});
    }
    ~FocusFixture() {
        stopped_=true; if(worker_.joinable()) worker_.join(); closesocket(socket_);
    }
private:
    void serve() noexcept {
        try {
            std::uint64_t sequence=0;
            while (!stopped_) {
                fd_set readers; FD_ZERO(&readers); FD_SET(socket_,&readers);
                timeval timeout{0,10000};
                if (select(0,&readers,nullptr,nullptr,&timeout)<=0) continue;
                source_context::detail::Packet packet{}; sockaddr_in peer{}; int length=sizeof(peer);
                const auto size=recvfrom(socket_,reinterpret_cast<char*>(packet.data()),static_cast<int>(packet.size()),
                    0,reinterpret_cast<sockaddr*>(&peer),&length);
                source_context::detail::Message message;
                require(size==static_cast<int>(packet.size()) && source_context::detail::decode(packet,config.token,message),
                    "回环焦点请求鉴权失败");
                message.response=true; message.session=11; message.sequence=++sequence;
                message.focus=focused ? source_context::detail::Focus::Foreground : source_context::detail::Focus::Background;
                require(source_context::detail::encode(message,config.token,packet),"回环焦点响应编码失败");
                require(sendto(socket_,reinterpret_cast<const char*>(packet.data()),static_cast<int>(packet.size()),0,
                    reinterpret_cast<sockaddr*>(&peer),length)==static_cast<int>(packet.size()),"回环焦点响应失败");
            }
        } catch (...) { failed=true; }
    }
    SOCKET socket_=INVALID_SOCKET;
    std::atomic_bool stopped_{false};
    std::thread worker_;
};
struct Scene {
    std::mutex mutex;
    cv::Mat crouched=texture(1347), standing=texture(7788);
    bool ctrl=false, space=false, left=false, right=false;
    std::uint8_t wasd=0, mask=0;
    double x=3,y=-2;
    std::uint64_t sequence=0;
    int creates=0, opens=0, closes=0, moves=0, ctrl_downs=0, ctrl_ups=0, cleanups=0;
    bool moved_without_crouch=false, jumped_without_crouch=false, walked_without_crouch=false;
    bool released_button_without_crouch=false;
    std::vector<std::string> events;
};
// 所有输出只改内存。软件组合状态与poll_input的合成物理状态分离。
class Device final : public IMouseController {
public:
    explicit Device(std::shared_ptr<Scene> scene) : scene_(std::move(scene)) {}
    bool open() noexcept override { return true; }
    void close() noexcept override {}
    MouseStatus status() const noexcept override { return MouseStatus::READY; }
    std::string last_error() const override { return {}; }
    bool output_owner_exclusive() const noexcept override { return true; }
    bool supports_wasd_keyboard() const noexcept override { return true; }
    bool supports_left_ctrl_key() const noexcept override { return true; }
    bool supports_left_button() const noexcept override { return true; }
    void set_poll_callback(std::function<void()> callback) {
        std::lock_guard lock(callback_mutex_); poll_callback_=std::move(callback);
    }
    bool poll_input(InputSnapshot& input) noexcept override {
        try {
            std::function<void()> callback;
            { std::lock_guard lock(callback_mutex_); callback=poll_callback_; }
            if(callback) callback();
        } catch (...) { return false; }
        input={}; input.state_valid=true; input.status=InputMonitorStatus::READY; return true;
    }
    MouseMoveReceipt move(const MouseMoveCommand& command) noexcept override {
        std::lock_guard lock(scene_->mutex); ++scene_->moves;
        scene_->moved_without_crouch |= !scene_->ctrl;
        scene_->x-=command.dx_counts; scene_->y-=command.dy_counts;
        MouseMoveReceipt receipt; receipt.succeeded=true; receipt.backend_completed_at=Clock::now(); return receipt;
    }
    KeyboardReceipt set_left_ctrl_key(bool down) noexcept override {
        std::lock_guard lock(scene_->mutex); scene_->ctrl=down;
        if(down) ++scene_->ctrl_downs; else ++scene_->ctrl_ups;
        scene_->events.push_back(down?"ctrl_down":"ctrl_up"); return {KeyboardDisposition::ACKNOWLEDGED};
    }
    KeyboardReceipt set_wasd_keyboard(std::uint8_t mask) noexcept override {
        std::lock_guard lock(scene_->mutex); scene_->wasd=mask;
        scene_->walked_without_crouch |= mask && !scene_->ctrl;
        scene_->events.push_back(mask?"w_down":"w_up"); return {KeyboardDisposition::ACKNOWLEDGED};
    }
    KeyboardReceipt set_space_key(bool down) noexcept override {
        std::lock_guard lock(scene_->mutex); scene_->space=down;
        scene_->jumped_without_crouch |= down && !scene_->ctrl;
        scene_->events.push_back(down?"space_down":"space_up"); return {KeyboardDisposition::ACKNOWLEDGED};
    }
    KeyboardReceipt set_wasd_mask(std::uint8_t mask,bool enabled) noexcept override {
        std::lock_guard lock(scene_->mutex);
        if(enabled) scene_->mask|=mask; else scene_->mask&=~mask;
        return {KeyboardDisposition::ACKNOWLEDGED};
    }
    ButtonReceipt set_left_button(bool down) noexcept override {
        std::lock_guard lock(scene_->mutex);
        scene_->released_button_without_crouch |= !down && scene_->left && !scene_->ctrl;
        scene_->left=down; scene_->events.push_back(down?"left_down":"left_up"); return {ButtonDisposition::ACKNOWLEDGED};
    }
    ButtonReceipt set_right_button(bool down) noexcept override {
        std::lock_guard lock(scene_->mutex); scene_->right=down; return {ButtonDisposition::ACKNOWLEDGED};
    }
    KeyboardReceipt cleanup_wasd_keyboard() noexcept override {
        std::lock_guard lock(scene_->mutex); ++scene_->cleanups;
        scene_->wasd=scene_->mask=0; scene_->ctrl=scene_->space=false;
        scene_->events.push_back("keyboard_cleanup"); return {KeyboardDisposition::ACKNOWLEDGED};
    }
private:
    std::shared_ptr<Scene> scene_;
    std::mutex callback_mutex_;
    std::function<void()> poll_callback_;
};
class SyntheticCapture final : public ICapture {
public:
    explicit SyntheticCapture(std::shared_ptr<Scene> scene):scene_(std::move(scene)) {}
    bool open() noexcept override { std::lock_guard lock(scene_->mutex); ++scene_->opens; return true; }
    void close() noexcept override { std::lock_guard lock(scene_->mutex); ++scene_->closes; }
    CaptureStatus status() const noexcept override { return CaptureStatus::READY; }
    std::string last_error() const override { return "合成蹲站CPU图"; }
    CaptureStatus grab(CapturedFrame& frame) noexcept override {
        std::this_thread::sleep_for(8ms);
        try {
            std::lock_guard lock(scene_->mutex);
            frame={}; frame.width=frame.height=frame.source_width=frame.source_height=320;
            frame.encoded_width=frame.encoded_height=320;
            const cv::Mat warp=(cv::Mat_<double>(2,3)<<1,0,scene_->x,0,1,scene_->y);
            cv::warpAffine(scene_->ctrl?scene_->crouched:scene_->standing,frame.bgr,warp,{320,320},
                cv::INTER_LINEAR,cv::BORDER_REFLECT_101);
            frame.timing.sequence=++scene_->sequence; frame.timing.captured_at=Clock::now();
            return CaptureStatus::FRAME;
        } catch (...) { return CaptureStatus::FAILURE; }
    }
private:
    std::shared_ptr<Scene> scene_;
};
void wait_idle(Session& session) {
    await([&]{session.poll();return !session.busy();},"生产Session后台任务未及时结束"); session.poll();
}
class Harness final {
public:
    FocusFixture focus;
    std::shared_ptr<Scene> scene=std::make_shared<Scene>();
    std::shared_ptr<Device> device=std::make_shared<Device>(scene);
    std::unique_ptr<Runtime> runtime;
    Session session;
    Context context;
    Request request;
    Json plan;
    Harness(const std::filesystem::path& root,const std::filesystem::path& model) {
        ::runtime::detail::StartupAdapter adapter;
        adapter.create_capture=[value=scene](const CaptureConfig& config) {
            require(config.roi_width==320 && config.roi_height==320,"必须复用320×320生产ROI");
            { std::lock_guard lock(value->mutex); ++value->creates; }
            return std::make_unique<SyntheticCapture>(value);
        };
        runtime=::runtime::detail::make_runtime_with_startup_adapter(std::move(adapter));
        auto& config=context.config;
        config.detector.model_path=utf8(model); config.detector.backend=BackendType::CPU;
        config.detector.output_format=OutputFormat::CHANNEL_FIRST;
        config.capture.backend=CaptureBackend::DESKTOP_DUPLICATION;
        config.capture.roi_width=config.capture.roi_height=320;
        config.mouse.backend=MouseBackend::KMBOX_NET;
        // 与现有Runtime启动专项一致的合成配置；设备仍只使用显式传入的内存替身。
        config.mouse.kmbox_ip="127.0.0.1"; config.mouse.kmbox_port=50000; config.mouse.kmbox_uuid="00000000";
        config.mouse.allow_send_input=true; // 只给显式传入的内存替身，不构造后端。
        config.source_context=focus.config;
        config.auto_stop.enabled=config.trigger.enabled=config.recoil.enabled=false;
        config.movement.enabled=config.gsi.enabled=false;
        config.keyboard.grenade_assist_enabled=true;
        config.keyboard.debug_test_enabled=true;
        config.keyboard.debug_test_virtual_keys={0x7A};
        config.keyboard.emergency_virtual_keys={0x23};
        config.aim.counts_per_pixel_x=config.aim.counts_per_pixel_y=1;
        const bool started=runtime->start(config,device);
        require(started,"真实CPU Runtime启动失败："+runtime->snapshot().last_error);
        require(runtime->post_intent({RuntimeIntentType::INPUT_HEALTH_CHANGED,true}),"内存输入健康意图未接受");
        await([&]{const auto s=runtime->snapshot();return s.input_healthy && s.source_context.available &&
            s.source_context.focused && s.processed_frames>0;},"CPU Runtime/源焦点未就绪");
        require(runtime->snapshot().provider=="CPUExecutionProvider","测试必须使用真实CPU Provider");
        context.device=device; context.runtime=runtime.get();
        context.input_recording_idle=context.cleanup_known=true;
        context.runtime_idle=false;
        context.map.valid=true; context.map.map_name="de_mirage"; context.map.local_team=weapon::Team::T;
        context.map.context_epoch=1;
        movement::timeline::Plan value; value.name="T垃圾桶左 蹲姿W跳投";
        value.map="de_mirage"; value.team="T"; value.crouch=true; value.mask={true,15,0,0};
        movement::timeline::Action left; left.kind=movement::timeline::ActionKind::BUTTON; left.duration_ms=180;
        value.actions={left,{movement::timeline::ActionKind::KEY,movement::timeline::Key::CTRL,0,1},
            {movement::timeline::ActionKind::KEY,movement::timeline::Key::W,80,160},
            {movement::timeline::ActionKind::KEY,movement::timeline::Key::SPACE,160,40}};
        plan=movement::timeline::plan_json(value);
        const auto plans=root/"plans", references=root/"references"/"sample";
        std::filesystem::create_directories(plans); std::filesystem::create_directories(references);
        save(plans/"sample.json",plan);
        std::vector<unsigned char> image; cv::imencode(".png",scene->crouched,image);
        { std::ofstream file(references/"aim.png",std::ios::binary);
          file.write(reinterpret_cast<const char*>(image.data()),static_cast<std::streamsize>(image.size())); }
        save(references/"reference.json",{{"schema_version",1},{"plan_id","sample.json"},{"reference_id","sample"},
            {"plan",plan},{"aim_plan",plan},{"manual_throw_verified",true},{"capture_complete",false},
            {"aim_image","aim.png"},{"image_sha256",{{"aim_image",digest(image)}}},
            {"geometry",{{"source_width",320},{"source_height",320},{"roi_x",0},{"roi_y",0},
                {"roi_width",320},{"roi_height",320},{"scale_x",1},{"scale_y",1}}}});
        request.mode=Mode::GRENADE_TEST; request.show_hud=false; request.plan_text=plan.dump();
        request.load_path=utf8(plans/"sample.json"); request.movement_library_root=utf8(plans);
        request.output_root=utf8(root/"reports");
        load_and_enable();
    }
    ~Harness() {
        session.cancel("合成集成测试结束");
        try {wait_idle(session);} catch(...) {}
        if(runtime) runtime->stop();
    }
    void load_and_enable() {
        require(session.dispatch(Action::LOAD_PLAN,request,context),"生产Session拒绝载入蹲姿方案");
        wait_idle(session);
        require(session.snapshot()->state==State::COMPLETED,"生产方案载入失败："+session.snapshot()->message);
        require(session.dispatch(Action::ENABLE_MOVEMENT_TEST,request,context),"生产Session拒绝启用蹲姿方案");
        require(session.snapshot()->repeat_ready,"蹲姿方案没有形成生产测试模板");
    }
    void align() {
        require(session.align_grenade(context),"生产Session未接受蹲姿找点："+session.snapshot()->message);
        wait_idle(session);
        const auto s=session.snapshot();
        require(s->grenade_guidance_state=="aligned" && !s->cleanup_unknown,
            "生产蹲姿找点未成功："+s->message);
        { std::lock_guard lock(scene->mutex);
          require(scene->ctrl && !scene->moved_without_crouch,"找点及等待下键必须在Ctrl已按下后执行"); }
        std::atomic_bool canceled{false}; std::string reason;
        require(!runtime->acquire_grenade_control(canceled,reason),"等待下键时独占租约被提前归还");
    }
    void assert_released() {
        { std::lock_guard lock(scene->mutex);
          require(!scene->ctrl && !scene->space && !scene->left && !scene->right && !scene->wasd && !scene->mask,
            "蹲姿会话结束后仍遗留软件输入");
          require(scene->creates==1 && scene->opens==1 && scene->closes==0,
            "找点或投掷不得重开采集器或另造320×320接收链"); }
        std::atomic_bool canceled{false}; std::string reason;
        auto control=runtime->acquire_grenade_control(canceled,reason);
        require(static_cast<bool>(control),"姿态结束后Runtime租约未恢复："+reason); control->finish();
        require(runtime->snapshot().state==RuntimeState::RUNNING && !session.snapshot()->cleanup_unknown,
            "合成蹲姿结束未恢复原Runtime或错误遗留清理未知");
    }
};
void end_to_end(const std::filesystem::path& root,const std::filesystem::path& model) {
    Harness h(root,model);
    h.align();
    std::this_thread::sleep_for(40ms); h.session.maintain_grenade(h.context,true);
    { std::lock_guard lock(h.scene->mutex); require(h.scene->ctrl,"闲置等待下键不能松蹲"); }
    require(h.session.throw_grenade(h.context),"生产下键未接受当前蹲姿身份");
    wait_idle(h.session);
    const auto completed=h.session.snapshot();
    require(completed->state==State::COMPLETED && completed->result && completed->result->value("success",false),
        "生产蹲姿找点至W跳投全链未成功："+completed->message);
    require(read(std::filesystem::u8path(completed->report_directory)/"grenade-run-plan.json")==h.plan,
        "组合执行必须保留原蹲姿/W/空格/鼠标计划");
    { std::lock_guard lock(h.scene->mutex);
      require(h.scene->moves>0 && !h.scene->jumped_without_crouch && !h.scene->walked_without_crouch &&
        !h.scene->released_button_without_crouch,"找点、W、空格和松鼠标期间都必须持续蹲姿");
      const auto jump=std::find(h.scene->events.begin(),h.scene->events.end(),"space_down");
      const auto release=std::find(h.scene->events.begin(),h.scene->events.end(),"left_up");
      require(jump!=h.scene->events.end() && release!=h.scene->events.end() && jump<release,
        "生产组合不得改变先跳再松鼠标的动作顺序"); }
    h.assert_released();
    h.align(); h.session.cancel_grenade("合成局部取消"); h.session.poll(); h.assert_released();
    h.align(); h.focus.focused=false;
    await([&]{return !h.runtime->snapshot().source_context.focused;},"真实回环失焦未到达Runtime");
    h.session.maintain_grenade(h.context,true); h.session.poll(); h.assert_released();
    h.focus.focused=true;
    await([&]{return h.runtime->snapshot().source_context.focused;},"回环焦点未恢复");
    h.align(); h.context.map.context_epoch=2; h.context.map.map_name="de_nuke";
    h.session.maintain_grenade(h.context,true); h.session.poll(); h.assert_released();
    h.context.map.context_epoch=3; h.context.map.map_name="de_mirage";
    h.align();
    auto edited=h.request; auto draft=h.plan; draft["notes"]="下一次草稿"; edited.plan_text=draft.dump();
    require(h.session.update_movement_draft(edited,h.context),"生产会话未接受修改草稿");
    h.session.poll(); h.assert_released();
    h.load_and_enable(); h.align();
    require(h.session.align_grenade(h.context),"重复上键未释放旧姿态后开始新找点");
    wait_idle(h.session);
    require(h.session.snapshot()->grenade_guidance_state=="aligned","重复上键没有得到新对齐身份");
    h.session.cancel_grenade("结束重复上键夹具"); h.session.poll(); h.assert_released();
    require(!h.focus.failed,"真实回环焦点夹具发生协议错误");
}
void publication_cancel(const std::filesystem::path& root,const std::filesystem::path& model) {
    Harness h(root,model);
    const auto prior_report=h.session.snapshot()->report_directory;
    auto canceled=std::make_shared<std::atomic_bool>(false);
    h.device->set_poll_callback([&h,prior_report,canceled] {
        if(canceled->load()) return;
        const auto snapshot=h.session.snapshot();
        if(snapshot->report_directory.empty() || snapshot->report_directory==prior_report) return;
        std::error_code error;
        const auto report=std::filesystem::u8path(snapshot->report_directory)/"grenade-alignment.json";
        if(std::filesystem::exists(report,error) && !error && !canceled->exchange(true))
            h.session.cancel_grenade("在最终许可读入之后、发布对齐身份之前取消");
    });
    require(h.session.align_grenade(h.context),"发布边界夹具未接受生产找点");
    wait_idle(h.session);
    h.device->set_poll_callback({});
    require(canceled->load(),"合成设备未命中报告已写完后的最终许可边界");
    h.assert_released();
    require(h.session.snapshot()->grenade_guidance_state=="hidden",
        "发布边界取消之后不能复活已撤销的对齐提示");
}
}
int main(int argc,char** argv) {
    if(argc!=3 && argc!=4) return 2;
    if(argc==4 && std::string(argv[3])!="--publication-cancel") return 2;
    WSADATA data{}; if(WSAStartup(MAKEWORD(2,2),&data)) return 2;
    _putenv_s("XEN_SOURCE_CONTEXT_TOKEN",""); // 仅隔离本测试进程，不读取或输出用户环境秘密。
    LogConfig logging; logging.enable_console=logging.enable_file=logging.enable_ringbuf=false; Log::init(logging);
    int status=0;
    const auto root=std::filesystem::absolute(std::filesystem::u8path(argv[1])/
        ("run-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(Clock::now().time_since_epoch().count()))).lexically_normal();
    try {
        std::filesystem::create_directories(root);
        const auto model=std::filesystem::u8path(argv[2]);
        if(argc==3) end_to_end(root/"complete",model);
        publication_cancel(root/"publication-cancel",model);
        std::filesystem::remove_all(root);
        std::cout << "生产Session蹲姿找点、保持、W跳投及失效清理集成通过；仅合成输入和CPU模型\n";
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';status=1;}
    Log::shutdown(); WSACleanup(); return status;
}
