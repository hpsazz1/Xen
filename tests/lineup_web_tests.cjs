'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const { createController, previewValid, filterRecipes } = require('../Xen/lineup/web/app.js');
const state = (revision = 1) => ({ epoch: 'session-a', revision, mode: 'browse', running: true, source_status: 'live', locked_id: 'r1', recipes: [], preview: { status: 'valid', recipe_id: 'r1', frame_id: '42', url: '/api/preview?frame_id=42', age_ms: 30 } });
const deferred = () => { let resolve, reject; const promise = new Promise((a,b) => { resolve=a; reject=b; }); return {promise,resolve,reject}; };

test('F7设置保留具体动作时长、名称与道具，清除动作不会暗中恢复投掷预设', () => {
  const {hostProfileRequest,throwPreset}=require('../Xen/lineup/web/app.js');
  const form={map:'de_dust2',team:'CT',method:'jump',buttons:'left',direction:'forward',grenade:'烟雾弹',name:'A点',target:''};
  const action=throwPreset('jump','left');
  action.phases.forEach((phase,index)=>{phase.duration_ms=index ? 0 : 500;});
  const payload=hostProfileRequest(form,action);
  assert.deepEqual(payload.throw_action,action);
  assert.equal(payload.target,'当前瞄点'); assert.equal(payload.name,'A点');
  assert.equal(hostProfileRequest(form,null).throw_action,null);
  assert.throws(()=>hostProfileRequest({...form,grenade:''},action),/请选择道具/);
});

test('主机采集状态区分等待几何、忙态和错误，不把导入或F8描述成投掷成功', () => {
  const {hostCaptureSummary}=require('../Xen/lineup/web/app.js');
  assert.match(hostCaptureSummary({enabled:false}),/未启用/);
  assert.match(hostCaptureSummary({enabled:true,status:'waiting_geometry'}),/等待辅机/);
  assert.match(hostCaptureSummary({enabled:true,status:'waiting_idle'}),/等待当前/);
  assert.match(hostCaptureSummary({enabled:true,status:'imported',last_id:'a'}),/已导入并选中.*F8 只回准/);
  assert.match(hostCaptureSummary({enabled:true,status:'error',error:'尺寸不匹配'}),/尺寸不匹配/);
});

test('采集地图使用内部ID，保留当前地图与旧库自定义地图且不重复', () => {
  const {mapChoices}=require('../Xen/lineup/web/app.js');
  const options=mapChoices([{map:'de_mirage'},{map:'workshop/123/custom'}],'de_new_map','旧地图');
  assert.equal(options.filter(o=>o.id==='de_mirage').length,1);
  assert.match(options.find(o=>o.id==='de_mirage').text,/Mirage/);
  for(const id of ['de_dust2','de_cache','workshop/123/custom','de_new_map','旧地图']) assert.ok(options.some(o=>o.id===id));
  assert.ok(options.every(o=>o.id));
});

test('采集投掷方式保留力度方向与跳跃，预设无猜测时长且最终全释放', () => {
  const {throwPreset}=require('../Xen/lineup/web/app.js');
  for(const method of ['stationary','jump','step','step_jump','runup','runup_jump']) {
    for(const buttons of ['left','right','left+right']) {
      const action=throwPreset(method,buttons,'back+left');
      assert.equal(action.schema,1); assert.equal(action.type,'phases');
      assert.ok(action.phases.every(p=>p.duration_ms===null));
      assert.deepEqual(action.phases[0].buttons,buttons.split('+'));
      assert.deepEqual(action.phases.at(-1),{buttons:[],movement:[],jump:false,duration_ms:null});
      assert.equal(action.phases.some(p=>p.jump),method.includes('jump'));
      const moving=method.startsWith('step')||method.startsWith('runup');
      assert.equal(action.phases.some(p=>p.movement.length>0),moving);
      if(moving) assert.ok(action.phases.some(p=>p.movement.join('+')==='back+left'));
    }
  }
  assert.throws(()=>throwPreset('unknown','left','forward'));
  assert.throws(()=>throwPreset('stationary','middle','forward'));
  assert.throws(()=>throwPreset('runup','left','forward+back'));
});

