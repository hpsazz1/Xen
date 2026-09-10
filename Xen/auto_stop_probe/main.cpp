#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include "config/config.h"
#include "auto_stop_probe/probe_internal.h"
#include "auto_stop_probe/session_internal.h"
#include "auto_stop_probe/mask_internal.h"

int main(int argc, char** argv) {
    using namespace auto_stop_probe_detail;
    std::ofstream output;
    std::filesystem::path session_status_path;
    try {
        std::string config_path, plan_path, output_path, confirmation;
        std::string session_directory;
        int session_seconds = 0;
        bool allowed = false, dry_run = false;
        bool mask_check = false;
        int monitor_ready_timeout_ms = 0;
        std::set<std::string> seen;
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            if (!seen.insert(option).second) throw std::runtime_error("参数重复");
            if (option == "--dry-run") dry_run = true;
            else if (option == "--mask-check") mask_check = true;
            else if (option == "--allow-physical-output") allowed = true;
            else if (i + 1 < argc && option == "--config") config_path = argv[++i];
            else if (i + 1 < argc && option == "--plan") plan_path = argv[++i];
            else if (i + 1 < argc && option == "--output") output_path = argv[++i];
            else if (i + 1 < argc && option == "--confirm") confirmation = argv[++i];
            else if (i + 1 < argc && option == "--monitor-ready-timeout-ms") monitor_ready_timeout_ms = parse_monitor_ready_timeout(argv[++i]);
            else if (i + 1 < argc && option == "--session-dir") session_directory = argv[++i];
            else if (i + 1 < argc && option == "--session-seconds") session_seconds = parse_session_seconds(argv[++i]);
            else throw std::runtime_error("未知参数或缺少参数值");
        }
        const bool session_mode = !session_directory.empty() || session_seconds != 0;
        if (mask_check && (session_mode || dry_run || !plan_path.empty() || output_path.empty()))
            throw std::runtime_error("mask-check仅接受config/output，与session、plan、dry-run互斥");
        if (session_mode && (session_directory.empty() || session_seconds == 0 || !plan_path.empty() || !output_path.empty() || dry_run))
            throw std::runtime_error("会话须同时指定目录和时长，不能同时指定计划、输出或dry-run");
        if (!session_mode && !mask_check && (plan_path.empty() || output_path.empty())) throw std::runtime_error("必须指定--plan和--output");
        // 不覆盖原始配置、计划或既有证据。
        std::vector<Step> plan;
        if (session_mode) {
            std::filesystem::create_directories(session_directory);
            if (std::filesystem::exists(std::filesystem::path(session_directory) / "session.status.json"))
                throw std::runtime_error("会话目录已有状态证据");
            session_status_path = std::filesystem::path(session_directory) / "session.status.json";
        } else {
            if (std::filesystem::exists(output_path)) throw std::runtime_error("输出文件已存在");
            if (!mask_check) {
                std::ifstream source(plan_path);
                if (!source) throw std::runtime_error("无法读取计划");
                plan = parse_plan(Json::parse(source));
            }
        }
        if (!dry_run && (!allowed || confirmation != "AUTO_STOP_WASD_PHYSICAL" || config_path.empty()))
            throw std::runtime_error("真实运行需要配置、--allow-physical-output及--confirm AUTO_STOP_WASD_PHYSICAL");
        // 在连接设备前检查报告路径可写，减少动作完成后无法保存证据的情况。
        if (!session_mode) {
            output.open(output_path, std::ios::out | std::ios::trunc);
            if (!output) throw std::runtime_error("无法创建输出文件");
        }
        Json report;
        if (dry_run) {
            report = {{"schema_version", 1}, {"dry_run", true}, {"success", true}, {"step_count", plan.size()},
                {"monitor_ready_timeout_ms", monitor_ready_timeout_ms}};
        } else {
            AppConfig config;
            std::string error;
            if (!load_app_config(config_path, config, error)) throw std::runtime_error("配置加载失败，详情不回显以保护设备凭据");
            if (config.mouse.backend != MouseBackend::KMBOX_NET) throw std::runtime_error("仅支持KMBOX NET");
            if (config.mouse.kmbox_connect_timeout_ms > 2000 || config.mouse.kmbox_command_timeout_ms > 300)
                throw std::runtime_error("连接超时不得超过2000ms，命令超时不得超过300ms");
            config.mouse.allow_send_input = true;
            auto mouse = MouseDeviceFactory::create(config.mouse);
            if (!mouse || !mouse->open()) {
                report = {{"schema_version", 1}, {"success", false}, {"failure", "OPEN_FAILED"}};
            } else if (!mouse->output_owner_exclusive() || !mouse->supports_wasd_keyboard()) {
                mouse->close();
                report = {{"schema_version", 1}, {"success", false}, {"failure", "OWNER_OR_CAPABILITY_MISSING"}};
            } else if (session_mode) report = execute_session(*mouse, session_directory, session_seconds, monitor_ready_timeout_ms);
            else if (mask_check) report = execute_mask_check(*mouse);
            else report = execute(*mouse, plan, monitor_ready_timeout_ms);
        }
        if (session_mode) {
            if (!report.contains("state")) {
                report["state"] = "error";
                write_session_json(std::filesystem::path(session_directory) / "session.status.json", report, false);
            }
        } else {
            output << report.dump(2) << '\n';
            output.flush();
            if (!output) throw std::runtime_error("报告写入失败");
        }
        std::cout << (report.value("success", false) ? "完成，ACK不代表人物停稳。\n" : "实验失败，详见报告。\n");
        return report.value("success", false) ? 0 : 2;
    } catch (const std::exception&) {
        // JSON解析异常可能包含文件内容，禁止把原异常文本写入日志。
        if (output.is_open() && output.good()) {
            output << "{\"schema_version\":1,\"success\":false,\"failure\":\"VALIDATION_OR_EXECUTION_EXCEPTION\"}\n";
        }
        if (!session_status_path.empty()) {
            try {
                if (!std::filesystem::exists(session_status_path)) write_session_json(session_status_path,
                    {{"schema_version", 1}, {"state", "error"}, {"success", false}, {"reason", "VALIDATION_OR_EXECUTION_EXCEPTION"}}, false);
            } catch (...) {}
        }
        std::cerr << "参数、计划、配置或输出错误；真实运行需显式授权，报告路径须不存在。\n";
        return 1;
    }
}
