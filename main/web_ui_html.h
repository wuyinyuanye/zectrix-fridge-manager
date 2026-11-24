#ifndef ZECTRIX_FRIDGE_WEB_UI_HTML_H_
#define ZECTRIX_FRIDGE_WEB_UI_HTML_H_

inline constexpr char kWebUiHtml[] = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<title>极趣小冰箱 · 手机管理台</title>
<style>
*{box-sizing:border-box;font-family:-apple-system,BlinkMacSystemFont,"PingFang SC","Helvetica Neue",sans-serif}
body{margin:0;background:#f5f6f8;color:#111;padding:12px 12px 48px}
.hdr{display:flex;justify-content:space-between;align-items:center;margin-bottom:10px}
.hdr h1{font-size:19px;margin:0}
.pill{background:#111;color:#fff;border-radius:999px;padding:3px 10px;font-size:12px}
.tabs{display:flex;gap:6px;margin-bottom:12px;overflow-x:auto}
.tab{flex:1;text-align:center;padding:9px 6px;border-radius:10px;background:#fff;border:1.5px solid #d8dce3;font-weight:600;font-size:13px;cursor:pointer;white-space:nowrap}
.tab.on{background:#111;color:#fff;border-color:#111}
.card{background:#fff;border:1.5px solid #e1e4ea;border-radius:14px;padding:12px;margin-bottom:10px}
.row{display:flex;gap:8px;align-items:center;margin-bottom:8px}
.row:last-child{margin-bottom:0}
input,select,textarea{width:100%;padding:9px 10px;border:1.5px solid #cfd5df;border-radius:10px;font-size:14px;background:#fff}
button{padding:9px 12px;border-radius:10px;border:1.5px solid #111;background:#111;color:#fff;font-weight:600;font-size:13px;cursor:pointer;white-space:nowrap}
button.ghost{background:#fff;color:#111}
button.sm{padding:5px 9px;font-size:12px}
.item{display:flex;justify-content:space-between;align-items:center;padding:9px 0;border-bottom:1px dashed #e1e4ea}
.item:last-child{border-bottom:none}
.badge{display:inline-block;padding:2px 7px;border-radius:6px;font-size:12px;font-weight:700;background:#eceff4;color:#222}
.badge.urg{background:#111;color:#fff}
.meta{font-size:12px;color:#555;margin-top:2px}
.lbl{font-size:12px;color:#444;font-weight:600;margin:8px 0 4px}
.hide{display:none}
.msg{font-size:13px;padding:8px 10px;border-radius:10px;background:#eef2f7;margin-top:8px;white-space:pre-wrap}
</style>
</head>
<body>
<div class="hdr">
  <h1>极趣小冰箱 v2</h1>
  <span class="pill" id="stat">加载中...</span>
</div>

<div class="tabs">
  <div class="tab on" onclick="sw('fridge',this)">冰箱库存</div>
  <div class="tab" onclick="sw('shop',this)">采购清单</div>
  <div class="tab" onclick="sw('ai',this)">文字指令</div>
  <div class="tab" onclick="sw('cfg',this)">Wi-Fi与语音</div>
</div>

<div id="p-fridge">
  <div class="card">
    <div class="row">
      <input id="fn" placeholder="食材名，如 鲜牛奶">
      <input id="fq" placeholder="数量，如 2盒" style="max-width:96px">
    </div>
    <div class="row">
      <select id="fz" style="max-width:96px">
        <option value="">自动分区</option>
        <option value="冷藏">冷藏</option>
        <option value="冷冻">冷冻</option>
        <option value="常温">常温</option>
      </select>
      <input id="fd" type="number" placeholder="保质天数(留空=仅记放入日)">
      <button onclick="addItem()">放入冰箱</button>
    </div>
  </div>
  <div class="card" id="items-list">加载中...</div>
</div>

<div id="p-shop" class="hide">
  <div class="card">
    <div class="row">
      <input id="sn" placeholder="要买什么，如 小香葱">
      <input id="sq" placeholder="数量" style="max-width:88px">
      <button onclick="addShop()">加入</button>
    </div>
    <div class="row">
      <button class="ghost" style="flex:1" onclick="commitShop()">已勾选一键放进冰箱</button>
    </div>
  </div>
  <div class="card" id="shop-list">加载中...</div>
</div>

<div id="p-ai" class="hide">
  <div class="card">
    <div class="lbl">给小智 + 冰箱 MCP 发自然语言指令（支持手机输入法语音听写）</div>
    <textarea id="ai-in" rows="2" placeholder="例如：放了两盒牛奶5天过期，番茄吃光了，定时15分钟"></textarea>
    <div class="row" style="margin-top:8px">
      <button style="flex:1" onclick="sendAi()">发送执行</button>
    </div>
    <div class="msg hide" id="ai-res"></div>
  </div>
  <div class="card" id="ai-logs">暂无记录</div>
</div>

<div id="p-cfg" class="hide">
  <div class="card">
    <h3 style="margin:0 0 8px;font-size:15px">1. 家庭 2.4GHz Wi-Fi</h3>
    <div class="row">
      <input id="c-ssid" placeholder="Wi-Fi 名称 (仅支持2.4G)">
      <input id="c-pass" type="password" placeholder="Wi-Fi 密码(留空不改)">
    </div>
    <div class="lbl">电池省电自动休眠（分钟，0=不自动休眠） / 次日早起刷新（点）</div>
    <div class="row">
      <input id="c-sleep" type="number" min="0" max="120" placeholder="休眠分钟(默认5)">
      <input id="c-wake" type="number" min="0" max="23" placeholder="早起刷新小时(默认8)">
    </div>
  </div>

  <div class="card">
    <h3 style="margin:0 0 6px;font-size:15px">2. 小智免费语音（0 API Key · 冰箱 MCP）</h3>
    <div class="meta" style="margin-bottom:8px">
      默认直连公开免费 <b>xiaozhi.me</b>（0 元免充值，按住 OK 说话自动控冰箱）；也可以一键切到自家局域网 NAS / 私有小智服务。
    </div>
    <div class="lbl">选择语音服务器</div>
    <select id="c-preset" onchange="applyPreset()">
      <option value="public">小智官方免费云（默认 · xiaozhi.me · 0 Key）</option>
      <option value="lan">自建局域网小智 / NAS（例如 192.168.1.100:8003）</option>
    </select>
    <div class="lbl">服务器 OTA 或 WebSocket 地址</div>
    <input id="c-ota" placeholder="https://api.tenclass.net/xiaozhi/ota/">
    <details style="margin-top:8px">
      <summary style="font-size:12px;color:#555;cursor:pointer">可选：自定义 WebSocket Token（一般留空）</summary>
      <div style="margin-top:6px">
        <input id="c-tok" type="password" placeholder="可选 Bearer Token（留空用服务器下发）">
      </div>
    </details>
    <div class="row" style="margin-top:10px">
      <button style="flex:1" onclick="saveCfg()">保存设置</button>
      <button class="ghost" onclick="testAi()">测通 / 查绑定码</button>
    </div>
    <div class="msg hide" id="cfg-msg"></div>
  </div>
</div>

<script>
const PUBLIC_OTA = 'https://api.tenclass.net/xiaozhi/ota/';
let cfgLoaded = false;
function $(id){return document.getElementById(id)}
function esc(s){return String(s??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))}
function sw(k,el){
  ['fridge','shop','ai','cfg'].forEach(x=>$('p-'+x).classList.toggle('hide',x!==k));
  document.querySelectorAll('.tab').forEach(t=>t.classList.remove('on'));
  el.classList.add('on');
}
async function post(u,d){
  const r=await fetch(u,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(d||{})});
  return r.json();
}
function applyPreset(){
  if($('c-preset').value==='public'){
    $('c-ota').value=PUBLIC_OTA;
  } else if($('c-ota').value===PUBLIC_OTA || !$('c-ota').value){
    $('c-ota').value='http://192.168.1.100:8003/xiaozhi/ota/';
  }
}
async function load(){
  const s=await (await fetch('/api/state')).json();
  $('stat').textContent=(s.wifi_ok?'Wi-Fi ':'AP ')+s.ip+' · '+s.battery+'%';
  $('items-list').innerHTML=s.items.length?s.items.map(it=>{
    const hasExp=it.days_left<900;
    const tag=hasExp?(it.days_left<0?('过期'+(-it.days_left)+'天'):(it.days_left===0?'今天到期':'剩'+it.days_left+'天')):(it.age_days===0?'今天放入':'已放'+it.age_days+'天');
    const urg=hasExp&&it.days_left<=2?' urg':'';
    return `<div class="item"><div><b>${esc(it.name)}</b> ${esc(it.qty)} <span class="badge${urg}">${esc(tag)}</span><div class="meta">${esc(it.zone)}</div></div><div style="display:flex;gap:5px"><button class="ghost sm" onclick="actItem('shop',${it.id})">补货</button><button class="ghost sm" onclick="actItem('del',${it.id})">吃完</button></div></div>`;
  }).join(''):'<div class="meta">冰箱空空的，快添加或按住 OK 说话放入食材吧~</div>';

  $('shop-list').innerHTML=s.shopping.length?s.shopping.map(x=>`<div class="item"><label style="display:flex;gap:8px;align-items:center;flex:1"><input type="checkbox" style="width:auto" ${x.checked?'checked':''} onchange="actShop('toggle',${x.id})"><span>${x.checked?'<s>':''}<b>${esc(x.name)}</b> ${esc(x.qty)}${x.checked?'</s>':''} <span class="meta">${esc(x.source||'')}</span></span></label><button class="ghost sm" onclick="actShop('del',${x.id})">删</button></div>`).join(''):'<div class="meta">采购清单是空的~</div>';

  $('ai-logs').innerHTML=s.ai_logs.length?s.ai_logs.map(l=>`<div class="item"><div><b>${esc(l.time)}</b> ${esc(l.input)}<div class="meta">${l.ok?'✓ ':'✗ '}${esc(l.output)} (${l.ms}ms)</div></div></div>`).join(''):'<div class="meta">暂无语音/AI记录</div>';

  if(!cfgLoaded && s.config){
    cfgLoaded=true;
    $('c-ssid').value=s.config.wifi_ssid||'';
    $('c-sleep').value=s.config.sleep_minutes??5;
    $('c-wake').value=s.config.wake_hour??8;
    const ota=s.config.ota_url||PUBLIC_OTA;
    $('c-ota').value=ota;
    $('c-preset').value=(ota.indexOf('tenclass.net')>=0)?'public':'lan';
    $('c-tok').placeholder=s.config.has_ws_token?'已保存 Token（留空不改）':'可选 Bearer Token（留空用服务器下发）';
  }
}
async function addItem(){
  const name=$('fn').value.trim(); if(!name)return;
  const d=$('fd').value.trim();
  const r=await post('/api/items',{action:'add',name,qty:$('fq').value.trim()||'1份',zone:$('fz').value,days:d===''?-999:parseInt(d,10)});
  if(!r.ok){alert(r.error||'添加失败');return;}
  $('fn').value='';$('fq').value='';$('fd').value='';load();
}
async function actItem(action,id){
  const r=await post('/api/items',{action,id});
  if(!r.ok)alert(r.error||'操作失败');
  load();
}
async function addShop(){
  const name=$('sn').value.trim(); if(!name)return;
  const r=await post('/api/shopping',{action:'add',name,qty:$('sq').value.trim()||'1份'});
  if(!r.ok){alert(r.error||'添加失败');return;}
  $('sn').value='';$('sq').value='';load();
}
async function actShop(action,id){await post('/api/shopping',{action,id});load();}
async function commitShop(){
  const r=await post('/api/shopping',{action:'commit'});
  if(r.skipped_full>0)alert('已入库 '+r.moved+' 样，另有 '+r.skipped_full+' 样因冰箱库存已满(80样)仍保留在采购单');
  load();
}
async function waitAiJob(jobId){
  for(let i=0;i<110;++i){
    await new Promise(r=>setTimeout(r,400));
    const st=await (await fetch('/api/ai/status')).json();
    if(!st.running && st.job_id===jobId) return st;
    if(!st.running && st.job_id>jobId) return {ok:false,error:'已被新请求覆盖'};
  }
  return {ok:false,error:'等待语音服务响应超时'};
}
async function sendAi(){
  const text=$('ai-in').value.trim(); if(!text)return;
  const box=$('ai-res'); box.classList.remove('hide'); box.textContent='发送中...';
  let r=await post('/api/ai/text',{text});
  if(r.pending && r.job_id){
    box.textContent='小智处理中（其他页面可正常操作）...';
    r=await waitAiJob(r.job_id);
  }
  if(r.ok){
    box.textContent='✓ '+r.reply+' ('+r.ms+'ms)';
    $('ai-in').value='';
  } else if(r.needs_activation && r.activation_code){
    box.textContent='需先绑定 6 位码: 【 '+r.activation_code+' 】';
  } else {
    box.textContent='✗ '+(r.error||'失败');
  }
  load();
}
async function saveCfg(){
  const m=$('cfg-msg'); m.classList.remove('hide'); m.textContent='保存中...';
  const r=await post('/api/config',{
    wifi_ssid:$('c-ssid').value.trim(),
    wifi_pass:$('c-pass').value,
    sleep_minutes:parseInt($('c-sleep').value||'5',10),
    wake_hour:parseInt($('c-wake').value||'8',10),
    ota_url:$('c-ota').value.trim()||PUBLIC_OTA,
    ws_token:$('c-tok').value.trim()
  });
  m.textContent=r.wifi_changed?'已保存，设备正在连接新 Wi-Fi，请稍候查看屏幕新 IP':'设置已保存！可点右侧「测通 / 查绑定码」测试';
  $('c-pass').value='';
}
async function testAi(){
  const m=$('cfg-msg'); m.classList.remove('hide'); m.textContent='正在连接小智服务器并测试 MCP 握手...';
  let r=await post('/api/ai/test',{});
  if(r.pending && r.job_id){
    r=await waitAiJob(r.job_id);
  }
  if(r.ok){
    m.textContent='✓ 语音服务已连通 ('+r.ms+'ms)\n地址: '+r.ws_url+'\n设备MAC: '+r.mac;
  } else if(r.needs_activation && r.activation_code){
    m.textContent='需要绑定设备！\n请打开 https://xiaozhi.me 登录后输入 6 位绑定码：\n【 '+r.activation_code+' 】\n输入完成后再点一次测试即可。';
  } else {
    m.textContent='✗ 未连通: '+(r.error||'连接失败');
  }
  load();
}
load();
</script>
</body></html>)HTML";

#endif  // ZECTRIX_FRIDGE_WEB_UI_HTML_H_
