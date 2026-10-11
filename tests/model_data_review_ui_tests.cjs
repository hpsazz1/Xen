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
const result=C.exportDraft(data,handled);
pass(()=>assert.equal(result.samples.length,1));
pass(()=>assert.equal(Object.hasOwn(result,'reviewer'),false));
pass(()=>assert.equal(C.importDraft(data,JSON.parse(JSON.stringify(result))).size,1));
pass(()=>assert.equal(C.importDraft(data,{...result,reviewer:'旧审核者'}).size,1));
rejects(()=>C.exportDraft(data,new Map()));
for(const patch of [{root_identity:'other'},{class_names:['head','body']},{format:'wrong'}])rejects(()=>C.importDraft(data,{...result,...patch}));
for(const patch of [{sample_id:'unknown'},{image_sha256:'b'.repeat(64)},{base_review_token:'new'},{state:'UNKNOWN'}])rejects(()=>C.importDraft(data,{...result,samples:[{...result.samples[0],...patch}]}));
rejects(()=>C.importDraft(data,{...result,samples:[result.samples[0],result.samples[0]]}));
// 原有审核只用于显示，未触碰不能进入导出。
const reviewed={...data,samples:[{...sample(1),state:'VERIFIED_POSITIVE'},sample(2)]};
rejects(()=>C.exportDraft(reviewed,new Map()));
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

