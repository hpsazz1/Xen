#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include "overlay/recoil_target_analysis_panel.h"
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <future>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
void help(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::BeginTooltip(); ImGui::PushTextWrapPos(ImGui::GetFontSize()*28);
        ImGui::TextUnformatted(text); ImGui::PopTextWrapPos(); ImGui::EndTooltip();
    }
}
std::wstring quote(const std::wstring& value) {
    std::wstring result=L"\"";
    std::size_t slashes=0;
    for (const auto c:value) {
        if (c==L'\\') { ++slashes; continue; }
        result.append(c==L'"'?slashes*2+1:slashes,L'\\'); slashes=0; result+=c;
    }
    result.append(slashes*2,L'\\'); result+=L'"'; return result;
}
std::wstring wide(const std::string& text) { return fs::u8path(text).wstring(); }
struct Result { std::string message, path; Json report; };
struct Handle { HANDLE value=nullptr; ~Handle(){ if(value && value!=INVALID_HANDLE_VALUE)CloseHandle(value); } };
Result run_tool(std::string python, std::string script, std::vector<std::string> args, fs::path output,
    std::shared_ptr<std::atomic<bool>> canceled) {
    try {
        if(canceled->load())return {"离线任务已取消，未启动",{}, {}};
        const auto executable=fs::absolute(fs::u8path(python));
        const auto tool=fs::absolute(fs::u8path(script));
        if (!fs::is_regular_file(executable) || !fs::is_regular_file(tool))
            throw std::runtime_error("请填写已安装的Python可执行文件和离线工具路径；不自动安装环境");
        fs::create_directories(output.parent_path());
        std::wstring command=quote(executable.wstring())+L" -B -X utf8 "+quote(tool.wstring());
        for(const auto& arg:args)command+=L" "+quote(wide(arg));
        command+=L" --output "+quote(output.wstring());
        if(command.size()>30000)throw std::runtime_error("分析参数过长");
        if(canceled->load())return {"离线任务已取消，未启动",{}, {}};
        STARTUPINFOW startup{}; startup.cb=sizeof(startup);
        PROCESS_INFORMATION process{};
        if(!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,
            nullptr,nullptr,&startup,&process))throw std::runtime_error("离线工具未能启动");
        Handle thread{process.hThread}, task{process.hProcess};
        // 有限离线作业，窗口关闭不留下无限后台任务。
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(120);
        while(WaitForSingleObject(task.value,50)!=WAIT_OBJECT_0) {
            if(canceled->load() || std::chrono::steady_clock::now()>=deadline) {
                TerminateProcess(task.value,2); WaitForSingleObject(task.value,5000);
                throw std::runtime_error("离线分析已取消或超时；原始记录保留，未完成的报告不可使用");
            }
        }
        DWORD exit_code=2; GetExitCodeProcess(task.value,&exit_code);
        const auto utf8=output.u8string();
        Result result; result.path.assign(reinterpret_cast<const char*>(utf8.data()),utf8.size());
        if(fs::is_regular_file(output) && fs::file_size(output)<=16*1024*1024) {
            std::ifstream file(output,std::ios::binary); result.report=Json::parse(file);
        }
        result.message=exit_code==0?"离线处理完成；未生成可执行弹道或改动活动曲线":"数据审核拒绝；请查看报告中的具体原因";
        if(result.report.contains("error"))result.message=result.report.at("error").get<std::string>();
        if(result.report.is_null())result.message="离线工具没有返回完整报告，请检查Python版本和工具路径";
        return result;
    } catch(const std::exception& error) { return {error.what(),{}, {}}; }
}
}
struct RecoilTargetAnalysisPanel::Impl {
    std::string python, script="scripts/recoil_target_iteration.py", runs, parent, observation;
    bool identity_confirmed=false;
    std::future<Result> job;
    std::shared_ptr<std::atomic<bool>> canceled=std::make_shared<std::atomic<bool>>(false);
    Result result;
    Impl() {
        // 只查解释器，不导入训练依赖、不触发安装。
        wchar_t path[32768]{};
        const auto length=SearchPathW(nullptr,L"python.exe",nullptr,32768,path,nullptr);
        if(length>0 && length<32768) {
            const auto text=fs::path(path).u8string(); python.assign(reinterpret_cast<const char*>(text.data()),text.size());
        }
    }
    std::vector<std::string> selected_runs() const {
        std::vector<std::string> values; std::istringstream lines(runs); std::string line;
        while(std::getline(lines,line)) {
            if(!line.empty() && line.back()=='\r')line.pop_back();
            if(!line.empty())values.push_back(line);
        }
        return values;
    }
    void start(const std::string& action) {
        if(job.valid())return;
        auto selected=selected_runs();
        if(selected.empty() || selected.size()>20) { result.message="请填写1至20个独立Run目录，每行一个"; return; }
        if(action!="fit" && selected.size()!=1) { result.message="核对或审核每次只处理一个Run"; return; }
        std::vector<std::string> args{action};
        args.insert(args.end(),selected.begin(),selected.end());
        args.insert(args.end(),{"--registry","cache/recoil/target-usage.json"});
        if(action=="fit")args.insert(args.end(),{"--parent",parent});
        if(action=="review")args.insert(args.end(),{"--identity-confirmed","--observation",observation});
        const auto output=fs::absolute(fs::path("cache/recoil/target-analysis")/
            ("analysis-"+std::to_string(GetCurrentProcessId())+"-"+
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".json"));
        canceled->store(false);
        job=std::async(std::launch::async,run_tool,python,script,std::move(args),output,canceled);
    }
};
RecoilTargetAnalysisPanel::RecoilTargetAnalysisPanel():impl_(std::make_unique<Impl>()){}
RecoilTargetAnalysisPanel::~RecoilTargetAnalysisPanel(){ impl_->canceled->store(true); }
void RecoilTargetAnalysisPanel::render(bool can_edit) {
    auto& s=*impl_;
    if(s.job.valid() && s.job.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
        s.result=s.job.get(); s.identity_confirmed=false;
    }
    if(!ImGui::TreeNode("固定目标：审核与多轮分析"))return;
    ImGui::TextWrapped("按同一父曲线收集独立轮，逐轮核对后合并。这里只分析残差；未证明时序映射时不会导出可执行曲线。");
    ImGui::BeginDisabled(!can_edit || s.job.valid());
    ImGui::InputText("Python可执行文件",&s.python); help("使用已有Python 3.12或更新版本；此工具只依赖标准库，不安装训练环境。");
    ImGui::InputText("离线分析工具",&s.script); help("选择随源码的scripts/recoil_target_iteration.py；正式包路径另行指定。");
    ImGui::TextUnformatted("Run目录（每行一个）");
    if(ImGui::InputTextMultiline("##target_run_directories",&s.runs,ImVec2(-1,ImGui::GetTextLineHeight()*4))) {
        s.identity_confirmed=false; s.observation.clear();
    }
    help("目录内应有manifest和原始记录。审核每次一个Run；合并至少五个同父版本且用途为fit的独立Run。");
    ImGui::InputText("父曲线文件",&s.parent); help("必须与本组实际执行的父曲线摘要一致；不加载为活动曲线。");
    if(ImGui::Button("核对记录"))s.start("inspect"); help("后台核对文件摘要、身份、命令守恒和时间；不执行设备动作。");
    ImGui::TextUnformatted("本轮人工观察");
    ImGui::InputTextMultiline("##target_human_observation",&s.observation,ImVec2(-1,ImGui::GetTextLineHeight()*3));
    help("直接记录目标稳定、过冲或异常情况；保留原话，不自动写成物理通过。");
    ImGui::Checkbox("已核对本轮目标与锚点身份",&s.identity_confirmed);
    help("仅确认本轮没有换目标或换锚点，不代表实际射向补偿已通过。");
    ImGui::BeginDisabled(!s.identity_confirmed || s.observation.empty());
    if(ImGui::Button("保存本轮审核"))s.start("review"); help("保存可追溯审核修订；不修改原始帧、命令、用途或活动曲线。");
    ImGui::EndDisabled(); ImGui::SameLine();
    if(ImGui::Button("合并五轮残差分析"))s.start("fit"); help("冻结数据用途，用中位数与MAD展示父基线加一次残差；没有独立时序资格时只返回分析，不生成弹道。");
    ImGui::EndDisabled();
    if(s.job.valid())ImGui::TextUnformatted("离线作业运行中；不会触发设备。");
    if(s.job.valid() && ImGui::Button("取消离线分析"))s.canceled->store(true);
    if(!s.result.message.empty())ImGui::TextWrapped("%s",s.result.message.c_str());
    if(s.result.report.contains("reasons"))for(const auto& reason:s.result.report.at("reasons"))
        if(reason.is_string())ImGui::TextWrapped("%s",reason.get_ref<const std::string&>().c_str());
    if(!s.result.path.empty() && ImGui::Button("查看离线报告"))
        ShellExecuteW(nullptr,L"open",fs::u8path(s.result.path).c_str(),nullptr,nullptr,SW_SHOWNORMAL);
    help("打开刚生成的离线报告；报告中的代数修正不是可执行曲线。");
    ImGui::TreePop();
}