test('采集请求一次携带明确地图阵营和动作设计，空项不静默采用默认阵营', () => {
  const {captureRequest}=require('../Xen/lineup/web/app.js');
  const form={map:'de_mirage',team:'CT',region:'A',standpoint_id:'',stance:'蹲姿',instructions:'背靠墙角',method:'runup_jump',buttons:'right',direction:'forward'};
  const request=captureRequest(form);
  assert.equal(request.map,'de_mirage'); assert.equal(request.team,'CT');
  assert.equal(request.stance,'蹲姿'); assert.equal(request.throw_action.movement_profile,'runup');
  assert.match(request.throw_instructions,/助跑跳投/); assert.match(request.throw_instructions,/右键/);
  assert.ok(!Object.hasOwn(request,'action'));
  for(const patch of [{map:''},{team:''},{team:'ANY'},{method:''}]) assert.throws(()=>captureRequest({...form,...patch}));
});
test('标记严格绑定锁定配方、帧龄、源及前台连接', () => {
  assert.equal(previewValid(state(),100,true),true);
  for (const patch of [{running:false},{mode:'capture'},{source_status:'disconnected'},{locked_id:'r2'},{preview:{...state().preview,age_ms:-1}},{preview:{...state().preview,age_ms:NaN}},{preview:{...state().preview,status:'unreliable'}}]) assert.equal(previewValid({...state(),...patch},0,true),false);
  assert.equal(previewValid(state(),1470,true),false);
  assert.equal(previewValid(state(),0,false),false);
});
test('多条件人工筛选且草稿不成为正式候选', () => {
  const rows = [{id:'a',map:'M',region:'R',target:'T',grenade:'烟雾'},{id:'b',map:'M',draft:true},{id:'c',map:'N'}];
  assert.deepEqual(filterRecipes(rows,{map:'M',region:'R',target:'T',grenade:'烟雾'}).map(x=>x.id),['a']);
  assert.equal(filterRecipes(rows,{map:'未知'}).length,0);
});
test('重复点击只发一个 POST；后续新操作使用新 revision 和请求编号', async () => {
  const write=deferred(), calls=[];
  const c=createController({request:async (url,body)=>{calls.push({url,body});return body?write.promise:state();}});
  await c.refresh(); const first=c.command('capture');
  assert.equal(await c.command('capture'),false); assert.equal(calls.filter(x=>x.body).length,1);
  write.resolve(state(2)); await first; await c.command('cancel');
  const posts=calls.filter(x=>x.body); assert.equal(posts[1].body.revision,2); assert.notEqual(posts[0].body.request_id,posts[1].body.request_id);
});
test('取消与切换命令携带当前 epoch，不自动更换锁定配方', async () => {
  const calls=[]; const c=createController({request:async (_,body)=>{calls.push(body);return body?{...state(2),locked_id:''}:state();}});
  await c.refresh(); await c.command('cancel'); assert.equal(calls[1].action,'cancel'); assert.equal(calls[1].epoch,'session-a'); assert.equal(c.model().state.locked_id,'');
});
test('断网失败后恢复只读状态，不重放 POST', async () => {
  const calls=[]; const c=createController({request:async (_,body)=>{calls.push(body);if(body)throw new Error('offline');return state();}});
  await c.refresh(); await assert.rejects(c.command('capture')); assert.equal(c.model().valid,false);
  await c.resume(); assert.equal(calls.filter(Boolean).length,1); assert.equal(c.model().valid,true);
});
test('页面后台立即撤标，旧请求晚到不能恢复标记', async () => {
  const wait=deferred(); let n=0;
  const c=createController({request:async()=>++n===1?state():wait.promise});
  await c.refresh(); const refresh=c.refresh(); c.suspend(); assert.equal(c.model().valid,false);
  wait.resolve(state(2)); await refresh; assert.equal(c.model().state.revision,1); assert.equal(c.model().valid,false);
  await c.resume(); assert.equal(c.model().state.revision,2); assert.equal(c.model().valid,true);
});
test('后台期间写请求完成不覆盖状态，返回前台不重放', async () => {
  const wait=deferred(), calls=[]; const c=createController({request:async(_,body)=>{calls.push(body);return body?wait.promise:state(3);}});
  await c.refresh(); const operation=c.command('lock',{id:'r2'}); c.suspend(); wait.resolve({...state(4),locked_id:'r2'}); await operation;
  assert.equal(c.model().state.revision,3); await c.resume(); assert.equal(calls.filter(Boolean).length,1);
});
test('慢状态请求不会被轮询反复作废或积压', async () => {
  const wait=deferred(); let calls=0; const c=createController({request:async()=>{calls++;return wait.promise;}});
  const first=c.refresh(); await c.refresh(); await c.refresh(); assert.equal(calls,1); wait.resolve(state()); await first; assert.equal(c.model().connected,true);
});
test('本地时间推进使标记过期；源时间未知不影响失效', async () => {
  let time=0; const c=createController({request:async()=>state(),now:()=>time}); await c.refresh(); time=1600; assert.equal(c.model().valid,false);
});
test('状态传输耗时计入帧龄，不能把慢响应当新帧', async () => {
  let time=0; const c=createController({now:()=>time,request:async()=>{time=1700;return state();}});
  await c.refresh(); assert.equal(c.model().valid,false);
});
test('锁定写请求等待期间立即撤销旧标记', async () => {
  const write=deferred(); const c=createController({request:async(_,body)=>body?write.promise:state()});
  await c.refresh(); const operation=c.command('lock',{id:'r2'}); assert.equal(c.model().valid,false);
  write.resolve({...state(2),locked_id:'r2'}); await operation; assert.equal(c.model().valid,false);
});
test('服务端更短的帧有效期优先，不能被前端1500ms放宽', () => {
  const s=state(); s.preview.max_age_ms=1000;
  assert.equal(previewValid(s,969,true),true);
  assert.equal(previewValid(s,970,true),false);
  s.preview.max_age_ms=5000; assert.equal(previewValid(s,1470,true),false);
  s.preview.max_age_ms=0; assert.equal(previewValid(s,0,true),false);
});
test('图片晚到仍受自身原始帧龄约束，新的valid状态不能使旧图复活', () => {
  const {imageResultValid}=require('../Xen/lineup/web/app.js');
  const old=state(); old.preview.max_age_ms=1000;
  const fresh=state(2); fresh.preview.max_age_ms=1000;
  const current={state:fresh,valid:true};
  assert.equal(imageResultValid(old,500,current),true);
  assert.equal(imageResultValid(old,970,current),false);
  for (const patch of [{epoch:'new-session'},{preview:{...fresh.preview,frame_id:'43'}},{preview:{...fresh.preview,recipe_id:'r2'}},{preview:{...fresh.preview,url:'/different-image'}}]) assert.equal(imageResultValid(old,500,{state:{...fresh,...patch},valid:true}),false);
  assert.equal(imageResultValid(old,500,{state:fresh,valid:false}),false);
});
test('scope写入只限定视觉搜索，刷新不重发且保留锁定', async () => {
  const calls=[]; let s=state();
  const c=createController({request:async(_,body)=>{calls.push(body);if(body)s={...s,revision:2,scope:{map:body.map,region:body.region}};return s;}});
  await c.refresh(); await c.command('scope',{map:'地图A',region:'区域B'}); await c.refresh(); await c.resume();
  assert.equal(calls.filter(Boolean).length,1); assert.equal(c.model().state.locked_id,'r1');
  assert.deepEqual(c.model().state.scope,{map:'地图A',region:'区域B'});
});
test('scope失败后刷新重连不自动重试，下一次用户改变才发送', async () => {
  const calls=[]; let fail=true;
  const c=createController({request:async(_,body)=>{calls.push(body);if(body&&fail)throw new Error('conflict');return state();}});
  await c.refresh(); await assert.rejects(c.command('scope',{map:'M',region:'R'})); await c.resume(); await c.refresh();
  assert.equal(calls.filter(Boolean).length,1); fail=false;
  await c.command('scope',{map:'M',region:'R2'}); assert.equal(calls.filter(Boolean).length,2);
});
test('采集与识别状态均使用中文，未知枚举不直接显示英文', () => {
  const {textStatus}=require('../Xen/lineup/web/app.js');
  for (const status of ['idle','waiting','saving','cancelled','no_reference','invalid_frame','not_found','unreliable','expired','error']) {
    assert.match(textStatus(status),/[\u4e00-\u9fff]/); assert.notEqual(textStatus(status),status);
  }
  assert.equal(textStatus('future_status'),'未知状态');
});
test('批量保存接收阶段保留编辑，字段回读一致后才清理', () => {
  const {settleBatch}=require('../Xen/lineup/web/app.js');
  const item={id:'r1',name:'名称',target:'目标'}, pending=[item], edits=new Map([['r1',{...item}]]);
  assert.equal(settleBatch({busy:true,capture_status:'saving',recipes:[]},pending,edits),pending);
  assert.equal(edits.size,1);
  assert.equal(settleBatch({busy:false,capture_status:'saved',recipes:[{...item,name:''}]},pending,edits),pending);
  assert.equal(edits.size,1);
  assert.equal(settleBatch({busy:false,capture_status:'saved',recipes:[item]},pending,edits),null);
  assert.equal(edits.size,0);
});
test('批量保存失败保留输入，成功也不清除提交后新增编辑', () => {
  const {settleBatch}=require('../Xen/lineup/web/app.js');
  const item={id:'r1',name:'原提交'},pending=[item],edits=new Map([['r1',{id:'r1',name:'后来编辑'}]]);
  assert.equal(settleBatch({busy:false,capture_status:'error'},pending,edits),null); assert.equal(edits.size,1);
  assert.equal(settleBatch({busy:false,capture_status:'saved',recipes:[item]},pending,edits),null);
  assert.equal(edits.get('r1').name,'后来编辑');
});
test('阵营筛选保留通用与未分类旧库，换边只切换T/CT配方', () => {
  const rows=[{id:'t',map:'A',team:'T'},{id:'ct',map:'A',team:'CT'},{id:'any',map:'A',team:'ANY'},{id:'old',map:'A'},{id:'u',map:'A',team:'UNCLASSIFIED'},{id:'other',map:'B',team:'T'}];
  assert.deepEqual(filterRecipes(rows,{map:'A',team:'T'}).map(r=>r.id),['t','any','old','u']);
  assert.deepEqual(filterRecipes(rows,{map:'A',team:'CT'}).map(r=>r.id),['ct','any','old','u']);
  assert.equal(filterRecipes(rows,{map:'A',team:'UNKNOWN'}).length,5);
  assert.deepEqual(filterRecipes(rows,{map:'B',team:'T'}).map(r=>r.id),['other']);
  assert.equal(filterRecipes([{...rows[0],compatible:false}],{map:'A',team:'T'}).length,0);
});
test('上下文未知不推断阵营，自动地图不依赖配方目录，手动值独立', () => {
  const {contextView,teamLabel}=require('../Xen/lineup/web/app.js');
  assert.deepEqual([contextView().map,contextView().team],['','UNKNOWN']);
  const auto=contextView({mode:'auto',map:'de_new_map',team:'CT',identity_confirmed:true,age_ms:10});
  assert.equal(auto.map,'de_new_map'); assert.equal(auto.team,'CT'); assert.equal(auto.age,'10 ms');
  const manual=contextView({mode:'manual',map:'de_manual',team:'UNKNOWN',identity_confirmed:false});
  assert.equal(manual.mode,'manual'); assert.equal(manual.map,'de_manual'); assert.match(manual.identity,/身份未知/);
  assert.equal(teamLabel(undefined),'未分类');
});
test('手动上下文与恢复自动仅用户发送，换图换边撤锁状态不会自动另锁', async () => {
  const calls=[]; let current={...state(),context:{mode:'auto',map:'A',team:'T'}};
  const c=createController({request:async(_,body)=>{calls.push(body);if(body)current={...current,revision:current.revision+1,context:body.mode==='manual'?{mode:'manual',map:body.map,team:body.team}:{mode:'auto',map:'B',team:'CT'},locked_id:'',lock_reason:'GSI map or team changed',preview:{}};return current;}});
  await c.refresh(); await c.command('context',{mode:'manual',map:'B',team:'CT'}); assert.equal(c.model().valid,false); assert.equal(c.model().state.locked_id,'');
  await c.refresh(); await c.resume(); assert.equal(calls.filter(Boolean).length,1);
  await c.command('context',{mode:'auto'}); assert.equal(c.model().state.context.mode,'auto'); assert.equal(calls.filter(x=>x?.action==='lock').length,0);
});

