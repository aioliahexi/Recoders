// camera_server NVR 管理面前端（通过 Node BFF 代理调用 C++ 后端）
'use strict';
const $ = id => document.getElementById(id);

async function api(path, opts) {
  opts = opts || {};
  opts.headers = Object.assign({ 'Content-Type': 'application/json' }, opts.headers || {});
  const r = await fetch(path, opts);
  if (r.status === 401) { showLogin(); throw new Error('401'); }
  return r;
}
async function apiJson(path, opts) { const r = await api(path, opts); return r.json(); }

function showLogin() { $('login').style.display = 'flex'; }
function hideLogin() { $('login').style.display = 'none'; }
async function doLogin() {
  const r = await fetch('/login', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ key: $('loginKey').value }) });
  const d = await r.json();
  if (d.code === 0) { $('loginErr').textContent = ''; hideLogin(); boot(); }
  else $('loginErr').textContent = '登录失败';
}

// ---------- 导航 ----------
const PAGES = ['live', 'playback', 'photos', 'camera', 'storage', 'system', 'onvif'];
const TITLES = { live: '实时预览', playback: '录像回放', photos: '图片浏览', camera: '相机配置', storage: '存储管理', system: '系统设置', onvif: 'ONVIF' };
document.querySelectorAll('.sidebar nav a').forEach(a => a.addEventListener('click', e => { e.preventDefault(); switchPage(a.dataset.page); }));
function switchPage(p) {
  if (p !== 'live') live.stopAll();   // 离开实时预览页先断开所有 MJPEG 流，避免残留连接
  PAGES.forEach(x => {
    document.getElementById('page-' + x).classList.toggle('active', x === p);
    document.querySelectorAll('.sidebar nav a').forEach(a => a.classList.toggle('active', a.dataset.page === p));
  });
  $('pageTitle').textContent = TITLES[p];
  const fn = { live: () => live.refresh(), playback: () => pb.load(), photos: () => ph.load(), camera: () => cam.load(), storage: () => stg.load(), system: () => sys.refresh(), onvif: () => onvif.load() }[p];
  if (fn) fn();
}
setInterval(() => { $('clock').textContent = new Date().toLocaleString('zh-CN'); }, 1000);
// 实时预览页：每 5 秒检测相机列表变化（新相机注册/离线恢复），自动刷新画面
let liveCamIds = null;
setInterval(async () => {
  if (!document.getElementById('page-live').classList.contains('active')) return;
  try {
    const d = (await apiJson('/api/v1/cameras')).data;
    const ids = d.items.map(c => c.id).sort().join(',');
    if (liveCamIds !== null && liveCamIds !== ids) live.refresh();
    liveCamIds = ids;
  } catch (e) {}
}, 5000);

// ---------- 工具 ----------
function esc(s) { return String(s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c])); }
function fmtBytes(n) { if (n >= 1 << 30) return (n / (1 << 30)).toFixed(1) + ' GB'; if (n >= 1 << 20) return (n / (1 << 20)).toFixed(1) + ' MB'; if (n >= 1024) return (n >> 10) + ' KB'; return n + ' B'; }
async function loadImg(id, el, ms) {
  try {
    const r = await api('/api/v1/cameras/' + id + '/photo/latest');
    const b = await r.blob();
    if (el.src) URL.revokeObjectURL(el.src);
    el.src = URL.createObjectURL(b);
  } catch (e) {}
  if (ms) setTimeout(() => { if (document.body.contains(el)) loadImg(id, el, ms); }, ms);
}
async function fillCamSelect(sel) {
  const d = (await apiJson('/api/v1/cameras')).data;
  sel.innerHTML = '';
  d.items.forEach(c => { const o = document.createElement('option'); o.value = c.id; o.textContent = c.name; sel.appendChild(o); });
  return d.items;
}