// 运行原始 UI 事件块，只替换浏览器资源边界；不复制 open/close/commit 逻辑。
// Image 完成由异步队列控制，DOM/canvas 替身不验证布局、解码或浏览器事件派发。
function eventPage({empty=false}={}) {
  const requests=[], drawn=[], confirmations=[], downloads=[];
  const listeners=new Map(), faults={export:null};
  const ctx={clearRect(){},save(){},restore(){},setTransform(){},strokeRect(){},fillText(){},
    drawImage(image){drawn.push(image);}};
  class Element {
    constructor(tag) {
      this.tagName=tag;this.children=[];this.textContent='';this.className='';
      this.value='';this.checked=false;this.disabled=false;this.open=false;
      this.classList={add:name=>{this.className+=' '+name;},toggle:()=>{}};
    }
    append(...children){this.children.push(...children);}
    replaceChildren(...children){this.children=[...children];}
    setAttribute(name,value){this[name]=value;}
    showModal(){assert.equal(this.open,false);this.open=true;}
    close(){this.open=false;}
    getContext(){return ctx;}
    getBoundingClientRect(){return {left:0,top:0,width:this.width,height:this.height};}
    setPointerCapture(){}
    click(){if(this.tagName==='a')downloads.push(this.href);else this.onclick?.();}
  }
  const elements=new Map([...html.matchAll(/<([\w-]+)\b[^>]*\bid="([^"]+)"[^>]*>/g)]
    .map(m=>[m[2],new Element(m[1])]));
  const get=id=>elements.get(id);
  const fixture={...data,samples:[1,2,3].map(i=>({...sample(i),detections:empty?[]:[{...box}]}))};
  get('review-data').textContent=JSON.stringify(fixture);
  get('sort').value='priority';get('mode').value='select';get('show-boxes').checked=true;
  const sandbox=vm.createContext({
    document:{getElementById:get,createElement:tag=>new Element(tag),createElementNS:(_,tag)=>new Element(tag)},
    window:{addEventListener:(name,handler)=>{assert.equal(listeners.has(name),false);listeners.set(name,handler);}},
    Image:class {constructor(){requests.push(this);}},
    confirm:()=>{assert.ok(confirmations.length,'必须显式指定确认回答');return confirmations.shift();},
    Blob,URL:{createObjectURL:blob=>{if(faults.export)throw new Error(faults.export);return blob;},revokeObjectURL(){}},setTimeout:()=>{}
  });
  for(const m of html.matchAll(/<script id="[^"]+">([\s\S]*?)<\/script>/g))
    vm.runInContext(m[1],sandbox,{filename:'model_data_review.html'});
  const click=id=>{assert.equal(get(id).disabled,false);get(id).onclick();};
  function open(id) {
    assert.equal(get('detail').open,false,'使用关闭后重开的实际入口，不点击模态框后的背景');
    const card=get('grid').children.find(c=>c.children[0].children[0].children[1].textContent===String(id));
    assert.ok(card,`样本 ${id} 应在当前页`);
    card.children.find(c=>c.className==='image').onclick();
    assert.equal(get('detail').open,true);
    return requests.at(-1);
  }
  function cancel() {
    let prevented=false;
    get('detail').oncancel({preventDefault(){prevented=true;}});
    assert.equal(prevented,true);
  }
  async function complete(request,outcome) {
    // 即使回调已排队，关闭/切换也必须使其失效。
    const callback=request['on'+outcome];
    await new Promise(resolve=>setImmediate(resolve));
    callback?.call(request);
  }
  // 兼容基线页面的全局图片提示，便于同一回归在修复前证明失败。
  const imageMessage=()=>get('detail-image-status')||get('message');
  const snapshot=()=>JSON.stringify({message:get('message').textContent,error:get('message').className,
    imageMessage:imageMessage().textContent,imageError:imageMessage().className,
    title:get('detail-title').textContent,draft:get('draft-status').textContent,
    progress:get('progress').textContent,open:get('detail').open,drawn:drawn.length});
  async function exported() {
    const previous=downloads.length;click('export');assert.equal(downloads.length,previous+1);
    return JSON.parse(await downloads.at(-1).text());
  }
  function leaving() {
    let prevented=false;
    const event={returnValue:undefined,preventDefault(){prevented=true;}};
    assert.ok(listeners.has('beforeunload'));listeners.get('beforeunload')(event);
    assert.equal(prevented,event.returnValue==='','现代与旧版离页保护应一致');
    return prevented;
  }
  return {get,click,open,cancel,complete,snapshot,imageMessage,confirmations,drawn,exported,leaving,faults};
}

async function eventChecks() {
  let passed=0,failed=0;
  async function scenario(name,run) {
    try {await run(eventPage());passed++;console.log(`  PASS ${name}`);}
    catch(error){failed++;console.error(`  FAIL ${name}: ${error.stack}`);}
  }
  await scenario('current success commits only the active sample',async h=>{
    const request=h.open(2);await h.complete(request,'load');
    assert.equal(h.drawn.at(-1),request);h.click('detail-positive');
    const result=await h.exported();assert.equal(result.samples.length,1);
    assert.equal(result.samples[0].sample_id,'2');assert.equal(result.samples[0].state,'VERIFIED_POSITIVE');
    assert.deepEqual(result.samples[0].detections,[box]);
  });
  await scenario('current failure remains visible and blocks confirmation',async h=>{
    const request=h.open(1);await h.complete(request,'error');
    assert.match(h.imageMessage().textContent,/详细图片无法读取/);
    assert.match(h.imageMessage().className,/error/);h.click('detail-positive');
    assert.equal(h.get('detail').open,true);assert.match(h.get('progress').textContent,/处理 0 /);
    assert.match(h.get('message').textContent,/请先加载图片/);
  });
  await scenario('old failure cannot change a newly opened sample',async h=>{
    const old=h.open(1);h.click('close-detail');h.open(2);const before=h.snapshot();
    await h.complete(old,'error');assert.equal(h.snapshot(),before);
  });
  await scenario('old success cannot draw or enable current confirmation',async h=>{
    const old=h.open(1);h.click('close-detail');const current=h.open(2),before=h.snapshot();
    await h.complete(old,'load');assert.equal(h.snapshot(),before);h.click('detail-positive');
    assert.equal(h.get('detail').open,true);assert.match(h.get('progress').textContent,/处理 0 /);
    await h.complete(current,'load');h.click('detail-positive');
    assert.equal((await h.exported()).samples[0].sample_id,'2');
  });
  await scenario('rapid three-image switch retains current failure',async h=>{
    const a=h.open(1);h.click('close-detail');const b=h.open(2);h.click('close-detail');
    const c=h.open(3);await h.complete(c,'error');const before=h.snapshot();
    await h.complete(b,'error');await h.complete(a,'load');assert.equal(h.snapshot(),before);
    assert.match(h.imageMessage().textContent,/详细图片无法读取/);
  });
  await scenario('reopening the same image uses request identity',async h=>{
    const old=h.open(1);h.click('close-detail');const current=h.open(1),before=h.snapshot();
    assert.equal(old.src,current.src);assert.notEqual(old,current);
    await h.complete(old,'error');assert.equal(h.snapshot(),before);
    await h.complete(current,'load');assert.equal(h.drawn.at(-1),current);
    h.click('detail-positive');assert.equal((await h.exported()).samples[0].sample_id,'1');
  });
  await scenario('late success from the same image cannot enable its replacement',async h=>{
    const old=h.open(1);h.click('close-detail');h.open(1);const before=h.snapshot();
    await h.complete(old,'load');assert.equal(h.snapshot(),before);h.click('detail-positive');
    assert.match(h.get('progress').textContent,/处理 0 /);assert.equal(h.get('detail').open,true);
  });
  for(const outcome of ['load','error'])for(const close of ['button','cancel'])
    await scenario(`${close} close ignores queued ${outcome}`,async h=>{
      const request=h.open(1),completion=h.complete(request,outcome);
      close==='button'?h.click('close-detail'):h.cancel();
      const before=h.snapshot();await completion;assert.equal(h.snapshot(),before);
    });
  await scenario('declining dirty cancellation retains the current success and draft',async h=>{
    const request=h.open(1);h.get('x1').value='4';h.click('apply-box');
    h.confirmations.push(false);h.cancel();assert.equal(h.get('detail').open,true);
    assert.match(h.get('draft-status').textContent,/有未确认修改/);
    await h.complete(request,'load');h.click('detail-positive');
    assert.equal((await h.exported()).samples[0].detections[0].x1,4);
  });
  await scenario('declining dirty close retains the current failure',async h=>{
    const request=h.open(1);h.get('x1').value='4';h.click('apply-box');
    h.confirmations.push(false);h.click('close-detail');await h.complete(request,'error');
    assert.equal(h.get('detail').open,true);assert.match(h.imageMessage().textContent,/详细图片无法读取/);
    h.confirmations.push(true);h.cancel();assert.equal(h.get('detail').open,false);
    assert.match(h.get('progress').textContent,/处理 0 /);
  });
  await scenario('old failure cannot overwrite a saved newer decision',async h=>{
    const old=h.open(1);h.click('close-detail');const current=h.open(2);
    await h.complete(current,'load');h.click('detail-positive');const before=h.snapshot();
    await h.complete(old,'error');assert.equal(h.snapshot(),before);
    assert.equal((await h.exported()).samples[0].sample_id,'2');
  });
  await scenario('new image does not inherit the previous image failure',async h=>{
    const old=h.open(1);await h.complete(old,'error');h.click('close-detail');
    const current=h.open(2);assert.doesNotMatch(h.imageMessage().className,/error/);
    await h.complete(current,'load');assert.doesNotMatch(h.imageMessage().className,/error/);
  });
  await scenario('late failure cannot replace the current loaded state',async h=>{
    const old=h.open(1);h.click('close-detail');const current=h.open(2);
    await h.complete(current,'load');const before=h.snapshot();
    await h.complete(old,'error');assert.equal(h.snapshot(),before);
    h.click('detail-positive');assert.equal((await h.exported()).samples[0].sample_id,'2');
  });
  await scenario('accepting dirty close ignores its pending failure',async h=>{
    const request=h.open(1);h.get('x1').value='4';h.click('apply-box');
    const completion=h.complete(request,'error');h.confirmations.push(true);h.click('close-detail');
    const before=h.snapshot();await completion;assert.equal(h.snapshot(),before);
    assert.equal(h.get('detail').open,false);assert.match(h.get('progress').textContent,/处理 0 /);
  });
  await scenario('image success preserves an unrelated coordinate error',async h=>{
    const request=h.open(1);h.get('x1').value='';h.click('apply-box');
    const error=h.get('message').textContent;assert.match(error,/坐标不能为空/);
    await h.complete(request,'load');assert.equal(h.get('message').textContent,error);
    assert.match(h.get('message').className,/error/);
  });
  await scenario('image failure preserves an unrelated coordinate error',async h=>{
    const request=h.open(1);h.get('x1').value='';h.click('apply-box');
    const error=h.get('message').textContent;assert.match(error,/坐标不能为空/);
    await h.complete(request,'error');assert.equal(h.get('message').textContent,error);
    assert.match(h.imageMessage().textContent,/详细图片无法读取/);
  });
  console.log(`model_data_review_ui_events: ${passed} scenarios passed, ${failed} failed`);
  if(failed)process.exitCode=1;
}
async function lifecycleChecks() {
  let passed=0,failed=0;
  async function scenario(name,run,options) {
    try {await run(eventPage(options));passed++;console.log(`  PASS ${name}`);}
    catch(error){failed++;console.error(`  FAIL ${name}: ${error.stack}`);}
  }
  const edit=(h,x)=>{h.get('x1').value=String(x);h.click('apply-box');};
  const load=async(h,id=1)=>{const request=h.open(id);await h.complete(request,'load');};
  await scenario('browsing and closing untouched samples needs no leave warning',async h=>{
    assert.equal(h.leaving(),false);await load(h);assert.equal(h.leaving(),false);
    h.click('close-detail');assert.equal(h.leaving(),false);await load(h,2);h.cancel();
    assert.equal(h.leaving(),false);assert.equal(h.leaving(),false);
  });
  for(const close of ['button','cancel'])
    await scenario(`accepting ${close} discard clears only the abandoned draft`,async h=>{
      await load(h);edit(h,4);assert.equal(h.leaving(),true);h.confirmations.push(true);
      close==='button'?h.click('close-detail'):h.cancel();
      assert.equal(h.get('detail').open,false);assert.equal(h.leaving(),false);assert.equal(h.leaving(),false);
      await load(h);assert.equal(h.get('x1').value,'2');h.click('close-detail');
      await load(h,2);assert.equal(h.leaving(),false);h.click('close-detail');
      assert.equal(h.leaving(),false);
    });
  for(const close of ['button','cancel'])
    await scenario(`declining ${close} discard preserves the applied edit`,async h=>{
      await load(h);edit(h,4);h.confirmations.push(false);
      close==='button'?h.click('close-detail'):h.cancel();
      assert.equal(h.get('detail').open,true);assert.equal(h.get('x1').value,'4');assert.equal(h.leaving(),true);
      h.click('detail-positive');assert.equal((await h.exported()).samples[0].detections[0].x1,4);
    });
  await scenario('applying unchanged coordinates does not create a draft',async h=>{
    await load(h);edit(h,'02');assert.equal(h.leaving(),false);
    assert.equal(h.get('draft-status').textContent,'未修改草稿');h.click('close-detail');
  });
  await scenario('editing back to the original coordinates clears draft changes',async h=>{
    await load(h);edit(h,4);assert.equal(h.leaving(),true);edit(h,2);
    assert.equal(h.leaving(),false);h.click('close-detail');
  });
  await scenario('clearing an already empty box list is unchanged',async h=>{
    await load(h);h.click('clear-boxes');assert.equal(h.leaving(),false);
    assert.equal(h.get('draft-status').textContent,'未修改草稿');h.click('close-detail');
  },{empty:true});
  await scenario('deletion is protected and undo restores the original draft',async h=>{
    await load(h);h.click('delete-box');assert.equal(h.leaving(),true);h.click('undo');
    assert.equal(h.leaving(),false);assert.equal(h.get('boxes').children.length,1);
    assert.equal(h.get('x1').value,'2');h.click('close-detail');
  });
  await scenario('adding then deleting a new box restores an unchanged draft',async h=>{
    await load(h);h.get('mode').value='draw';const canvas=h.get('canvas');
    canvas.onpointerdown({clientX:20,clientY:20,pointerId:1});
    canvas.onpointerup({clientX:30,clientY:30,pointerId:1});
    assert.equal(h.leaving(),true);assert.equal(h.get('boxes').children.length,2);
    h.click('delete-box');assert.equal(h.get('boxes').children.length,1);
    assert.equal(h.leaving(),false);h.click('close-detail');
  });
  await scenario('confirmed edits remain protected as page decisions',async h=>{
    await load(h);edit(h,4);h.click('detail-positive');
    assert.equal(h.get('detail').open,false);assert.equal(h.leaving(),true);
    const result=await h.exported();assert.equal(result.samples[0].detections[0].x1,4);
    assert.equal(h.leaving(),true,'下载请求不能证明磁盘保存成功');
  });
  await scenario('rejected confirmation retains editable data and leave protection',async h=>{
    await load(h);edit(h,4);h.click('detail-negative');
    assert.equal(h.get('detail').open,true);assert.match(h.get('message').textContent,/带框不能确认负样本/);
    assert.equal(h.get('x1').value,'4');assert.equal(h.leaving(),true);
    assert.match(h.get('progress').textContent,/处理 0 /);
    h.click('detail-positive');assert.equal((await h.exported()).samples[0].detections[0].x1,4);
  });
  await scenario('image failure cannot discard applied edits on confirmation',async h=>{
    const request=h.open(1);edit(h,4);await h.complete(request,'error');h.click('detail-positive');
    assert.equal(h.get('detail').open,true);assert.equal(h.get('x1').value,'4');assert.equal(h.leaving(),true);
    assert.match(h.get('message').textContent,/请先加载图片/);h.click('undo');assert.equal(h.leaving(),false);
  });
  await scenario('failed export retains decisions for an unchanged retry',async h=>{
    await load(h);edit(h,4);h.click('detail-positive');h.faults.export='fixture export failure';h.click('export');
    assert.match(h.get('message').textContent,/fixture export failure/);assert.equal(h.leaving(),true);
    h.faults.export=null;const result=await h.exported();assert.equal(result.samples.length,1);
    assert.equal(result.samples[0].sample_id,'1');assert.equal(result.samples[0].detections[0].x1,4);
    assert.equal(h.leaving(),true);
  });
  await scenario('export with no decisions reports the error without false dirty state',async h=>{
    h.click('export');assert.match(h.get('message').textContent,/尚未明确处理/);
    assert.equal(h.leaving(),false);assert.equal(h.leaving(),false);
  });
  await scenario('discarding re-edits preserves the previous confirmed decision',async h=>{
    await load(h);edit(h,4);h.click('detail-positive');await load(h);edit(h,6);
    h.confirmations.push(true);h.click('close-detail');assert.equal(h.leaving(),true);
    await load(h);assert.equal(h.get('x1').value,'4');assert.equal(h.get('draft-status').textContent,'未修改草稿');
    h.click('close-detail');assert.equal((await h.exported()).samples[0].detections[0].x1,4);
  });
  await scenario('discarding a different sample keeps the earlier decision',async h=>{
    await load(h);h.click('detail-positive');await load(h,2);edit(h,6);
    h.confirmations.push(true);h.cancel();assert.equal(h.leaving(),true);
    const result=await h.exported();assert.equal(result.samples.length,1);assert.equal(result.samples[0].sample_id,'1');
  });
  await scenario('export never silently commits or clears an unconfirmed re-edit',async h=>{
    await load(h);edit(h,4);h.click('detail-positive');await load(h);edit(h,6);
    const result=await h.exported();assert.equal(result.samples[0].detections[0].x1,4);
    assert.equal(h.get('x1').value,'6');assert.equal(h.get('draft-status').textContent,'有未确认修改');
    assert.equal(h.leaving(),true);h.click('undo');assert.equal(h.get('x1').value,'4');
    assert.equal(h.leaving(),true,'撤销草稿不删除已确认决定');
  });
  console.log(`model_data_review_ui_lifecycle: ${passed} scenarios passed, ${failed} failed`);
  if(failed)process.exitCode=1;
}
async function runEventTests(){await eventChecks();await lifecycleChecks();}
runEventTests().catch(error=>{console.error(error);process.exitCode=1;});
