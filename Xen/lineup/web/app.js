/* 手机端只发送当前用户操作；网络恢复不重放写请求。 */
(function (root) {
  'use strict';
  const MAX_AGE = 1500;
  function previewValid(state, elapsed, available) {
    const p = state && state.preview;
    return Boolean(available && state.running && state.mode === 'browse' && state.locked_id &&
      (state.frame_mode !== 'roi' || state.locating === true) &&
      ['live', 'ready', 'connected'].includes(state.source_status) && p && p.status === 'valid' &&
      p.recipe_id === state.locked_id && p.frame_id && p.url && Number.isFinite(p.age_ms) &&
      p.age_ms >= 0 && Number.isFinite(elapsed) && elapsed >= 0 && p.age_ms + elapsed <
      Math.min(MAX_AGE, Number.isFinite(p.max_age_ms) && p.max_age_ms >= 0 ? p.max_age_ms : MAX_AGE));
  }
  // 图片到达时同时校验请求时证据和当前证据，不能借新状态延长旧图寿命。
  function imageResultValid(original, elapsed, current) {
    const p = original?.preview, next = current.state?.preview;
    return Boolean(current.valid && previewValid(original, elapsed, true) &&
      original.epoch === current.state.epoch && p.frame_id === next?.frame_id &&
      p.recipe_id === next?.recipe_id && p.url === next?.url);
  }
  const teamLabel = value => ({ T: 'T', CT: 'CT', ANY: '通用', UNCLASSIFIED: '未分类', UNKNOWN: '未知' })[value] || '未分类';
  function filterRecipes(recipes, filters) {
    return recipes.filter(r => !r.draft && r.compatible !== false && Object.entries(filters).every(([k, v]) => {
      if (!v || (k === 'team' && v === 'UNKNOWN')) return true;
      if (k === 'team') return [v, 'ANY', 'UNCLASSIFIED'].includes(r.team || 'UNCLASSIFIED');
      return r[k] === v;
    }));
  }
  function practiceRecipes(recipes, filters, practice, kind) {
    const list = filterRecipes(recipes, filters);
    const ids = practice?.[kind === 'favorites' ? 'favorites' : 'queue'] || [];
    return kind === 'all' ? list : list.filter(r => ids.includes(r.id));
  }
  function actionPhase(buttons, direction, jump, duration) {
    return { buttons: buttons ? buttons.split('+') : [], movement: direction ? direction.split('+') : [],
      jump: Boolean(jump), duration_ms: String(duration).trim() === '' ? null : Number(duration) };
  }
  function actionSummary(action, status) {
    if (!action) return '尚无结构化动作；投掷说明仅供人工阅读。';
    const phases = action.phases || [];
    const timing = phases.some(p => p.duration_ms === null) ? '时长未填写，仅设计' : '时长已填写';
    const simulation = status?.simulation?.supported ? '可作阶段模拟；尚未据此证明模拟已通过' : '阶段模拟不可用或时序未完整';
    const hardware = status?.hardware?.supported ? '后端具备所需动作能力，不代表真实效果已验证' : status?.hardware?.available ? '后端缺少所需能力，不能降级执行' : '尚未取得可用后端能力';
    return `${phases.length} 个阶段 · ${timing} · ${simulation} · ${hardware}。跳跃使用 Space，原滚轮绑定不变。真实投掷未验证；中止已按住动作时，释放可能投出道具。`;
  }
  function executionSummary(execution) {
    if (!execution) return '执行状态：尚未收到运行时状态。标定状态：未知。真实设备/游戏效果待验证。';
    const states={idle:'待命',aligning:'正在对准',aligned:'已对准',throwing:'动作进行中',complete:'动作序列结束',completed:'动作序列结束',cancelled:'已中止',stopped:'已停止',fault:'故障',disabled:'未启用'};
    const state=String(execution.state || 'unknown').toLowerCase();
    const calibration=execution.calibration_status;
    const raw=typeof calibration==='string'?calibration:calibration?.status || (calibration?.valid===true?'valid':calibration?.missing===true?'missing':'unknown');
    const cal=({valid:'已有有效标定',missing:'缺少标定',invalid:'标定无效',unknown:'未知'})[raw] || '未知';
    const reported=execution.capabilities || {};
    const capabilities={...reported,left:reported.left ?? reported.left_button,right:reported.right ?? reported.right_button};
    const names={relative_move:'相对位移',left:'左键',right:'右键',movement:'方向移动',jump:'Space跳跃'};
    const supported=Object.entries(names).filter(([key])=>capabilities[key]===true).map(([,label])=>label);
    const reason=[execution.reason,typeof calibration==='object'?calibration?.reason:''].filter(Boolean).join('；');
    return `执行状态：${states[state] || '未知'} · 标定：${cal} · 后端能力：${supported.join('、') || '尚未确认'}` + (reason?' · '+reason:'') + '。真实设备/游戏效果待验证；中止释放已按住攻击键可能直接投出道具。';
  }
  function canLocate(state) {
    return Boolean(state?.running && state.locked_id && state.mode === 'browse');
  }
  function locationHint(state) {
    if (!state?.locked_id) return '先选择点位并确认锁定，按站位说明人工移到附近；视觉匹配不等于三维站位正确。';
    if (!state.locating) return '请按局部参考和站位说明人工就位、转向参考附近，再按“已就位，定位”（快捷键待绑定）。';
    if (state.location_status === 'valid') return '当前局部画面匹配有效；视觉匹配不等于三维站位正确，请人工核对站位和姿态。';
    return '正在定位已锁配方；未识别或目标移出 ROI 时，请按参考人工转向。视觉匹配不等于三维站位正确。';
  }
  function startupSummary(start, preview) {
    const geometry = preview?.source_mapping_verified === true ? '源几何映射已验证' : '源几何未知/未验证，仅局部像素';
    return `配置来源：${start.config_path || '未知'} · 采集：${start.source || '未知'} · 配置 ROI ${start.configured_roi_width || '?'}×${start.configured_roi_height || '?'} · ${geometry} · GSI：${start.gsi_mode || '未知'}` + (start.config_error || start.capture_error ? ' · ' + [start.config_error,start.capture_error].filter(Boolean).join('；') : '');
  }
  function contextView(context) {
    const c = context || {}, team = ['T', 'CT'].includes(c.team) ? c.team : 'UNKNOWN';
    return { mode: c.mode === 'manual' ? 'manual' : 'auto', map: c.map || '', team,
      identity: c.identity_confirmed ? '已确认本地玩家身份' : '本地身份未知；不会把被观战玩家当成本地阵营',
      age: Number.isFinite(c.age_ms) && c.age_ms >= 0 ? Math.round(c.age_ms) + ' ms' : '未知' };
  }
  function sameJson(left, right) {
    if (left === right) return true;
    if (!left || !right || typeof left !== 'object' || typeof right !== 'object' || Array.isArray(left) !== Array.isArray(right)) return false;
    const keys=Object.keys(left); return keys.length===Object.keys(right).length && keys.every(key=>Object.hasOwn(right,key) && sameJson(left[key],right[key]));
  }
  function settleBatch(state, pending, edited) {
    if (!pending || state.busy) return pending;
    if (state.capture_status === 'saved' && pending.every(item => {
      const saved = (state.recipes || []).find(r => r.id === item.id);
      return saved && Object.entries(item).every(([key, value]) => sameJson(saved[key], value));
    })) {
      for (const item of pending) {
        if (JSON.stringify(edited.get(item.id)) === JSON.stringify(item)) edited.delete(item.id);
      }
      return null;
    }
    return ['error', 'cancelled'].includes(state.capture_status) ? null : pending;
  }
  function createController({ request, now = () => performance.now(), changed = () => {} }) {
    let state = null, received = 0, connected = false, visible = true, busy = false, refreshing = false, generation = 0, serial = 0;
    const clientId = Math.random().toString(36).slice(2) + Date.now().toString(36);
    const model = () => ({ state, connected, visible, busy, elapsed: now() - received,
      valid: previewValid(state, now() - received, connected && visible && !busy) });
    const emit = () => changed(model());
    async function refresh() {
      if (!visible || busy || refreshing) return;
      refreshing = true;
      const ticket = ++generation, started = now();
      try {
        const result = await request('/api/state');
        if (ticket !== generation || !visible) return;
        state = result; received = started; connected = true; emit();
      } catch (_) { if (ticket === generation) { connected = false; emit(); } } finally { refreshing = false; }
    }
    async function command(action, args = {}) {
      if (!connected || !visible || busy || !state) return false;
      busy = true; ++generation; emit();
      const ticket = generation, started = now();
      try {
        const result = await request('/api/command', { ...args, action, epoch: state.epoch,
          revision: state.revision, request_id: clientId + '-' + (++serial) });
        if (ticket === generation && visible) { state = result; received = started; connected = true; }
        return true;
      } catch (error) {
        connected = false;
        throw error;
      } finally { busy = false; emit(); }
    }
    function suspend() { visible = false; connected = false; ++generation; emit(); }
    async function resume() { visible = true; connected = false; emit(); await refresh(); }
    function offline() { connected = false; ++generation; emit(); }
    return { model, refresh, command, suspend, resume, offline, tick: emit };
  }
  const labels = { live: '画面源已连接', connected: '画面源已连接', ready: '画面源已连接', disconnected: '画面源已断开', stopped: '已停止', unknown: '画面源未知', valid: '匹配可靠', not_found: '未找到参照', unreliable: '匹配不可靠', expired: '已过期', invalid: '无效', invalid_frame: '画面无效，请检查完整画面源', no_reference: '尚无可用参考', pending: '等待新帧', idle: '等待采集', waiting: '等待下一张有效新帧', saving: '正在保存，请稍候', cancelled: '采集已取消', saved: '已保存', error: '失败，请查看错误信息' };
  const textStatus = value => labels[value] || '未知状态';
  const api = { executionSummary, actionPhase, actionSummary, canLocate, locationHint, startupSummary, practiceRecipes, previewValid, imageResultValid, filterRecipes, createController, textStatus, settleBatch, contextView, teamLabel };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  if (!root.document) return;
  const $ = id => document.getElementById(id);
  let selected = '', view = null, pendingImage = '', shownImage = '', imageTicket = 0, scopeInitialized = false, contextSignature = null;
  let annotation = { id: '', aim: [0.5, 0.5], rect: [0.1, 0.1, 0.8, 0.6], corner: null, tool: 'aim' };
  const edited = new Map();
  let pendingBatch = null, lastStartup = null;

  function safeUrl(value) { try { const u = new URL(value, location.href); return u.origin === location.origin ? u.href : ''; } catch (_) { return ''; } }
  function setImage(id, url) { const el = $(id), safe = url ? safeUrl(url) : ''; if (safe) { if (el.src !== safe) el.src = safe; } else el.removeAttribute('src'); }
  function clearPreview() { ++imageTicket; pendingImage = ''; shownImage = ''; $('preview').hidden = true; $('preview').removeAttribute('src'); $('preview-empty').hidden = false; }
  async function request(url, body) {
    const abort = new AbortController(), timer = setTimeout(() => abort.abort(), 4000);
    try {
      const response = await fetch(url, { method: body ? 'POST' : 'GET', cache: 'no-store', signal: abort.signal,
        headers: body ? { 'Content-Type': 'application/json' } : {}, body: body ? JSON.stringify(body) : undefined });
      if (!response.ok) throw new Error(response.status === 409 ? '状态已更新，请重新选择操作。' : '请求未完成，请核对当前状态。');
      return await response.json();
    } finally { clearTimeout(timer); }
  }
  const controller = createController({ request, changed: render });
  async function send(action, args) {
    $('message').textContent = '';
    if (['lock', 'locate', 'cancel', 'mode', 'stop'].includes(action)) clearPreview();
    try { return await controller.command(action, args); }
    catch (e) { $('message').textContent = e.message + ' 未自动重试；请核对是否已生效。'; return false; }
    finally { await controller.refresh(); }
  }
  function filters() { return Object.fromEntries(['map', 'region', 'target', 'grenade', 'team'].map(k => [k, $(k).value])); }
  function contextReason(reason) {
    const reasons = { 'GSI context confirmed': '已确认可信游戏上下文。', 'Manual override': '正在使用手动地图与阵营。', 'Automatic GSI context': '已恢复自动游戏上下文。', 'GSI unavailable; manual browsing': '尚无可用游戏上下文，可人工浏览。', 'GSI expired; manual browsing': '游戏消息已过期，可人工浏览。', 'GSI unknown or expired; manual browsing': '游戏上下文未知或过期，可人工浏览。', identity_unknown: '本地玩家身份尚未确认。', local_identity_unknown: '本地玩家身份尚未确认。', stale: '游戏消息已过期，自动上下文已撤销。', gsi_stale: '游戏消息已过期，自动上下文已撤销。', no_gsi: '尚未收到游戏消息。', disconnected: '游戏上下文已断开。', team_unknown: '本地阵营未知，可手动选择。', map_unknown: '地图未知，可手动选择。', context_changed: '游戏上下文已变化，请重新确认配方。', incompatible_lock: '原锁定配方已不兼容，已解除锁定，请重新确认。', locked_recipe_incompatible: '原锁定配方已不兼容，已解除锁定，请重新确认。' };
    return reasons[reason] || (/[\u4e00-\u9fff]/.test(reason) ? reason : '游戏上下文暂不可确认，请核对自动状态或手动选择。');
  }
  function optionList(el, values, first) {
    const old = el.value, next = JSON.stringify(values);
    if (el.dataset.options === next) return;
    el.dataset.options = next; el.replaceChildren();
    if (first !== null) el.add(new Option(first, ''));
    values.forEach(v => el.add(new Option(typeof v === 'string' ? v : v.text, typeof v === 'string' ? v : v.id)));
    if ([...el.options].some(o => o.value === old)) el.value = old;
  }
  function render(model) {
    view = model; const s = model.state;
    $('connection').textContent = !model.visible ? '已暂停' : model.connected ? '已连接' : '连接中 / 离线';
    document.querySelectorAll('button').forEach(b => b.disabled = !model.connected || model.busy);
    if (!s) return;
    $('source').textContent = textStatus(s.source_status);
    $('execution-status').textContent = executionSummary(s.execution);
    if (s.startup) lastStartup = s.startup;
    if (lastStartup) {
      const start = lastStartup;
      $('startup').textContent = startupSummary(start, s.preview);
    }
    $('run').textContent = s.running ? '停止识别 / 采集' : '开始识别 / 采集';
    $('browse').hidden = s.mode !== 'browse'; $('capture').hidden = s.mode !== 'capture';
    $('browse-mode').classList.toggle('active', s.mode === 'browse'); $('capture-mode').classList.toggle('active', s.mode === 'capture');
    if (s.error) $('message').textContent = s.error;
    else if (s.export_path) $('message').textContent = '证据已导出至服务所在电脑：' + s.export_path;
    $('capture-status').textContent = textStatus(s.capture_status);
    const recipes = s.recipes || [];
    if (pendingBatch) {
      const settled = settleBatch(s, pendingBatch, edited);
      if (!settled) $('drafts').dataset.signature = '';
      pendingBatch = settled;
    }
    const context = contextView(s.context);
    ['map', 'region', 'target', 'grenade'].forEach(k => {
      const values = recipes.filter(r => !r.draft).map(r => r[k]).filter(Boolean);
      if (k === 'map') values.push(context.map, s.context?.auto_map || '', $('map').value);
      optionList($(k), [...new Set(values.filter(Boolean))].sort(), '全部 / 人工选择');
    });
    if (s.context) {
      const nextContext = JSON.stringify([context.mode, context.map, context.team, s.scope?.region || '']);
      if (nextContext !== contextSignature) {
        $('map').value = context.map; $('team').value = context.team;
        if ([...$('region').options].some(o => o.value === (s.scope?.region || ''))) $('region').value = s.scope?.region || '';
        contextSignature = nextContext;
      }
    }
    $('context-mode').textContent = context.mode === 'manual' ? '手动覆盖' : '自动识别';
    $('context-summary').textContent = '当前地图：' + (context.map || '未知') + ' · 本地阵营：' + teamLabel(context.team);
    const gsiLabels = { fresh: '新鲜', valid: '新鲜', ready: '新鲜', stale: '已过期', expired: '已过期', disconnected: '未连接', missing: '尚未收到', unavailable: '不可用', unknown: '未知', invalid: '无效', identity_unknown: '身份未知', disabled: '未启用', invalid_payload: '消息无效', identity_mismatch: '本地身份不匹配', player_inactive: '本地玩家不在活动状态', unknown_weapon: '武器未知', reloading: '正在换弹', empty: '弹药为空', clock_rejected: '消息时间校验失败', out_of_order: '消息乱序', duplicate: '重复消息', counter_exhausted: '消息计数无效', non_firearm: '当前持非枪械物品' };
    $('context-freshness').textContent = 'GSI：' + (gsiLabels[(s.context?.gsi_status || '').toLowerCase()] || '未知') + ' · 消息年龄 ' + context.age + ' · ' + context.identity;
    $('context-reason').textContent = context.mode === 'manual' ? '地图与阵营使用手动选择；恢复自动后使用最新可信游戏上下文。' : '自动地图：' + (s.context?.auto_map || '未知') + ' · 自动本地阵营：' + teamLabel(s.context?.auto_team || 'UNKNOWN') + '。未分类配方仍保留，可人工查看并补分类。';
    if (s.context?.reason) $('context-reason').textContent += ' ' + contextReason(s.context.reason);
    if (s.lock_reason && !s.locked_id) $('context-reason').textContent += ' 原锁定配方因上下文或分类变化已解除，请重新确认；不会自动替换配方。';
    if (!scopeInitialized && !s.context) {
      for (const field of ['map', 'region']) {
        const value = s.scope?.[field] || '';
        if ([...$(field).options].some(o => o.value === value)) $(field).value = value;
      }
      scopeInitialized = true;
    }
    const list = practiceRecipes(recipes, filters(), s.practice, $('practice-filter').value);
    if (!list.some(r => r.id === selected)) selected = list.some(r => r.id === s.locked_id) ? s.locked_id : list[0]?.id || '';
    const signature = JSON.stringify([list, selected, s.locked_id]);
    if ($('candidates').dataset.signature !== signature) {
      $('candidates').dataset.signature = signature; $('candidates').replaceChildren();
      list.forEach(r => {
        const card = document.createElement('button'); card.className = 'card' + (r.id === selected ? ' selected' : '');
        card.setAttribute('aria-pressed', String(r.id === selected));
        if (r.standpoint_url) { const img = document.createElement('img'); img.src = safeUrl(r.standpoint_url); img.alt = '站位 ' + r.standpoint_id; card.append(img); }
        const title = document.createElement('strong'); title.textContent = r.name || r.target;
        const detail = document.createElement('small'); detail.textContent = `${r.target} · ${r.grenade} · ${teamLabel(r.team)} · ${r.standpoint_id}`;
        card.append(title, detail); card.onclick = () => { selected = r.id; render(controller.model()); }; $('candidates').append(card);
      });
    }
    $('candidates').querySelectorAll('button').forEach(b => b.disabled = model.busy);
    const suggestions = (s.suggestions || []).filter(x => x.status === 'valid').map(x => recipes.find(r => r.id === x.id)?.standpoint_id || x.id);
    const scopeMatches = (!s.scope || (s.scope.map === $('map').value && s.scope.region === $('region').value));
    $('suggestion').textContent = (scopeMatches && suggestions.length ? '视觉建议站位：' + [...new Set(suggestions)].join('、') : '站位识别未知，可人工筛选。') + (list.length === 1 ? ' 当前唯一候选，请确认。' : ` 当前 ${list.length} 个候选。`) + (!scopeMatches ? ' 视觉搜索范围尚未同步，请再次选择地图或区域。' : s.search_limited ? ' 当前区域参考较多，正在分批轮转搜索。' : s.searching ? ' 正在搜索视觉候选。' : '');
    if (s.frame_mode === 'roi') $('suggestion').textContent = `当前 ${list.length} 个配方；选择点位并人工就位后，定位仅识别已锁配方。`;
    const r = recipes.find(x => x.id === (s.locked_id || selected)); $('detail').hidden = !r;
    if (r) {
      $('throw-action-status').textContent = actionSummary(r.throw_action, r.action_status);
      $('recipe-title').textContent = r.name || r.id; $('locked').textContent = s.locked_id ? '已锁定' : '待确认';
      $('recipe-info').replaceChildren();
      for (const [title, value] of [['目标 / 道具', `${r.target || '未命名'} / ${r.grenade || '未知'}`], ['站位 / 姿态', `${r.standpoint_id} / ${r.stance || '未知'}`], ['配方阵营', teamLabel(r.team)], ['靠位说明', r.instructions], ['投掷说明', r.throw_instructions], ['人工结果', ({ success: '成功', failure: '失败', unverified: '未验证' })[r.validation] || r.validation || '未验证'], ['练习条件', r.conditions], ['备注', r.notes]]) {
        const dt = document.createElement('dt'), dd = document.createElement('dd'); dt.textContent = title; dd.textContent = value || '未填写'; $('recipe-info').append(dt, dd);
      }
      setImage('standpoint', r.standpoint_url); setImage('reference', r.reference_url);
    }
    $('locate').disabled ||= !canLocate(s);
    $('location-hint').textContent = locationHint(s);
    $('lock').disabled ||= !selected; $('previous').disabled = $('next').disabled = model.busy || list.length < 2;
    $('cancel').disabled ||= !s.locked_id; $('take').disabled ||= !s.running || ['pending', 'waiting', 'saving'].includes(s.capture_status);
    const practice = s.practice || {};
    $('favorite').textContent = (practice.favorites || []).includes(r?.id) ? '取消收藏' : '收藏配方';
    $('practice-queue').textContent = (practice.queue || []).includes(r?.id) ? '移出练习清单' : '加入练习清单';
    for (const id of ['favorite','practice-queue','practice-success','practice-failure','practice-skip']) $(id).disabled ||= !r || Boolean(practice.error) || !s.practice;
    $('practice-status').textContent = practice.error ? '练习记录读取失败：' + practice.error : `累计人工记录 ${practice.history_count || 0} 次；不代表自动落点判断。`;
    $('practice-history').replaceChildren();
    (practice.history || []).filter(e => e.recipe?.id === r?.id).slice(-10).reverse().forEach(e => {
      const row = document.createElement('li');
      row.textContent = `${new Date(e.recorded_at_unix_ms).toLocaleString()} · ${({success:'成功',failure:'失败',skip:'跳过'})[e.command.outcome]} · 参考 ${e.recipe.reference_id || '未知'} · ${e.command.conditions || e.recipe.conditions || '条件未填写'}`;
      $('practice-history').append(row);
    });
    renderPreview(model);
    const ids = recipes.map(r => ({ id: r.id, text: (r.name || '草稿') + ' · ' + r.id }));
    optionList($('annotation-id'), ids, null);
    if (annotation.id !== $('annotation-id').value) resetAnnotation();
    if (s.mode === 'capture') renderEditors(recipes);
  }
  function renderPreview(model) {
    const s = model.state, p = s.preview || {};
    $('preview-status').textContent = model.valid ? '同帧结果' : !model.connected || !model.visible ? '连接中断 / 已撤标' : textStatus(p.status || 'invalid');
    $('metrics').textContent = (p.source_mapping_verified === true ? '源几何映射已验证。' : '仅局部像素；源几何映射未知/未验证。') + `本地帧龄 ${Number.isFinite(p.age_ms) ? Math.round(p.age_ms + model.elapsed) + ' ms' : '未知'} · 处理 ${Number.isFinite(p.processing_ms) ? p.processing_ms.toFixed(1) + ' ms' : '未知'} · 丢弃 ${s.dropped || 0} 帧。${p.source_time_known ? '源时间已提供' : '源时间未知'}；手机端到端延迟待实测。`;
    if (!model.valid) { clearPreview(); return; }
    const key = `${s.epoch}/${p.recipe_id}/${p.frame_id}`;
    if (key === pendingImage || key === shownImage) return;
    clearPreview(); pendingImage = key;
    const ticket = imageTicket, img = new Image(), imageStarted = performance.now();
    img.onload = () => {
      const m = controller.model();
      const imageElapsed = model.elapsed + performance.now() - imageStarted;
      if (ticket !== imageTicket || !imageResultValid(s, imageElapsed, m)) return;
      $('preview').src = img.src; $('preview').hidden = false; $('preview-empty').hidden = true; shownImage = key; pendingImage = '';
    };
    img.onerror = () => { if (ticket === imageTicket) { clearPreview(); $('preview-status').textContent = '预览不可用 / 已撤标'; } };
    img.src = safeUrl(p.url);
  }
  function renderEditors(recipes) {
    const signature = JSON.stringify(recipes);
    if ($('drafts').dataset.signature === signature) return;
    $('drafts').dataset.signature = signature; $('drafts').replaceChildren();
    recipes.forEach(r => {
      const row = document.createElement('div'); row.className = 'edit-row'; const title = document.createElement('h3'); title.textContent = r.id + (r.draft ? ' · 草稿' : ''); row.append(title);
      for (const [field, label] of [['name','名称'],['target','目标'],['grenade','道具类型'],['notes','备注'],['throw_instructions','投掷说明'],['validation','人工投掷结果'],['conditions','练习条件'],['team','配方阵营']]) {
        const wrap = document.createElement('label'); wrap.textContent = label;
        const input = document.createElement(['validation', 'team'].includes(field) ? 'select' : 'input');
        if (field === 'validation') [['unverified','未验证'],['success','成功'],['failure','失败']].forEach(([v,t]) => input.add(new Option(t,v)));
        if (field === 'team') [['UNCLASSIFIED','未分类'],['ANY','通用'],['T','T'],['CT','CT']].forEach(([v,t]) => input.add(new Option(t,v)));
        input.value = edited.get(r.id)?.[field] ?? r[field] ?? (field === 'validation' ? 'unverified' : field === 'team' ? 'UNCLASSIFIED' : ''); input.maxLength = 2000;
        input.oninput = () => { const item = edited.get(r.id) || { id: r.id, ...Object.fromEntries(['name','target','grenade','notes','throw_instructions','validation','conditions','team'].map(k => [k,r[k] || (k === 'validation' ? 'unverified' : k === 'team' ? 'UNCLASSIFIED' : '')])) }; item[field] = input.value; edited.set(r.id,item); };
        wrap.append(input); row.append(wrap);
      }
      renderActionEditor(row, r);
      $('drafts').append(row);
    });
  }
  function renderActionEditor(row, recipe) {
    const panel = document.createElement('div'); panel.className = 'action-editor';
    const title = document.createElement('h4'); title.textContent = '投掷动作设计'; panel.append(title);
    const content = document.createElement('div'); panel.append(content); row.append(panel);
    const hasActionEdit = () => Object.hasOwn(edited.get(recipe.id) || {}, 'throw_action');
    let action = hasActionEdit() ? edited.get(recipe.id).throw_action : recipe.throw_action ?? null;
    if (action) action = JSON.parse(JSON.stringify(action));
    const save = () => { const item = edited.get(recipe.id) || {id:recipe.id}; item.throw_action = action ? JSON.parse(JSON.stringify(action)) : null; edited.set(recipe.id,item); };
    function draw() {
      content.replaceChildren();
      const note = document.createElement('p'); note.className='muted'; note.textContent = actionSummary(action, hasActionEdit() ? null : recipe.action_status); content.append(note);
      if (!action) {
        const create = document.createElement('button'); create.textContent='设计投掷动作'; create.type='button';
        create.onclick=()=>{action={schema:1,type:'phases',phases:[actionPhase('','','',''),actionPhase('','','','')]};save();draw();}; content.append(create);return;
      }
      const profileWrap=document.createElement('label');profileWrap.textContent='移动条件';const profile=document.createElement('select');
      [['unspecified','未填写'],['stationary','原地'],['step','一步'],['runup','助跑'],['already_moving','已在移动']].forEach(([value,text])=>profile.add(new Option(text,value)));profile.value=action.movement_profile || 'unspecified';profile.oninput=()=>{action.movement_profile=profile.value;save();};profileWrap.append(profile);content.append(profileWrap);
      const guide=document.createElement('p'); guide.className='muted'; guide.textContent='每阶段声明完整按住集合：未选的按键会释放。时长留空保持设计态，不猜跳投时序；最后阶段必须全部释放，无等待时请明确填 0。';content.append(guide);
      action.phases.forEach((phase,index)=>{
        const line=document.createElement('div');line.className='filters';
        const heading=document.createElement('strong');heading.textContent=`阶段 ${index+1}`;line.append(heading);
        const controls={};
        for(const [key,label,options] of [
          ['buttons','鼠标按住',[['','全部释放'],['left','左键'],['right','右键'],['left+right','双键']]],
          ['movement','移动按住',[['','静止 / 全释放'],['forward','前'],['back','后'],['left','左'],['right','右'],['forward+left','左前'],['forward+right','右前'],['back+left','左后'],['back+right','右后']]]]) {
          const wrap=document.createElement('label');wrap.textContent=label;const input=document.createElement('select');
          options.forEach(([value,text])=>input.add(new Option(text,value)));
          const values=phase[key] || []; const existing=options.find(([value])=>value.split('+').filter(Boolean).length===values.length && value.split('+').filter(Boolean).every(v=>values.includes(v)));
          input.value=existing?.[0] || ''; controls[key]=input;wrap.append(input);line.append(wrap);
        }
        const jumpWrap=document.createElement('label');jumpWrap.textContent='按住跳跃（Space）';const jump=document.createElement('input');jump.type='checkbox';jump.checked=phase.jump;jumpWrap.append(jump);line.append(jumpWrap);
        const timeWrap=document.createElement('label');timeWrap.textContent='持续毫秒（不填=待设计）';const duration=document.createElement('input');duration.type='number';duration.min='0';duration.max='2000';duration.step='1';duration.value=phase.duration_ms ?? '';timeWrap.append(duration);line.append(timeWrap);
        const change=()=>{action.phases[index]=actionPhase(controls.buttons.value,controls.movement.value,jump.checked,duration.value);save();note.textContent=actionSummary(action,null);};
        for(const control of [controls.buttons,controls.movement,jump,duration]) control.oninput=change;
        if(index<action.phases.length-1){const remove=document.createElement('button');remove.textContent='删除阶段';remove.type='button';remove.onclick=()=>{action.phases.splice(index,1);save();draw();};line.append(remove);}
        content.append(line);
      });
      const add=document.createElement('button');add.textContent='在最终释放前添加阶段';add.type='button';add.disabled=action.phases.length>=16;add.onclick=()=>{action.phases.splice(action.phases.length-1,0,actionPhase('','','',''));save();draw();};content.append(add);
      const clear=document.createElement('button');clear.textContent='移除动作设计';clear.type='button';clear.onclick=()=>{action=null;save();draw();};content.append(clear);
    }
    draw();
  }
  function resetAnnotation() {
    annotation = { id: $('annotation-id').value, aim: [0.5,0.5], rect:[0.1,0.1,0.8,0.6], corner:null, tool:'aim' };
    const r = view?.state?.recipes.find(x => x.id === annotation.id); setImage('annotation-image',r?.reference_url);
    if (Array.isArray(r?.aim) && r.aim.length === 2) annotation.aim = r.aim.slice();
    if (Array.isArray(r?.static_rect) && r.static_rect.length === 4) annotation.rect = r.static_rect.slice();
    $('aim-dot').hidden = $('static-box').hidden = true; $('annotation-status').textContent = '未修改；保留记录的瞄点和静态区域，可点选调整。';
  }
  ['map','team','region','target','grenade'].forEach(k => $(k).onchange = () => {
    const chosen = filters();
    render(controller.model());
    // 地图只有上下文一个写入口；区域仍限定视觉搜索。失败和恢复均不重放。
    if (k === 'map' || k === 'team') send('context', { mode: 'manual', map: chosen.map, team: chosen.team });
    else if (k === 'region') send('scope', { map: view.state.context?.map || chosen.map, region: chosen.region });
  });
  $('context-auto').onclick = () => send('context', { mode: 'auto' });
  for (const [id,delta] of [['previous',-1],['next',1]]) $(id).onclick = () => { const list = practiceRecipes(view.state.recipes,filters(),view.state.practice,$('practice-filter').value); selected = list[(list.findIndex(r => r.id === selected) + delta + list.length) % list.length]?.id || ''; render(controller.model()); };
  $('locate').onclick = () => send('locate');
  $('lock').onclick = () => send('lock',{id:selected}); $('cancel').onclick = () => send('cancel');
  $('browse-mode').onclick = () => send('mode',{value:'browse'}); $('capture-mode').onclick = () => send('mode',{value:'capture'});
  $('run').onclick = () => send(view.state.running ? 'stop' : 'start'); $('export').onclick = () => send('export');
  $('take').onclick = () => send('capture', {map:$('capture-map').value,region:$('capture-region').value,standpoint_id:$('capture-standpoint').value,stance:$('capture-stance').value,instructions:$('capture-instructions').value});
  $('save-batch').onclick = async () => {
    if (!edited.size || pendingBatch) return;
    pendingBatch = [...edited.values()].map(item => ({ ...item }));
    if (!await send('update', { items: pendingBatch })) pendingBatch = null;
  };
  $('annotation-id').onchange = resetAnnotation;
  $('aim-tool').onclick = () => { annotation.tool = 'aim'; annotation.corner = null; $('annotation-status').textContent = '请点击瞄点。'; };
  $('rect-tool').onclick = () => { annotation.tool = 'rect'; annotation.corner = null; $('annotation-status').textContent = '请点击静态区域左上角，再点击右下角。'; };
  $('annotation-image').onclick = e => {
    const box = e.currentTarget.getBoundingClientRect(), point = [Math.max(0,Math.min(1,(e.clientX-box.left)/box.width)), Math.max(0,Math.min(1,(e.clientY-box.top)/box.height))];
    if (annotation.tool === 'aim') { annotation.aim = point; $('aim-dot').hidden = false; $('aim-dot').style.left = point[0]*100+'%'; $('aim-dot').style.top = point[1]*100+'%'; $('annotation-status').textContent = '瞄点已调整，待保存。'; }
    else if (!annotation.corner) { annotation.corner = point; $('annotation-status').textContent = '请点击矩形另一角。'; }
    else { const a = annotation.corner; annotation.rect = [Math.min(a[0],point[0]),Math.min(a[1],point[1]),Math.abs(a[0]-point[0]),Math.abs(a[1]-point[1])]; annotation.corner = null; const [x,y,w,h] = annotation.rect; Object.assign($('static-box').style,{left:x*100+'%',top:y*100+'%',width:w*100+'%',height:h*100+'%'}); $('static-box').hidden = false; $('annotation-status').textContent = '静态矩形已调整，待保存。'; }
  };
  $('save-annotation').onclick = async () => { if (!annotation.id || annotation.rect[2] <= 0 || annotation.rect[3] <= 0) return; if (await send('annotate',{id:annotation.id,aim:annotation.aim,static_rect:annotation.rect})) $('annotation-status').textContent = '校准已提交，请查看上方保存状态。'; };
  $('practice-filter').onchange = () => render(controller.model());
  const practiceId = () => view?.state?.locked_id || selected;
  for (const [button,action,key] of [['favorite','favorite','favorites'],['practice-queue','practice_queue','queue']]) {
    $(button).onclick = () => send(action,{id:practiceId(),enabled:!(view.state.practice?.[key] || []).includes(practiceId())});
  }
  for (const outcome of ['success','failure','skip']) $('practice-' + outcome).onclick = () => send('practice_record',{id:practiceId(),outcome,conditions:$('practice-conditions').value});
  document.addEventListener('visibilitychange', () => { if (document.hidden) controller.suspend(); else controller.resume(); });
  root.addEventListener('offline', controller.offline); root.addEventListener('online', () => controller.resume());
  root.addEventListener('pagehide', controller.suspend); root.addEventListener('pageshow', () => { if (!document.hidden) controller.resume(); });
  setInterval(() => controller.refresh(),350); setInterval(() => { if (view?.state) renderPreview(controller.model()); },100);
  if (document.hidden) controller.suspend(); else controller.refresh();
})(typeof window !== 'undefined' ? window : globalThis);