// ---------- 实时预览 ----------
const live = {
  async refresh() {
    const d = (await apiJson('/api/v1/cameras')).data;
    const grid = $('liveGrid');
    this.stopAll();               // 先断开旧流，再重建画面（修复返回页面后抓流失败）
    grid.innerHTML = '';
    d.items.forEach(c => {
      const div = document.createElement('div'); div.className = 'cam';
      const liveFps = typeof c.fps_actual === 'number' && c.fps_actual > 0 ? ' · 实时 ' + c.fps_actual.toFixed(1) + 'fps' : '';
      const online = c.online === false ? '<span class="off">未连接</span>' : '<span class="on">在线</span>';
      div.innerHTML = '<h3>' + esc(c.name) + '<span class="tag">' + esc(c.encoder) + ' · ' + c.resolution + '@' + c.fps + 'fps' + liveFps + ' · ' +
        online + ' · ' +
        (c.recording ? '<span class="on">录像中</span>' : '<span class="off">未录像</span>') + '</span></h3>' +
        '<div class="livebox">' + (c.online === false
          ? '<div class="offline-tip">相机未连接<br><span class="hint">历史录像/照片仍可浏览</span></div>'
          : '<img id="snap' + c.id + '" alt="live" onerror="live.err(' + c.id + ')">') + '</div>' +
        '<div class="row">' +
        (c.recording ? '<button class="stop" onclick="live.rec(' + c.id + ',false)">停止录像</button>'
                     : '<button onclick="live.rec(' + c.id + ',true)">开始录像</button>') +
        '<button class="ghost" onclick="live.snap(' + c.id + ')">抓拍</button>' +
        (c.online === false ? '<button class="stop ghost" onclick="live.del(' + c.id + ')">删除</button>' : '') +
        '<span class="hint" id="msg' + c.id + '"></span></div>';
      grid.appendChild(div);
      if (c.online !== false) live.attach(c.id);
    });
    $('srvStat').textContent = '共 ' + d.items.length + ' 路相机';
  },
  // 关闭所有实时流连接（离开页面/刷新前调用，避免连接残留导致再次抓流失败）
  stopAll() {
    document.querySelectorAll('#liveGrid img').forEach(im => { im.onerror = null; im.src = ''; });
  },
  // 挂载 MJPEG 实时视频流（multipart/x-mixed-replace，浏览器原生播放）
  attach(id) {
    const img = $('snap' + id);
    if (!img) return;
    img.dataset.fallback = '0';
    img.src = '/api/v1/cameras/' + id + '/mjpeg?_=' + Date.now();
  },
  // 实时流失败时降级为 1.5s 快照轮询（保证画面不黑屏）
  err(id) {
    const img = $('snap' + id);
    if (!img || (img.dataset.fallback || '0') === '1') return;
    img.dataset.fallback = '1';
    img.onerror = null;
    loadImg(id, img, 1500);
    const msg = $('msg' + id);
    if (msg) msg.textContent = '实时流不可用，已切换快照模式';
  },
  async rec(id, start) {
    const d = await apiJson('/api/v1/cameras/' + id + '/record/' + (start ? 'start' : 'stop'), { method: 'POST', body: '{}' });
    $('msg' + id).textContent = d.code === 0 ? (start ? '已开始' : '已停止') : ('错误:' + d.message);
    this.refresh();
  },
  async snap(id) {
    const d = await apiJson('/api/v1/cameras/' + id + '/photo', { method: 'POST', body: '{"quality":90}' });
    $('msg' + id).textContent = d.code === 0 ? '抓拍成功 ' + new Date().toLocaleTimeString() : '抓拍失败';
  },
  async del(id) {
    if (!confirm('删除该相机？（保留其录像/照片数据，仅从系统移除）')) return;
    if (!confirm('再次确认删除相机？')) return;
    const d = await apiJson('/api/v1/cameras/' + id, { method: 'DELETE' });
    if (d.code === 0) { liveCamIds = null; this.refresh(); }
    else alert('删除失败: ' + (d.message || ''));
  }
};

