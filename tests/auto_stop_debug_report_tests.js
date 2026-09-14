// 执行报告模板内的真实脚本；假DOM仅记录显示与canvas操作，不连接浏览器或设备。
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const header = fs.readFileSync(process.argv[2] || path.join(__dirname, '../Xen/auto_stop_probe/debug_report_internal.h'), 'utf8');
const start = header.indexOf('</script><script>') + '</script><script>'.length;
const end = header.indexOf('</script></html>', start);
assert(start > 16 && end > start, '必须找到生产HTML脚本');
const script = header.slice(start, end);
function render(analysis) {
    const elements = new Map();
    class Element {
        constructor() { this.textContent = ''; this.children = []; this.rows = []; this.value = '0'; }
        append(child) { this.children.push(child); }
        replaceChildren(child) { this.children = [child]; }
        createTHead() { return this; }
        createTBody() { return this; }
        insertRow() { const row = new Element(); this.rows.push(row); return row; }
        insertCell() { const cell = new Element(); this.children.push(cell); return cell; }
        getContext() {
            if (this.context) return this.context;
            this.context = {strokes: 0, fills: [], arcs: [], beginPath() {}, moveTo() {}, lineTo() {},
                clearRect() {}, fillText() {}, setLineDash() {},
                stroke() { ++this.strokes; },
                arc(...args) { this.arcs.push(args); },
                fill() { this.fills.push(this.fillStyle); }};
            return this.context;
        }
    }
    const document = {
        getElementById(id) { if (!elements.has(id)) elements.set(id, new Element()); return elements.get(id); },
        createElement() { return new Element(); },
        createTextNode(text) { return {textContent: text}; }
    };
    document.getElementById('data').textContent = JSON.stringify(analysis);
    vm.runInNewContext(script, {document}, {timeout: 2000});
    return id => document.getElementById(id);
}
const summary = {count: 99, valid_count: 88, latest_delta_ms: 7, latest_grade: '优秀',
    mean_ms: 111, stddev_ms: 12, habit: '已计算习惯', excellent_percent: 45};
let ui = render({source: 'COMMAND_ACK_PROXY', shots: [], feedback: {timing: summary}, timings: [
    {ordinal: 1, delta_ms: 1, grade: 'ACK_INTERVAL'},
    {ordinal: 2, delta_ms: 2, grade: 'ACK_INTERVAL'}
]});
const cells = ui('timing-summary').children[0].rows[1].children.map(cell => cell.textContent);
assert.deepEqual(cells, [99, 88, '7.000', '优秀', '111.000', '12.000', '已计算习惯', '45.000'],
    '必须显示分析器汇总，不得从折线重算统计');
assert.match(ui('timing-source').textContent, /ACK 间隔.*不是物理按键间隔/);
assert.equal(ui('timing-plot').context.strokes, 2, '参考线及两个连续有效点的一段线');
assert.deepEqual(ui('timing-plot').context.fills, ['#89b5ef', '#89b5ef'], 'ACK未分类用中性蓝');
for (const invalid of [{delta_ms: 9, valid: false}, {delta_ms: 9, atomic_ambiguous: true}, {delta_ms: 9, uncertainty_crosses_boundary: true}, {delta_ms: null}]) {
    ui = render({source: 'KMBOX_MONITOR', shots: [], timings: [
        {delta_ms: -3, grade: 'EARLY'}, invalid, {delta_ms: 4, grade: 'EXCELLENT'}
    ]});
    assert.equal(ui('timing-plot').context.strokes, 1, '无效点两侧不能跨缺口连线');
    assert.deepEqual(ui('timing-plot').context.fills, ['#fbbf24', '#4ade80'], '直接使用点的既有分类颜色');
    assert.match(ui('timing-source').textContent, /人工键鼠接收间隔.*不等同物理停稳/);
}
ui = render({source: 'COMMAND_ACK_PROXY', shots: [], timings: []});
assert.match(ui('timing-empty').textContent, /暂无有效换键数据/);
assert.match(ui('timing-summary').textContent, /暂无已计算换键反馈/);
assert.equal(ui('timing-plot').context.arcs.length, 0, '空数据不得画零点');
ui = render({source: 'KMBOX_MONITOR', shots: [], timings: Array.from({length: 40}, (_, i) => ({ordinal: i + 1, delta_ms: i}))});
assert.equal(ui('timing-plot').context.arcs.length, 32, '折线仅保留最近32条');
console.log('HTML急停反馈、来源、分类颜色、空数据与断线专项通过');
