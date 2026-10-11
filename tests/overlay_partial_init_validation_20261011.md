# Overlay部分初始化清理

原shutdown仅在整体initialized为true时释放ImGui。context已创建、平台后端已创建而后续初始化失败时，Overlay析构仍跳过这些资源。先把原清理条件移入私有函数，再用实际ImGui核心及内存后端验证：三种未完成状态留下9/10/11次未配对分配，共10条断言失败；其中后端分配是替身，不是实测Win32/DX资源数量。完整成功控制例原本通过。

Overlay现在保存并显式选择自有context，shutdown按该context实际存在的renderer/platform资源逆序释放，再销毁context、清空所有权。重复shutdown不重复释放，不触碰其他context；没有改变UI导航、配置、Provider、设备输出或数据审核行为。内联私有辅助函数由生产与测试共用，无新公有接口。

无窗口回归覆盖context-only、platform-only、两个后端部分状态、完整成功、重复关闭、其他context保护及清理后重建；实际ImGui allocator检查分配回到基线。首次扩展夹具遗漏CreateContext后的显式切换，失败记录保留并纠正，不计新生产缺陷。实际Overlay.init、窗口创建、DX11/GPU、DPI和字体失败时序仍未动态运行，不把此回归当作整窗验收。

核对本地固定ImGui 1.92.9b与上游同版本：[Win32初始化/清理](https://github.com/ocornut/imgui/blob/v1.92.9b/backends/imgui_impl_win32.cpp)、[DX11初始化/清理](https://github.com/ocornut/imgui/blob/v1.92.9b/backends/imgui_impl_dx11.cpp)。两者的BackendUserData代表已建立资源，未建立时直接Shutdown会触发断言，因此按实际存在状态清理；[CreateContext/DestroyContext](https://github.com/ocornut/imgui/blob/v1.92.9b/imgui.cpp)会保留或恢复之前context，所有权必须显式处理。沿用已有MIT依赖，仅使用其API契约，不复制上游实现或升级依赖。
