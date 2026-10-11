#include "overlay/imgui_lifecycle_internal.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {
int failures = 0;
int allocations = 0;
int platform_stops = 0;
int renderer_stops = 0;
std::string cleanup_order;

void check(bool condition, const char* label) {
    if (condition) return;
    ++failures;
    std::cerr << "[失败] " << label << '\n';
}
void* allocate(std::size_t size, void*) {
    void* result = std::malloc(size);
    if (result) ++allocations;
    return result;
}
void deallocate(void* pointer, void*) {
    if (pointer) { --allocations; std::free(pointer); }
}
} // namespace

// 链接期替代两个后端，只操作实际ImGui context内的自有内存。
// 不链接使用真实Win32/DX11后端对象，不创建窗口、设备或提交GPU工作。
bool ImGui_ImplWin32_Init(void*) {
    ImGui::GetIO().BackendPlatformUserData = ImGui::MemAlloc(1);
    return true;
}
bool ImGui_ImplDX11_Init(ID3D11Device*, ID3D11DeviceContext*) {
    ImGui::GetIO().BackendRendererUserData = ImGui::MemAlloc(1);
    return true;
}
void ImGui_ImplWin32_Shutdown() {
    check(ImGui::GetIO().BackendPlatformUserData != nullptr, "不得清理未创建的平台后端");
    ImGui::MemFree(ImGui::GetIO().BackendPlatformUserData);
    ImGui::GetIO().BackendPlatformUserData = nullptr;
    ++platform_stops;
    cleanup_order += 'P';
}
void ImGui_ImplDX11_Shutdown() {
    check(ImGui::GetIO().BackendRendererUserData != nullptr, "不得清理未创建的渲染后端");
    ImGui::MemFree(ImGui::GetIO().BackendRendererUserData);
    ImGui::GetIO().BackendRendererUserData = nullptr;
    ++renderer_stops;
    cleanup_order += 'R';
}

namespace {
void cleanup_fixture(ImGuiContext*& context) {
    if (!context) return;
    ImGui::SetCurrentContext(context);
    auto& io = ImGui::GetIO();
    if (io.BackendRendererUserData) ImGui_ImplDX11_Shutdown();
    if (io.BackendPlatformUserData) ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext(context);
    context = nullptr;
}

void run_stage(int stage, bool initialized) {
    const int baseline = allocations;
    ImGuiContext* owned = ImGui::CreateContext();
    ImGui::SetCurrentContext(owned);
    if (stage >= 1) ImGui_ImplWin32_Init(nullptr);
    if (stage >= 2) ImGui_ImplDX11_Init(nullptr, nullptr);
    platform_stops = renderer_stops = 0;
    cleanup_order.clear();
    overlay::detail::shutdown_imgui_context(owned);
    check(owned == nullptr && ImGui::GetCurrentContext() == nullptr,
          "任意部分初始化状态都必须销毁自有context");
    check(allocations == baseline, "退出后实际ImGui内存分配必须回到基线");
    check(platform_stops == (stage >= 1 ? 1 : 0) &&
          renderer_stops == (stage >= 2 ? 1 : 0), "只清理已创建的后端且恰好一次");
    check(cleanup_order == (stage == 2 ? "RP" : stage == 1 ? "P" : ""),
          "后端必须按渲染器、平台的逆序释放");
    const auto completed_order = cleanup_order;
    overlay::detail::shutdown_imgui_context(owned);
    check(cleanup_order == completed_order && allocations == baseline,
          "重复shutdown不能再次释放后端或context");
    std::cout << "stage=" << stage << " initialized=" << initialized
              << " remaining_allocations=" << allocations - baseline << '\n';
    // 红版遗留只由夹具在断言之后清理，不掩盖原始失败。
    cleanup_fixture(owned);
    check(allocations == baseline, "夹具不能残留实际ImGui分配");
}

void test_foreign_context_and_recovery() {
    const int baseline = allocations;
    ImGuiContext* foreign = ImGui::CreateContext();
    ImGui_ImplWin32_Init(nullptr);
    void* foreign_backend = ImGui::GetIO().BackendPlatformUserData;
    ImGuiContext* owned = ImGui::CreateContext();
    ImGui::SetCurrentContext(owned);
    ImGui_ImplWin32_Init(nullptr);
    ImGui::SetCurrentContext(foreign);
    overlay::detail::shutdown_imgui_context(owned);
    check(owned == nullptr && ImGui::GetCurrentContext() == foreign &&
          ImGui::GetIO().BackendPlatformUserData == foreign_backend,
          "只销毁自有context并恢复原先的其他context");
    const int live_foreign = allocations;
    overlay::detail::shutdown_imgui_context(owned);
    check(allocations == live_foreign && ImGui::GetCurrentContext() == foreign,
          "空所有权shutdown不能触碰其他context");
    // 部分失败清理后，新context可完整建立并再次清理。
    owned = ImGui::CreateContext();
    ImGui::SetCurrentContext(owned);
    ImGui_ImplWin32_Init(nullptr);
    ImGui_ImplDX11_Init(nullptr, nullptr);
    overlay::detail::shutdown_imgui_context(owned);
    check(owned == nullptr && allocations == live_foreign,
          "失败清理后完整重建必须没有累计分配");
    cleanup_fixture(foreign);
    check(allocations == baseline, "外部context由其自身所有者清理");
    std::cout << "foreign/recovery remaining_allocations=" << allocations - baseline << '\n';
}
} // namespace

int main() {
    ImGui::SetAllocatorFunctions(allocate, deallocate);
    run_stage(0, false);
    run_stage(1, false);
    run_stage(2, false);
    run_stage(2, true);
    test_foreign_context_and_recovery();
    check(allocations == 0, "全部测试退出后零未释放分配");
    std::cout << "ImGui生命周期失败数: " << failures << '\n';
    return failures == 0 ? 0 : 1;
}
