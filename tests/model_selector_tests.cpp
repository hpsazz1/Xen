#include "overlay/model_selector_internal.h"
#include <imgui_internal.h>
#include <cmath>
#include <iostream>
#include <map>
#include <vector>

namespace {
int failures = 0;
void expect(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << message << '\n'; }
}
void setup() {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.LogFilename = nullptr;
    io.DisplaySize = {1000,700}; io.DeltaTime = 1.0f/60.0f;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigDebugHighlightIdConflicts = false;
    auto& platform = ImGui::GetPlatformIO();
    platform.Platform_GetClipboardTextFn = [](ImGuiContext*) { return ""; };
    platform.Platform_SetClipboardTextFn = [](ImGuiContext*, const char*) { ++failures; };
    platform.Platform_SetImeDataFn = nullptr; platform.Platform_OpenInShellFn = nullptr;
    unsigned char* pixels = nullptr; int w=0,h=0; io.Fonts->GetTexDataAsRGBA32(&pixels,&w,&h);
}
void begin_frame() {
    ImGui::NewFrame(); ImGui::SetNextWindowPos({10,10}); ImGui::SetNextWindowSize({900,600});
    ImGui::Begin("model-fixture",nullptr,ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoResize);
}
void end_frame() { ImGui::End(); ImGui::Render(); }

// 独立以完整UTF-8文字生成字形几何，检查生产控件实际生成了完整文字；不以ID/字符串变量代替显示验证。
bool contains_literal_text(ImDrawList* draw, int begin, int end, ImVec2 position, const std::string& text) {
    const int reference = draw->VtxBuffer.Size;
    draw->AddText(position,ImGui::GetColorU32(ImGuiCol_Text),text.c_str(),text.c_str()+text.size());
    const int count = draw->VtxBuffer.Size-reference;
    for (int start=begin;start+count<=end;++start) {
        bool equal = true;
        for (int i=0;i<count && equal;++i) {
            const auto& a=draw->VtxBuffer[start+i]; const auto& b=draw->VtxBuffer[reference+i];
            equal=std::abs(a.pos.x-b.pos.x)<0.01f && std::abs(a.pos.y-b.pos.y)<0.01f && a.uv.x==b.uv.x && a.uv.y==b.uv.y;
        }
        if(equal && count>0)return true;
    }
    return false;
}
void text_and_ids() {
    setup();
    std::vector<std::string> names{"plain.onnx","model##v2.onnx","alpha###same.onnx","beta###same.onnx","模型##版本.onnx"};
    std::map<std::string,ImGuiID> ids;
    begin_frame();
    for (const auto& name:names) {
        auto* draw=ImGui::GetWindowDrawList();const int begin=draw->VtxBuffer.Size;const auto pos=ImGui::GetCursorScreenPos();
        overlay::detail::model_selectable("fixture/A/models",name,false);
        ids[name]=ImGui::GetItemID(); const int end=draw->VtxBuffer.Size;
        expect(contains_literal_text(draw,begin,end,pos,name),"列表项必须完整显示字面文件名，包括##/###和Unicode");
    }
    expect(ids[names[2]]!=ids[names[3]],"同###后缀的不同文件必须有独立控件ID");
    overlay::detail::model_selectable("fixture/B/models",names[0],false);
    expect(ImGui::GetItemID()!=ids[names[0]],"同文件名在不同目录必须具有独立内部身份");
    end_frame();
    begin_frame();
    std::reverse(names.begin(),names.end());
    for(const auto& name:names) {
        overlay::detail::model_selectable("fixture/A/models",name,false);
        expect(ImGui::GetItemID()==ids[name],"刷新排序不应改变同目录同文件的内部身份");
    }
    std::string selected="beta###same.onnx";
    auto* draw=ImGui::GetWindowDrawList(); const int begin=draw->VtxBuffer.Size;
    auto pos=ImGui::GetCursorScreenPos();pos.x+=ImGui::GetStyle().FramePadding.x;pos.y+=ImGui::GetStyle().FramePadding.y;
    ImGui::SetNextItemWidth(600);
    overlay::detail::render_model_combo(names,"fixture/A/models",selected);
    const int end=draw->VtxBuffer.Size;
    expect(contains_literal_text(draw,begin,end,pos,selected),"折叠后的预览也必须显示完整文件名");
    end_frame(); ImGui::DestroyContext();
}