test('收藏与练习清单复用地图阵营过滤，旧库可见，不更换锁定', () => {
  const {practiceRecipes}=require('../Xen/lineup/web/app.js');
  const rows=[{id:'t',map:'A',team:'T'},{id:'ct',map:'A',team:'CT'},{id:'old',map:'A'}];
  const practice={favorites:['t','ct','old'],queue:['ct']};
  assert.deepEqual(practiceRecipes(rows,{map:'A',team:'T'},practice,'favorites').map(r=>r.id),['t','old']);
  assert.deepEqual(practiceRecipes(rows,{map:'A',team:'T'},practice,'queue'),[]);
  assert.equal(practiceRecipes(rows,{},undefined,'all').length,3);
});
test('人工逐次结果发送一次，保留锁定，不自动切换下一配方', async () => {
  const calls=[]; const c=createController({request:async(_,body)=>{if(body)calls.push(body);return state();}});
  await c.refresh(); await c.command('practice_record',{id:'r1',outcome:'skip',conditions:'离线人工'}); await c.refresh();
  assert.equal(calls.length,1); assert.equal(calls[0].outcome,'skip'); assert.equal(c.model().state.locked_id,'r1');
});

test('局部定位必须显式触发且只在浏览已运行锁定配方时可用', async () => {
  const {canLocate,locationHint}=require('../Xen/lineup/web/app.js');
  assert.equal(canLocate(state()),true);
  for (const patch of [{running:false},{locked_id:''},{mode:'capture'}]) assert.equal(canLocate({...state(),...patch}),false);
  assert.equal(previewValid({...state(),frame_mode:'roi',locating:false},0,true),false);
  assert.equal(previewValid({...state(),frame_mode:'roi',locating:true},0,true),true);
  assert.match(locationHint({...state(),locating:false}),/人工就位/);
  assert.match(locationHint({...state(),locating:true,location_status:'not_found'}),/人工转向/);
  assert.match(locationHint({...state(),locating:true,location_status:'valid'}),/不等于三维站位/);
  const calls=[]; const c=createController({request:async(_,body)=>{if(body)calls.push(body);return state();}});
  await c.refresh(); await c.command('lock',{id:'r1'}); await c.refresh();
  assert.equal(calls.filter(x=>x.action==='locate').length,0);
  await c.command('locate'); assert.equal(calls.filter(x=>x.action==='locate').length,1);
  assert.equal(calls.filter(x=>['move','throw','execute'].includes(x.action)).length,0);
});
test('启动行展示共享配置ROI，源全屏尺寸不冒充传入局部几何', () => {
  const {startupSummary}=require('../Xen/lineup/web/app.js');
  const startup={config_path:'shared.ini',source:'LAN',source_width:2560,source_height:1440,configured_roi_width:320,configured_roi_height:320};
  const text=startupSummary(startup,{source_mapping_verified:false});
  assert.match(text,/320×320/); assert.doesNotMatch(text,/2560/); assert.match(text,/仅局部像素/);
  assert.match(startupSummary(startup,{source_mapping_verified:true}),/映射已验证/);
});

