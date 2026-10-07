'use strict';
// 主机面板复用网页的唯一选项和动作预设，不另维护一份地图/投掷时序。
const fs = require('node:fs');
const path = require('node:path');
const {captureOptions,captureRequest} = require('../Xen/lineup/web/app.js');

function buildOptions() {
  const options = captureOptions();
  const presets = [];
  for (const method of options.methods) {
    const moving = method.id.startsWith('step') || method.id.startsWith('runup');
    const buttonChoices = method.fixed_buttons ? [{id:method.fixed_buttons}] : options.buttons;
    const directionChoices = method.fixed_direction ? [{id:method.fixed_direction}] :
      (moving ? options.directions : [{id:'forward'}]);
    for (const buttons of buttonChoices) {
      for (const direction of directionChoices) {
        const value = captureRequest({map:options.maps[0].id,team:'CT',method:method.id,
          buttons:buttons.id,direction:direction.id});
        presets.push({method:method.id,buttons:buttons.id,direction:direction.id,
          throw_instructions:value.throw_instructions,throw_action:value.throw_action});
      }
    }
  }
  return {...options,presets};
}
module.exports = {buildOptions};
if (require.main === module) {
  if (process.argv.length !== 4 || process.argv[2] !== '--output') throw new Error('用法：node build_lineup_capture_options.cjs --output <选项JSON>');
  const output = path.resolve(process.argv[3]);
  fs.mkdirSync(path.dirname(output),{recursive:true});
  fs.writeFileSync(output,JSON.stringify(buildOptions(),null,2)+'\n','utf8');
  console.log('已生成主机本地采集选项：'+output);
}
