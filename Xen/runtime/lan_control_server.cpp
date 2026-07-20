#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_
#include <winsock2.h>
#include <Windows.h>

#include "runtime/lan_control_server.h"

#include "Xen.h"
#include "capture.h"
#include "runtime/startup_helpers.h"
#include "runtime/application_shutdown.h"
#include "modules/httplib.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>

namespace
{
constexpr const char* kConsoleHtml = R"HTML(<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Xen 局域网控制台</title>
<style>
:root{color-scheme:light;--surface:#fff;--surface2:#f4f6f8;--line:#dfe3e8;--ink:#1a1c1f;--muted:#60656d;--accent:#339cff;--ok:#00a240;--warn:#a85b00;--danger:#ba2623}*{box-sizing:border-box}body{margin:0;background:var(--surface2);color:var(--ink);font:14px/1.45 system-ui,-apple-system,"Segoe UI","Microsoft YaHei",sans-serif}button,input{font:inherit}button{border:0;border-radius:6px;padding:9px 14px;cursor:pointer;background:var(--accent);color:#fff;font-weight:600}button.secondary{background:#e8edf2;color:var(--ink)}button.danger{background:var(--danger)}button:disabled{opacity:.5;cursor:not-allowed}.shell{max-width:1060px;margin:auto;padding:18px}.top{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:16px}.brand{font-size:20px;font-weight:700}.status{display:flex;align-items:center;gap:7px;color:var(--muted)}.dot{width:9px;height:9px;border-radius:50%;background:var(--ok)}.grid{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:10px;margin-bottom:14px}.stat,.panel{background:var(--surface);border:1px solid var(--line);border-radius:7px}.stat{padding:13px}.stat small{display:block;color:var(--muted);margin-bottom:3px}.stat strong{font-size:18px}.layout{display:grid;grid-template-columns:1.15fr .85fr;gap:14px}.panel{padding:16px}.panel h2{font-size:15px;margin:0 0 12px}.actions{display:flex;flex-wrap:wrap;gap:8px}.form{display:grid;grid-template-columns:1fr 1fr;gap:10px 12px}.field label{display:block;color:var(--muted);font-size:12px;margin-bottom:4px}.field input{width:100%;padding:8px 9px;border:1px solid var(--line);border-radius:5px;background:#fff;color:var(--ink)}.field.full{grid-column:1/-1}.foot{margin-top:12px;color:var(--muted);font-size:12px}.toast{position:fixed;right:16px;bottom:16px;padding:10px 13px;background:var(--ink);color:#fff;border-radius:6px;display:none}.login{max-width:390px;margin:12vh auto;padding:22px;background:var(--surface);border:1px solid var(--line);border-radius:8px}.login h1{font-size:20px;margin:0 0 5px}.login p{color:var(--muted);margin:0 0 18px}.login input{width:100%;padding:10px;border:1px solid var(--line);border-radius:5px;margin-bottom:10px}.hidden{display:none}@media(max-width:760px){.shell{padding:12px}.grid{grid-template-columns:repeat(2,minmax(0,1fr))}.layout{grid-template-columns:1fr}.top{align-items:flex-start;flex-direction:column}.form{grid-template-columns:1fr}}
</style></head><body>
<main id="login" class="login"><h1>Xen 局域网控制台</h1><p>输入主程序控制台显示的配对码。</p><input id="pairing" inputmode="numeric" maxlength="6" placeholder="六位配对码"><button onclick="pair()">配对</button></main>
<main id="app" class="shell hidden"><header class="top"><div class="brand">Xen 控制台</div><div class="status"><i id="dot" class="dot"></i><span id="statusText">连接中</span><button class="secondary" onclick="logout()">退出</button></div></header>
<section class="grid"><div class="stat"><small>捕获帧率</small><strong id="captureFps">-</strong></div><div class="stat"><small>检测发布</small><strong id="detectFps">-</strong></div><div class="stat"><small>输入设备</small><strong id="inputMethod">-</strong></div><div class="stat"><small>后端</small><strong id="backend">-</strong></div></section>
<section class="layout"><div class="panel"><h2>运行控制</h2><div class="actions"><button onclick="control('pause')">暂停</button><button onclick="control('resume')">恢复</button><button class="secondary" onclick="control('reload')">重载配置</button></div><p class="foot">当前状态：<b id="paused">-</b>　瞄准：<b id="aiming">-</b></p></div>
<div class="panel"><h2>模型与捕获</h2><p class="foot">模型：<b id="model">-</b></p><p class="foot">分辨率：<b id="resolution">-</b>　配置帧率：<b id="configFps">-</b></p></div>
<div class="panel"><h2>运行参数</h2><form class="form" onsubmit="saveConfig(event)"><div class="field"><label>置信度阈值</label><input name="confidence_threshold" type="number" step="0.01" min="0.01" max="1"></div><div class="field"><label>NMS 阈值</label><input name="nms_threshold" type="number" step="0.01" min="0" max="1"></div><div class="field"><label>响应时间（毫秒）</label><input name="move_response_ms" type="number" step="0.1" min="1" max="250"></div><div class="field"><label>最大速度（counts/s）</label><input name="move_max_speed_cps" type="number" step="1" min="100" max="10000"></div><div class="field"><label>预测提前（毫秒）</label><input name="prediction_lead_ms" type="number" step="0.1" min="0" max="300"></div><div class="field"><label>捕获帧率</label><input name="capture_fps" type="number" step="1" min="0" max="500"></div><div class="field"><label>自动瞄准</label><input name="auto_aim" type="checkbox"></div><div class="field"><label>预测启用</label><input name="prediction_enabled" type="checkbox"></div><div class="field full"><button type="submit">保存参数</button></div></form></div></section><div id="toast" class="toast"></div></main>
<script>
let session=localStorage.getItem('xenSession')||'';const $=id=>document.getElementById(id);const fields=['confidence_threshold','nms_threshold','move_response_ms','move_max_speed_cps','prediction_lead_ms','capture_fps','auto_aim','prediction_enabled'];
async function req(path,opt={}){opt.headers={...(opt.headers||{}),'X-Xen-Session':session};const r=await fetch(path,opt);if(r.status===401){logout();throw Error('未授权')}if(!r.ok)throw Error(await r.text());return r.json()}
async function pair(){try{const body=new URLSearchParams({pairing:$('pairing').value.trim()});const r=await fetch('/api/auth',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});if(!r.ok)throw Error('配对码错误');const d=await r.json();session=d.session;localStorage.setItem('xenSession',session);showApp();refresh()}catch(e){toast(e.message)}}
function showApp(){$('login').classList.add('hidden');$('app').classList.remove('hidden')};function logout(){localStorage.removeItem('xenSession');session='';$('app').classList.add('hidden');$('login').classList.remove('hidden')}
async function control(action){try{await req('/api/control',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({action})});toast('操作已提交');setTimeout(refresh,250)}catch(e){toast(e.message)}}
async function saveConfig(e){e.preventDefault();try{const f=e.target;const data={};fields.forEach(k=>{const el=f.elements[k];data[k]=el.type==='checkbox'?String(el.checked):el.value});await req('/api/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(data)});toast('参数已保存');refresh()}catch(e){toast(e.message)}}
async function refresh(){try{const d=await req('/api/status');$('statusText').textContent=d.paused?'已暂停':'运行中';$('dot').style.background=d.paused?'#a85b00':'#00a240';$('captureFps').textContent=d.capture_fps+' FPS';$('detectFps').textContent=d.detection_fps+' FPS';$('inputMethod').textContent=d.input_method;$('backend').textContent=d.backend;$('model').textContent=d.model;$('resolution').textContent=d.resolution+' px';$('configFps').textContent=d.capture_fps_limit+' FPS';$('paused').textContent=d.paused?'是':'否';$('aiming').textContent=d.aiming?'是':'否';fields.forEach(k=>{const el=document.querySelector('[name="'+k+'"]');if(el){if(el.type==='checkbox')el.checked=!!d[k];else if(document.activeElement!==el)el.value=d[k]}})}catch(e){$('statusText').textContent='连接失败';$('dot').style.background='#ba2623'}}
function toast(t){$('toast').textContent=t;$('toast').style.display='block';setTimeout(()=>$('toast').style.display='none',2200)}
if(session){showApp();refresh()}setInterval(()=>{if(session)refresh()},1200);
</script></body></html>)HTML";

constexpr const char* kClassPanelHtml = R"HTML(<div class="panel"><h2>模型类别映射</h2><form class="form" onsubmit="saveClasses(event)"><div class="field"><label>身体类别 ID</label><select id="class_player" name="class_player"><option value="0">0 · 警身</option><option value="1">1 · 警头</option><option value="2">2 · 匪身</option><option value="3">3 · 匪头</option><option value="4">4</option><option value="5">5</option><option value="6">6</option><option value="7">7</option></select></div><div class="field"><label>头部类别 ID</label><select id="class_head" name="class_head"><option value="0">0 · 警身</option><option value="1">1 · 警头</option><option value="2">2 · 匪身</option><option value="3">3 · 匪头</option><option value="4">4</option><option value="5">5</option><option value="6">6</option><option value="7">7</option></select></div><div class="field full"><button type="submit">保存类别映射</button></div></form><p class="foot">当前配置用于身体/头部目标类别识别。</p></div>)HTML";

constexpr const char* kClassPanelScript = R"JS(<script>
async function saveClasses(e){e.preventDefault();try{const f=e.target;await req('/api/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({class_player:f.elements.class_player.value,class_head:f.elements.class_head.value})});toast('类别映射已保存');refresh()}catch(e){toast(e.message)}}
</script>)JS";

std::string RenderConsoleHtml()
{
    std::string page(kConsoleHtml);
    const std::string panelMarker = "<div id=\"toast\" class=\"toast\"></div>";
    const auto panelPosition = page.find(panelMarker);
    if (panelPosition != std::string::npos)
        page.insert(panelPosition, std::string(kClassPanelHtml) + kClassPanelScript);
    const std::string refreshMarker = "$('aiming').textContent=d.aiming?'是':'否';";
    const auto refreshPosition = page.find(refreshMarker);
    if (refreshPosition != std::string::npos)
        page.insert(refreshPosition + refreshMarker.size(), "$('class_player').value=d.class_player;$('class_head').value=d.class_head;");
    return page;
}

std::string JsonEscape(const std::string& value)
{
    std::string result;
    for (const char ch : value)
    {
        if (ch == '\\' || ch == '"') result += '\\';
        result += ch;
    }
    return result;
}

std::string MakeToken(std::mt19937_64& generator, int digits)
{
    std::uniform_int_distribution<unsigned long long> distribution(0, digits == 6 ? 999999ULL : 0xffffffffffffffffULL);
    std::ostringstream out;
    if (digits == 6) out << std::setw(6) << std::setfill('0') << distribution(generator);
    else out << std::hex << std::setw(16) << std::setfill('0') << distribution(generator);
    return out.str();
}

bool ParseFloat(const httplib::Request& request, const char* name, float min, float max, float& output, std::string& error)
{
    if (!request.has_param(name)) return true;
    try
    {
        const float value = std::stof(request.get_param_value(name));
        if (!std::isfinite(value) || value < min || value > max) throw std::out_of_range("range");
        output = value;
        return true;
    }
    catch (...)
    {
        error = std::string(name) + " 超出允许范围";
        return false;
    }
}

bool ParseBool(const httplib::Request& request, const char* name, bool& output, std::string& error)
{
    if (!request.has_param(name)) return true;
    const std::string value = request.get_param_value(name);
    if (value == "true" || value == "1") output = true;
    else if (value == "false" || value == "0") output = false;
    else { error = std::string(name) + " 不是有效布尔值"; return false; }
    return true;
}
}

struct LanControlServer::Impl
{
    httplib::Server server;
    std::thread thread;
    std::string pairingCode;
    std::string sessionToken;
    std::atomic<bool> running{ false };
    std::string bindAddress;
    int port = 0;
};

LanControlServer::LanControlServer() : impl_(std::make_unique<Impl>()) {}

LanControlServer::~LanControlServer()
{
    Stop();
}

bool LanControlServer::Start(const std::string& bindAddress, int port)
{
    if (impl_->running.load()) return true;
    std::random_device randomDevice;
    std::mt19937_64 generator(randomDevice());
    impl_->pairingCode = MakeToken(generator, 6);
    impl_->sessionToken = MakeToken(generator, 16) + MakeToken(generator, 16);
    impl_->bindAddress = bindAddress;
    impl_->port = port;

    impl_->server.Get("/", [](const httplib::Request&, httplib::Response& response) {
        response.set_content(RenderConsoleHtml(), "text/html; charset=UTF-8");
    });
    impl_->server.Post("/api/auth", [this](const httplib::Request& request, httplib::Response& response) {
        if (!request.has_param("pairing") || request.get_param_value("pairing") != impl_->pairingCode)
        {
            response.status = 401;
            response.set_content("{\"error\":\"配对码错误\"}", "application/json; charset=UTF-8");
            return;
        }
        response.set_content("{\"session\":\"" + impl_->sessionToken + "\"}", "application/json; charset=UTF-8");
    });

    auto authenticated = [this](const httplib::Request& request) {
        return request.has_header("X-Xen-Session") && request.get_header_value("X-Xen-Session") == impl_->sessionToken;
    };
    impl_->server.Get("/api/status", [authenticated](const httplib::Request& request, httplib::Response& response) {
        if (!authenticated(request)) { response.status = 401; return; }
        std::lock_guard<std::mutex> lock(configMutex);
        std::ostringstream json;
        json << std::boolalpha
            << "{\"paused\":" << detectionPaused.load()
            << ",\"aiming\":" << aiming.load()
            << ",\"capture_fps\":" << captureFps.load()
            << ",\"detection_fps\":" << detectionBuffer.getPublishFps()
            << ",\"capture_fps_limit\":" << config.capture_fps
            << ",\"resolution\":" << config.detection_resolution
            << ",\"confidence_threshold\":" << config.confidence_threshold
            << ",\"nms_threshold\":" << config.nms_threshold
            << ",\"move_response_ms\":" << config.move_response_ms
            << ",\"move_max_speed_cps\":" << config.move_max_speed_cps
            << ",\"prediction_lead_ms\":" << config.prediction_lead_ms
            << ",\"class_player\":" << config.class_player
            << ",\"class_head\":" << config.class_head
            << ",\"auto_aim\":" << config.auto_aim
            << ",\"prediction_enabled\":" << config.prediction_enabled
            << ",\"input_method\":\"" << JsonEscape(config.input_method)
            << "\",\"backend\":\"" << JsonEscape(config.backend)
            << "\",\"model\":\"" << JsonEscape(config.ai_model) << "\"}";
        response.set_content(json.str(), "application/json; charset=UTF-8");
    });
    impl_->server.Post("/api/control", [authenticated](const httplib::Request& request, httplib::Response& response) {
        if (!authenticated(request)) { response.status = 401; return; }
        if (!request.has_param("action")) { response.status = 400; response.set_content("{\"error\":\"缺少操作\"}", "application/json; charset=UTF-8"); return; }
        const auto action = request.get_param_value("action");
        if (action == "pause") detectionPaused.store(true);
        else if (action == "resume") detectionPaused.store(false);
        else if (action == "toggle") detectionPaused.store(!detectionPaused.load());
        else if (action == "reload") remoteReloadRequested.store(true);
        else { response.status = 400; response.set_content("{\"error\":\"不支持的操作\"}", "application/json; charset=UTF-8"); return; }
        response.set_content("{\"ok\":true}", "application/json; charset=UTF-8");
    });
    impl_->server.Post("/api/config", [authenticated](const httplib::Request& request, httplib::Response& response) {
        if (!authenticated(request)) { response.status = 401; return; }
        std::string error;
        std::lock_guard<std::mutex> lock(configMutex);
        float confidence = config.confidence_threshold, nms = config.nms_threshold;
        float responseMs = config.move_response_ms, maxSpeed = config.move_max_speed_cps, leadMs = config.prediction_lead_ms;
        int classPlayer = config.class_player, classHead = config.class_head;
        bool autoAim = config.auto_aim, prediction = config.prediction_enabled;
        int captureLimit = config.capture_fps;
        if (!ParseFloat(request, "confidence_threshold", 0.01f, 1.0f, confidence, error) ||
            !ParseFloat(request, "nms_threshold", 0.0f, 1.0f, nms, error) ||
            !ParseFloat(request, "move_response_ms", 1.0f, 250.0f, responseMs, error) ||
            !ParseFloat(request, "move_max_speed_cps", 100.0f, 10000.0f, maxSpeed, error) ||
            !ParseFloat(request, "prediction_lead_ms", 0.0f, 300.0f, leadMs, error) ||
            !ParseBool(request, "auto_aim", autoAim, error) ||
            !ParseBool(request, "prediction_enabled", prediction, error))
        { response.status = 400; response.set_content("{\"error\":\"" + JsonEscape(error) + "\"}", "application/json; charset=UTF-8"); return; }
        try
        {
            if (request.has_param("class_player")) classPlayer = std::clamp(std::stoi(request.get_param_value("class_player")), 0, 255);
            if (request.has_param("class_head")) classHead = std::clamp(std::stoi(request.get_param_value("class_head")), 0, 255);
        }
        catch (...) { response.status = 400; response.set_content("{\"error\":\"类别 ID 无效\"}", "application/json; charset=UTF-8"); return; }
        if (classPlayer == classHead) { response.status = 400; response.set_content("{\"error\":\"身体类别和头部类别不能相同\"}", "application/json; charset=UTF-8"); return; }
        if (request.has_param("capture_fps"))
        {
            try { captureLimit = std::clamp(std::stoi(request.get_param_value("capture_fps")), 0, 500); }
            catch (...) { response.status = 400; response.set_content("{\"error\":\"捕获帧率无效\"}", "application/json; charset=UTF-8"); return; }
        }
        const bool fpsChanged = captureLimit != config.capture_fps;
        config.confidence_threshold = confidence; config.nms_threshold = nms;
        config.move_response_ms = responseMs; config.move_max_speed_cps = maxSpeed;
        config.prediction_lead_ms = leadMs; config.auto_aim = autoAim;
        config.prediction_enabled = prediction; config.capture_fps = captureLimit;
        config.class_player = classPlayer; config.class_head = classHead;
        if (fpsChanged) capture_fps_changed.store(true);
        if (!config.saveConfig()) { response.status = 500; response.set_content("{\"error\":\"配置保存失败\"}", "application/json; charset=UTF-8"); return; }
        response.set_content("{\"ok\":true}", "application/json; charset=UTF-8");
    });

    impl_->thread = std::thread([this] {
        impl_->running.store(true);
        if (!impl_->server.listen(impl_->bindAddress.c_str(), impl_->port))
            WriteConsoleLine(ConsoleTone::Error, "[局域网控制台] 启动失败，端口可能已被占用。");
        impl_->running.store(false);
    });
    WriteConsoleLine(ConsoleTone::Accent, "[局域网控制台] http://" + bindAddress + ":" + std::to_string(port) + " 配对码：" + impl_->pairingCode);
    return true;
}

void LanControlServer::Stop() noexcept
{
    if (!impl_ || !impl_->thread.joinable()) return;
    impl_->server.stop();
    impl_->thread.join();
}

bool LanControlServer::IsRunning() const noexcept
{
    return impl_ && impl_->running.load();
}
