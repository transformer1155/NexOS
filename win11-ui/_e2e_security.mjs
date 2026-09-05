import { chromium } from 'playwright';
const BASE = 'http://127.0.0.1:8000/win11-ui/nexos-desktop.html';
const sleep = ms => new Promise(r => setTimeout(r, ms));

const browser = await chromium.launch({ executablePath: 'C:\\Users\\trans\\AppData\\Local\\ms-playwright\\chromium-1234\\chrome-win64\\chrome.exe', headless: true });
const page = await browser.newPage();
const logs = [];
page.on('pageerror', e => logs.push('[PAGEERR] ' + e.message));

await page.goto(BASE, { waitUntil: 'load' });
await page.waitForFunction(() => window.NexOS && window.NexOS.connected === true, { timeout: 20000 });
await page.fill('#loginName', 'nexos');
await page.fill('#loginPwd', 'nexos');
await page.click('#loginBtn');
await page.waitForFunction(() => !document.body.classList.contains('locked'), { timeout: 8000 });
await sleep(500);

// bug-fix check: #conn must be hidden after login
const connDisp = await page.evaluate(() => getComputedStyle(document.querySelector('#conn')).display);
console.log('CONN display after login =', connDisp, connDisp === 'none' ? 'OK' : 'FAIL');

// open security guard
await page.evaluate(() => window.openApp('security'));
await page.waitForSelector('#secScore', { timeout: 8000 });
// wait for scan to finish: score text should change from "— / 100"
await page.waitForFunction(() => {
  const t = document.querySelector('#secScore')?.textContent || '';
  return t.includes('/ 100') && !t.startsWith('—');
}, { timeout: 12000 }).catch(()=>{});

const res = await page.evaluate(() => ({
  score: document.querySelector('#secScore')?.textContent,
  status: document.querySelector('#secStatus')?.textContent,
  procRows: document.querySelectorAll('#secProc .row').length,
  procHasKernel: /kernel/.test(document.querySelector('#secProc')?.textContent || ''),
  killBtns: document.querySelectorAll('#secProc .kill').length,
  fsRows: document.querySelectorAll('#secFs .row').length,
  fsText: (document.querySelector('#secFs')?.textContent||'').slice(0,60),
  permText: (document.querySelector('#secPerm')?.textContent||'').slice(0,60),
  logRows: document.querySelectorAll('#secLog .row').length
}));
console.log('SECURITY =>', JSON.stringify(res, null, 0));

// click perm reset button (real command)
await page.click('#secPermReset');
await sleep(800);
const permMsg = await page.evaluate(() => document.querySelector('#secPermMsg')?.textContent);
console.log('PERM RESET msg =>', JSON.stringify(permMsg));

console.log('\n=== RESULT ===');
const ok = connDisp === 'none' && /\d/.test(res.score||'') && res.procRows > 0 && res.procHasKernel && res.fsRows > 0 && res.logRows > 0;
console.log(ok ? 'E2E SECURITY OK' : 'E2E SECURITY FAIL');
if (logs.length) logs.forEach(l => console.log(l));
await browser.close();
process.exit(ok ? 0 : 1);
