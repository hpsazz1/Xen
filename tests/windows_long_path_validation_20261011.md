# Windows 长路径离线回归

本轮基线为 `3912bf3d4b966f4117aa9c8f1dd00bb7e12474bd`，在隔离分支继续修复五项原登记深路径失败。

- Producer 和 Sequence 共用文件 API 路径转换，Producer 将配置路径和 CLI 输出明确编码为 UTF-8。
- 三个 Prepare 同步启动冻结的 Sequence 副本；共享模块保留真实退出码、无窗口运行、超时和归属清理。
- Physical B 发布检查按命令启用 Git 内置长路径支持，并将新模块纳入固定哈希清单。
- 新回归实际通过 346 字符的中文、空格和非 BMP 路径，以及超过 MAX_PATH 的冻结 native EXE。
  现有目标、他人的 incoming、同级数据、junction 拒绝和失败清理均继续检查。

在 `cfdf28c` 检查点，原 `test_root_owner_cli_tests`、A2 S1、B holdout 和 B command-magnitude Prepare 四项已恢复。
`runtime_component_producer_tests` 仍失败：同目录扩展 TLogLocation、停用一次性增量跟踪和晚期
扩展对象输出路径三个候选均未闭合原构建根。最后一轮明确捕获 NVCC 的 GetLongPathName 长度拒绝，
CL 的扩展 `/Fo`/`/Fd` 也仍失败。实验补丁已撤回；原测试、根、参数和编译器检测不变。

后续的完整根工程 Ninja Multi-Config 同根对照也在 C++ 探测深 PDB 失败，不再重试路径补丁。
运行库规则现提取为真实工程和元数据夹具共用的 ORT 解析与 metadata 模块；原四组 owned 目录
及白名单、必需文件、CRT、identity/layout 断言保留。夹具显式使用 Ninja Multi-Config /
`LANGUAGES NONE`，调用根配置解析的真实 linker 和官方 CRT 目录；新增真实 TensorRT ABI
不匹配拒绝及更深 Unicode 输出逐字节一致性。它不设置 `CMAKE_*_COMPILER_WORKS` 或伪造
`MSVC` 变量，不创建可执行产物，不在合成 SDK 上运行部署目标。

根工程继续原 `project(... LANGUAGES CXX)`、`enable_language(CUDA)` 和真实编译器检查。
本次解决的是元数据生产的无关编译器耦合，未消除工具链深路径编译边界；旧完整根调用失败
不可被新的元数据专项通过覆盖。Ninja 缺失使元数据测试失败，不阻断根工程配置。

这不是全量当前 HEAD 通过，也不代表真实设备、原生窗口或暂停的候选采用入口已验证。
最终提交、逐测试命令/结果、产物哈希、远端和 CI 见本轮覆盖报告；此前 SHA 的绿色仅属于其原始检查点。
