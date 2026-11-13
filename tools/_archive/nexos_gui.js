
document.querySelectorAll('.tab').forEach(t => t.addEventListener('click', () => {
  document.querySelectorAll('.tab').forEach(x => x.classList.remove('tab-active'));
  document.querySelectorAll('.panel').forEach(x => x.classList.remove('active'));
  t.classList.add('tab-active');
  document.getElementById('panel-' + t.dataset.tab).classList.add('active');
}));
function badge(type, text) {
  const cls = { ok: 'ok-badge', err: 'err-badge', warn: 'warn-badge' }[type];
  return '<span class="' + cls + '">' + text + '</span>';
}
async function bridge(method, args) {
  if (!args) args = [];
  return await window.pywebview.api[method](...args);
}
async function doNew() {
  const name = document.getElementById('new-name').value.trim();
  const dir  = document.getElementById('new-dir').value.trim();
  const r = await bridge('new_project', [name, dir]);
  document.getElementById('new-result').innerHTML = r.ok
    ? '<div class="mt-3 p-3 rounded-lg bg-slate-800">' + badge('ok', 'OK') + ' Created: <code>' + r.path + '</code></div>'
    : '<div class="mt-3 p-3 rounded-lg bg-red-900/40">' + badge('err', 'FAIL') + ' ' + r.message + '</div>';
}
async function pickNewDir() { const d = await bridge('pick_dir', []); if (d) document.getElementById('new-dir').value = d; }
async function pickBuildDir() { const d = await bridge('pick_dir', []); if (d) { document.getElementById('build-dir').value = d; listProjs(d); } }
async function listProjs(d) {
  const r = await bridge('list_projects', [d]);
  document.getElementById('build-projects').textContent = r.ok ? 'Found: ' + r.projects.join(', ') : r.error;
}
let buildPoll = null;
async function doBuild() {
  const dir = document.getElementById('build-dir').value.trim();
  const btn = document.getElementById('build-btn');
  const log = document.getElementById('build-log');
  log.textContent = ''; btn.disabled = true;
  const r = await bridge('build_start', [dir]);
  buildPoll = setInterval(async () => {
    const st = await bridge('get_job', [r.job_id]);
    log.textContent = (st.log || []).join('\n');
    log.scrollTop = log.scrollHeight;
    if (st.status === 'done') {
      clearInterval(buildPoll); btn.disabled = false;
      log.textContent += '\n\n' + (st.ok ? 'SUCCESS: ' + st.nexpack + ' (' + st.size + ' bytes)\nSHA256: ' + st.sha256 : '\n\nFAIL: ' + st.error);
    }
  }, 500);
}
async function pickVerifyFile() { const f = await bridge('pick_file', ['*.nexpack']); if (f) document.getElementById('verify-path').value = f; }
async function doVerify() {
  const p = document.getElementById('verify-path').value.trim();
  const r = await bridge('verify', [p]);
  document.getElementById('verify-result').innerHTML = r.ok
    ? '<div class="mt-3 p-3 rounded-lg bg-slate-800">' + badge('ok', 'VALID') + '<br><span class="text-xs text-slate-400">' + r.message + '</span></div>'
    : '<div class="mt-3 p-3 rounded-lg bg-red-900/40">' + badge('err', 'INVALID') + ' ' + r.message + '</div>';
}
async function pickDeployFile() { const f = await bridge('pick_file', ['*.nexpack']); if (f) document.getElementById('deploy-path').value = f; }
async function doDeploy() {
  const p = document.getElementById('deploy-path').value.trim();
  const h = document.getElementById('deploy-host').value.trim();
  const port = parseInt(document.getElementById('deploy-port').value.trim());
  const el = document.getElementById('deploy-result');
  el.innerHTML = '<div class="p-3 text-slate-400">deploying...</div>';
  const r = await bridge('deploy', [p, h, port]);
  if (r.ok) {
    const rep = r.report;
    el.innerHTML = '<div class="mt-3 p-3 rounded-lg bg-slate-800">' + badge('ok', 'DEPLOYED')
      + '<div class="text-xs text-slate-400 mt-2">app_id: <code>' + rep.app_id + '</code> | version: ' + rep.version + ' | mex: ' + rep.mex_size + ' bytes | sha256: <code>' + rep.sha256.substring(0,32) + '...</code></div></div>';
  } else {
    el.innerHTML = '<div class="mt-3 p-3 rounded-lg bg-red-900/40">' + badge('err', 'FAIL') + ' ' + (r.error || r.http) + '</div>';
  }
}
async function refreshGwStatus() {
  // 先等 Bridge 就绪
  if (!(await waitPywebview(500))) return;

  const st = await bridge('gw_status', []);
  document.getElementById('gw-indicator').textContent = st.running ? 'RUNNING ' + st.host + ':' + st.port : 'STOPPED';
  document.getElementById('gw-indicator').className = st.running ? 'ok-badge' : 'err-badge';
  document.getElementById('gw-status-text').textContent = st.running ? 'Listening on ' + st.host + ':' + st.port : 'Not running';
  const actions = document.getElementById('gw-actions');
  if (st.running) {
    actions.innerHTML = '<button class="btn btn-danger" onclick="doGwStop()">Stop</button><button class="btn bg-slate-700" onclick="refreshGwStatus()">Refresh</button>';
  } else {
    actions.innerHTML = '<button class="btn btn-success" onclick="doGwStart()">Start</button><button class="btn bg-slate-700" onclick="refreshGwStatus()">Refresh</button>';
  }
}
async function doGwStart() {
  const port = parseInt(document.getElementById('gw-port').value);
  const pkgDir = document.getElementById('gw-pkgdir').value.trim() || null;
  await bridge('gw_start', [port, pkgDir]);
  setTimeout(refreshGwStatus, 1500);
}
async function doGwStop() { await bridge('gw_stop', []); setTimeout(refreshGwStatus, 500); }