// ---------- 录像回放 ----------
const pb = {
  async load() {
    const cams = await fillCamSelect($('pbCam'));
    if (!cams.length) return;
    // 默认当天：用【本地日期】（toISOString 是 UTC，跨日会导致查不到最新录像）
    if (!$('pbDate').value) {
      const d = new Date();
      $('pbDate').value = d.getFullYear() + '-' + String(d.getMonth() + 1).padStart(2, '0') + '-' + String(d.getDate()).padStart(2, '0');
    }
    const id = $('pbCam').value || cams[0].id;
    const date = $('pbDate').value;
    const dayStart = new Date(date + 'T00:00:00').getTime(), dayEnd = dayStart + 86400000;
    const d = (await apiJson('/api/v1/cameras/' + id + '/recordings?start=' + dayStart + '&end=' + dayEnd + '&page=1&page_size=200')).data;
    const list = $('pbList'); list.innerHTML = '';
    $('pbMsg').textContent = '';
    if (!d.items.length) { list.innerHTML = '<div class="hint">当天无录像</div>'; return; }
    d.items.forEach(it => {
      const el = document.createElement('div'); el.className = 'pb-item';
      const meta = document.createElement('div');
      meta.innerHTML = '<b>' + new Date(it.start_ms).toLocaleString('zh-CN') + '</b><br>' +
        '<span class="hint">时长 ' + (it.duration_s || 0).toFixed(0) + 's · ' + fmtBytes(it.size_bytes) + '</span>';
      el.appendChild(meta);
      el.onclick = () => {
        document.querySelectorAll('.pb-item').forEach(x => x.classList.remove('active'));
        el.classList.add('active');
        $('pbMsg').textContent = '加载 ' + fmtBytes(it.size_bytes) + ' 中...';
        $('pbVideo').src = it.url;
        $('pbVideo').play().then(() => { $('pbMsg').textContent = ''; }).catch(() => {});
      };
      const del = document.createElement('button'); del.textContent = '删除'; del.className = 'ghost';
      del.style.marginLeft = '8px';
      del.onclick = async e => {
        e.stopPropagation();
        if (!confirm('删除该录像片段？')) return;
        await apiJson('/api/v1/recordings/' + it.id, { method: 'DELETE' });
        pb.load();
      };
      el.appendChild(del); list.appendChild(el);
    });
  }
};

// ---------- 图片浏览 ----------
const ph = {
  page: 1,
  async load() {
    const cams = await fillCamSelect($('phCam'));
    if (!cams.length) return;
    const id = $('phCam').value || cams[0].id;
    // 事件标签下拉（保留当前选择）
    try {
      const tg = (await apiJson('/api/v1/cameras/' + id + '/photos/tags')).data.tags || [];
      const sel = document.getElementById('phTag');
      const cur = sel.value;
      sel.innerHTML = '<option value="">全部标签</option>' + tg.map(t => '<option value="' + esc(t) + '">' + esc(t) + '</option>').join('');
      if (tg.indexOf(cur) >= 0) sel.value = cur;
    } catch (e) {}
    const tag = $('phTag').value;
    const q = tag ? '&tag=' + encodeURIComponent(tag) : '';
    const d = (await apiJson('/api/v1/cameras/' + id + '/photos?page=' + ph.page + '&page_size=24' + q)).data;
    const grid = $('phGrid'); grid.innerHTML = '';
    if (!d.items.length) { grid.innerHTML = '<div class="hint">暂无照片</div>'; }
    d.items.forEach(it => {
      const img = document.createElement('img'); img.className = 'thumb'; img.loading = 'lazy';
      img.src = it.url; img.title = it.name;
      img.onclick = () => window.open(it.url, '_blank');
      const div = document.createElement('div'); div.className = 'cam'; div.style.padding = '8px';
      div.appendChild(img);
      const ts = it.ts_ms ? new Date(it.ts_ms).toLocaleString('zh-CN') : it.name;
      const meta = document.createElement('div'); meta.className = 'row';
      meta.innerHTML = '<span class="tag">' + esc(it.tag || '手动抓拍') + '</span><span class="hint">' + esc(ts) + '</span>';
      div.appendChild(meta);
      const del = document.createElement('button'); del.className = 'ghost'; del.textContent = '删除';
      del.style.marginTop = '6px';
      del.onclick = async e => {
        e.stopPropagation();
        if (!confirm('删除照片 ' + it.name + '？')) return;
        await apiJson('/api/v1/cameras/' + id + '/photos/' + it.name, { method: 'DELETE' });
        ph.load();
      };
      div.appendChild(del);
      grid.appendChild(div);
    });
    $('phInfo').textContent = '共 ' + d.total + ' 张';
    const pages = Math.ceil(d.total / 24);
    $('phPages').innerHTML = '<button class="ghost" onclick="ph.go(' + (ph.page - 1) + ')">上一页</button> <span class="hint">' + ph.page + '/' + (pages || 1) + '</span> <button class="ghost" onclick="ph.go(' + (ph.page + 1) + ')">下一页</button>';
  },
  go(p) { if (p < 1) return; ph.page = p; ph.load(); }
};

