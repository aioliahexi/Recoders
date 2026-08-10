#include "web_ui.h"

#include <string>

namespace camera {

void setup_web_ui(net::HttpServer& http, Context* ctx) {
    (void)ctx;
    static const std::string kHtml = R"HTML(<!DOCTYPE html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>camera_server 管理台</title>
<style>
*{box-sizing:border-box} body{margin:0;background:#0f1420;color:#e6e6e6;font-family:-apple-system,"PingFang SC","Microsoft YaHei",sans-serif}
header{padding:14px 20px;background:#161d2e;border-bottom:1px solid #2a3550;display:flex;justify-content:space-between;align-items:center}
header h1{font-size:18px;margin:0} .hint{color:#8b94a7;font-size:12px}
.wrap{padding:16px 20px}
#login{margin:60px auto;max-width:380px;background:#161d2e;border:1px solid #2a3550;border-radius:10px;padding:24px;text-align:center}
#login h2{margin-top:0} #login input{width:100%;padding:10px;margin:10px 0;border-radius:6px;border:1px solid #2a3550;background:#0f1420;color:#fff}
button{padding:8px 16px;border:0;border-radius:6px;cursor:pointer;background:#2f6fed;color:#fff;font-size:14px}
button:hover{opacity:.9} button.stop{background:#d64545}
.cam{background:#161d2e;border:1px solid #2a3550;border-radius:10px;padding:14px;margin-bottom:14px}
.cam h3{margin:0 0 8px;font-size:16px} .tag{color:#8b94a7;font-size:12px;margin-left:8px}
.cam img{width:100%;max-width:480px;border-radius:6px;background:#000;display:block;margin:10px 0}
.row{display:flex;gap:10px;align-items:center;flex-wrap:wrap}
.recs{font-size:13px;color:#c3cadb} .recs a{color:#7fb0ff;text-decoration:none}
.err{color:#ff8a8a;font-size:13px} .on{color:#4cdf6b} .off{color:#ff9a4d}
</style></head><body>
<header><h1>camera_server 管理台</h1><span class="hint" id="srv">连接中...</span></header>
<div class="wrap">
<div id="login" style="display:none">
  <h2>登录</h2>
  <p class="hint">请输入服务器 API Key（config.ini 的 api_key；为空则直接进入）</p>
  <input id="key" type="password" placeholder="API Key" autocomplete="current-password">
  <button onclick="saveKey()">登录</button>
  <div class="err" id="lgerr"></div>
</div>
<div id="dash" style="display:none">
  <div class="row" style="margin-bottom:12px">
    <button onclick="refreshCameras()">刷新</button>
    <span class="hint" id="stat"></span>
  </div>
  <div id="cameras"></div>
</div>
</div>
<script>
let key = sessionStorage.getItem('cs_key') || '';
const $=id=>document.getElementById(id);

function saveKey(){ key=$('key').value.trim(); sessionStorage.setItem('cs_key', key); $('lgerr').textContent=''; show(); }

function authHeaders(extra){ const h = Object.assign({'Content-Type':'application/json'}, extra||{}); if(key) h['Authorization']='Bearer '+key; return h; }

async function api(path, opts){
  opts = opts||{};
  opts.headers = authHeaders(opts.headers||{});
  const r = await fetch(path, opts);
  if(r.status===401){ showLogin('API Key 无效或未登录'); throw new Error('401'); }
  return r;
}
function showLogin(msg){ $('login').style.display='block'; $('dash').style.display='none'; if(msg) $('lgerr').textContent=msg; }
function show(){ $('login').style.display='none'; $('dash').style.display='block'; refreshCameras(); }

async function loadImg(id){
  try{
    const r = await api('/api/v1/cameras/'+id+'/photo/latest');
    const b = await r.blob();
    let el = document.getElementById('snap'+id);
    if(el && el.src) URL.revokeObjectURL(el.src);
    if(el) el.src = URL.createObjectURL(b);
  }catch(e){}
}

function fmtBytes(n){ if(n>=1048576) return (n/1048576).toFixed(1)+' MB'; if(n>=1024) return (n/1024).toFixed(0)+' KB'; return n+' B'; }

async function refreshCameras(){
  try{
    const r = await api('/api/v1/cameras');
    const d = (await r.json()).data;
    $('srv').textContent = '已连接 · ' + d.items.length + ' 路相机';
    const box = $('cameras');
    for(const c of d.items){
      let div = document.getElementById('cam'+c.id);
      if(!div){ div=document.createElement('div'); div.className='cam'; div.id='cam'+c.id; box.appendChild(div); }
      div.innerHTML =
        '<h3>'+c.name+'<span class="tag">'+c.encoder+' · '+c.resolution+'@'+c.fps+'fps · '+
        (c.recording?'<span class="on">录像中</span>':'<span class="off">未录像</span>')+'</span></h3>'+
        '<img id="snap'+c.id+'" alt="live">'+
        '<div class="row">'+
          (c.recording ? '<button class="stop" onclick="rec('+c.id+',false)">停止录像</button>'
                       : '<button onclick="rec('+c.id+',true)">开始录像</button>')+
          '<button onclick="snapNow('+c.id+')">立即抓拍</button>'+
          '<span class="hint" id="rec'+c.id+'"></span>'+
        '</div>'+
        '<div class="recs" id="recs'+c.id+'"></div>';
      loadImg(c.id);
      loadRecs(c.id);
      // 每秒刷新画面
      const iv = 'iv'+c.id;
      if(!window[iv]){ window[iv]=setInterval(()=>{ if(document.getElementById('snap'+c.id)) loadImg(c.id); else clearInterval(window[iv]); }, 1000); }
    }
    $('stat').textContent = '更新于 ' + new Date().toLocaleTimeString();
  }catch(e){ showLogin('连接失败: '+e.message); }
}

async function rec(id, start){
  const r = await api('/api/v1/cameras/'+id+'/record/'+(start?'start':'stop'), {method:'POST', body:'{}'});
  const d = await r.json();
  $('rec'+id).textContent = d.code===0 ? (start?'已开始录像':'已停止录像') : ('错误: '+d.message);
  refreshCameras();
}
async function snapNow(id){
  const r = await api('/api/v1/cameras/'+id+'/photo', {method:'POST', body:'{"quality":90}'});
  const d = await r.json();
  $('rec'+id).textContent = d.code===0 ? '抓拍成功 '+new Date().toLocaleTimeString() : '抓拍失败';
  loadImg(id);
}
async function loadRecs(id){
  try{
    const r = await api('/api/v1/cameras/'+id+'/recordings?page=1&page_size=8');
    const d = (await r.json()).data;
    let html = '最近录像('+d.total+'): ';
    for(const it of d.items){
      html += '<a href="javascript:void(0)" onclick="download('+it.id+')">'+new Date(it.start_ms).toLocaleString()+'('+fmtBytes(it.size_bytes)+')</a> ';
    }
    $('recs'+id).innerHTML = html || '暂无录像';
  }catch(e){}
}
async function download(id){
  try{
    const r = await api('/api/v1/recordings/'+id+'/download');
    const b = await r.blob();
    const a = document.createElement('a');
    a.href = URL.createObjectURL(b);
    a.download = 'rec_'+id+'.mp4';
    a.click();
    URL.revokeObjectURL(a.href);
  }catch(e){ alert('下载失败(可能录像仍在上传中): '+e.message); }
}

(function init(){ key ? show() : showLogin(''); })();
setInterval(()=>{ if($('dash').style.display!=='none') refreshCameras(); }, 30000);
</script></body></html>
)HTML";

        http.route("GET", "/", [](const net::Request&) {
        net::Response r;
        r.status = 200;
        r.content_type = "text/html; charset=utf-8";
        r.body = kHtml;
        return r;
    });
}

}  // namespace camera
