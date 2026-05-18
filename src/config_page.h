#pragma once
static const char CONFIG_PAGE[] PROGMEM = R"CONFIGPAGE(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<title>灯带消消乐 · 参数配置</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;background:linear-gradient(160deg,#0f172a,#1e293b);color:#e2e8f0;min-height:100vh;padding:16px}
.wrap{max-width:480px;margin:0 auto}
h1{font-size:1.25rem;margin-bottom:4px}
.sub{font-size:.8rem;color:#94a3b8;margin-bottom:16px;line-height:1.5}
.card{background:#1e293b;border:1px solid #334155;border-radius:12px;padding:14px;margin-bottom:12px}
.card h2{font-size:.95rem;margin-bottom:10px;color:#38bdf8}
label{display:block;font-size:.75rem;color:#94a3b8;margin:8px 0 4px}
.row{display:grid;grid-template-columns:1fr 1fr;gap:10px}
input{width:100%;padding:10px;border:1px solid #475569;border-radius:8px;background:#0f172a;color:#f8fafc;font-size:16px}
input:focus{outline:none;border-color:#38bdf8}
.lv{margin-bottom:10px;padding-bottom:10px;border-bottom:1px dashed #334155}
.lv:last-child{border:none;margin:0;padding:0}
.btn{display:block;width:100%;padding:14px;border:none;border-radius:10px;font-size:1rem;font-weight:600;cursor:pointer;margin-top:8px}
.btn-primary{background:#2563eb;color:#fff}
.btn-secondary{background:#334155;color:#e2e8f0}
.hint{font-size:.7rem;color:#64748b;margin-top:6px}
#msg{margin-top:12px;padding:10px;border-radius:8px;display:none;font-size:.85rem}
.ok{background:#14532d;color:#86efac;display:block!important}
.err{background:#7f1d1d;color:#fca5a5;display:block!important}
</style>
</head>
<body>
<div class="wrap">
<h1>灯带消消乐</h1>
<div class="card">
<h2>灯带长度</h2>
<label>灯珠数量（重新开局后生效）</label>
<input type="number" id="ledCount" min="8" max="64" step="1">
<p class="hint">含顶部 3 颗预览区， playable 区 = 总数 − 3</p>
</div>
<div class="card">
<h2>子弹速度</h2>
<label>移动间隔（毫秒/格，越小越快）</label>
<input type="number" id="bulletMs" min="20" max="500" step="1">
<p class="hint">例：15ms ≈ 每秒约 67 格</p>
</div>
<div class="card" id="levels"></div>
<button class="btn btn-primary" onclick="saveCfg()">保存配置</button>
<button class="btn btn-secondary" onclick="resetCfg()">恢复默认</button>
<div id="msg"></div>
</div>
<script>
const lvTitle=['难度一','难度二','难度三','难度四','难度五'];
function el(id){return document.getElementById(id);}
function showMsg(t,ok){const m=el('msg');m.textContent=t;m.className=ok?'ok':'err';}
function buildLevels(){
  let h='<h2>各难度灯珠参数</h2><p class="hint">生成间隔：多久下落一颗；移动间隔：下移一格的时间（越小越快）</p>';
  for(let i=1;i<=5;i++){
    h+='<div class="lv"><strong>'+lvTitle[i-1]+'</strong><div class="row">';
    h+='<div><label>生成间隔 (ms)</label><input type="number" id="spawn'+i+'" min="300" max="15000" step="50"></div>';
    h+='<div><label>移动间隔 (ms/格)</label><input type="number" id="grav'+i+'" min="50" max="2000" step="1"></div>';
    h+='</div></div>';
  }
  el('levels').innerHTML=h;
}
function collect(){
  const d={ledCount:parseInt(el('ledCount').value,10),bulletMs:parseInt(el('bulletMs').value,10)};
  for(let i=1;i<=5;i++){
    d['spawn'+i]=parseInt(el('spawn'+i).value,10);
    d['grav'+i]=parseInt(el('grav'+i).value,10);
  }
  return d;
}
function fillForm(d){
  el('ledCount').value=d.ledCount;
  el('bulletMs').value=d.bulletMs;
  for(let i=1;i<=5;i++){
    el('spawn'+i).value=d['spawn'+i];
    el('grav'+i).value=d['grav'+i];
  }
}
async function loadCfg(){
  try{
    const r=await fetch('/api/config');
    fillForm(await r.json());
  }catch(e){showMsg('读取失败',false);}
}
async function saveCfg(){
  try{
    const r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(collect())});
    const j=await r.json();
    showMsg(j.ok?'已保存（灯珠数需重新开局）':'保存失败',!!j.ok);
  }catch(e){showMsg('保存失败',false);}
}
async function resetCfg(){
  if(!confirm('恢复出厂默认参数？'))return;
  try{
    const r=await fetch('/api/reset',{method:'POST'});
    const j=await r.json();
    if(j.ok){fillForm(j);showMsg('已恢复默认',true);}else showMsg('失败',false);
  }catch(e){showMsg('失败',false);}
}
buildLevels();
loadCfg();
</script>
</body>
</html>

)CONFIGPAGE";