test('动作阶段完整声明左右键/方向/跳跃，空白时长不被猜成0', () => {
  const {actionPhase,actionSummary}=require('../Xen/lineup/web/app.js');
  assert.deepEqual(actionPhase('left+right','forward+left',true,''),{buttons:['left','right'],movement:['forward','left'],jump:true,duration_ms:null});
  assert.deepEqual(actionPhase('','','',0),{buttons:[],movement:[],jump:false,duration_ms:0});
  assert.equal(actionPhase('left','',false,'40').duration_ms,40);
  assert.match(actionSummary(null),/仅供人工阅读/);
  assert.match(actionSummary({phases:[actionPhase('right','left',true,'')]}),/仅设计/);
  assert.match(actionSummary({phases:[]},{simulation:{supported:true}}),/尚未据此证明模拟已通过/);
  assert.match(actionSummary({phases:[]}),/释放可能投出/);
});

test('结构化动作保存按深层值回读确认，服务端字段排序不丢编辑', () => {
  const {settleBatch}=require('../Xen/lineup/web/app.js');
  const item={id:'r1',throw_action:{schema:1,type:'phases',phases:[{buttons:[],movement:[],jump:false,duration_ms:null}]}};
  const edits=new Map([['r1',item]]),pending=[item];
  const saved={id:'r1',throw_action:{phases:[{duration_ms:null,jump:false,movement:[],buttons:[]}],type:'phases',schema:1}};
  assert.equal(settleBatch({capture_status:'saved',recipes:[saved]},pending,edits),null);assert.equal(edits.size,0);
  const changed={...saved,throw_action:{...saved.throw_action,phases:[]}};edits.set('r1',item);
  assert.equal(settleBatch({capture_status:'saved',recipes:[changed]},pending,edits),pending);assert.equal(edits.size,1);
});

test('Space说明与运行时能力、标定只读分离，不宣称硬件或真实通过', () => {
  const {executionSummary,actionSummary}=require('../Xen/lineup/web/app.js');
  assert.match(executionSummary(),/未知/);
  const text=executionSummary({state:'ALIGNED',calibration_status:{status:'valid',reason:'shared calibration'},capabilities:{left:true,right:true,movement:true,jump:true},real_verified:false});
  assert.match(text,/已对准/);assert.match(text,/有效标定/);assert.match(text,/Space跳跃/);assert.match(text,/效果待验证/);assert.match(text,/可能直接投出/);
  assert.match(executionSummary({calibration_status:'missing'}),/缺少标定/);
  assert.match(executionSummary({capabilities:{left_button:true,right_button:true}}),/左键、右键/);
  const summary=actionSummary({phases:[]},{simulation:{supported:true},hardware:{available:true,supported:false}});
  assert.match(summary,/不能降级执行/);assert.match(summary,/原滚轮绑定不变/);
});
