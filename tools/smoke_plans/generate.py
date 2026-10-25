"""从带来源的烟雾清单生成身法编辑器调试初稿；不会执行或安装方案。"""
import argparse
import hashlib
import json
import re
from pathlib import Path


def plan_for(row):
    if row.get('guide_command_conflict'):
        return None, '来源瞄角冲突，先人工核对'
    if row.get('requires_second_position') or row.get('intermediate_targets'):
        return None, '需要第二站位，先人工拆分定位与动作'
    text = ' '.join(row['instructions'])
    moving = row['has_movement_input']
    simultaneous_w = ('同时按 W 和跳' in text or
        (row['map'] == 'inferno' and 'W + 跳跃 + 左键投掷' in text))
    if moving and not simultaneous_w:
        return None, '跑动或静步出手条件尚需人工确定，未猜测时长'
    button = row['throw_button']
    ordered_both = row['map'] == 'mirage' and row['id'] == 'setpos:155' and '先松开右键，再松开左键' in text
    if button == 'both' and not ordered_both:
        return None, '双键释放次序尚需人工确定'
    if button not in ('left', 'right', 'both'):
        raise ValueError(f"不支持的按钮：{row['key']}")
    actions = []
    if ordered_both:
        actions.extend([
            dict(type='button', button='right', start_ms=0, duration_ms=1000),
            dict(type='button', button='left', start_ms=0, duration_ms=1001)])
    else:
        actions.append(dict(type='button', button=button, start_ms=0, duration_ms=1000))
    if row['jump']:
        actions.append(dict(type='key', key='SPACE', start_ms=1000, duration_ms=40))
    if moving:
        actions.append(dict(type='key', key='W', start_ms=1000, duration_ms=100))
    end = max(a['start_ms'] + a['duration_ms'] for a in actions)
    if row['crouch']:
        actions.append(dict(type='key', key='CTRL', start_ms=0, duration_ms=end))
    notes = '\n'.join([row['command'],
        '未游戏实测。毫秒时序为工程调试初值，不是来源测量；请手动定位、确认瞄点并切出烟雾后再触发。',
        '初值：拉烟1000ms；需要跳跃时1000ms起跳40ms；同时W跳投的W从1000ms保持100ms；蹲姿从0ms保持到结束。',
        '双键特例先右后左，1000/1001ms仅表达顺序，真实设备间隔待验证。' if ordered_both else '',
        '来源：'+row['source_url'],
        '源说明：'+' / '.join(row['instructions'])]).replace('\n\n','\n')
    plan = dict(schema_version=3, name=f"烟雾 {row['map']} {row['id']}",
        map='de_'+row['map'].removeprefix('de_'), location=row['destination_name'],
        notes=notes, voice_command='', actions=actions,
        mask=dict(enabled=True, wasd=15, start_ms=0, duration_ms=0))
    for field, limit in [('name',256),('map',256),('location',256),('notes',4096)]:
        if len(plan[field].encode('utf-8')) > limit:
            raise ValueError(f"{row['key']} 的 {field} 超过生产契约字节上限")
    return plan, '调试初稿，未实测'


def encode(value):
    return (json.dumps(value, ensure_ascii=False, indent=2)+'\n').encode('utf-8')


def generate(source, destination):
    rows = json.loads(source.read_text(encoding='utf-8-sig'))
    if not isinstance(rows,list) or not rows:
        raise ValueError('输入须为非空配方数组')
    keys = set()
    outputs = {}
    entries = []
    for row in rows:
        key = row['key']
        if key in keys:
            raise ValueError('重复配方键：'+key)
        keys.add(key)
        plan, reason = plan_for(row)
        entry = dict(key=key, source_url=row['source_url'], command=row['command'],
            status='draft' if plan else 'pending', reason=reason, action_type=row['action_type'],
            plan_file=None, source_recipe=row)
        if plan:
            filename = re.sub(r'[^a-zA-Z0-9_.-]+','-',key)+'.json'
            if filename in outputs:
                raise ValueError('文件名冲突：'+filename)
            outputs[filename] = encode(plan)
            entry['plan_file'] = filename
            entry['sha256'] = hashlib.sha256(outputs[filename]).hexdigest()
        entries.append(entry)
    manifest = dict(schema_version=1, total=len(entries), drafts=len(outputs),
        pending=sum(e['status']=='pending' for e in entries),
        timing_status='engineering_initial_values_not_source_measurements', entries=entries)
    outputs['manifest.json'] = encode(manifest)
    # 先检查全部冲突再写入，避免遇到用户修改后只生成半批。
    for filename, data in outputs.items():
        target = destination / filename
        if target.is_symlink():
            raise ValueError('拒绝写入符号链接：'+str(target))
        if target.exists() and target.read_bytes() != data:
            raise ValueError('文件已存在且内容不同，保留用户修改：'+str(target))
    destination.mkdir(parents=True,exist_ok=True)
    for filename, data in outputs.items():
        target = destination / filename
        if not target.exists():
            with target.open('xb') as stream:
                stream.write(data)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input',required=True,type=Path,help='data-smokes.json路径')
    parser.add_argument('--output',required=True,type=Path,help='库外输出目录，不自动安装')
    args=parser.parse_args()
    try:
        result=generate(args.input,args.output)
    except (OSError,ValueError,KeyError,TypeError) as error:
        parser.exit(1,f'生成失败：{error}\n')
    print(f"共{result['total']}条；调试初稿{result['drafts']}条；待补{result['pending']}条。未安装或执行。")


if __name__=='__main__':
    main()
