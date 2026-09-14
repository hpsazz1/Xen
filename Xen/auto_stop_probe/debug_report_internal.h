#ifndef AUTO_STOP_DEBUG_REPORT_INTERNAL_H
#define AUTO_STOP_DEBUG_REPORT_INTERNAL_H
#include "probe_internal.h"

namespace auto_stop_probe_detail {
inline std::string render_counterpulse_debug_report(const Json& analysis) {
    std::string data;
    for (char c : analysis.dump()) {
        if (c == '<') data += "\\u003c";
        else if (c == '>') data += "\\u003e";
        else if (c == '&') data += "\\u0026";
        else data += c;
    }
    return std::string(R"HTML(<!doctype html><html lang="zh-CN"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>开火采样调试报告</title>
<style>body{font:16px system-ui,sans-serif;background:#101923;color:#e3edf5;margin:28px auto;max-width:1100px;padding:0 20px}h1{font-size:28px}h2{font-size:19px;margin-top:30px}.note{color:#a9bfd0;line-height:1.7}.panel{background:#192734;border:1px solid #314959;border-radius:12px;padding:20px;margin:16px 0}table{border-collapse:collapse;width:100%;font-size:14px}td,th{text-align:left;padding:10px;border-bottom:1px solid #314959}select,button{font:inherit;background:#203b4c;color:white;padding:8px;border:1px solid #66899c;border-radius:6px}pre{white-space:pre-wrap;overflow-wrap:anywhere;font-size:13px}canvas{width:100%;height:240px}.scroll{overflow:auto}.bad{color:#ffcb88}</style>
<h1>开火采样调试报告</h1><p class="note" id="source">输入估计模型 · 初始静止未经实测确认 · 采样次数不等于子弹数<br>“模型阈值内”只表示当前参数下的估计。游戏中是否停稳，以人工观察独立记录。</p>
<div class="panel"><strong id="status"></strong><p id="quality" class="bad"></p><span id="counts"></span></div>
<div class="panel"><label>查看开火尝试 <select id="attempt"></select></label><p id="detail"></p><canvas id="plot" width="1000" height="240"></canvas><p class="note">纵轴：估计速度 / 稳定阈值；横轴：相对按下时间（毫秒）。虚线为阈值 1。无效样本不绘制。</p><div id="stages" class="scroll"></div></div>
<h2>每次尝试</h2><div id="shots" class="panel scroll"></div>
<h2>本次模型和采样参数</h2><pre id="settings" class="panel"></pre>
<h2>本次动作参数</h2><pre id="plan" class="panel"></pre>
<h2>人工标记与参数约束</h2><pre id="labels" class="panel"></pre>
<h2>人工操作间隔（毫秒）</h2><div id="intervals" class="panel scroll"></div>
<h2>自动复测候选与不支持原因</h2><pre id="proposals" class="panel"></pre>
<details><summary>原始分析数据与来源</summary><pre id="raw"></pre></details>
<script id="data" type="application/json">)HTML") + data + R"HTML(</script><script>
const a=JSON.parse(document.getElementById('data').textContent),$=id=>document.getElementById(id);
const f=v=>typeof v==='number'?v.toFixed(3):'—';
const label=m=>!m||!m.valid?'数据无效':({WITHIN_MODEL_THRESHOLD:'模型阈值内',MICRO:'模型微动',RUNNING:'模型移动'})[m.classification]||'未知';
function table(id,heads,rows){const t=document.createElement('table'),h=t.createTHead().insertRow();heads.forEach(x=>{const c=document.createElement('th');c.textContent=x;h.append(c)});const b=t.createTBody();rows.forEach(row=>{const r=b.insertRow();row.forEach(x=>r.insertCell().textContent=x)});$(id).replaceChildren(t)}
const manual=a.source==='KMBOX_MONITOR';
$('source').append(document.createTextNode(manual?' 来源：KMBOX 人工输入接收域。':' 来源：命令 ACK 代理。'));
$('status').textContent=manual?`人工录制 ${a.recording_id||''} · ${a.recording?'录制中':'已停止'}`:a.execution_success===true?'动作执行完成':`动作未完整完成：${a.execution_failure||'状态未知'}`;
$('quality').textContent=(a.quality_issues||[]).length?'数据质量：'+a.quality_issues.join('；'):'已检查记录结构；仍需人工验证模型。';
$('counts').textContent=`记录 ${a.shot_count||0} 次按住，${a.first_sample_count||0} 个首发模型样本，${a.held_sample_count||0} 个持续按住模型样本。`;
$('settings').textContent=JSON.stringify(a.settings,null,2);$('plan').textContent=JSON.stringify(a.actual_plan,null,2);$('raw').textContent=JSON.stringify(a,null,2);
$('labels').textContent=a.calibration_envelope?JSON.stringify({ranges:a.human_labels,bounds:a.calibration_envelope},null,2):'尚未提供人工标记；模型评分不会自动成为合格标签。';
const metrics=a.operation_intervals?.metrics;
if(metrics)table('intervals',['操作','有效数','平均','波动','最小','最大'],Object.entries(metrics).map(([key,m])=>[m.label||key,m.count,f(m.mean_ms),f(m.stddev_ms),f(m.min_ms),f(m.max_ms)]));else $('intervals').textContent='本报告没有人工操作间隔。';
$('proposals').textContent=a.manual_plan_proposals?JSON.stringify(a.manual_plan_proposals,null,2):'请从人工录制分析生成候选。';
const shots=a.shots||[];
shots.forEach((s,i)=>{const o=document.createElement('option');o.value=i;o.textContent=`第 ${s.ordinal} 次`; $('attempt').append(o)});
table('shots',['尝试','按住实测/计划 ms','相邻按下间隔 ms','按下时模型','首发采样模型'],shots.map(s=>[s.ordinal,`${f(s.observed_hold_ms)} / ${f(s.planned_hold_ms)}`,f(s.previous_down_submit_interval_ms),label(s.down_model),label(s.samples?.[0])]));
function draw(){const s=shots[Number($('attempt').value)];if(!s)return;$('detail').textContent=`第 ${s.ordinal} 次：按下时 ${label(s.down_model)}；首发采样 ${label(s.samples?.[0])}。`;
table('stages',['阶段','ACK 间隔 ms','计划等待 ms'],(s.stages||[]).map(x=>[({move_hold:'移动按住',counter_hold:'反向按住',counter_gap:'反向前间隔',shot_after_release:'释放后到开火'})[x.name]||x.name,f(x.observed_ms),f(x.planned_ms)]));
const downTime=s.down_ack_ns??s.down_received_ns;
const ctx=$('plot').getContext('2d'),points=[s.down_model,...(s.samples||[])].filter(m=>m?.valid&&typeof m.speed_ratio==='number');ctx.clearRect(0,0,1000,240);const maxX=Math.max(1,...points.map(p=>(p.time_ns-downTime)/1e6)),maxY=Math.max(1.5,...points.map(p=>p.speed_ratio))*1.1;const x=t=>50+t/maxX*900,y=v=>205-v/maxY*180;
ctx.font='14px sans-serif';ctx.fillStyle='#a9bfd0';ctx.fillText('0 ms',40,230);ctx.fillText(`${maxX.toFixed(1)} ms`,890,230);ctx.fillText('1',15,y(1)+5);ctx.strokeStyle='#f3c67a';ctx.setLineDash([6,5]);ctx.beginPath();ctx.moveTo(45,y(1));ctx.lineTo(960,y(1));ctx.stroke();ctx.setLineDash([]);ctx.strokeStyle='#61c6db';ctx.beginPath();points.forEach((p,i)=>{const px=x((p.time_ns-downTime)/1e6),py=y(p.speed_ratio);i?ctx.lineTo(px,py):ctx.moveTo(px,py)});ctx.stroke();points.forEach(p=>{ctx.fillStyle=p.speed_ratio<=1?'#7dddaf':'#ffcb88';ctx.beginPath();ctx.arc(x((p.time_ns-downTime)/1e6),y(p.speed_ratio),4,0,Math.PI*2);ctx.fill()})}
$('attempt').onchange=draw;draw();
</script></html>)HTML";
}
}
#endif