// 等待 pywebview API 就绪 (最多 5 秒)
async function waitPywebview(maxMs = 5000) {
  const start = Date.now();
  const REQUIRED = ['gw_status', 'new_project', 'verify'];
  while (Date.now() - start < maxMs) {
    if (window.pywebview && window.pywebview.api) {
      // 检查关键方法是否已被注入 (api 初始是空对象 {}，注入后才有方法)
      const hasAll = REQUIRED.every(m => typeof window.pywebview.api[m] === 'function');
      if (hasAll) return true;
    }
    await new Promise(r => setTimeout(r, 30));
  }
  return false;
}

window.addEventListener('pywebviewready', async () => {
  const ok = await waitPywebview(3000);
  const bs = document.getElementById('bridge-status');
  if (ok) {
    bs.innerHTML = '<span class="ok-badge">Backend connected ✅</span> ' + 
      '<span class="text-xs text-slate-500">11 Bridge methods ready</span>';
  } else {
    bs.innerHTML = '<span class="err-badge">Backend NOT connected ❌</span> ' +
      '<span class="text-xs text-slate-500">Try clicking "Test Connection"</span>';
  }
  refreshGwStatus();
});

// 兜底：3 秒后如果 pywebviewready 没触发，手动检查
setTimeout(async () => {
  const bs = document.getElementById('bridge-status');
  if (!bs.innerHTML.includes('connected') && !bs.innerHTML.includes('NOT')) {
    const ok = await waitPywebview(1000); if (ok) {
      bs.innerHTML = '<span class="ok-badge">Backend connected ✅</span>';
    } else {
      bs.innerHTML = '<span class="warn-badge">Waiting for backend...</span>';
    }
  }
}, 3000);

async function testBridge() {
  const bs = document.getElementById('bridge-status');
  try {
    const r = await bridge('gw_status', []);
    bs.innerHTML = '<span class="ok-badge">Backend connected ✅ ping OK</span>';
    alert('✅ Bridge connection OK!\ngw_status returned:\n' + JSON.stringify(r, null, 2));
  } catch(e) {
    bs.innerHTML = '<span class="err-badge">Bridge error: ' + e.message + '</span>';
    alert('❌ Bridge error: ' + e.message + '\n\nCheck that nexos_gui.py main() is running and has js_api=bridge set.');
  }
}
