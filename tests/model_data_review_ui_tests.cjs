const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const path = require('node:path');
const html = fs.readFileSync(path.join(__dirname,'../scripts/model_data_review.html'),'utf8');
const source = html.match(/<script id="review-core">([\s\S]*?)<\/script>/)[1];
const context = vm.createContext({});
vm.runInContext(source,context);
const C=context.XenReviewCore;
let checks=0;
function pass(fn){fn();checks++;}
function rejects(fn){pass(()=>assert.throws(fn));}
const box={class_id:0,x1:2,y1:3,x2:12,y2:15};
const sample=i=>({session_id:'s1',sample_id:String(i),image_sha256:'a'.repeat(64),width:32,height:32,image:`images/s1__${i}.png`,detections:i%2?[box]:[],state:'UNKNOWN',reasons:['需复核'],group:'复核',priority:i,base_review_token:'none'});
const data={schema_version:1,class_names:['body','head'],reviewer:'',root_identity:'root',samples:Array.from({length:30},(_,i)=>sample(i+1))};
pass(()=>assert.equal(C.validate(data),data));
pass(()=>assert.equal(C.decision(data.samples[0],'VERIFIED_POSITIVE',[box],data.class_names).detections.length,1));
rejects(()=>C.decision(data.samples[1],'VERIFIED_POSITIVE',[],data.class_names));
rejects(()=>C.decision(data.samples[0],'VERIFIED_NEGATIVE',[box],data.class_names));
pass(()=>assert.equal(C.decision(data.samples[0],'VERIFIED_NEGATIVE',[],data.class_names).state,'VERIFIED_NEGATIVE'));
rejects(()=>C.decision(data.samples[0],'UNKNOWN',[box],data.class_names));
pass(()=>assert.equal(C.decision(data.samples[0],'EXCLUDED',[box],data.class_names).detections.length,0));
for(const patch of [{x1:NaN},{x2:33},{x1:-1},{x2:2},{class_id:2},{class_id:.5}]) rejects(()=>C.boxes([{...box,...patch}],data.samples[0],data.class_names));
for(const image of ['https://evil/a.png','file:///secret.png','images/../secret.png','images/a\\b.png','images/x.svg','//evil/a.png'])rejects(()=>C.validate({...data,samples:[{...sample(1),image}]}));
rejects(()=>C.validate({...data,samples:[sample(1),sample(1)]}));
const visible=data.samples.slice(0,24).map(C.key),all=new Set(data.samples.map(C.key));
pass(()=>assert.equal(C.batch(data.samples,visible,all,'EXCLUDED',new Map(),data.class_names).length,24));
rejects(()=>C.batch(data.samples,visible,new Set([C.key(data.samples[24])]),'EXCLUDED',new Map(),data.class_names));
rejects(()=>C.batch(data.samples,visible,all,'VERIFIED_POSITIVE',new Map(),data.class_names));
const handled=new Map([[C.key(data.samples[0]),C.decision(data.samples[0],'VERIFIED_POSITIVE',[box],data.class_names)]]);
const result=C.exportDraft(data,' 审核者 ',handled);
pass(()=>assert.equal(result.samples.length,1));
pass(()=>assert.equal(result.reviewer,'审核者'));
pass(()=>assert.equal(C.importDraft(data,JSON.parse(JSON.stringify(result))).size,1));
rejects(()=>C.exportDraft(data,' ',handled));
rejects(()=>C.exportDraft(data,'人',new Map()));
for(const patch of [{root_identity:'other'},{class_names:['head','body']},{reviewer:''},{format:'wrong'}])rejects(()=>C.importDraft(data,{...result,...patch}));
for(const patch of [{sample_id:'unknown'},{image_sha256:'b'.repeat(64)},{base_review_token:'new'},{state:'UNKNOWN'}])rejects(()=>C.importDraft(data,{...result,samples:[{...result.samples[0],...patch}]}));
rejects(()=>C.importDraft(data,{...result,samples:[result.samples[0],result.samples[0]]}));
// 原有审核只用于显示，未触碰不能进入导出。
const reviewed={...data,samples:[{...sample(1),state:'VERIFIED_POSITIVE'},sample(2)]};
rejects(()=>C.exportDraft(reviewed,'人',new Map()));
pass(()=>assert.equal((html.match(/__XEN_REVIEW_DATA__/g)||[]).length,1));
pass(()=>assert.ok(html.includes("connect-src 'none'")));
pass(()=>assert.ok(!/\b(fetch|XMLHttpRequest|localStorage)\b/.test(html)));
// 所有脚本均可解析；纯契约测试不伪称浏览器画面验证。
for(const match of html.matchAll(/<script id="[^"]+">([\s\S]*?)<\/script>/g))pass(()=>new vm.Script(match[1]));
// 多成员组才可筛选；内部hash不泄漏为用户标签。
const groupSamples=[{...sample(1),group:'hash-a'},{...sample(2),group:'hash-a'},
  {...sample(3),group:'hash-b',group_label:'近重复候选'},
  {...sample(4),group:'hash-b'}, {...sample(5),group:'singleton'}];
const groups=C.duplicateGroups(groupSamples);
pass(()=>assert.equal(groups.length,2));
pass(()=>assert.equal(groups[0].label,'重复组1（2张）'));
pass(()=>assert.equal(groups[0].value,'hash-a'));
pass(()=>assert.equal(groups[1].label,'近重复候选（2张）'));
pass(()=>assert.equal(C.duplicateGroups(data.samples.map((s,i)=>({...s,group:`hash-${i}`}))).length,0));
const filters={showReviewed:false,unprocessed:true,group:'',reason:'',sort:'id'};
const mutable=new Map();
const frozen=C.filterRows(data.samples,filters,mutable);
const firstPage=C.pageRows(frozen,0).map(C.key);
mutable.set(C.key(data.samples[0]),C.decision(data.samples[0],'EXCLUDED',[],data.class_names));
pass(()=>assert.deepEqual(C.pageRows(frozen,0).map(C.key),firstPage));
const refreshed=C.filterRows(data.samples,filters,mutable);
pass(()=>assert.equal(refreshed.length,29));
pass(()=>assert.equal(refreshed[0].sample_id,'2'));
pass(()=>assert.equal(C.pageRows(frozen,1).length,6));
pass(()=>assert.equal(C.filterRows(reviewed.samples,filters,new Map()).length,1));
pass(()=>assert.equal(C.filterRows(reviewed.samples,{...filters,showReviewed:true},new Map()).length,2));
pass(()=>assert.equal(C.filterRows(groupSamples,{...filters,group:'hash-a'},new Map()).length,2));
// 未开始加载、部分加载或失败均不得批量确认；排除不依赖加载成功。
const pair=[data.samples[0],data.samples[1]];
rejects(()=>C.requireLoaded(pair,new Set(),'VERIFIED_POSITIVE'));
rejects(()=>C.requireLoaded(pair,new Set([C.key(pair[0])]),'VERIFIED_NEGATIVE'));
pass(()=>C.requireLoaded(pair,new Set(pair.map(C.key)),'VERIFIED_POSITIVE'));
pass(()=>C.requireLoaded(pair,new Set(),'EXCLUDED'));
const loaded=new Set(pair.map(C.key));loaded.delete(C.key(pair[1]));
rejects(()=>C.requireLoaded(pair,loaded,'VERIFIED_NEGATIVE'));
pass(()=>assert.ok(html.includes("img.loading='eager'")));
// 详情位图有界，编辑数据始终使用原图坐标；含非整比和屏幕偏移往返。
for (const [width,height] of [[320,320],[16384,16384],[16384,8191],[8191,16384],[1,16384]]) {
  const size=C.canvasSize(width,height);
  pass(()=>assert.ok(size.width<=1024&&size.height<=1024&&size.width>=1&&size.height>=1));
  const rect={left:123,top:77,width:500,height:500*size.height/size.width};
  const original={x:width*.37,y:height*.61};
  const restored=C.displayToImage(C.imageToDisplay(original,width,height,rect),width,height,rect);
  pass(()=>assert.ok(Math.abs(restored.x-original.x)<1e-8&&Math.abs(restored.y-original.y)<1e-8));
}
pass(()=>assert.equal(C.canvasSize(320,320).width,320));
pass(()=>assert.equal(C.displayToImage({x:-50,y:1000},320,320,{left:0,top:0,width:640,height:640}).x,0));
pass(()=>assert.equal(C.displayToImage({x:-50,y:1000},320,320,{left:0,top:0,width:640,height:640}).y,320));
console.log(`model_data_review_ui_tests: ${checks} checks passed`);
