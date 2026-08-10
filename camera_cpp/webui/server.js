// camera_server NVR Web 管理面（Node.js 零依赖）
// 职责：1) 提供海康风格 Web 界面静态资源
//       2) 代理 /api/* 与 /onvif/* 到 C++ 后端（注入 API Key，浏览器无需带鉴权头）
const http = require('http');
const fs = require('fs');
const path = require('path');
const { execFile } = require('child_process');

// 统一配置：优先与 C++ 后端同一个 config.json（部署时位于本目录上级 /opt/camera_server/config.json），
// 兜底本目录 webui/config.json（仅 port/backend，api_key/data_dir 为旧字段兼容）
function loadCfg() {
  const local = JSON.parse(fs.readFileSync(path.join(__dirname, 'config.json'), 'utf8'));
  const cands = [process.env.CAMERA_CONFIG, path.join(__dirname, '..', 'config.json'),
                 path.join(__dirname, 'config.json')].filter(Boolean);
  let unified = null;
  for (const c of cands) {
    try { unified = JSON.parse(fs.readFileSync(c, 'utf8')); break; } catch (e) {}
  }
  const backendPort = unified && unified.http_port ? unified.http_port
                       : (local.backend ? new URL(local.backend).port : '8000');
  return {
    port: local.port || 8081,
    backend: 'http://127.0.0.1:' + backendPort,
    api_key: (unified && unified.api_key) || local.api_key || '',
    data_dir: (unified && unified.data_dir) || local.data_dir || '/data/camera'
  };
}
const cfg = loadCfg();
const PUBLIC = path.join(__dirname, 'public');
const MIME = {
  '.html': 'text/html; charset=utf-8', '.css': 'text/css; charset=utf-8',
  '.js': 'application/javascript; charset=utf-8', '.json': 'application/json; charset=utf-8',
  '.png': 'image/png', '.jpg': 'image/jpeg', '.jpeg': 'image/jpeg',
  '.svg': 'image/svg+xml', '.ico': 'image/x-icon', '.mp4': 'video/mp4',
};

let apiKey = cfg.api_key || '';
const DATA_DIR = cfg.data_dir || '/data/camera';

// 服务动作日志（追加 jsonl，供后端 /api/v1/system/logs 合并展示）
function logNodeOp(action, detail, result) {
  try {
    const line = JSON.stringify({ ts: Date.now(), action: action, detail: detail, result: result ? 1 : 0 }) + '\n';
    fs.appendFileSync(path.join(DATA_DIR, 'oplog_node.jsonl'), line);
  } catch (e) {}
}

function sendJson(res, code, obj) { res.writeHead(code, { 'Content-Type': 'application/json; charset=utf-8' }); res.end(JSON.stringify(obj)); }

function proxy(req, res, u) {
  const target = new URL(cfg.backend + u.pathname + u.search);
  const headers = Object.assign({}, req.headers);
  headers.host = target.host;
  if (apiKey) headers['Authorization'] = 'Bearer ' + apiKey;
  const p = http.request(target, { method: req.method, headers }, (pr) => {
    const out = { 'Content-Type': pr.headers['content-type'] || 'application/octet-stream', 'Cache-Control': 'no-cache' };
    for (const k of ['content-range', 'content-length', 'accept-ranges', 'content-disposition'])
      if (pr.headers[k]) out[k[0].toUpperCase() + k.slice(1)] = pr.headers[k];
    res.writeHead(pr.statusCode, out);
    pr.pipe(res);
  });
  p.on('error', () => sendJson(res, 502, { code: 50002, message: 'C++ 后端不可达' }));
  // 客户端断开（切页/更换 img src）时立即销毁上游连接，避免 C++ 侧连接/线程泄漏
  res.on('close', () => p.destroy());
  req.on('aborted', () => p.destroy());
  req.pipe(p);
}

// ---------- 服务控制（Node 侧直接管理 systemd，camera_server 停止时也能用） ----------
function serviceStatus(cb) {
  execFile('systemctl', ['show', 'camera_server', '-p', 'ActiveState', '-p', 'SubState', '-p', 'ActiveEnterTimestamp', '-p', 'LoadState'], (err, stdout) => {
    const kv = {};
    (stdout || '').split('\n').forEach(l => { const i = l.indexOf('='); if (i > 0) kv[l.slice(0, i)] = l.slice(i + 1); });
    cb({
      active: kv.ActiveState === 'active',
      state: kv.ActiveState || (err ? 'unknown' : 'inactive'),
      substate: kv.SubState || '',
      since: kv.ActiveEnterTimestamp || ''
    });
  });
}

const server = http.createServer((req, res) => {
  const u = new URL(req.url, 'http://x');
  if (req.method === 'POST' && u.pathname === '/login') {
    let body = '';
    req.on('data', c => body += c);
    req.on('end', () => {
      try { apiKey = (JSON.parse(body).key || '').trim() || apiKey; sendJson(res, 200, { code: 0, data: { ok: true } }); }
      catch (e) { sendJson(res, 400, { code: 40003, message: 'JSON 解析失败' }); }
    });
    return;
  }
  if (u.pathname === '/api/v1/system/service') {
    if (req.method === 'GET') {
      serviceStatus(s => sendJson(res, 200, { code: 0, data: s, message: 'ok' }));
      return;
    }
    if (req.method === 'POST') {
      let body = '';
      req.on('data', c => body += c);
      req.on('end', () => {
        let action = '';
        try { action = (JSON.parse(body).action || '').trim(); } catch (e) {}
        if (!['start', 'stop', 'restart'].includes(action)) {
          sendJson(res, 400, { code: 40003, message: 'action 需为 start/stop/restart' });
          return;
        }
        execFile('systemctl', [action, 'camera_server'], (err, so, se) => {
          const ok = !err;
          logNodeOp('service_' + action, 'systemctl ' + action + ' camera_server', ok);
          sendJson(res, ok ? 200 : 500, {
            code: ok ? 0 : 50003,
            message: ok ? ('已执行 ' + action) : ((se || err.message || '').trim() || 'systemctl 执行失败'),
            data: { action: action }
          });
        });
      });
      return;
    }
    sendJson(res, 405, { code: 40501, message: 'Method Not Allowed' });
    return;
  }
  if (u.pathname.startsWith('/api/') || u.pathname.startsWith('/onvif/') || u.pathname === '/health') return proxy(req, res, u);

  // 静态资源
  let rel = u.pathname === '/' ? '/index.html' : u.pathname;
  const fp = path.normalize(path.join(PUBLIC, rel));
  if (!fp.startsWith(PUBLIC)) { res.writeHead(403); res.end(); return; }
  fs.readFile(fp, (err, data) => {
    if (err) { res.writeHead(404, { 'Content-Type': 'text/plain; charset=utf-8' }); res.end('Not Found'); return; }
    res.writeHead(200, { 'Content-Type': MIME[path.extname(fp)] || 'application/octet-stream', 'Cache-Control': 'no-cache' });
    res.end(data);
  });
});

server.listen(cfg.port, '0.0.0.0', () => {
  console.log(`[camera-web] NVR 管理面: http://0.0.0.0:${cfg.port}  -> 后端 ${cfg.backend}`);
});