// ---------- 相机配置 ----------
const cam = {
  ctlCache: {},   // camera_id -> {ts, items}，5s 缓存避免切换/刷新卡顿
  async load() {
    if (!cam.cams) { cam.cams = await fillCamSelect($('camSel')); }
    const id = $('camSel').value || 1;
    $('cfgMsg').textContent = '加载中...';
    $('ctlList').innerHTML = '<span class="hint">读取相机参数中...</span>';
    // 配置与控件并行获取，显著减少等待
    const [cfgR, ctlItems] = await Promise.all([
      apiJson('/api/v1/cameras/' + id + '/config'),
      cam.controls(id)
    ]);
    const d = cfgR.data;
    $('cfgName').value = d.name;
    $('cfgRes').value = d.width + 'x' + d.height;
    $('cfgFps').value = d.fps;
    $('cfgBitrate').value = d.bitrate_kbps;
    $('cfgGop').value = d.gop;
    $('cfgFmt').value = d.input_format;
    $('cfgMsg').textContent = '当前相机: ' + d.name + '（' + d.device + '） · 编码器: ' + d.encoder;
    const box = $('ctlList'); box.innerHTML = '';
    ctlItems.forEach(c => {
      const row = document.createElement('div'); row.className = 'ctl-row';
      row.innerHTML = '<span style="width:90px">' + esc(c.name) + '</span>' +
        '<input type="range" min="' + c.min + '" max="' + c.max + '" step="' + (c.step || 1) + '" value="' + c.value + '">' +
        '<span class="val">' + c.value + '</span>' +
        '<button class="ghost" onclick="cam.setCtl(' + c.id + ', this)">应用</button>';
      const rng = row.querySelector('input');
      rng.oninput = () => row.querySelector('.val').textContent = rng.value;
      box.appendChild(row);
    });
    if (!ctlItems.length) box.innerHTML = '<span class="hint">当前无 V4L2 控件（mock 或设备不支持）</span>';
  },
  async controls(id) {
    const now = Date.now();
    const cached = cam.ctlCache[id];
    if (cached && now - cached.ts < 5000) return cached.items;
    try {
      const d = (await apiJson('/api/v1/cameras/' + id + '/controls')).data;
      cam.ctlCache[id] = { ts: now, items: d.items };
      return d.items;
    } catch (e) { return []; }
  },
  async save() {
    const id = $('camSel').value || 1;
    const [w, h] = $('cfgRes').value.split('x').map(Number);
    const body = { name: $('cfgName').value, width: w, height: h, fps: +$('cfgFps').value, bitrate_kbps: +$('cfgBitrate').value, gop: +$('cfgGop').value, input_format: $('cfgFmt').value };
    const r = await api('/api/v1/cameras/' + id + '/config', { method: 'PUT', body: JSON.stringify(body) });
    const d = await r.json();
    $('cfgMsg').textContent = d.code === 0 ? '已保存并应用（画面会短暂中断）' : '失败: ' + (d.message || '参数无效');
    if (d.code === 0) { cam.cams = null; this.load(); }  // 刷新相机列表（名称可能已改）
  },
  async setCtl(ctlId, btn) {
    const id = $('camSel').value || 1;
    const row = btn.closest('.ctl-row');
    const val = +row.querySelector('.val').textContent;
    const d = await apiJson('/api/v1/cameras/' + id + '/controls', { method: 'PUT', body: JSON.stringify({ id: ctlId, value: val }) });
    btn.textContent = d.data.ok ? '已应用' : '失败';
    setTimeout(() => btn.textContent = '应用', 1200);
  },
  // 刷新相机列表（重新从后端拉取）
  async refreshList() {
    cam.cams = null;
    await cam.load();
    $('camToolMsg').textContent = '已刷新';
    setTimeout(() => $('camToolMsg').textContent = '', 1500);
  },
  // 打开"添加相机"弹窗
  async openAdd() {
    $('addModal').style.display = 'flex';
    await cam.refreshDiscover();
  },
  closeAdd() { $('addModal').style.display = 'none'; },
  async refreshDiscover() {
    $('addMsg').textContent = '扫描中...';
    try {
      const d = (await apiJson('/api/v1/system/discover')).data;
      const box = $('discoverList'); box.innerHTML = '';
      if (!d.items.length) { box.innerHTML = '<div class="hint">未发现新的 USB 相机</div>'; }
      d.items.forEach(it => {
        const row = document.createElement('div'); row.className = 'disc-item';
        row.innerHTML = '<input type="text" value="' + esc(it.product || it.device) + '">' +
          '<span class="hint">' + esc(it.device) + ' · ' + it.width + 'x' + it.height + '@' + it.fps + ' · ' + esc(it.input_format) + '</span>' +
          '<button class="ghost btn-add">添加</button>';
        row.querySelector('.btn-add').onclick = () => cam.add(it.device, row.querySelector('input'));
        box.appendChild(row);
      });
      $('addMsg').textContent = '共 ' + d.items.length + ' 台可添加';
    } catch (e) { $('addMsg').textContent = '扫描失败: ' + e.message; }
  },
  async add(device, btn) {
    const input = btn.closest('.disc-item').querySelector('input');
    const body = { device: device, name: input.value.trim() || undefined };
    btn.disabled = true; btn.textContent = '添加中...';
    try {
      const d = await apiJson('/api/v1/cameras', { method: 'POST', body: JSON.stringify(body) });
      $('addMsg').textContent = d.code === 0 ? ('已添加 ' + d.data.name) : ('失败: ' + (d.message || ''));
      if (d.code === 0) { cam.cams = null; await cam.load(); cam.refreshDiscover(); }
    } catch (e) { $('addMsg').textContent = '失败: ' + e.message; }
    btn.disabled = false; btn.textContent = '添加';
  },
  // 删除当前选中相机（保留数据）
  async del() {
    const id = $('camSel').value;
    if (!id) return;
    if (!confirm('删除相机 ' + (cam.cams ? (cam.cams.find(c => c.id == id) || {}).name || id : id) + '？（保留其录像/照片数据）')) return;
    if (!confirm('再次确认删除？')) return;
    const d = await apiJson('/api/v1/cameras/' + id, { method: 'DELETE' });
    if (d.code === 0) { cam.cams = null; await cam.load(); $('camToolMsg').textContent = '已删除'; }
    else alert('删除失败: ' + (d.message || ''));
  }
};