void interactions(float scale) {
    setup(); ImGui::GetStyle().FontScaleMain=scale;
    auto& io=ImGui::GetIO();
    std::vector<std::string> names{"plain.onnx","model##v2.onnx","alpha###same.onnx","beta###same.onnx","模型##版本.onnx"};
    std::string selection="plain.onnx", directory="fixture/A/models";
    ImVec2 combo{}, first{};float step=0;
    auto frame=[&] {
        begin_frame();const auto pos=ImGui::GetCursorScreenPos();combo={pos.x+20,pos.y+8};
        ImGui::SetNextItemWidth(600);overlay::detail::render_model_combo(names,directory,selection);
        if(!GImGui->OpenPopupStack.empty() && GImGui->OpenPopupStack.back().Window) {
            first=GImGui->OpenPopupStack.back().Window->DC.CursorStartPos;first.x+=20;first.y+=5;
        }
        step=ImGui::GetTextLineHeightWithSpacing();end_frame();
    };
    auto click=[&](ImVec2 pos) {io.AddMousePosEvent(pos.x,pos.y);frame();io.AddMouseButtonEvent(0,true);frame();io.AddMouseButtonEvent(0,false);frame();};
    auto open=[&] {click(combo);frame();frame();expect(!GImGui->OpenPopupStack.empty(),"下拉框必须实际打开");};
    auto escape=[&] {io.AddKeyEvent(ImGuiKey_Escape,true);frame();io.AddKeyEvent(ImGuiKey_Escape,false);frame();};
    auto row=[&](int index) {return ImVec2(first.x,first.y+step*index);};
    frame();frame();
    for(int index: {1,3,4}) {
        open();click(row(index));frame();
        expect(selection==names[index],"点击必须选择准确的完整文件名，不能被同ID条目抢占");
    }
    const auto kept=selection;open();escape();
    expect(selection==kept && GImGui->OpenPopupStack.empty(),"Escape取消不应更改选择");
    open();click({850,500});frame();
    expect(selection==kept && GImGui->OpenPopupStack.empty(),"点击外部取消不应更改选择");
    std::reverse(names.begin(),names.end());frame();
    expect(selection==kept,"正常刷新排序应保留按文件名绑定的选择");
    // 重命名后旧选择显式不可用，不自动取邻项；与生产路径再次校验的合同一致。
    auto chosen=std::find(names.begin(),names.end(),selection);*chosen="renamed##版本.onnx";frame();
    expect(selection==kept && std::find(names.begin(),names.end(),selection)==names.end(),"重命名不得悄悄把选择迁移到新文件或邻项");
    names={"old###same.onnx","plain.onnx"};selection="plain.onnx";frame();
    open();auto pos=row(0);io.AddMousePosEvent(pos.x,pos.y);frame();io.AddMouseButtonEvent(0,true);frame();
    names[0]="new###same.onnx";frame();io.AddMouseButtonEvent(0,false);frame();
    expect(selection=="plain.onnx","按下后文件重命名不得在松开时误选新文件");escape();
    open();click(row(0));frame();expect(selection==names[0],"重命名后重新明确点击应能选择新文件");
    selection="plain.onnx";frame();open();pos=row(0);io.AddMousePosEvent(pos.x,pos.y);frame();io.AddMouseButtonEvent(0,true);frame();
    std::swap(names[0],names[1]);frame();io.AddMouseButtonEvent(0,false);frame();
    expect(selection=="plain.onnx","按下后刷新排序不得误选当前位置的另一条");escape();
    open();pos=row(1);io.AddMousePosEvent(pos.x,pos.y);frame();io.AddMouseButtonEvent(0,true);frame();
    directory="fixture/B/models";frame();io.AddMouseButtonEvent(0,false);frame();
    expect(selection=="plain.onnx","按下后目录上下文改变不得误选另一目录中的同名文件");escape();
    names.clear();frame();click(combo);frame();
    expect(selection=="plain.onnx" && GImGui->OpenPopupStack.empty(),"空目录禁用选择并保留失效值供用户处理");
    std::cout<<"interaction_scale="<<scale<<" completed\n";ImGui::DestroyContext();
}
}
int main() {
    text_and_ids();interactions(1.0f);interactions(1.25f);
    std::cout<<"native_windows=0 system_input=0 system_clipboard=0 failures="<<failures<<'\n';
    return failures ? 1 : 0;
}
