'use strict';

// 执行生产页面内的真实函数；DOM只替身，不启动HTTP、浏览器或游戏输入。
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const html = fs.readFileSync(path.join(__dirname, '..', 'Xen', 'grenade_web', 'page.html'), 'utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/)?.[1];
assert.ok(script, '生产OBS菜单必须包含可执行脚本');
const nodes = new Map();
const node = () => ({dataset:{},classList:{toggle(){}},addEventListener(){},querySelectorAll(){return [];},
    replaceChildren(){},append(){},setAttribute(){},innerHTML:'',textContent:''});
const document = {hidden:false,getElementById(id){if(!nodes.has(id))nodes.set(id,node());return nodes.get(id);},
    addEventListener(){},createElement:node,createElementNS:node};
const context = {document,window:{addEventListener(){},scrollTo(){}},history:{replaceState(){},pushState(){}},
    location:{pathname:'/obs-menu'},performance:{now:()=>100},setTimeout:()=>0,clearTimeout(){},setInterval:()=>0,
    fetch:()=>new Promise(()=>{}),AbortController,AbortSignal,console};
vm.runInNewContext(script+'\nglobalThis.menu={state,title,station,target,groups,groupPlans,standingSource,sharedSpawn};',context);
const {state,title,station,target,groups,groupPlans,standingSource,sharedSpawn} = context.menu;
const plan = (id,name,map='de_mirage',team='T') => ({id,name,map,team});
const reset = (plans,mode='station',group=null,selected=null) => {
    state.plans=plans;state.map='de_mirage';state.team='T';state.deadline=10000;
    state.mode=mode;state.group=group;state.selected=selected;state.refs.clear();
};
const standing = (id,marker=false) => state.refs.set(id,{standing_ready:true,revision:id,
    ...(marker?{standing_marker:{x:.2,y:.4,label:'T2'}}:{})});
let failures=0,checks=0;
const check = (name,run) => {++checks;try{run();}catch(error){++failures;console.error('[失败] '+name+': '+error.message);}};

check('自定义站位使用首个空白前的完整token',()=>{
    const p=plan('a','T垃圾桶左 快跳台烟 备用瞄角');
    assert.equal(station(p).key,'T垃圾桶左');assert.equal(station(p).label,'T垃圾桶左');
    assert.equal(target(p),'快跳台烟 备用瞄角');
});
check('已有地图前缀不进入站位名',()=>{
    for(const name of ['Mirage T垃圾桶左 快跳台烟','de_mirage T垃圾桶左 快跳台烟']){
        const p=plan('a',name);assert.equal(station(p).key,'T垃圾桶左');assert.equal(target(p),'快跳台烟');
    }
    assert.equal(target(plan('dust','Dust II T1 中门烟','de_dust2')),'中门烟');
});
check('无空白名称各自独立分组',()=>{
    assert.equal(station(plan('a','独立点位甲')).key,'独立点位甲');
    assert.equal(station(plan('b','独立点位乙')).key,'独立点位乙');
    assert.equal(target(plan('a','独立点位甲')),'独立点位甲');
});
check('编号站位兼容原T和CT来源前缀',()=>{
    assert.equal(station(plan('a','Mirage T1 VIP烟 R2')).key,'T1');
    assert.equal(station(plan('a','Mirage T1 VIP烟 R2')).number,1);
    assert.equal(target(plan('a','Mirage T1 VIP烟 R2')),'VIP烟');
    assert.equal(station(plan('b','CT来源3号 楼梯烟')).key,'CT3');
    assert.equal(target(plan('b','CT来源3号 楼梯烟')),'楼梯烟');
});
check('空白后的英文及中文落点完整保留',()=>{
    const p=plan('a','T垃圾桶左\tFast Platform under smoke 长名称备用角');
    assert.equal(station(p).key,'T垃圾桶左');assert.equal(target(p),'Fast Platform under smoke 长名称备用角');
});
check('自定义站位不混进同一个其他组',()=>{
    reset([plan('a','T垃圾桶左 快跳台烟'),plan('b','T垃圾桶左 拱门烟'),
        plan('c','T垃圾桶右 快跳台烟'),plan('d','独立点位甲'),plan('e','独立点位乙')]);
    const values=groups();assert.equal(values.length,4);
    assert.equal(values.find(item=>item.key==='T垃圾桶左').plans.length,2);
});
check('按站位不能拿另一自定义站位图片',()=>{
    const a=plan('a','T垃圾桶左 快跳台烟'),b=plan('b','T垃圾桶右 拱门烟');
    reset([a,b],'station',station(a).key,'a');standing('b');assert.equal(standingSource(),null);
});
check('按落点优先已选方案的站位图',()=>{
    const a=plan('a','T垃圾桶左 快跳台烟'),b=plan('b','T垃圾桶右 快跳台烟');
    reset([b,a],'target',target(a),'a');assert.equal(groupPlans().length,2);
    standing('a');standing('b');assert.equal(standingSource().id,'a');
});
check('已选方案缺图时不能借另一站位图',()=>{
    const a=plan('a','T垃圾桶左 快跳台烟'),b=plan('b','T垃圾桶右 快跳台烟');
    reset([a,b],'target',target(a),'a');assert.equal(groupPlans().length,2);
    standing('b');assert.equal(standingSource(),null);
});
check('按落点未选站位时不任取多站位图片',()=>{
    const a=plan('a','T垃圾桶左 快跳台烟'),b=plan('b','T垃圾桶右 快跳台烟');
    reset([a,b],'target',target(a));assert.equal(groupPlans().length,2);
    standing('b');assert.equal(standingSource(),null);
});
check('仅一个站位的落点可以显示其自己的站位图',()=>{
    const a=plan('a','T垃圾桶左 快跳台烟');reset([a],'target',target(a));standing('a');
    assert.equal(standingSource().id,'a');
});
check('原Mirage T1到T5带编号共图仍可复用',()=>{
    const a=plan('a','T1 VIP烟'),b=plan('b','T2 过点烟');
    reset([a,b],'station','T1','a');standing('b',true);assert.equal(standingSource().id,'b');
    assert.equal(sharedSpawn(a),true);
});
check('普通站位和CT不能误入Mirage T出生点共图',()=>{
    const a=plan('a','T垃圾桶左 快跳台烟'),b=plan('b','T2 过点烟');
    reset([a,b],'station',station(a).key,'a');standing('b',true);assert.equal(standingSource(),null);
    state.team='CT';assert.equal(sharedSpawn(b),false);
});
if(failures){console.error(`OBS道具菜单真实JS专项失败：${failures}/${checks}`);process.exitCode=1;}
else console.log(`OBS道具菜单真实JS专项通过：${checks}/${checks}`);