// ---------- 存储 ----------
const stg = {
  async load() {
    const s = (await apiJson('/api/v1/system/storage')).data;
    const pct = Math.min(100, s.used_percent || 0);
    $('stgFill').style.width = pct + '%';
    const used = (s.total_bytes || 0) - (s.free_bytes || 0);
    $('stgKv').innerHTML =
      '<b>挂载点</b> ' + esc(s.mount) + '<br>' +
      '<b>总容量</b> ' + fmtBytes(s.total_bytes) + '　<b>已用</b> ' + fmtBytes(used) +
      '（' + pct.toFixed(1) + '%）<br>' +
      '<b>空闲</b> ' + fmtBytes(s.free_bytes) + '　<b>录像配额</b> ' + fmtBytes(s.quota_bytes) + '<br>' +
      '<b>录像段数</b> ' + s.recordings_count + '　<b>录像占用</b> ' + fmtBytes(s.recordings_bytes);
    const cams = (await apiJson('/api/v1/cameras')).data.items;
    let html = '';
    for (const c of cams) {
      const r = await apiJson('/api/v1/cameras/' + c.id + '/recordings?page=1&page_size=1');
      html += '<div>' + esc(c.name) + '：<b>' + r.data.total + '</b> 段</div>';
    }
    $('stgPerCam').innerHTML = html;
  },
  // 二次确认后清空数据（scope=recordings 仅录像 / all 录像+照片）
  async clear(scope) {
    const msg = $('clearMsg');
    const label = scope === 'all' ? '全部数据（录像 + 照片）' : '全部录像';
    if (!confirm('确定要清空' + label + '吗？此操作不可恢复！')) return;
    if (!confirm('再次确认：将删除全部录像文件' + (scope === 'all' ? '及全部照片' : '') + '，确定继续？')) return;
    msg.textContent = '清理中，请稍候...';
    try {
      const d = await apiJson('/api/v1/system/clear', { method: 'POST', body: JSON.stringify({ scope: scope }) });
      msg.textContent = d.code === 0
        ? ('已删除录像 ' + d.data.deleted_recordings + ' 段' + (scope === 'all' ? '、照片 ' + d.data.deleted_photos + ' 张' : ''))
        : ('失败: ' + (d.message || ''));
      this.load();
    } catch (e) { msg.textContent = '失败: ' + e.message; }
  }
};

