import { chromium } from 'playwright';

const BASE = 'http://127.0.0.1:8000/win11-ui/nexos-desktop.html';
const sleep = ms => new Promise(r => setTimeout(r, ms));

const browser = await chromium.launch({ executablePath: 'C:\\Users\\trans\\AppData\\Local\\ms-playwright\\chromium-1234\\chrome-win64\\chrome.exe', headless: true });
const page = await browser.newPage();
const logs = [];
page.on('console', m => logs.push('[PAGE] ' + m.text()));
page.on('pageerror', e => logs.push('[PAGEERR] ' + e.message));

await page.goto(BASE, { waitUntil: 'load' });
console.log('STEP loaded page');

// wait for NexOS global + auto-connect (we patched auto-connect on load)
try {
  await page.waitForFunction(() => window.NexOS && window.NexOS.connected === true, { timeout: 20000 });
  console.log('STEP NexOS connected to bridge');
} catch (e) {
  const st = await page.evaluate(() => ({
    hasNexOS: !!window.NexOS,
    connected: window.NexOS ? window.NexOS.connected : null,
    host: window.NexOS ? window.NexOS.host : null,
    port: window.NexOS ? window.NexOS.port : null
  }));
  console.log('STEP NexOS NOT connected yet:', JSON.stringify(st));
  console.log('--- recent page logs ---');
  logs.slice(-20).forEach(l => console.log(l));
  await browser.close();
  process.exit(2);
}

// login via lock screen
await page.waitForSelector('#loginName', { timeout: 10000 });
await page.fill('#loginName', 'nexos');
await page.fill('#loginPwd', 'nexos');
await page.click('#loginBtn');
await page.waitForFunction(() => !document.body.classList.contains('locked'), { timeout: 8000 });
console.log('STEP logged in');
// ensure connect panel is hidden (it auto-hides on successful login)
await page.evaluate(() => { const c = document.querySelector('#conn'); if (c) c.style.display = 'none'; });

// ---- raw kernel probe: what does the kernel actually return for ls/ps? ----
const probe = await page.evaluate(() => new Promise(res => {
  const out = {};
  window.NexOS.run('ls', (r, e) => {
    out.ls = { resp: r, err: e };
    if (r && /not formatted/i.test(r)) {
      window.NexOS.run('mkfs', () => {
        window.NexOS.run('ls', (r2) => { out.ls_after_mkfs = r2; window.NexOS.run('ps', (r3)=>{ out.ps = r3; res(out); }); });
      });
    } else { window.NexOS.run('ps', (r3)=>{ out.ps = r3; res(out); }); }
  });
}));
console.log('KERNEL PROBE =>', JSON.stringify(probe));

// ---- File Explorer ----
await page.evaluate(() => window.openApp('explorer'));
await page.waitForSelector('#fl .row', { timeout: 8000 });
// wait until ls result rendered (loading placeholder gone, any real content)
await page.waitForFunction(() => {
  const t = document.querySelector('#fl')?.textContent || '';
  return t.includes('读取内核 ls') === false && t.trim().length > 0;
}, { timeout: 8000 });
const fe = await page.evaluate(() => {
  const rows = [...document.querySelectorAll('#fl .row')];
  return {
    total: rows.length,
    dirs: document.querySelectorAll('#fl .row.dir').length,
    files: document.querySelectorAll('#fl .row.file').length,
    sample: rows.slice(0, 6).map(r => r.textContent.trim())
  };
});
console.log('FILE EXPLORER (before create) =>', JSON.stringify(fe));

// create a file directly via kernel command (bypassing prompt dialog in headless)
let created = false;
const createRes = await page.evaluate(() => new Promise(res => {
  window.NexOS.run('touch demo.txt', () => {
    window.NexOS.run('ls', (r) => res(r));
  });
}));
console.log('LS after touch =>', JSON.stringify(createRes));
await sleep(800);
// re-open Explorer so it re-runs load() and picks up the new file
await page.evaluate(() => window.openApp('explorer'));
await sleep(1500);
const fe2 = await page.evaluate(() => ({
  files: document.querySelectorAll('#fl .row.file').length,
  hasDemo: [...document.querySelectorAll('#fl .row.file')].some(r => /demo\.txt/.test(r.textContent))
}));
console.log('FILE EXPLORER (after create) =>', JSON.stringify(fe2));
created = fe2.hasDemo;

// open the created file by clicking it
if (created) {
  await page.evaluate(() => {
    const f = [...document.querySelectorAll('#fl .row.file')].find(r => /demo\.txt/.test(r.textContent));
    if (f) f.click();
  });
  await sleep(1200);
  const fview = await page.evaluate(() => {
    const v = document.querySelector('#fview');
    return v ? { visible: v.style.display !== 'none', body: (document.querySelector('#fbody')?.textContent||'').slice(0,80) } : null;
  });
  console.log('FILE OPENED =>', JSON.stringify(fview));
}

// ---- Task Manager ----
await page.evaluate(() => window.openApp('taskmgr'));
await page.waitForSelector('#pl .row', { timeout: 8000 });
await page.waitForFunction(() => {
  const t = document.querySelector('#pl')?.textContent || '';
  return /PID|未实现|未连接/.test(t);
}, { timeout: 8000 });
const tm = await page.evaluate(() => {
  const rows = [...document.querySelectorAll('#pl .row')];
  return {
    total: rows.length,
    killBtns: document.querySelectorAll('#pl .kill').length,
    sample: rows.slice(0, 6).map(r => r.textContent.trim())
  };
});
console.log('TASK MANAGER =>', JSON.stringify(tm));

// click first kill button -> should issue kill <pid>
let killFired = false;
if (tm.killBtns > 0) {
  await page.evaluate(() => document.querySelector('#pl .kill').click());
  await sleep(1500);
  killFired = true;
  console.log('STEP clicked a kill button');
}

console.log('\\n=== RESULT ===');
console.log('explorer_rows=' + fe.total + ' dirs=' + fe.dirs + ' files=' + fe.files + ' created_file=' + created);
console.log('taskmgr_rows=' + tm.total + ' kill_buttons=' + tm.killBtns + ' kill_fired=' + killFired);
const ok = created && tm.killBtns > 0 && killFired;
console.log(ok ? 'E2E FRONTEND OK' : 'E2E FRONTEND FAIL');
if (logs.length) { console.log('--- page logs ---'); logs.slice(-15).forEach(l => console.log(l)); }
await browser.close();
process.exit(ok ? 0 : 1);
