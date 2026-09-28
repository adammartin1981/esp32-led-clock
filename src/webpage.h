#pragma once

// Single-page config UI, served from flash (no filesystem/LittleFS needed).
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>LED Ring Clock</title>
<style>
  :root{
    --bg:#111318; --card:#1b1e26; --text:#eef1f6; --muted:#9aa3b2;
    --accent:#5b8cff; --border:#2a2e3a;
  }
  *{box-sizing:border-box}
  body{margin:0;font-family:-apple-system,Segoe UI,Roboto,sans-serif;background:var(--bg);color:var(--text)}
  header{padding:20px 16px;text-align:center}
  header h1{margin:0;font-size:22px}
  #clock-face{font-size:40px;font-weight:600;margin-top:6px}
  #clock-date{color:var(--muted);font-size:14px}
  main{max-width:640px;margin:0 auto;padding:0 16px 60px}
  .card{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:16px;margin-bottom:16px}
  .card h2{margin:0 0 12px;font-size:16px}
  label{display:block;font-size:13px;color:var(--muted);margin:10px 0 4px}
  input[type=color]{width:48px;height:32px;padding:0;border:none;background:none}
  input[type=text],input[type=number]{width:100%;padding:8px;border-radius:8px;border:1px solid var(--border);background:#12141a;color:var(--text)}
  input[type=range]{width:100%}
  select{width:100%;padding:8px;border-radius:8px;border:1px solid var(--border);background:#12141a;color:var(--text)}
  .row{display:flex;gap:12px;flex-wrap:wrap}
  .row > div{flex:1;min-width:120px}
  .switch{display:flex;align-items:center;gap:8px}
  button{background:var(--accent);color:#fff;border:none;padding:10px 16px;border-radius:8px;font-size:14px;cursor:pointer}
  button.secondary{background:#333947}
  button.danger{background:#a8402f}
  .btnbar{display:flex;gap:8px;flex-wrap:wrap;margin-top:12px}
  table{width:100%;border-collapse:collapse;font-size:13px}
  th,td{padding:6px 4px;border-bottom:1px solid var(--border);text-align:left}
  .days{display:flex;gap:4px;flex-wrap:wrap}
  .days label{display:flex;align-items:center;gap:3px;font-size:11px;margin:0;color:var(--text)}
  .pill{display:inline-block;padding:2px 8px;border-radius:999px;font-size:11px}
  .pill.on{background:#1e4d2b;color:#7be89b}
  .pill.off{background:#4d1e1e;color:#e87b7b}
  #status{font-size:12px;color:var(--muted);text-align:center;margin-top:8px}
  .alarm-row td{vertical-align:top}
  .remove{color:#e87b7b;cursor:pointer;font-size:12px}
</style>
</head>
<body>
<header>
  <h1>LED Ring Clock</h1>
  <div id="clock-face">--:--:--</div>
  <div id="clock-date">-</div>
  <div id="status">connecting…</div>
</header>
<main>

  <div class="card">
    <h2>Display style</h2>
    <div class="row">
      <div><label>Hour hand</label><input type="color" id="hourColor"></div>
      <div><label>Minute hand</label><input type="color" id="minuteColor"></div>
      <div><label>Second hand</label><input type="color" id="secondColor"></div>
      <div><label>Tick marks</label><input type="color" id="tickColor"></div>
    </div>
    <label>Brightness</label>
    <input type="range" id="brightness" min="4" max="255" step="1">
    <div class="row">
      <div>
        <label>Background mode</label>
        <select id="bgMode">
          <option value="0">Off</option>
          <option value="1">Dim hour ticks</option>
          <option value="2">Rainbow sweep</option>
        </select>
      </div>
      <div>
        <label>Time format</label>
        <select id="use24Hour">
          <option value="true">24 hour</option>
          <option value="false">12 hour</option>
        </select>
      </div>
    </div>
    <label>Timezone (POSIX TZ string)</label>
    <input type="text" id="timezone" placeholder="GMT0BST,M3.5.0/1,M10.5.0">
    <div class="btnbar">
      <button onclick="saveConfig()">Save style</button>
    </div>
  </div>

  <div class="card">
    <h2>Alarms</h2>
    <table id="alarmTable">
      <thead><tr><th>Time</th><th>Days</th><th>Label</th><th>Color</th><th>On</th><th></th></tr></thead>
      <tbody id="alarmBody"></tbody>
    </table>
    <div class="btnbar">
      <button class="secondary" onclick="addAlarmRow()">+ Add alarm</button>
      <button onclick="saveAlarms()">Save alarms</button>
      <button class="danger" onclick="stopAlarm()">Stop ringing alarm</button>
    </div>
  </div>

  <div class="card">
    <h2>Wi-Fi</h2>
    <p style="color:var(--muted);font-size:13px">Forget the saved network and reboot into setup-hotspot mode so you can connect it to a different Wi-Fi.</p>
    <button class="danger" onclick="resetWifi()">Forget Wi-Fi &amp; restart</button>
  </div>

</main>

<script>
const DAY_LABELS = ['Su','Mo','Tu','We','Th','Fr','Sa'];

async function loadStatus(){
  try{
    const r = await fetch('/api/status');
    const s = await r.json();
    document.getElementById('clock-face').textContent = s.time;
    document.getElementById('clock-date').textContent = s.date;
    document.getElementById('status').textContent = 'IP ' + s.ip + (s.alarmActive ? ' — ALARM RINGING' : '');
  }catch(e){
    document.getElementById('status').textContent = 'disconnected';
  }
}

async function loadConfig(){
  const r = await fetch('/api/config');
  const c = await r.json();
  hourColor.value = c.hourColor;
  minuteColor.value = c.minuteColor;
  secondColor.value = c.secondColor;
  tickColor.value = c.tickColor;
  brightness.value = c.brightness;
  bgMode.value = c.bgMode;
  use24Hour.value = c.use24Hour ? 'true' : 'false';
  timezone.value = c.timezone;
}

async function saveConfig(){
  const body = {
    hourColor: hourColor.value,
    minuteColor: minuteColor.value,
    secondColor: secondColor.value,
    tickColor: tickColor.value,
    brightness: parseInt(brightness.value,10),
    bgMode: parseInt(bgMode.value,10),
    use24Hour: use24Hour.value === 'true',
    timezone: timezone.value
  };
  await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
}

function alarmRowTemplate(a){
  const tr = document.createElement('tr');
  tr.className = 'alarm-row';
  const daysBoxes = DAY_LABELS.map((lbl,i)=>{
    const checked = (a.days & (1<<i)) ? 'checked' : '';
    return `<label><input type="checkbox" data-day="${i}" ${checked}>${lbl}</label>`;
  }).join('');
  tr.innerHTML = `
    <td><input type="text" style="width:70px" class="a-time" value="${String(a.hour).padStart(2,'0')}:${String(a.minute).padStart(2,'0')}"></td>
    <td><div class="days">${daysBoxes}</div></td>
    <td><input type="text" style="width:90px" class="a-label" value="${a.label || ''}"></td>
    <td><input type="color" class="a-color" value="${a.color}"></td>
    <td><input type="checkbox" class="a-enabled" ${a.enabled ? 'checked' : ''}></td>
    <td><span class="remove" onclick="this.closest('tr').remove()">remove</span></td>
  `;
  return tr;
}

function addAlarmRow(){
  alarmBody.appendChild(alarmRowTemplate({hour:7,minute:0,days:0b0111110,label:'Alarm',color:'#ff2020',enabled:true}));
}

async function loadAlarms(){
  const r = await fetch('/api/alarms');
  const list = await r.json();
  alarmBody.innerHTML = '';
  list.forEach(a => alarmBody.appendChild(alarmRowTemplate(a)));
}

async function saveAlarms(){
  const rows = [...alarmBody.querySelectorAll('.alarm-row')];
  const alarms = rows.map(tr => {
    const [h,m] = tr.querySelector('.a-time').value.split(':').map(n=>parseInt(n,10)||0);
    let days = 0;
    tr.querySelectorAll('[data-day]').forEach(cb => { if(cb.checked) days |= (1 << parseInt(cb.dataset.day,10)); });
    return {
      hour: h, minute: m, days,
      label: tr.querySelector('.a-label').value,
      color: tr.querySelector('.a-color').value,
      enabled: tr.querySelector('.a-enabled').checked
    };
  });
  await fetch('/api/alarms',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(alarms)});
  loadAlarms();
}

async function stopAlarm(){
  await fetch('/api/alarm/stop',{method:'POST'});
}

async function resetWifi(){
  if(!confirm('This will forget the saved Wi-Fi network and reboot into setup mode. Continue?')) return;
  await fetch('/api/reset-wifi',{method:'POST'});
  alert('Rebooting into setup mode — connect to the "ESP32-Clock-Setup" hotspot to reconfigure.');
}

loadStatus();
loadConfig();
loadAlarms();
setInterval(loadStatus, 1000);
</script>
</body>
</html>
)rawliteral";