// ---------- 系统 ----------
const sys = {
  async load() { await this.refresh(); },
  async refresh() {
    // 服务状态：由 Node 侧 systemctl 查询，camera_server 停止时也能显示
    try {
      const svc = (await apiJson('/api/v1/system/service')).data;
      $('svcInfo').innerHTML =
        '<b>服务状态</b> ' + (svc.active ? '<span class="on">运行中</span>' : '<span class="off">已停止</span>') + '<br>' +
        '<b>ActiveState</b> ' + esc(svc.state || '') + ' · ' + esc(svc.substate || '') + '<br>' +
        '<b>最近启动</b> ' + esc(svc.since || '-');
    } catch (e) {
      $('svcInfo').innerHTML = '<span class="off">服务状态获取失败</span>';
    }
    // 后端信息（重启中会短暂不可达）
    try {
      const h = (await apiJson('/health')).data;
      const s = (await apiJson('/api/v1/system/stats')).data;
      $('sysInfo').innerHTML =
        '<b>状态</b> ' + esc(h.status) + '<br><b>服务时间</b> ' + esc(h.time) + '<br>' +
        '<b>运行时长</b> ' + Math.round(s.uptime_ms / 60000) + ' 分钟<br>' +
        '<b>相机数</b> ' + s.cameras.length + '<br>' +
        '<b>编码器</b> ' + s.cameras.map(c => c.name + '=' + c.encoder).join('，');
    } catch (e) {
      $('sysInfo').innerHTML = '<span class="off">后端不可达（服务已停止或重启中）</span>';
    }
    // 自定义 NVR 名称 / 免鉴权开关
    try {
      const st = (await apiJson('/api/v1/system/settings')).data;
      $('nvrName').value = st.nvr_name || '';
      $('restPublic').checked = !!st.rest_public;
    } catch (e) {}
    // 运行日志
    try {
      const lg = (await apiJson('/api/v1/system/logs?page=1&page_size=30')).data;
      const ACT = { photo_delete: '删除照片', recording_delete: '删除录像', data_clear: '清理数据',
        photo_take: '抓拍', record_start: '开始录像', record_stop: '停止录像',
        camera_rename: '相机改名', camera_config: '相机参数', control_set: '设置控件',
        nvr_name_set: 'NVR名称', onvif_config: 'ONVIF配置',
        service_start: '服务启动', service_stop: '服务停止', service_restart: '服务重启' };
      $('oplog').innerHTML = (lg.items || []).map(e => {
        const t = e.ts_ms ? new Date(e.ts_ms).toLocaleString('zh-CN') : '--';
        const a = ACT[e.action] || e.action;
        return '<div class="op-item">' + t + '  <b>' + esc(a) + '</b>  ' + esc(e.detail || '') +
          (e.result ? ' <span class="on">成功</span>' : ' <span class="off">失败</span>') + '</div>';
      }).join('') || '<span class="hint">暂无日志</span>';
      $('oplogInfo').textContent = '共 ' + lg.total + ' 条（显示最近 30 条）';
    } catch (e) { $('oplog').innerHTML = '<span class="hint">日志加载失败</span>'; }
  },
  async service(action) {
    const msg = $('svcMsg');
    msg.textContent = '执行 ' + action + ' 中...';
    try {
      const d = await apiJson('/api/v1/system/service', { method: 'POST', body: JSON.stringify({ action: action }) });
      msg.textContent = d.message || ('已执行 ' + action);
    } catch (e) { msg.textContent = '执行失败: ' + e.message; }
    // 后端停止/重启时短暂不可达，延迟后自动刷新状态
    setTimeout(() => { this.refresh(); }, action === 'stop' ? 2000 : 6000);
  },
  async saveName() {
    const name = $('nvrName').value.trim() || 'camera_server NVR';
    try {
      const d = await apiJson('/api/v1/system/settings', { method: 'PUT', body: JSON.stringify({ nvr_name: name, rest_public: $('restPublic').checked }) });
      $('nameMsg').textContent = d.code === 0 ? '已保存' : '保存失败';
      if (d.code === 0) { $('logoName').textContent = name; }
    } catch (e) { $('nameMsg').textContent = '保存失败: ' + e.message; }
  }
};

