#pragma once
// web_ui.h — WebUI HTML pages and API handlers for standalone_rp2350_relay
// Target: RP2350, arduino-pico framework, WiFiClient raw HTTP pattern
// All labels in Japanese. Dark theme. No external dependencies.

#include <Arduino.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include "rule_engine.h"
#include "scheduler.h"
#include "event_log.h"

// ============================================================
// Shared CSS and Navigation (referenced by all page functions)
// ============================================================

static const char WEB_CSS[] PROGMEM = R"CSS(
<style>
*{box-sizing:border-box}
body{font-family:sans-serif;margin:0;background:#0f1011;color:#f7f8f8;font-size:15px}
nav{background:#08090a;padding:8px 12px;display:flex;flex-wrap:wrap;gap:6px;border-bottom:2px solid #5e6ad2}
nav a{color:#5e6ad2;text-decoration:none;padding:4px 10px;border-radius:4px;font-size:14px}
nav a:hover,nav a.act{background:#191a1b;color:#fff}
.wrap{padding:10px 12px;max-width:900px;margin:0 auto}
h2{color:#5e6ad2;margin:10px 0 6px;font-size:1.2em}
h3{color:#d0d6e0;margin:6px 0 4px;font-size:1em}
.card{background:#191a1b;border-radius:6px;padding:10px;margin:8px 0}
table{border-collapse:collapse;width:100%;margin:4px 0}
th,td{border:1px solid #2e2e2e;padding:5px 8px;text-align:left;font-size:13px}
th{background:#08090a;color:#d0d6e0}
.on{color:#66bb6a;font-weight:bold}.off{color:#ef5350}
.src-manual{color:#ffd54f}.src-rule{color:#ff8a65}.src-timer{color:#ce93d8}.src-none{color:#78909c}
.bon{background:#43a047;color:#fff;border:none;padding:3px 8px;border-radius:3px;cursor:pointer;font-size:12px}
.bof{background:#e53935;color:#fff;border:none;padding:3px 8px;border-radius:3px;cursor:pointer;font-size:12px}
.bdel{background:#3e3e44;color:#fff;border:none;padding:3px 8px;border-radius:3px;cursor:pointer;font-size:12px}
.bedit{background:#1976d2;color:#fff;border:none;padding:3px 8px;border-radius:3px;cursor:pointer;font-size:12px}
input[type=text],input[type=number],select{
  padding:4px 6px;background:#1a1a1f;color:#eee;
  border:1px solid #3e3e44;border-radius:3px;font-size:14px;width:100%}
input[type=checkbox]{width:auto;margin-right:4px}
input[type=submit],.bsave{
  background:#1976d2;color:#fff;border:none;
  padding:7px 18px;border-radius:4px;cursor:pointer;margin-top:8px;font-size:14px}
.frow{margin:5px 0}label{display:block;font-size:13px;margin-bottom:2px;color:#d0d6e0}
.fgrid{display:grid;grid-template-columns:1fr 1fr;gap:8px}
@media(max-width:520px){.fgrid{grid-template-columns:1fr}}
.note{color:#8a8f98;font-size:0.82em}
.badge{display:inline-block;padding:2px 7px;border-radius:10px;font-size:11px}
.b-relay{background:#1b5e20}.b-sensor{background:#0d47a1}
.b-system{background:#2e2e2e}.b-rule{background:#e65100}.b-timer{background:#4a148c}
#toast{position:fixed;bottom:16px;right:16px;background:#2e2e2e;color:#fff;
  padding:8px 16px;border-radius:4px;display:none;font-size:13px}
</style>
)CSS";

// ============================================================
// Helper: send common HTTP 200 HTML header
// ============================================================
static inline void sendHtmlHeader(WiFiClient& client) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html; charset=UTF-8");
  client.println("Connection: close");
  client.println();
}

static inline void sendJsonHeader(WiFiClient& client) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: application/json");
  client.println("Access-Control-Allow-Origin: *");
  client.println("Connection: close");
  client.println();
}

static inline void sendDocStart(WiFiClient& client, const char* title, const char* activePage) {
  sendHtmlHeader(client);
  client.print("<!DOCTYPE html><html><head>"
               "<meta charset=UTF-8>"
               "<meta name=viewport content='width=device-width,initial-scale=1'>"
               "<title>");
  client.print(title);
  client.print(" - 温室制御</title>");
  client.print(WEB_CSS);
  client.println("</head><body>");
  // Nav with active page highlight
  client.print("<nav>");
  const char* pages[][2] = {
    {"/",         "ダッシュボード"},
    {"/rules",    "ルール"},
    {"/schedule", "タイマー"},
    {"/config",   "設定"},
    {"/log",      "ログ"}
  };
  for (int i = 0; i < 5; i++) {
    bool act = (strcmp(pages[i][0], activePage) == 0);
    client.print("<a href='");
    client.print(pages[i][0]);
    client.print("'");
    if (act) client.print(" class='act'");
    client.print(">");
    client.print(pages[i][1]);
    client.print("</a>");
  }
  client.println("</nav>");
  client.println("<div id='toast'></div>");
  client.println("<script>"
    "function toast(msg,ok){"
    "var t=document.getElementById('toast');"
    "t.textContent=msg;t.style.background=ok?'#2e7d32':'#b71c1c';"
    "t.style.display='block';setTimeout(function(){t.style.display='none';},3000);}"
    "</script>");
  client.println("<div class='wrap'>");
}

// ============================================================
// Page 1: Dashboard (/) — static shell, JS fetches /api/state
// ============================================================

static const char DASHBOARD_HTML[] PROGMEM = R"DASH(
<h2>ダッシュボード</h2>
<div class='card' id='statusbar'>読込中...</div>
<div class='card'>
  <h3>センサー</h3>
  <table><thead><tr><th>項目</th><th>値</th></tr></thead>
  <tbody id='sens'><tr><td colspan=2>読込中...</td></tr></tbody>
  </table>
</div>
<div class='card'>
  <h3>リレー状態</h3>
  <table><thead><tr><th>CH</th><th>名前</th><th>状態</th><th>制御元</th><th>操作</th></tr></thead>
  <tbody id='rtbl'><tr><td colspan=5>読込中...</td></tr></tbody>
  </table>
</div>
<div class='card' id='activerules'></div>
<div class='card' id='nextsched'></div>
<script>
var names=['CH1','CH2','CH3','CH4','CH5','CH6','CH7','CH8'];
function srcLabel(s){
  if(s==='manual')return "<span class='src-manual'>手動</span>";
  if(s==='rule')return "<span class='src-rule'>ルール</span>";
  if(s==='timer')return "<span class='src-timer'>タイマー</span>";
  return "<span class='src-none'>--</span>";
}
function relayCtrl(ch,v){
  fetch('/api/relay/'+ch,{method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({value:v})
  }).then(function(r){return r.json();}).then(function(d){
    toast(d.ok?'CH'+ch+(v?' ON':' OFF')+'完了':'エラー',d.ok);
    load();
  }).catch(function(){toast('通信エラー',false);});
}
function load(){
  fetch('/api/state').then(function(r){return r.json();}).then(function(d){
    // status bar
    var jst=new Date((d.epoch+32400)*1000);
    var ts=jst.toLocaleString('ja-JP',{timeZone:'UTC'});
    document.getElementById('statusbar').innerHTML=
      '<b>'+d.node_name+'</b> ('+d.node_id+')'+
      ' &nbsp;|&nbsp; FW: '+d.fw+
      ' &nbsp;|&nbsp; 稼働: '+Math.floor(d.uptime/60)+'分'+
      ' &nbsp;|&nbsp; 現在時刻: '+ts+
      ' &nbsp;|&nbsp; NTP: <span class="'+(d.ntp_ok?'on':'off')+'">'+(d.ntp_ok?'OK':'未同期')+'</span>';
    // sensors
    var sv='';
    var smap=[
      ['温度(°C)',d.sensors.temp,'1'],
      ['湿度(%)',d.sensors.hum,'1'],
      ['VPD(kPa)',d.sensors.vpd,'2'],
      ['降水量(mm)',d.sensors.rain,'1']
    ];
    for(var i=0;i<smap.length;i++){
      var val=smap[i][1];
      sv+='<tr><td>'+smap[i][0]+'</td><td>'+(val===null?'--':parseFloat(val).toFixed(parseInt(smap[i][2])))+'</td></tr>';
    }
    // DI summary
    var diOn=[];
    for(var i=0;i<8;i++){if(d.di[i])diOn.push('DI'+(i+1));}
    sv+='<tr><td>DI入力</td><td>'+(diOn.length?diOn.join(', '):'なし')+'</td></tr>';
    document.getElementById('sens').innerHTML=sv;
    // relays
    var rt='';
    for(var i=0;i<8;i++){
      var r=d.relays[i];
      var nm=r.name||names[i];
      rt+='<tr>'+
        '<td>'+(i+1)+'</td>'+
        '<td>'+nm+'</td>'+
        '<td class="'+(r.on?'on':'off')+'">'+(r.on?'ON':'OFF')+'</td>'+
        '<td>'+srcLabel(r.source)+(r.source_name?' '+r.source_name:'')+'</td>'+
        '<td>'+
          '<button class=bon onclick="relayCtrl('+(i+1)+',1)">ON</button> '+
          '<button class=bof onclick="relayCtrl('+(i+1)+',0)">OFF</button>'+
        '</td>'+
        '</tr>';
    }
    document.getElementById('rtbl').innerHTML=rt;
    // active rules
    var ar='<h3>動作中ルール</h3>';
    if(d.active_rules&&d.active_rules.length){
      ar+='<table><thead><tr><th>ID</th><th>名前</th><th>センサー値</th></tr></thead><tbody>';
      for(var i=0;i<d.active_rules.length;i++){
        var rl=d.active_rules[i];
        ar+='<tr><td>'+rl.id+'</td><td>'+rl.name+'</td><td>'+rl.sensor_val.toFixed(2)+'</td></tr>';
      }
      ar+='</tbody></table>';
    }else{ar+='<span class=note>なし</span>';}
    document.getElementById('activerules').innerHTML=ar;
    // next schedules
    var ns='<h3>次のタイマー</h3>';
    if(d.next_schedules&&d.next_schedules.length){
      ns+='<table><thead><tr><th>ID</th><th>名前</th><th>リレー</th><th>時刻</th><th>あと(分)</th></tr></thead><tbody>';
      for(var i=0;i<d.next_schedules.length;i++){
        var sc=d.next_schedules[i];
        ns+='<tr><td>'+sc.id+'</td><td>'+sc.name+'</td><td>CH'+sc.relay+'</td><td>'+sc.time+'</td><td>'+sc.in_min+'</td></tr>';
      }
      ns+='</tbody></table>';
    }else{ns+='<span class=note>登録なし</span>';}
    document.getElementById('nextsched').innerHTML=ns;
  }).catch(function(){
    document.getElementById('statusbar').innerHTML='<span class=off>通信エラー — 再接続中...</span>';
  });
}
load();setInterval(load,5000);
</script>
)DASH";

static inline void sendDashboard(WiFiClient& client) {
  sendDocStart(client, "ダッシュボード", "/");
  client.print(DASHBOARD_HTML);
  client.println("</div></body></html>");
}

// ============================================================
// Page 2: Rules (/rules)
// ============================================================

static const char RULES_HTML[] PROGMEM = R"RULES(
<h2>ルール管理</h2>
<div class='card'>
  <h3>ルール一覧</h3>
  <div style='overflow-x:auto'>
  <table><thead>
    <tr><th>ID</th><th>名前</th><th>センサー</th><th>条件</th><th>ON閾値</th><th>OFF閾値</th><th>リレー</th><th>有効</th><th>操作</th></tr>
  </thead><tbody id='rtbl'><tr><td colspan=9>読込中...</td></tr></tbody></table>
  </div>
</div>
<div class='card'>
  <h3 id='formtitle'>ルール追加</h3>
  <form id='rform'>
  <input type=hidden id='rid' name='id' value=''>
  <div class='fgrid'>
    <div class='frow'><label>名前</label><input type=text name='name' id='rname' required maxlength=24></div>
    <div class='frow'><label>センサー</label>
      <select name='sensor' id='rsen'>
        <option value=0>温度</option><option value=1>湿度</option>
        <option value=2>VPD</option><option value=3>降水量</option>
        <option value=4>DI1</option><option value=5>DI2</option>
        <option value=6>DI3</option><option value=7>DI4</option>
        <option value=8>DI5</option><option value=9>DI6</option>
        <option value=10>DI7</option><option value=11>DI8</option>
      </select>
    </div>
    <div class='frow'><label>条件</label>
      <select name='cond' id='rcond'>
        <option value=0>＞（以上）</option>
        <option value=1>＜（以下）</option>
      </select>
    </div>
    <div class='frow'><label>ON閾値</label><input type=number name='onValue' id='ronv' step=0.1 value=25.0 required></div>
    <div class='frow'><label>OFF閾値</label><input type=number name='offValue' id='rofv' step=0.1 value=23.0 required></div>
    <div class='frow'><label>対象リレー</label>
      <select name='relayCh' id='rrch'>
        <option value=1>CH1</option><option value=2>CH2</option><option value=3>CH3</option>
        <option value=4>CH4</option><option value=5>CH5</option><option value=6>CH6</option>
        <option value=7>CH7</option><option value=8>CH8</option>
      </select>
    </div>
    <div class='frow'><label>&nbsp;</label>
      <label><input type=checkbox name='inverted' id='rinv' value=1> 反転（アクティブLOW）</label>
    </div>
    <div class='frow'><label>&nbsp;</label>
      <label><input type=checkbox name='enabled' id='ren' value=1 checked> 有効</label>
    </div>
  </div>
  <input type=submit class=bsave value='保存'>
  <button type=button class=bdel onclick='cancelEdit()' style='margin-left:8px'>キャンセル</button>
  </form>
</div>
<script>
var SENSOR_NAMES=['温度','湿度','VPD','降水量','DI1','DI2','DI3','DI4','DI5','DI6','DI7','DI8'];
var editId=null;
function loadRules(){
  fetch('/api/rules').then(function(r){return r.json();}).then(function(rules){
    var html='';
    for(var i=0;i<rules.length;i++){
      var r=rules[i];
      html+='<tr>'+
        '<td>'+r.id+'</td>'+
        '<td>'+r.name+'</td>'+
        '<td>'+SENSOR_NAMES[r.sensor]+'</td>'+
        '<td>'+(r.cond===0?'＞':'＜')+'</td>'+
        '<td>'+r.onValue+'</td>'+
        '<td>'+r.offValue+'</td>'+
        '<td>CH'+r.relayCh+'</td>'+
        '<td>'+(r.enabled?'<span class=on>有効</span>':'<span class=off>無効</span>')+'</td>'+
        '<td>'+
          '<button class=bedit onclick="editRule('+r.id+')">編集</button> '+
          '<button class=bdel onclick="delRule('+r.id+')">削除</button>'+
        '</td></tr>';
    }
    document.getElementById('rtbl').innerHTML=html||'<tr><td colspan=9>ルールなし</td></tr>';
  });
}
function editRule(id){
  fetch('/api/rules').then(function(r){return r.json();}).then(function(rules){
    var r=rules.find(function(x){return x.id===id;});
    if(!r)return;
    editId=id;
    document.getElementById('formtitle').textContent='ルール編集 (ID:'+id+')';
    document.getElementById('rid').value=id;
    document.getElementById('rname').value=r.name;
    document.getElementById('rsen').value=r.sensor;
    document.getElementById('rcond').value=r.cond;
    document.getElementById('ronv').value=r.onValue;
    document.getElementById('rofv').value=r.offValue;
    document.getElementById('rrch').value=r.relayCh;
    document.getElementById('rinv').checked=r.inverted;
    document.getElementById('ren').checked=r.enabled;
    document.getElementById('rform').scrollIntoView({behavior:'smooth'});
  });
}
function cancelEdit(){
  editId=null;
  document.getElementById('formtitle').textContent='ルール追加';
  document.getElementById('rform').reset();
  document.getElementById('rid').value='';
}
function delRule(id){
  if(!confirm('ID:'+id+' を削除しますか?'))return;
  var fd=new FormData();fd.append('id',id);
  fetch('/api/rules/delete',{method:'POST',body:new URLSearchParams(fd)})
    .then(function(){toast('削除しました',true);loadRules();})
    .catch(function(){toast('エラー',false);});
}
document.getElementById('rform').addEventListener('submit',function(e){
  e.preventDefault();
  var fd=new FormData(e.target);
  if(!fd.get('inverted'))fd.set('inverted','0');
  if(!fd.get('enabled'))fd.set('enabled','0');
  fetch('/api/rules',{method:'POST',body:new URLSearchParams(fd)})
    .then(function(r){return r.json();}).then(function(d){
      toast(d.ok?'保存しました':'エラー: '+d.err,d.ok);
      if(d.ok){cancelEdit();loadRules();}
    }).catch(function(){toast('通信エラー',false);});
});
loadRules();
</script>
)RULES";

static inline void sendRulesPage(WiFiClient& client) {
  sendDocStart(client, "ルール", "/rules");
  client.print(RULES_HTML);
  client.println("</div></body></html>");
}

// ============================================================
// Page 3: Schedule (/schedule)
// ============================================================

static const char SCHEDULE_HTML[] PROGMEM = R"SCHED(
<h2>タイマー管理</h2>
<div class='card'>
  <h3>タイマー一覧</h3>
  <div style='overflow-x:auto'>
  <table><thead>
    <tr><th>ID</th><th>名前</th><th>リレー</th><th>時刻</th><th>時間(分)</th><th>曜日</th><th>有効</th><th>操作</th></tr>
  </thead><tbody id='stbl'><tr><td colspan=8>読込中...</td></tr></tbody></table>
  </div>
</div>
<div class='card'>
  <h3 id='sformtitle'>タイマー追加</h3>
  <form id='sform'>
  <input type=hidden id='sid' name='id' value=''>
  <div class='fgrid'>
    <div class='frow'><label>名前</label><input type=text name='name' id='sname' required maxlength=24></div>
    <div class='frow'><label>対象リレー</label>
      <select name='relayCh' id='srch'>
        <option value=1>CH1</option><option value=2>CH2</option><option value=3>CH3</option>
        <option value=4>CH4</option><option value=5>CH5</option><option value=6>CH6</option>
        <option value=7>CH7</option><option value=8>CH8</option>
      </select>
    </div>
    <div class='frow'><label>開始時刻</label>
      <div style='display:flex;gap:6px;align-items:center'>
        <select name='hour' id='shour' style='width:70px'></select>
        <span>時</span>
        <select name='minute' id='smin' style='width:70px'></select>
        <span>分</span>
      </div>
    </div>
    <div class='frow'><label>継続時間(分)</label><input type=number name='duration' id='sdur' min=1 max=1440 value=10 required></div>
    <div class='frow' style='grid-column:1/-1'>
      <label>曜日</label>
      <div style='display:flex;gap:8px;flex-wrap:wrap'>
        <label><input type=checkbox name='dow_1' id='d1' checked> 月</label>
        <label><input type=checkbox name='dow_2' id='d2' checked> 火</label>
        <label><input type=checkbox name='dow_3' id='d3' checked> 水</label>
        <label><input type=checkbox name='dow_4' id='d4' checked> 木</label>
        <label><input type=checkbox name='dow_5' id='d5' checked> 金</label>
        <label><input type=checkbox name='dow_6' id='d6' checked> 土</label>
        <label><input type=checkbox name='dow_0' id='d0' checked> 日</label>
      </div>
    </div>
    <div class='frow'><label>&nbsp;</label>
      <label><input type=checkbox name='enabled' id='sen' value=1 checked> 有効</label>
    </div>
  </div>
  <input type=submit class=bsave value='保存'>
  <button type=button class=bdel onclick='cancelSEdit()' style='margin-left:8px'>キャンセル</button>
  </form>
</div>
<script>
var DOW_NAMES=['日','月','火','水','木','金','土'];
// Populate hour/minute selects
(function(){
  var hs=document.getElementById('shour');
  var ms=document.getElementById('smin');
  for(var h=0;h<24;h++){var o=document.createElement('option');o.value=h;o.text=('0'+h).slice(-2);hs.appendChild(o);}
  for(var m=0;m<60;m+=5){var o=document.createElement('option');o.value=m;o.text=('0'+m).slice(-2);ms.appendChild(o);}
})();
function dowStr(mask){
  var days=[];
  for(var i=0;i<7;i++){if((mask>>i)&1)days.push(DOW_NAMES[i]);}
  return days.join(' ');
}
function loadSchedules(){
  fetch('/api/schedules').then(function(r){return r.json();}).then(function(scheds){
    var html='';
    for(var i=0;i<scheds.length;i++){
      var s=scheds[i];
      html+='<tr>'+
        '<td>'+s.id+'</td>'+
        '<td>'+s.name+'</td>'+
        '<td>CH'+s.relayCh+'</td>'+
        '<td>'+('0'+s.hour).slice(-2)+':'+('0'+s.minute).slice(-2)+'</td>'+
        '<td>'+Math.floor(s.durationSec/60)+'</td>'+
        '<td>'+dowStr(s.dowMask)+'</td>'+
        '<td>'+(s.enabled?'<span class=on>有効</span>':'<span class=off>無効</span>')+'</td>'+
        '<td>'+
          '<button class=bedit onclick="editSched('+s.id+')">編集</button> '+
          '<button class=bdel onclick="delSched('+s.id+')">削除</button>'+
        '</td></tr>';
    }
    document.getElementById('stbl').innerHTML=html||'<tr><td colspan=8>タイマーなし</td></tr>';
  });
}
function editSched(id){
  fetch('/api/schedules').then(function(r){return r.json();}).then(function(scheds){
    var s=scheds.find(function(x){return x.id===id;});
    if(!s)return;
    document.getElementById('sformtitle').textContent='タイマー編集 (ID:'+id+')';
    document.getElementById('sid').value=id;
    document.getElementById('sname').value=s.name;
    document.getElementById('srch').value=s.relayCh;
    document.getElementById('shour').value=s.hour;
    document.getElementById('smin').value=s.minute;
    document.getElementById('sdur').value=Math.floor(s.durationSec/60);
    for(var i=0;i<7;i++){
      var cb=document.getElementById('d'+i);
      if(cb)cb.checked=!!((s.dowMask>>i)&1);
    }
    document.getElementById('sen').checked=s.enabled;
    document.getElementById('sform').scrollIntoView({behavior:'smooth'});
  });
}
function cancelSEdit(){
  document.getElementById('sformtitle').textContent='タイマー追加';
  document.getElementById('sform').reset();
  document.getElementById('sid').value='';
  for(var i=0;i<7;i++){var cb=document.getElementById('d'+i);if(cb)cb.checked=true;}
}
function delSched(id){
  if(!confirm('ID:'+id+' を削除しますか?'))return;
  var fd=new FormData();fd.append('id',id);
  fetch('/api/schedules/delete',{method:'POST',body:new URLSearchParams(fd)})
    .then(function(){toast('削除しました',true);loadSchedules();})
    .catch(function(){toast('エラー',false);});
}
document.getElementById('sform').addEventListener('submit',function(e){
  e.preventDefault();
  var fd=new FormData(e.target);
  if(!fd.get('enabled'))fd.set('enabled','0');
  // dow checkboxes may not be in FormData if unchecked
  fetch('/api/schedules',{method:'POST',body:new URLSearchParams(fd)})
    .then(function(r){return r.json();}).then(function(d){
      toast(d.ok?'保存しました':'エラー: '+d.err,d.ok);
      if(d.ok){cancelSEdit();loadSchedules();}
    }).catch(function(){toast('通信エラー',false);});
});
loadSchedules();
</script>
)SCHED";

static inline void sendSchedulePage(WiFiClient& client) {
  sendDocStart(client, "タイマー", "/schedule");
  client.print(SCHEDULE_HTML);
  client.println("</div></body></html>");
}

// ============================================================
// Page 4: Config (/config)
// ============================================================

static inline void sendConfigPage(WiFiClient& client,
                                  const char* curNodeId,
                                  const char* curNodeName,
                                  const char* curIp,
                                  const char* curSubnet,
                                  const char* curGateway,
                                  const char* curDns,
                                  bool        curMdns,
                                  uint32_t    curBaud,
                                  uint16_t    curOverrideSec,
                                  const char* curNtpServer = "pool.ntp.org",
                                  const char* curMdnsHostname = "uecs-relay-01") {
  sendDocStart(client, "設定", "/config");
  client.println("<h2>設定</h2>");
  client.println("<div class='card'>");
  client.println("<form method=POST action='/api/config'>");
  // Identity
  client.println("<h3>ノード識別</h3><div class='fgrid'>");
  client.print("<div class='frow'><label>ノードID</label><input type=text name='node_id' value='");
  client.print(curNodeId); client.println("' maxlength=32></div>");
  client.print("<div class='frow'><label>ノード名</label><input type=text name='node_name' value='");
  client.print(curNodeName); client.println("' maxlength=32></div>");
  client.println("</div>");
  // Network
  client.println("<h3>ネットワーク</h3>");
  client.println("<p class=note>IPアドレスを空にするとDHCPになります。</p>");
  client.println("<div class='fgrid'>");
  client.print("<div class='frow'><label>IPアドレス（空=DHCP）</label><input type=text name='ip' value='");
  client.print(curIp); client.println("' placeholder='例: 192.168.1.50'></div>");
  client.print("<div class='frow'><label>サブネット</label><input type=text name='subnet' value='");
  client.print(curSubnet); client.println("'></div>");
  client.print("<div class='frow'><label>ゲートウェイ</label><input type=text name='gateway' value='");
  client.print(curGateway); client.println("'></div>");
  client.print("<div class='frow'><label>DNS</label><input type=text name='dns' value='");
  client.print(curDns); client.println("'></div>");
  client.println("</div>");
  // mDNS
  client.println("<h3>mDNS</h3><div class='fgrid'>");
  client.print("<div class='frow'><label>ホスト名</label><input type=text name='mdns_hostname' value='");
  client.print(curMdnsHostname); client.println("' maxlength=32 placeholder='uecs-relay-01'></div>");
  client.println("</div>");
  client.println("<p style='font-size:0.85em;color:#8a8f98'>アクセス: &lt;ホスト名&gt;.local</p>");
  client.print("<label><input type=checkbox name='mdns_enabled' value=1");
  if (curMdns) client.print(" checked");
  client.println("> mDNS有効</label>");
  // NTP
  client.println("<h3>NTP</h3><div class='fgrid'>");
  client.print("<div class='frow'><label>NTPサーバ</label><input type=text name='ntp_server' value='");
  client.print(curNtpServer); client.println("' maxlength=64 placeholder='pool.ntp.org'></div>");
  client.println("</div>");
  // RS485
  client.println("<h3>RS485</h3><div class='fgrid'>");
  client.print("<div class='frow'><label>ボーレート</label><input type=number name='rs485_baud' value='");
  client.print(curBaud); client.println("' min=1200 max=115200></div>");
  // Manual override
  client.print("<div class='frow'><label>手動オーバーライド時間（分、0=無期限）</label><input type=number name='override_min' value='");
  client.print(curOverrideSec / 60); client.println("' min=0 max=1440></div>");
  client.println("</div>");
  client.println("<input type=submit class=bsave value='保存して再起動'>");
  client.println("</form></div>");
  client.println("</div></body></html>");
}

// ============================================================
// Page 5: Log (/log)
// ============================================================

static const char LOG_HTML[] PROGMEM = R"LOGH(
<h2>イベントログ</h2>
<div class='card'>
  <div style='display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:6px'>
    <span class=note id='logmeta'>読込中...</span>
    <button class=bedit onclick='loadLog()'>更新</button>
  </div>
  <div style='overflow-x:auto;margin-top:8px'>
  <table><thead>
    <tr><th>時刻(JST)</th><th>種別</th><th>詳細</th></tr>
  </thead><tbody id='ltbl'><tr><td colspan=3>読込中...</td></tr></tbody></table>
  </div>
</div>
<script>
var EVT_LABELS=['リレー','センサー','システム','ルール','タイマー'];
var EVT_BADGE=['b-relay','b-sensor','b-system','b-rule','b-timer'];
function fmtTs(epoch){
  var d=new Date((epoch+32400)*1000);
  return d.toISOString().replace('T',' ').substring(0,19).replace(/\.\d+/,'');
}
function loadLog(){
  fetch('/api/log').then(function(r){return r.json();}).then(function(entries){
    document.getElementById('logmeta').textContent='全'+entries.length+'件（最新順）';
    var html='';
    for(var i=0;i<entries.length;i++){
      var e=entries[i];
      var t=e.type||0;
      var lbl=EVT_LABELS[t]||'不明';
      var cls=EVT_BADGE[t]||'b-system';
      html+='<tr>'+
        '<td style="white-space:nowrap">'+fmtTs(e.ts)+'</td>'+
        '<td><span class="badge '+cls+'">'+lbl+'</span></td>'+
        '<td>'+e.msg+'</td>'+
        '</tr>';
    }
    document.getElementById('ltbl').innerHTML=html||'<tr><td colspan=3>ログなし</td></tr>';
  }).catch(function(){toast('読込エラー',false);});
}
loadLog();setInterval(loadLog,10000);
</script>
)LOGH";

static inline void sendLogPage(WiFiClient& client) {
  sendDocStart(client, "ログ", "/log");
  client.print(LOG_HTML);
  client.println("</div></body></html>");
}

// ============================================================
// API: GET /api/state
// ============================================================

static inline void sendAPIState(WiFiClient& client,
                                uint8_t      relayState,
                                float        temp,
                                float        hum,
                                float        vpd,
                                float        rain,
                                bool         di[8],
                                Rule         rules[],
                                int          ruleCount,
                                Schedule     scheds[],
                                int          schedCount,
                                uint8_t      chCtrl[8],   // 0=none,1=manual,2=rule,3=timer
                                uint8_t      chSrcId[8],  // rule/sched id for source
                                const char*  chSrcName[], // name of rule/sched
                                unsigned long utcEpoch,
                                bool         ntpOk,
                                const char*  fwVersion,
                                const char*  nodeId,
                                const char*  nodeName) {
  // Build JSON in a buffer — use ArduinoJson
  JsonDocument doc;

  doc["node_id"]   = nodeId;
  doc["node_name"] = nodeName;
  doc["fw"]        = fwVersion;
  doc["uptime"]    = (unsigned long)(millis() / 1000UL);
  doc["epoch"]     = utcEpoch;
  doc["ntp_ok"]    = ntpOk;

  JsonObject sensors = doc["sensors"].to<JsonObject>();
  if (!isnan(temp)) sensors["temp"] = serialized(String(temp, 1));
  else              sensors["temp"] = nullptr;
  if (!isnan(hum))  sensors["hum"]  = serialized(String(hum, 1));
  else              sensors["hum"]  = nullptr;
  if (!isnan(vpd))  sensors["vpd"]  = serialized(String(vpd, 2));
  else              sensors["vpd"]  = nullptr;
  if (!isnan(rain)) sensors["rain"] = serialized(String(rain, 1));
  else              sensors["rain"] = nullptr;

  JsonArray relays = doc["relays"].to<JsonArray>();
  for (int i = 0; i < 8; i++) {
    JsonObject r = relays.add<JsonObject>();
    r["ch"]   = i + 1;
    r["on"]   = (bool)((relayState >> i) & 0x01);
    // chSrcName may be null for a given channel
    r["name"] = (chSrcName && chSrcName[i]) ? chSrcName[i] : "";
    if (chCtrl && chCtrl[i] == 1) {
      r["source"] = "manual"; r["source_name"] = "";
    } else if (chCtrl && chCtrl[i] == 2) {
      r["source"] = "rule";
      r["source_name"] = (chSrcName && chSrcName[i]) ? chSrcName[i] : "";
    } else if (chCtrl && chCtrl[i] == 3) {
      r["source"] = "timer";
      r["source_name"] = (chSrcName && chSrcName[i]) ? chSrcName[i] : "";
    } else {
      r["source"] = "none"; r["source_name"] = "";
    }
  }

  JsonArray diArr = doc["di"].to<JsonArray>();
  for (int i = 0; i < 8; i++) diArr.add(di[i]);

  // Active rules
  JsonArray activeRules = doc["active_rules"].to<JsonArray>();
  for (int i = 0; i < ruleCount; i++) {
    if (!rules[i].active || !rules[i].enabled) continue;
    JsonObject ar = activeRules.add<JsonObject>();
    ar["id"]         = rules[i].id;
    ar["name"]       = rules[i].name;
    float sv = getSensorValue(rules[i].sensor, temp, hum, rain, di);
    ar["sensor_val"] = serialized(String(sv, 2));
  }

  // Next 3 upcoming schedules (enabled, not running, sorted by minutes-until)
  unsigned long jstEpoch  = utcEpoch + 32400UL;
  uint32_t daySeconds = (uint32_t)(jstEpoch % 86400UL);
  JsonArray nextScheds = doc["next_schedules"].to<JsonArray>();
  int nsCount = 0;
  // Simple: collect up to 3 enabled schedules sorted by distance from now
  // Use a small selection sort to keep it MCU-friendly
  int picked[3] = {-1, -1, -1};
  uint32_t minDist[3] = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
  for (int i = 0; i < schedCount && nsCount < 3; i++) {
    if (!scheds[i].enabled) continue;
    uint32_t startSec = (uint32_t)scheds[i].hour * 3600UL + (uint32_t)scheds[i].minute * 60UL;
    uint32_t dist = (startSec >= daySeconds) ? (startSec - daySeconds) : (86400UL - daySeconds + startSec);
    // insert into top-3 sorted list
    for (int slot = 0; slot < 3; slot++) {
      if (dist < minDist[slot]) {
        // shift right
        for (int k = 2; k > slot; k--) { minDist[k] = minDist[k-1]; picked[k] = picked[k-1]; }
        minDist[slot] = dist; picked[slot] = i;
        if (nsCount < 3) nsCount++;
        break;
      }
    }
  }
  for (int s = 0; s < 3 && picked[s] >= 0; s++) {
    Schedule& sc = scheds[picked[s]];
    char timeBuf[6];
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", sc.hour, sc.minute);
    JsonObject ns = nextScheds.add<JsonObject>();
    ns["id"]     = sc.id;
    ns["name"]   = sc.name;
    ns["relay"]  = sc.relayCh;
    ns["time"]   = timeBuf;
    ns["in_min"] = (int)(minDist[s] / 60);
  }

  char buf[2048];
  serializeJson(doc, buf, sizeof(buf));
  sendJsonHeader(client);
  client.print(buf);
}

// ============================================================
// API: GET /api/rules
// ============================================================

static inline void sendAPIRules(WiFiClient& client, Rule rules[], int count) {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < count; i++) {
    Rule& r = rules[i];
    JsonObject obj = arr.add<JsonObject>();
    obj["id"]       = r.id;
    obj["name"]     = r.name;
    obj["enabled"]  = r.enabled;
    obj["sensor"]   = (uint8_t)r.sensor;
    obj["cond"]     = (uint8_t)r.cond;
    obj["onValue"]  = serialized(String(r.onValue, 2));
    obj["offValue"] = serialized(String(r.offValue, 2));
    obj["relayCh"]  = r.relayCh;
    obj["inverted"] = r.inverted;
    obj["active"]   = r.active;
  }
  char buf[1024];
  serializeJson(doc, buf, sizeof(buf));
  sendJsonHeader(client);
  client.print(buf);
}

// ============================================================
// API: GET /api/schedules
// ============================================================

static inline void sendAPISchedules(WiFiClient& client, Schedule scheds[], int count) {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < count; i++) {
    Schedule& s = scheds[i];
    JsonObject obj = arr.add<JsonObject>();
    obj["id"]          = s.id;
    obj["name"]        = s.name;
    obj["enabled"]     = s.enabled;
    obj["relayCh"]     = s.relayCh;
    obj["hour"]        = s.hour;
    obj["minute"]      = s.minute;
    obj["durationSec"] = s.durationSec;
    obj["dowMask"]     = s.dowMask;
    obj["running"]     = s.running;
  }
  char buf[1024];
  serializeJson(doc, buf, sizeof(buf));
  sendJsonHeader(client);
  client.print(buf);
}

// ============================================================
// API: GET /api/log
// ============================================================

static inline void sendAPILog(WiFiClient& client, EventLog& log) {
  // Stream JSON array directly to avoid large stack allocation
  sendJsonHeader(client);
  client.print("[");
  int total = log.count;
  bool first = true;
  for (int i = 0; i < total && i < LOG_MAX_ENTRIES; i++) {
    int slot = ((log.head - 1 - i) + LOG_MAX_ENTRIES) % LOG_MAX_ENTRIES;
    LogEntry& e = log.entries[slot];
    if (!first) client.print(",");
    first = false;
    char buf[160];
    // Escape msg for JSON (replace " with ', skip control chars — sufficient for Japanese UTF-8 log msgs)
    char safe[sizeof(e.msg) * 2];
    int si = 0;
    for (int j = 0; e.msg[j] && si < (int)sizeof(safe) - 2; j++) {
      char c = e.msg[j];
      if (c == '"')      { safe[si++] = '\\'; safe[si++] = '"'; }
      else if (c == '\\') { safe[si++] = '\\'; safe[si++] = '\\'; }
      else if (c == '\n' || c == '\r') { /* skip */ }
      else               { safe[si++] = c; }
    }
    safe[si] = '\0';
    snprintf(buf, sizeof(buf),
             "{\"ts\":%lu,\"type\":%u,\"ch\":%u,\"val\":%u,\"sid\":%u,\"msg\":\"%s\"}",
             e.ts, (unsigned)e.type, (unsigned)e.relayCh, (unsigned)e.value, (unsigned)e.sourceId, safe);
    client.print(buf);
  }
  client.print("]");
}

// ============================================================
// API: Reboot response helper
// ============================================================

static inline void sendRebootResponse(WiFiClient& client) {
  sendHtmlHeader(client);
  client.println("<!DOCTYPE html><html><head><meta charset=UTF-8>"
                 "<meta http-equiv='refresh' content='8;url=/'></head><body>"
                 "<p style='font-family:sans-serif;color:#eee;background:#0f1011;"
                 "padding:24px;margin:0'>再起動中... 8秒後にダッシュボードへ戻ります。</p>"
                 "</body></html>");
}

// ============================================================
// 404 helper
// ============================================================

static inline void send404(WiFiClient& client) {
  client.println("HTTP/1.1 404 Not Found");
  client.println("Content-Type: text/plain");
  client.println("Connection: close");
  client.println();
  client.println("404 Not Found");
}
