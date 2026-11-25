#ifndef ZECTRIX_FRIDGE_WEB_UI_HTML_H_
#define ZECTRIX_FRIDGE_WEB_UI_HTML_H_

inline constexpr char kWebUiHtml[] = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#F4EFE6">
<title>极趣小冰箱 · 手机管理台</title>
<style>
:root{
  --bg:#F4EFE6;--card:#FFFFFF;--ink:#1F2421;--sub:#6E685F;--line:#E5DEC9;
  --soft:#F8F4EC;--track:#E9E2D4;--accent:#1F2421;
  --red-bg:#FDECEC;--red-fg:#C62828;--red-bd:#F6C4C4;
  --amb-bg:#FFF4E0;--amb-fg:#B55A00;--amb-bd:#F3D5A3;
  --fr-bg:#EAF5EE;--fr-fg:#256642;
  --fz-bg:#EAF3FC;--fz-fg:#1D5B94;
  --rm-bg:#F4EFE4;--rm-fg:#735B38;
}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent;font-family:-apple-system,BlinkMacSystemFont,"PingFang SC","Hiragino Sans GB","Helvetica Neue",sans-serif}
body{
  margin:0;color:var(--ink);
  background:var(--bg);
  background-image:radial-gradient(#DDD4C4 1px,transparent 1px);
  background-size:18px 18px;
  padding:14px 12px 54px;
}
.wrap{max-width:560px;margin:0 auto}
.hdr{display:flex;justify-content:space-between;align-items:center;gap:10px;margin-bottom:12px}
.brand{display:flex;align-items:center;gap:10px;min-width:0}
.mascot-box{
  width:42px;height:42px;border-radius:13px;background:#fff;
  border:1.5px solid var(--line);box-shadow:0 3px 10px rgba(31,36,33,.06);
  display:flex;align-items:center;justify-content:center;flex-shrink:0;
}
.brand h1{font-size:19px;margin:0;line-height:1.15;letter-spacing:-.2px}
.brand-sub{font-size:12px;color:var(--sub);margin-top:2px}
.stat-pill{
  display:flex;align-items:center;gap:6px;
  background:#fff;border:1.5px solid var(--line);border-radius:999px;
  padding:5px 11px;font-size:12px;font-weight:600;color:var(--ink);
  box-shadow:0 2px 6px rgba(31,36,33,.04);white-space:nowrap;
}
.dot{width:7px;height:7px;border-radius:50%;background:#2E9E5B;flex-shrink:0}
.dot.ap{background:#D97706}
.summary{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin-bottom:12px}
.kpi{
  background:var(--card);border:1.5px solid var(--line);border-radius:15px;
  padding:10px 10px 9px;cursor:pointer;box-shadow:0 3px 10px rgba(31,36,33,.04);
  transition:transform .1s;
}
.kpi:active{transform:scale(.98)}
.kpi-lbl{font-size:11px;color:var(--sub);font-weight:600}
.kpi-val{font-size:20px;font-weight:800;margin-top:2px;line-height:1.2}
.kpi-val span{font-size:12px;font-weight:600;color:var(--sub);margin-left:2px}
.kpi.warn{background:var(--amb-bg);border-color:var(--amb-bd)}
.kpi.warn .kpi-val{color:var(--amb-fg)}
.kpi.crit{background:var(--red-bg);border-color:var(--red-bd)}
.kpi.crit .kpi-val{color:var(--red-fg)}
.tabs{
  display:flex;gap:4px;margin-bottom:12px;padding:4px;
  background:var(--track);border-radius:14px;
}
.tab{
  flex:1;text-align:center;padding:8px 4px;border-radius:11px;
  font-weight:600;font-size:13px;color:var(--sub);cursor:pointer;white-space:nowrap;
  transition:background .15s,color .15s;
}
.tab.on{background:#fff;color:var(--ink);box-shadow:0 2px 6px rgba(31,36,33,.08)}
.tab-cnt{
  display:inline-block;margin-left:3px;padding:0 5px;min-width:17px;
  border-radius:999px;font-size:11px;line-height:16px;
  background:var(--ink);color:#fff;vertical-align:1px;
}
.card{
  background:var(--card);border:1.5px solid var(--line);border-radius:18px;
  padding:13px;margin-bottom:11px;box-shadow:0 4px 14px rgba(31,36,33,.05);
}
.card-title{font-size:15px;font-weight:700;margin:0 0 8px;display:flex;align-items:center;gap:6px}
.row{display:flex;gap:8px;align-items:center;margin-bottom:8px}
.row:last-child{margin-bottom:0}
input,select,textarea{
  width:100%;padding:9px 11px;border:1.5px solid #D9D1C1;border-radius:11px;
  font-size:14px;background:#FCFBF8;color:var(--ink);outline:none;
}
input:focus,select:focus,textarea:focus{border-color:var(--ink);background:#fff}
button,.btn{
  display:inline-flex;align-items:center;justify-content:center;gap:5px;
  padding:9px 12px;border-radius:11px;border:1.5px solid var(--ink);
  background:var(--ink);color:#fff;font-weight:600;font-size:13px;
  cursor:pointer;white-space:nowrap;text-decoration:none;transition:transform .08s,opacity .12s;
}
button:active,.btn:active{transform:scale(.98)}
button.ghost,.btn.ghost{background:#fff;color:var(--ink);border-color:#CFC7B6}
button.soft{background:var(--soft);color:var(--ink);border-color:var(--line)}
button.sm{padding:5px 10px;font-size:12px;border-radius:9px}
.chips{display:flex;gap:6px;margin-bottom:10px;overflow-x:auto;padding-bottom:2px}
.chip{
  padding:5px 11px;border-radius:999px;font-size:12px;font-weight:600;
  background:#fff;color:var(--sub);border:1.5px solid var(--line);cursor:pointer;white-space:nowrap;
}
.chip.on{background:var(--ink);color:#fff;border-color:var(--ink)}
.item{
  display:flex;justify-content:space-between;align-items:center;gap:10px;
  padding:10px 0;border-bottom:1px dashed #E8E1D3;
}
.item:last-child{border-bottom:none;padding-bottom:2px}
.item:first-child{padding-top:2px}
.item-l{display:flex;align-items:center;gap:10px;min-width:0;flex:1}
.ava{
  width:34px;height:34px;border-radius:11px;background:var(--soft);
  border:1px solid var(--line);display:flex;align-items:center;justify-content:center;
  font-size:17px;flex-shrink:0;
}
.item-main{min-width:0;flex:1}
.item-top{display:flex;align-items:center;flex-wrap:wrap;gap:6px}
.item-name{font-size:15px;font-weight:700;color:var(--ink)}
.qty-pill{
  display:inline-block;padding:1px 7px;border-radius:7px;
  font-size:12px;font-weight:600;background:#F1ECE1;color:#3E3A34;
}
.badge{
  display:inline-block;padding:2px 7px;border-radius:7px;
  font-size:11px;font-weight:700;background:#EFEAE0;color:#4B463F;border:1px solid transparent;
}
.badge.crit{background:#D93829;color:#fff}
.badge.over{background:var(--red-bg);color:var(--red-fg);border-color:var(--red-bd)}
.badge.soon{background:var(--amb-bg);color:var(--amb-fg);border-color:var(--amb-bd)}
.ztag{display:inline-block;padding:1px 6px;border-radius:6px;font-size:11px;font-weight:600;margin-right:4px}
.z-fr{background:var(--fr-bg);color:var(--fr-fg)}
.z-fz{background:var(--fz-bg);color:var(--fz-fg)}
.z-rm{background:var(--rm-bg);color:var(--rm-fg)}
.meta{font-size:12px;color:var(--sub);margin-top:3px;line-height:1.45}
.lbl{font-size:12px;color:#544F47;font-weight:600;margin:9px 0 4px}
.hide{display:none!important}
.msg{
  font-size:13px;padding:9px 11px;border-radius:11px;
  background:var(--soft);border:1px solid var(--line);
  margin-top:8px;white-space:pre-wrap;line-height:1.5;
}
.msg.ok{background:var(--fr-bg);border-color:#C5E4D0;color:#1D5235}
.msg.err{background:var(--red-bg);border-color:var(--red-bd);color:var(--red-fg)}
.shop-chk{
  width:21px;height:21px;accent-color:var(--ink);flex-shrink:0;cursor:pointer;
}
.shop-row{display:flex;gap:10px;align-items:center;flex:1;min-width:0;cursor:pointer}
.shop-done{color:#9A948A;text-decoration:line-through}
.src-tag{
  display:inline-block;padding:1px 7px;border-radius:6px;
  font-size:11px;background:var(--soft);color:var(--sub);border:1px solid var(--line);
  margin-left:4px;
}
.empty{text-align:center;padding:24px 12px;color:var(--sub);font-size:13px;line-height:1.6}
.modal{position:fixed;inset:0;background:rgba(20,22,20,.64);z-index:99;display:flex;align-items:center;justify-content:center;padding:14px}
.modal-box{background:#fff;border-radius:20px;padding:13px;width:100%;max-width:420px;max-height:92vh;overflow:auto;box-shadow:0 16px 40px rgba(0,0,0,.3)}
.modal-tip{font-size:13px;font-weight:600;color:var(--ink);background:var(--soft);border:1px solid var(--line);padding:9px 10px;border-radius:12px;margin-bottom:10px;text-align:center;line-height:1.45}
#card-img{width:100%;height:auto;display:block;border-radius:13px;border:1px solid var(--line);-webkit-touch-callout:default}
.sec-hr{border:none;border-top:1px dashed var(--line);margin:10px 0}
</style>
</head>
<body>
<div class="wrap">
  <div class="hdr">
    <div class="brand">
      <div class="mascot-box" id="mascot" aria-hidden="true"></div>
      <div>
        <h1>极趣小冰箱</h1>
        <div class="brand-sub" id="ip-sub">正在连接设备...</div>
      </div>
    </div>
    <div class="stat-pill"><span class="dot" id="net-dot"></span><span id="stat">加载中</span></div>
  </div>

  <div class="summary">
    <div class="kpi" onclick="swKey('fridge');setFilter('all')">
      <div class="kpi-lbl">冰箱库存</div>
      <div class="kpi-val" id="k-all">-<span>样</span></div>
    </div>
    <div class="kpi" id="kpi-urg" onclick="swKey('fridge');setFilter('urg')">
      <div class="kpi-lbl">临期提醒</div>
      <div class="kpi-val" id="k-urg">-<span>样</span></div>
    </div>
    <div class="kpi" onclick="swKey('shop')">
      <div class="kpi-lbl">待买清单</div>
      <div class="kpi-val" id="k-shop">-<span>样</span></div>
    </div>
  </div>

  <div class="tabs">
    <div class="tab on" onclick="sw('fridge',this)">冰箱库存</div>
    <div class="tab" onclick="sw('shop',this)">采购清单<span class="tab-cnt hide" id="shop-cnt">0</span></div>
    <div class="tab" onclick="sw('ai',this)">文字指令</div>
    <div class="tab" onclick="sw('cfg',this)">设置</div>
  </div>

  <div id="p-fridge">
    <div class="card">
      <div class="row">
        <input id="fn" placeholder="食材名，如 鲜牛奶">
        <input id="fq" placeholder="数量，如 2盒" style="max-width:98px">
      </div>
      <div class="row">
        <select id="fz" style="max-width:98px">
          <option value="">自动分区</option>
          <option value="冷藏">冷藏</option>
          <option value="冷冻">冷冻</option>
          <option value="常温">常温</option>
        </select>
        <input id="fd" type="number" placeholder="保质天数（留空只记放入日）">
        <button onclick="addItem()">放入</button>
      </div>
    </div>

    <div class="chips" id="f-chips">
      <div class="chip on" data-f="all" onclick="setFilter('all')">全部</div>
      <div class="chip" data-f="urg" onclick="setFilter('urg')">临期优先</div>
      <div class="chip" data-f="冷藏" onclick="setFilter('冷藏')">冷藏</div>
      <div class="chip" data-f="冷冻" onclick="setFilter('冷冻')">冷冻</div>
      <div class="chip" data-f="常温" onclick="setFilter('常温')">常温</div>
    </div>

    <div class="card" id="items-list"><div class="empty">加载中...</div></div>
  </div>

  <div id="p-shop" class="hide">
    <div class="card">
      <div class="row">
        <input id="sn" placeholder="要买什么，如 小香葱">
        <input id="sq" placeholder="数量" style="max-width:88px">
        <button onclick="addShop()">加入</button>
      </div>
      <hr class="sec-hr">
      <div class="row">
        <button style="flex:1.35" onclick="makeCard()">生成购物卡图片</button>
        <button class="soft" style="flex:1" onclick="copyShop()">复制文字清单</button>
      </div>
      <div class="row">
        <button class="ghost" style="flex:1" onclick="commitShop()">已勾选入库</button>
        <button class="ghost" style="flex:1" onclick="commitAllShop()">全部买回入库</button>
      </div>
      <div class="msg hide" id="shop-msg"></div>
    </div>
    <div class="card" id="shop-list"><div class="empty">加载中...</div></div>
  </div>

  <div id="card-modal" class="modal hide" onclick="if(event.target===this)closeCard()">
    <div class="modal-box">
      <div class="modal-tip">长按下方图片 → 选择「存储到“照片”」<br>即可离线带去超市，离开 Wi-Fi 也能随时看</div>
      <img id="card-img" alt="采购清单购物卡">
      <div class="row" style="margin-top:10px">
        <a id="card-dl" class="btn" style="flex:1" download="采购清单.png">下载图片</a>
        <button class="ghost" onclick="closeCard()">关闭</button>
      </div>
    </div>
  </div>

  <div id="p-ai" class="hide">
    <div class="card">
      <div class="card-title">给小智发自然语言指令</div>
      <div class="meta" style="margin:0 0 8px">支持手机输入法语音听写，一次可说多件事，自动调用设备端冰箱 MCP。</div>
      <textarea id="ai-in" rows="2" placeholder="例如：放了两盒牛奶5天过期，番茄吃光了，定时15分钟"></textarea>
      <div class="row" style="margin-top:8px">
        <button style="flex:1" onclick="sendAi()">发送执行</button>
      </div>
      <div class="msg hide" id="ai-res"></div>
    </div>
    <div class="card" id="ai-logs"><div class="empty">暂无语音 / AI 记录</div></div>
  </div>

  <div id="p-cfg" class="hide">
    <div class="card">
      <div class="card-title">1. 墨水屏外观与待机封面</div>
      <div class="meta" style="margin:0 0 8px">整机内页支持 2 套风格同步换肤，休眠常亮支持 5 套真 16 灰阶海报自由切换。</div>
      <div class="lbl">整机界面风格（库存 / 食谱 / 烹饪 / 采购 / 弹窗同步）</div>
      <select id="c-theme" onchange="onThemeChange()">
        <option value="0">极简高级风（MUJI / Apple / Kindle · 默认）</option>
        <option value="1">温馨圆角卡片风（温暖厨房手账 · 像素图标）</option>
      </select>
      <div class="lbl">16 灰阶待机封面（小憩常亮海报）</div>
      <select id="c-style">
        <option value="0">MUJI · 瑞士双栏日历（极简）</option>
        <option value="1">Apple · 极简大日历桌搭（极简）</option>
        <option value="2">Kindle · 单向历书页日历（极简）</option>
        <option value="3">原研哉 · 黑白撞色海报（极简）</option>
        <option value="4">温馨圆角卡片看板（卡片风配套）</option>
      </select>
      <div class="row" style="margin-top:10px">
        <button style="flex:1" onclick="applyTheme()">立即应用外观</button>
      </div>
      <div class="msg hide" id="theme-msg"></div>
    </div>

    <div class="card">
      <div class="card-title">2. 家庭 2.4GHz Wi-Fi 与省电</div>
      <div class="row">
        <input id="c-ssid" placeholder="Wi-Fi 名称（仅支持 2.4G）">
        <input id="c-pass" type="password" placeholder="Wi-Fi 密码（留空不改）">
      </div>
      <div class="lbl">电池自动休眠（分钟，0=不休眠） / 次日早起刷新（点）</div>
      <div class="row">
        <input id="c-sleep" type="number" min="0" max="120" placeholder="休眠分钟（默认5）">
        <input id="c-wake" type="number" min="0" max="23" placeholder="早起刷新小时（默认8）">
      </div>
    </div>

    <div class="card">
      <div class="card-title">3. 小智免费语音（0 API Key）</div>
      <div class="meta" style="margin:0 0 8px">
        默认直连公开免费 <b>xiaozhi.me</b>（免充值、按住 OK 说话自动控冰箱）；也可切换自家局域网 NAS / 私有小智服务。
      </div>
      <div class="lbl">语音服务器</div>
      <select id="c-preset" onchange="applyPreset()">
        <option value="public">小智官方免费云（默认 · xiaozhi.me · 0 Key）</option>
        <option value="lan">自建局域网小智 / NAS（例如 192.168.1.100:8003）</option>
      </select>
      <div class="lbl">服务器 OTA 或 WebSocket 地址</div>
      <input id="c-ota" placeholder="https://api.tenclass.net/xiaozhi/ota/">
      <details style="margin-top:8px">
        <summary style="font-size:12px;color:var(--sub);cursor:pointer">可选：自定义 Token / 外部 NFC 离线静态清单页（一般留空）</summary>
        <div style="margin-top:6px">
          <input id="c-tok" type="password" placeholder="可选 Bearer Token（留空用服务器下发）">
        </div>
        <div class="lbl">自定义 NFC 离线清单静态页（留空 = 默认一碰直达本机采购页）</div>
        <input id="c-nfc" placeholder="留空 = 自动写入本机局域网采购页">
      </details>
      <div class="row" style="margin-top:10px">
        <button style="flex:1" onclick="saveCfg()">保存设置</button>
        <button class="soft" onclick="testAi()">测通 / 查绑定码</button>
      </div>
      <div class="msg hide" id="cfg-msg"></div>
    </div>

    <div class="card">
      <div class="card-title">4. 设备电源</div>
      <div class="meta" style="margin:0 0 8px">
        远程重启设备，或清空墨水屏并关机（所有库存、菜谱与配置都会完整保留）。
      </div>
      <div class="row">
        <button class="ghost" style="flex:1" onclick="powerAct('reboot')">重启设备</button>
        <button style="flex:1" onclick="powerAct('shutdown')">清屏并关机</button>
      </div>
      <div class="msg hide" id="pwr-msg"></div>
    </div>
  </div>
</div>

<script>
const PUBLIC_OTA = 'https://api.tenclass.net/xiaozhi/ota/';
let cfgLoaded = false;
let lastState = null;
let curFilter = 'all';
function $(id){return document.getElementById(id)}
function esc(s){return String(s??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))}
function mascotSvg(worried){
  const mouth=worried
    ?'<rect x="9" y="10" width="2" height="1" fill="#1F2421"/><rect x="8" y="11" width="1" height="1" fill="#1F2421"/><rect x="11" y="11" width="1" height="1" fill="#1F2421"/>'
    :'<rect x="7" y="10" width="1" height="1" fill="#1F2421"/><rect x="8" y="11" width="4" height="1" fill="#1F2421"/><rect x="12" y="10" width="1" height="1" fill="#1F2421"/>';
  return '<svg width="30" height="30" viewBox="0 0 20 20" shape-rendering="crispEdges">'
    +'<rect x="4" y="2" width="12" height="15" rx="2" fill="#FFF" stroke="#1F2421" stroke-width="1.4"/>'
    +'<rect x="5" y="13" width="10" height="1.2" fill="#1F2421"/>'
    +'<rect x="13" y="14" width="1.4" height="2.4" fill="#1F2421"/>'
    +'<rect x="5" y="17" width="3" height="1.4" fill="#1F2421"/><rect x="12" y="17" width="3" height="1.4" fill="#1F2421"/>'
    +'<rect x="6" y="6" width="2.4" height="2.4" fill="#1F2421"/><rect x="11.6" y="6" width="2.4" height="2.4" fill="#1F2421"/>'
    +'<rect x="6.5" y="6.5" width="0.9" height="0.9" fill="#FFF"/><rect x="12.1" y="6.5" width="0.9" height="0.9" fill="#FFF"/>'
    +'<rect x="4.8" y="9.2" width="1.8" height="1.2" fill="#F2A7A0"/><rect x="13.4" y="9.2" width="1.8" height="1.2" fill="#F2A7A0"/>'
    +mouth+'</svg>';
}
function foodEmoji(name,zone){
  if(zone==='冷冻')return '❄️';
  const s=String(name||'');
  if(/奶|芝士|黄油|奶油/.test(s))return '🥛';
  if(/蛋/.test(s))return '🥚';
  if(/虾|鱼|蟹|贝|蛤|鱿|海鲜/.test(s))return '🦐';
  if(/肉|排骨|牛|羊|猪|鸡|鸭|鹅|火腿|培根|香肠|丸/.test(s))return '🥩';
  if(/水|茶|可乐|雪碧|汁|咖啡|酒|豆浆|饮料/.test(s))return '🥤';
  if(/果|苹|梨|橙|橘|柚|莓|葡萄|瓜|桃|李|樱桃|香蕉|芒果|柠檬|番茄|西红柿/.test(s))return '🍓';
  if(/饭|面|包|吐司|馒头|饺|饼/.test(s))return '🍚';
  return '🥬';
}
function zoneCls(z){return z==='冷冻'?'z-fz':(z==='常温'?'z-rm':'z-fr');}
function sw(k,el){
  ['fridge','shop','ai','cfg'].forEach(x=>$('p-'+x).classList.toggle('hide',x!==k));
  document.querySelectorAll('.tab').forEach(t=>t.classList.remove('on'));
  if(el) el.classList.add('on');
  try{if(location.hash!=='#'+k)history.replaceState(null,'','#'+k);}catch(e){}
}
function tabFromUrl(){
  const h=(location.hash||'').replace(/^#/,'').trim();
  const q=new URLSearchParams(location.search).get('tab')||'';
  const k=h||q;
  return ['fridge','shop','ai','cfg'].includes(k)?k:'';
}
function swKey(k){
  const idx={fridge:0,shop:1,ai:2,cfg:3}[k];
  if(idx===undefined)return;
  sw(k,document.querySelectorAll('.tab')[idx]);
}
function setFilter(f){
  curFilter=f;
  document.querySelectorAll('#f-chips .chip').forEach(c=>c.classList.toggle('on',c.dataset.f===f));
  if(lastState)renderItems(lastState.items||[]);
}
window.addEventListener('hashchange',()=>{const k=tabFromUrl();if(k){swKey(k);load();}});
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
function renderItems(items){
  const list=items.filter(it=>{
    if(curFilter==='all')return true;
    if(curFilter==='urg')return it.days_left<900&&it.days_left<=2;
    return it.zone===curFilter;
  });
  $('items-list').innerHTML=list.length?list.map(it=>{
    const hasExp=it.days_left<900;
    const tag=hasExp?(it.days_left<0?('过期'+(-it.days_left)+'天'):(it.days_left===0?'今天到期':'剩'+it.days_left+'天')):(it.age_days===0?'今天放入':'已放'+it.age_days+'天');
    const bcls=hasExp?(it.days_left<0?' over':(it.days_left===0?' crit':(it.days_left<=2?' soon':''))):'';
    return `<div class="item"><div class="item-l"><div class="ava">${foodEmoji(it.name,it.zone)}</div><div class="item-main"><div class="item-top"><span class="item-name">${esc(it.name)}</span><span class="qty-pill">${esc(it.qty)}</span><span class="badge${bcls}">${esc(tag)}</span></div><div class="meta"><span class="ztag ${zoneCls(it.zone)}">${esc(it.zone)}</span></div></div></div><div style="display:flex;gap:5px"><button class="soft sm" onclick="actItem('shop',${it.id})">补货</button><button class="ghost sm" onclick="actItem('del',${it.id})">吃完</button></div></div>`;
  }).join(''):`<div class="empty">${items.length?'这个分类下暂时没有食材~':'冰箱空空的，快添加或按住 OK 说话放入食材吧~'}</div>`;
}
async function load(){
  const s=await (await fetch('/api/state')).json();
  lastState=s;
  const items=s.items||[];
  const shop=s.shopping||[];
  const critCnt=items.filter(it=>it.days_left<900&&it.days_left<=0).length;
  const urgCnt=items.filter(it=>it.days_left<900&&it.days_left<=2).length;
  const pendShop=shop.filter(x=>!x.checked).length;

  $('mascot').innerHTML=mascotSvg(urgCnt>0);
  $('ip-sub').textContent=(s.wifi_ok?'家庭局域网 ':'配网热点 ')+s.ip;
  $('stat').textContent=(s.wifi_ok?'在线':'AP')+' · '+s.battery+'%';
  $('net-dot').classList.toggle('ap',!s.wifi_ok);

  $('k-all').innerHTML=items.length+'<span>样</span>';
  $('k-urg').innerHTML=urgCnt+'<span>样</span>';
  $('k-shop').innerHTML=pendShop+'<span>样</span>';
  const ku=$('kpi-urg');
  ku.classList.remove('warn','crit');
  if(critCnt>0)ku.classList.add('crit');
  else if(urgCnt>0)ku.classList.add('warn');

  const sc=$('shop-cnt');
  sc.textContent=pendShop;
  sc.classList.toggle('hide',pendShop===0);

  renderItems(items);

  $('shop-list').innerHTML=shop.length?shop.map(x=>`<div class="item"><label class="shop-row"><input class="shop-chk" type="checkbox" ${x.checked?'checked':''} onchange="actShop('toggle',${x.id})"><span class="${x.checked?'shop-done':''}"><b>${esc(x.name)}</b> <span class="qty-pill">${esc(x.qty)}</span>${x.source?`<span class="src-tag">${esc(x.source)}</span>`:''}</span></label><button class="ghost sm" onclick="actShop('del',${x.id})">删</button></div>`).join(''):'<div class="empty">采购清单空空的~<br>缺料时点食谱页或按住 OK 说话就能加入</div>';

  $('ai-logs').innerHTML=(s.ai_logs&&s.ai_logs.length)?s.ai_logs.map(l=>`<div class="item"><div class="item-main"><div class="item-top"><span class="badge">${esc(l.time)}</span><b>${esc(l.input)}</b></div><div class="meta">${l.ok?'✓ ':'✗ '}${esc(l.output)} (${l.ms}ms)</div></div></div>`).join(''):'<div class="empty">暂无语音 / AI 记录</div>';

  if(!cfgLoaded && s.config){
    cfgLoaded=true;
    $('c-ssid').value=s.config.wifi_ssid||'';
    $('c-sleep').value=s.config.sleep_minutes??5;
    $('c-wake').value=s.config.wake_hour??8;
    $('c-theme').value=String(s.config.ui_theme??0);
    $('c-style').value=String(s.config.standby_style??0);
    const ota=s.config.ota_url||PUBLIC_OTA;
    $('c-ota').value=ota;
    $('c-nfc').value=s.config.nfc_shop_url||'';
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
async function commitAllShop(){
  const r=await post('/api/shopping',{action:'commit_all'});
  if(r.skipped_full>0)alert('已入库 '+r.moved+' 样，另有 '+r.skipped_full+' 样因冰箱库存已满(80样)仍保留在采购单');
  load();
}
function showShopMsg(t,cls){
  const m=$('shop-msg');
  m.classList.remove('ok','err');
  if(!t){m.classList.add('hide');return;}
  if(cls)m.classList.add(cls);
  m.textContent=t;m.classList.remove('hide');
}
function rrect(ctx,x,y,w,h,r){
  ctx.beginPath();
  ctx.moveTo(x+r,y);
  ctx.arcTo(x+w,y,x+w,y+h,r);
  ctx.arcTo(x+w,y+h,x,y+h,r);
  ctx.arcTo(x,y+h,x,y,r);
  ctx.arcTo(x,y,x+w,y,r);
  ctx.closePath();
}
function fitText(ctx,str,maxW){
  const s=String(str||'');
  if(ctx.measureText(s).width<=maxW)return s;
  let n=s.length;
  while(n>1&&ctx.measureText(s.slice(0,n)+'…').width>maxW)n--;
  return s.slice(0,n)+'…';
}
function drawCardMascot(ctx,x,y,sz){
  const u=sz/20;
  rrect(ctx,x,y,sz,sz,14);
  ctx.fillStyle='#F8F4EC';ctx.fill();
  ctx.strokeStyle='#E2DAC9';ctx.lineWidth=2;ctx.stroke();
  // Cute pixel fridge matching the e-paper mascot
  rrect(ctx,x+4*u,y+2.5*u,12*u,14.5*u,5);
  ctx.fillStyle='#FFFFFF';ctx.fill();
  ctx.strokeStyle='#1F2421';ctx.lineWidth=2.6;ctx.stroke();
  ctx.fillStyle='#1F2421';
  ctx.fillRect(x+4.5*u,y+12.6*u,11*u,1.2*u);
  ctx.fillRect(x+13*u,y+13.8*u,1.3*u,2.4*u);
  ctx.fillRect(x+5.4*u,y+17*u,2.8*u,1.4*u);
  ctx.fillRect(x+11.8*u,y+17*u,2.8*u,1.4*u);
  ctx.fillRect(x+6.2*u,y+6.2*u,2.2*u,2.2*u);
  ctx.fillRect(x+11.6*u,y+6.2*u,2.2*u,2.2*u);
  ctx.fillStyle='#FFFFFF';
  ctx.fillRect(x+6.7*u,y+6.7*u,0.8*u,0.8*u);
  ctx.fillRect(x+12.1*u,y+6.7*u,0.8*u,0.8*u);
  ctx.fillStyle='#F2A7A0';
  ctx.fillRect(x+5*u,y+9.3*u,1.8*u,1.1*u);
  ctx.fillRect(x+13.2*u,y+9.3*u,1.8*u,1.1*u);
  ctx.fillStyle='#1F2421';
  ctx.fillRect(x+7.4*u,y+9.8*u,1*u,1*u);
  ctx.fillRect(x+8.4*u,y+10.7*u,3.2*u,1*u);
  ctx.fillRect(x+11.6*u,y+9.8*u,1*u,1*u);
}
async function copyShop(){
  if(!lastState)await load();
  const all=(lastState&&lastState.shopping)||[];
  if(!all.length){showShopMsg('采购清单是空的，先添加要买的食材吧~','err');return;}
  const pend=all.filter(x=>!x.checked);
  const rows=pend.length?pend:all;
  const d=new Date();
  const md=(d.getMonth()+1)+'/'+d.getDate();
  const lines=['极趣小冰箱 · 采购清单（'+md+'）'];
  rows.forEach(x=>{
    let l=(x.checked?'☑ ':'☐ ')+x.name+(x.qty?' '+x.qty:'');
    if(x.source)l+='（'+x.source+'）';
    lines.push(l);
  });
  const txt=lines.join('\n');
  let ok=false;
  if(navigator.clipboard&&window.isSecureContext){
    try{await navigator.clipboard.writeText(txt);ok=true;}catch(e){}
  }
  if(!ok){
    const ta=document.createElement('textarea');
    ta.value=txt;ta.setAttribute('readonly','');
    ta.style.cssText='position:fixed;left:-9999px;top:0;opacity:0';
    document.body.appendChild(ta);
    ta.focus();ta.select();
    try{ta.setSelectionRange(0,txt.length);}catch(e){}
    try{ok=document.execCommand('copy');}catch(e){}
    document.body.removeChild(ta);
  }
  showShopMsg(ok?('✓ 已复制 '+rows.length+' 样清单，可直接发微信或粘贴到备忘录'):'复制受限，请直接点左侧「生成购物卡图片」保存',ok?'ok':'err');
}
async function makeCard(){
  if(!lastState)await load();
  const all=(lastState&&lastState.shopping)||[];
  if(!all.length){showShopMsg('采购清单是空的，先添加要买的食材吧~','err');return;}
  showShopMsg('');
  const pend=all.filter(x=>!x.checked);
  const done=all.filter(x=>x.checked);
  const rows=pend.length?pend:all;

  const W=750,M=28,CW=W-M*2,HDR=162,FTR=86;
  const rowHs=rows.map(x=>x.source?94:76);
  const listH=rowHs.reduce((a,b)=>a+b,0);
  const H=M+HDR+listH+FTR+M;

  const cv=document.createElement('canvas');
  cv.width=W;cv.height=H;
  const ctx=cv.getContext('2d');
  const FF='-apple-system,BlinkMacSystemFont,"PingFang SC","Hiragino Sans GB","Microsoft YaHei",sans-serif';

  // Warm paper background with subtle dot grain
  ctx.fillStyle='#F4EFE6';
  ctx.fillRect(0,0,W,H);
  ctx.fillStyle='#E1D8C8';
  for(let gx=12;gx<W;gx+=22){
    for(let gy=12;gy<H;gy+=22)ctx.fillRect(gx,gy,2,2);
  }

  // Soft card shadow + white card body
  ctx.save();
  ctx.shadowColor='rgba(31,36,33,0.11)';
  ctx.shadowBlur=26;
  ctx.shadowOffsetY=9;
  rrect(ctx,M,M,CW,H-M*2,26);
  ctx.fillStyle='#FFFFFF';
  ctx.fill();
  ctx.restore();

  rrect(ctx,M,M,CW,H-M*2,26);
  ctx.strokeStyle='#E2DACB';
  ctx.lineWidth=2;
  ctx.stroke();

  // Top warm charcoal accent strip clipped inside rounded card
  ctx.save();
  rrect(ctx,M,M,CW,H-M*2,26);
  ctx.clip();
  ctx.fillStyle='#1F2421';
  ctx.fillRect(M,M,CW,14);
  ctx.restore();

  // Mascot avatar + title + count badge
  drawCardMascot(ctx,M+28,M+40,78);

  const now=new Date();
  const wks=['周日','周一','周二','周三','周四','周五','周六'];
  const dateStr=now.getFullYear()+'年'+(now.getMonth()+1)+'月'+now.getDate()+'日 '+wks[now.getDay()]+' · 逛超市清单';
  const badge=pend.length?('待买 '+pend.length+' 样'):('已购 '+rows.length+' 样');

  ctx.textBaseline='middle';
  ctx.font='700 22px '+FF;
  const bw=Math.ceil(ctx.measureText(badge).width)+36;
  const bx=M+CW-28-bw,by=M+44,bh=42;
  rrect(ctx,bx,by,bw,bh,21);
  ctx.fillStyle='#1F2421';
  ctx.fill();
  ctx.fillStyle='#FFFFFF';
  ctx.textAlign='center';
  ctx.fillText(badge,bx+bw/2,by+bh/2+1);

  const titleX=M+122;
  ctx.textAlign='left';
  ctx.fillStyle='#1F2421';
  ctx.font='800 32px '+FF;
  ctx.fillText(fitText(ctx,'极趣小冰箱 · 采购清单',bx-titleX-14),titleX,M+65);

  ctx.fillStyle='#6E685F';
  ctx.font='500 21px '+FF;
  ctx.fillText(fitText(ctx,dateStr,M+CW-28-titleX),titleX,M+104);

  // Dashed header divider
  ctx.save();
  ctx.strokeStyle='#DDD6C8';
  ctx.lineWidth=2;
  ctx.setLineDash([8,6]);
  ctx.beginPath();
  ctx.moveTo(M+24,M+HDR-14);
  ctx.lineTo(M+CW-24,M+HDR-14);
  ctx.stroke();
  ctx.restore();

  // Shopping rows
  let y=M+HDR-2;
  rows.forEach((it,i)=>{
    const rh=rowHs[i];
    if(i%2===1){
      rrect(ctx,M+18,y+4,CW-36,rh-8,14);
      ctx.fillStyle='#FAF7F0';
      ctx.fill();
    }
    const cy=y+rh/2;
    const cbx=M+36,cby=cy-14;
    rrect(ctx,cbx,cby,28,28,7);
    if(it.checked){
      ctx.fillStyle='#1F2421';
      ctx.fill();
      ctx.strokeStyle='#FFFFFF';
      ctx.lineWidth=3;
      ctx.lineCap='round';
      ctx.lineJoin='round';
      ctx.beginPath();
      ctx.moveTo(cbx+7,cby+14);
      ctx.lineTo(cbx+12,cby+20);
      ctx.lineTo(cbx+22,cby+9);
      ctx.stroke();
    }else{
      ctx.fillStyle='#FFFFFF';
      ctx.fill();
      ctx.strokeStyle='#2E3330';
      ctx.lineWidth=2.6;
      ctx.stroke();
    }

    const qty=String(it.qty||'').trim();
    let nameRight=M+CW-30;
    if(qty){
      ctx.font='700 23px '+FF;
      const qw=Math.min(186,Math.ceil(ctx.measureText(qty).width)+32);
      const qx=M+CW-30-qw,qy=cy-20,qh=40;
      rrect(ctx,qx,qy,qw,qh,12);
      ctx.fillStyle='#F1ECE1';
      ctx.fill();
      ctx.fillStyle='#242825';
      ctx.textAlign='center';
      ctx.fillText(fitText(ctx,qty,qw-16),qx+qw/2,cy+1);
      nameRight=qx-16;
    }

    const nx=M+82,maxNW=Math.max(120,nameRight-nx);
    ctx.textAlign='left';
    ctx.font='700 30px '+FF;
    ctx.fillStyle=it.checked?'#908A80':'#1F2421';
    const nameY=it.source?(y+32):cy+1;
    const drawnName=fitText(ctx,it.name,maxNW);
    ctx.fillText(drawnName,nx,nameY);
    if(it.checked){
      const nw=ctx.measureText(drawnName).width;
      ctx.strokeStyle='#908A80';
      ctx.lineWidth=2.4;
      ctx.beginPath();
      ctx.moveTo(nx,nameY);
      ctx.lineTo(nx+nw,nameY);
      ctx.stroke();
    }
    if(it.source){
      ctx.font='500 20px '+FF;
      ctx.fillStyle='#7D776C';
      ctx.fillText(fitText(ctx,it.source,maxNW),nx,y+66);
    }
    y+=rh;
  });

  // Dashed footer divider
  ctx.save();
  ctx.strokeStyle='#DDD6C8';
  ctx.lineWidth=2;
  ctx.setLineDash([8,6]);
  ctx.beginPath();
  ctx.moveTo(M+24,y+12);
  ctx.lineTo(M+CW-24,y+12);
  ctx.stroke();
  ctx.restore();

  const fy=y+FTR/2+8;
  ctx.font='500 21px '+FF;
  ctx.fillStyle='#7D776C';
  ctx.textAlign='left';
  ctx.fillText('把每一样食材都好好吃掉 · 回家一键入库',M+30,fy);
  ctx.textAlign='right';
  const sumStr=(pend.length&&done.length)?('待买 '+pend.length+' · 已买 '+done.length):('共 '+rows.length+' 样');
  ctx.fillText(sumStr,M+CW-30,fy);

  const url=cv.toDataURL('image/png');
  const ymd=''+now.getFullYear()+String(now.getMonth()+1).padStart(2,'0')+String(now.getDate()).padStart(2,'0');
  $('card-img').src=url;
  $('card-dl').href=url;
  $('card-dl').download='采购清单-'+ymd+'.png';
  $('card-modal').classList.remove('hide');
}
function closeCard(){$('card-modal').classList.add('hide');}
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
  const box=$('ai-res'); box.classList.remove('hide','ok','err'); box.textContent='发送中...';
  let r=await post('/api/ai/text',{text});
  if(r.pending && r.job_id){
    box.textContent='小智处理中（其他页面可正常操作）...';
    r=await waitAiJob(r.job_id);
  }
  if(r.ok){
    box.classList.add('ok');
    box.textContent='✓ '+r.reply+' ('+r.ms+'ms)';
    $('ai-in').value='';
  } else if(r.needs_activation && r.activation_code){
    box.classList.add('err');
    box.textContent='需先绑定 6 位码: 【 '+r.activation_code+' 】';
  } else {
    box.classList.add('err');
    box.textContent='✗ '+(r.error||'失败');
  }
  load();
}
function onThemeChange(){
  const t=parseInt($('c-theme').value||'0',10);
  const st=parseInt($('c-style').value||'0',10);
  if(t===1) $('c-style').value='4';
  else if(st===4) $('c-style').value='0';
}
async function applyTheme(){
  const m=$('theme-msg'); m.classList.remove('hide','ok','err'); m.textContent='正在同步墨水屏...';
  const r=await post('/api/config',{
    ui_theme:parseInt($('c-theme').value||'0',10),
    standby_style:parseInt($('c-style').value||'0',10)
  });
  if(r.ok){
    m.classList.add('ok');
    m.textContent='✓ 外观已切换！设备屏幕已立即刷新，休眠小憩时生效新待机封面';
  } else {
    m.classList.add('err');
    m.textContent='✗ 切换失败，请重试';
  }
}
async function saveCfg(){
  const m=$('cfg-msg'); m.classList.remove('hide','ok','err'); m.textContent='保存中...';
  const r=await post('/api/config',{
    wifi_ssid:$('c-ssid').value.trim(),
    wifi_pass:$('c-pass').value,
    sleep_minutes:parseInt($('c-sleep').value||'5',10),
    wake_hour:parseInt($('c-wake').value||'8',10),
    ui_theme:parseInt($('c-theme').value||'0',10),
    standby_style:parseInt($('c-style').value||'0',10),
    ota_url:$('c-ota').value.trim()||PUBLIC_OTA,
    nfc_shop_url:$('c-nfc').value.trim(),
    ws_token:$('c-tok').value.trim()
  });
  m.classList.add('ok');
  m.textContent=r.wifi_changed?'✓ 已保存，设备正在连接新 Wi-Fi，请稍候查看屏幕新 IP':'✓ 设置已保存！可点右侧「测通 / 查绑定码」测试';
  $('c-pass').value='';
}
async function testAi(){
  const m=$('cfg-msg'); m.classList.remove('hide','ok','err'); m.textContent='正在连接小智服务器并测试 MCP 握手...';
  let r=await post('/api/ai/test',{});
  if(r.pending && r.job_id){
    r=await waitAiJob(r.job_id);
  }
  if(r.ok){
    m.classList.add('ok');
    m.textContent='✓ 语音服务已连通 ('+r.ms+'ms)\n地址: '+r.ws_url+'\n设备MAC: '+r.mac;
  } else if(r.needs_activation && r.activation_code){
    m.textContent='需要绑定设备！\n请打开 https://xiaozhi.me 登录后输入 6 位绑定码：\n【 '+r.activation_code+' 】\n输入完成后再点一次测试即可。';
  } else {
    m.classList.add('err');
    m.textContent='✗ 未连通: '+(r.error||'连接失败');
  }
  load();
}
async function powerAct(action){
  const label=action==='reboot'?'重启设备':'清屏并关机';
  if(!confirm('确定要'+label+'吗？'))return;
  const m=$('pwr-msg'); m.classList.remove('hide','ok','err'); m.textContent='正在发送'+label+'指令...';
  try{
    const r=await post('/api/power',{action});
    if(r.ok){
      m.classList.add('ok');
      m.textContent=action==='reboot'?'✓ 已下发重启指令，设备约 5 秒后重新连网。':'✓ 已下发清屏关机指令，设备即将关机。';
    }else{
      m.classList.add('err');
      m.textContent='✗ '+(r.error||'操作失败');
    }
  }catch(e){
    m.textContent='指令已发送，设备正在执行'+label+'。';
  }
}
$('mascot').innerHTML=mascotSvg(false);
const initTab=tabFromUrl();
if(initTab)swKey(initTab);
load();
</script>
</body></html>)HTML";

#endif  // ZECTRIX_FRIDGE_WEB_UI_HTML_H_