// ---------- ONVIF ----------
const onvif = {
  async load() {
    const d = (await apiJson('/api/v1/system/onvif')).data;
    $('onvifInfo').innerHTML =
      '<b>服务地址(XAddr)</b> ' + esc(d.xaddr) + '<br><b>RTSP 端口</b> ' + d.rtsp_port + '<br>' +
      '<b>当前状态</b> ' + (d.enabled ? '<span class="on">启用</span>' : '<span class="off">停用</span>') + ' · 发现 ' + (d.discovery ? '开' : '关');
    $('onvifEnabled').checked = d.enabled;
    $('onvifDisc').checked = d.discovery;
  },
  async save() {
    const body = { enabled: $('onvifEnabled').checked, discovery: $('onvifDisc').checked };
    const d = await apiJson('/api/v1/system/onvif', { method: 'PUT', body: JSON.stringify(body) });
    $('onvifMsg').textContent = d.code === 0 ? '已应用' : '失败';
    this.load();
  }
};

// ---------- 启动 ----------
function boot() {
  if (location.hash && PAGES.includes(location.hash.slice(1))) switchPage(location.hash.slice(1));
  else { switchPage('live'); live.refresh(); }
  stg.load();
  // 应用自定义 NVR 名称（左上角）
  apiJson('/api/v1/system/settings').then(d => {
    if (d.code === 0 && d.data.nvr_name) $('logoName').textContent = d.data.nvr_name;
  }).catch(() => {});
}
(async function init() {
  try { const d = await apiJson('/api/v1/cameras'); if (d.code === 0) { hideLogin(); boot(); } }
  catch (e) { showLogin(); }
})();
