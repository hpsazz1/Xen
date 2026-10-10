# U01 ImGui 1.92.9b 热修复

基线657e42d3c701c690fefcfaf78ae39e4e8b736a8b，分支fix/imgui-hotfix-20261011。旧声明及实际编译头文件均为1.92.9/19290。采用[官方1.92.9b hotfix](https://github.com/ocornut/imgui/releases/tag/v1.92.9b)，tag提交f1cc2ae15e53a861a874c3034aae6798fde194ab，头文件1.92.9b/19291，保留MIT许可证；仅独立任务依赖目录切换，原共享依赖不变。

实际用途：input_training_panel.cpp的原始事件时间轴使用允许文字输入的SliderInt。旧库在真实ImGui控件事件序列中复现：Ctrl+点击→输入77→Enter→普通点击时短暂重新进入文字输入；ImDrawData::CmdListsCount也与实际CmdLists.Size不一致。后一问题还使既有面板测试的顶点循环跳过实际列表。

无窗口专项7断言：两红例已转绿，Enter提交、范围、再次编辑、Escape取消等控制保持。四项相关CTest全部通过，共0.08秒：imgui_hotfix_tests、input_training_panel_tests、debug_panel_tests、overlay_tests。没有平台/渲染后端初始化、原生窗口、桌面截图、系统输入或设备操作；面板动作只写夹具数据，没有调用Session执行。

实际CMake另复现SOURCE_DIR缓存覆盖问题：声明新tag但指向旧头文件时原配置仍成功。现配置读取实际头文件版本并严格匹配所需版本，旧覆盖被拒绝，正确独立1.92.9b路径恢复配置成功。不会把更改tag当成已实际升级。

公共文档19文件195链接0错误。正式Release、身份、远端与CI查询见本地docs/evidence/XEN-IMGUI-HOTFIX-20261011-001。当前禁止原生UI/桌面操作，所以实显/截图验收未执行；这些测试只证明无窗口控件与面板合同。Provider、输出权限、UNKNOWN和审核边界保持。
