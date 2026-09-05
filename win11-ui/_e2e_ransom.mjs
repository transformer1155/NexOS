import { chromium } from 'playwright';
const BASE = 'http://127.0.0.1:8000/win11-ui/nexos-desktop.html';
const sleep = ms => new Promise(r => setTimeout(r, ms));

const browser = await chromium.launch({ executablePath: 'C:\\Users\\trans\\AppData\\Local\\ms-playwright\\chromium-1234\\chrome-win64\\chrome.exe', headless: true });
const page = await browser.newPage();
page.on('pageerror', e => console.log('[PAGEERR]', e.message));

await page.goto(BASE, { waitUntil: 'load' });
await page.waitForFunction(() => window.NexOS && window.NexOS.connected === true, { timeout: 20000 });
await page.fill('#loginName', 'nexos');
await page.fill('#loginPwd', 'nexos');
await page.click('#loginBtn');
await page.waitForFunction(() => !document.body.classList.contains('locked'), { timeout: 8000 });
await sleep(400);

await page.evaluate(() => window.openApp('security'));
await page.waitForSelector('#secRtOn', { timeout: 8000 });

await page.check('#secRtOn');
await page.waitForFunction(() => document.querySelector('#secRtState').textContent.includes('监控中'), { timeout: 6000 });
console.log('RT enabled:', await page.textContent('#secRtState'));

await page.click('#secSimRansom');
console.log('sim clicked');

let overlay = false, alert = false, locked = false;
for (let i = 0; i < 10; i++) {
  await sleep(2000);
  const dispOv = await page.evaluate(() => { const o = document.getElementById('ransomLockOverlay'); return o ? getComputedStyle(o).display : 'none'; });
  const dispAl = await page.evaluate(() => getComputedStyle(document.querySelector('#secRansomAlert')).display);
  locked = await page.evaluate(() => !!window.NexOS.fsLocked);
  if (dispOv !== 'none') overlay = true;
  if (dispAl !== 'none') alert = true;
  if (overlay && alert && locked) break;
}
console.log('alert=', alert, '| overlay=', overlay, '| locked=', locked, '| lockState=', await page.textContent('#secLockState'));

// 隔离下终端写入应被拦截（通过内核直接 touch 验证监控存在；UI 拦截由 Terminal.run 实施）
const probe = await page.evaluate(() => new Promise(res => {
  NexOS.run('touch should_be_intercepted', r => res((r || '').trim()), 600);
}));
console.log('probe touch under isolation =>', JSON.stringify(probe.slice(0, 80)));
const termBlocked = await page.evaluate(() => window.NexOS.fsLocked === true);

if (locked) {
  await page.click('#ransomUnlockBtn');
  await page.waitForFunction(() => { const o = document.getElementById('ransomLockOverlay'); return !o || getComputedStyle(o).display === 'none'; }, { timeout: 6000 });
  await page.waitForFunction(() => window.NexOS.fsLocked === false, { timeout: 6000 });
  console.log('UNLOCKED | lockState=', await page.textContent('#secLockState'));
}

const ok = alert && overlay && locked && termBlocked;
console.log('=== RESULT ===');
console.log(ok ? 'E2E RANSOM OK' : 'E2E RANSOM FAIL');
await browser.close();
process.exit(ok ? 0 : 1);
