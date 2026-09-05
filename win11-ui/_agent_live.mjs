import { chromium } from 'playwright';
import { pathToFileURL, fileURLToPath } from 'url';
import { dirname, resolve } from 'path';
const __dirname = dirname(fileURLToPath(import.meta.url));
const url = pathToFileURL(resolve(__dirname, 'nexos-desktop.html')).href;

(async () => {
  const browser = await chromium.launch({ executablePath: 'D:\\pw-browsers\\chromium-1234\\chrome-win64\\chrome.exe' });
  const page = await browser.newPage();
  const errors = [];
  page.on('pageerror', e => { errors.push('PAGEERROR: ' + e.message); console.log('PAGEERROR:', e.message); });
  page.on('console', m => { console.log('[PAGE]', m.type(), m.text()); });

  await page.goto(url);
  await page.waitForTimeout(500);

  // connect to the REAL bridge (which talks to the real QEMU kernel)
  await page.evaluate(() => window.NexOS.connect('127.0.0.1', '8765'));
  await page.waitForFunction(() => window.NexOS && window.NexOS.connected === true, { timeout: 8000 });
  console.log('CONNECTED to real bridge');
  // give the guest kernel time to finish booting and bring up its shell
  await page.waitForTimeout(15000);

  // diagnostics: is NexOS still connected right before login?
  const diag = await page.evaluate(() => ({
    connected: window.NexOS.connected,
    readyState: window.NexOS.sock ? window.NexOS.sock.readyState : -1,
    authed: window.NexOS.authed,
  }));
  console.log('PRE-LOGIN diag:', JSON.stringify(diag));

  // log in (real kernel shell requires it before running commands).
  // Use the proven NexOS.login path directly (handles the kernel's byte-fragmented echo).
  const loggedIn = await page.evaluate(() => new Promise(resolve => {
    window.NexOS.login('nexos', 'nexos', (ok, user, err) => resolve({ ok: !!ok, err: err || null }));
  }));
  console.log('LOGGED IN:', JSON.stringify(loggedIn));
  // dismiss the login overlay and the auto-connect retry dialog (#conn) so the desktop is interactive
  await page.evaluate(() => { ['login','conn'].forEach(id => { const e = document.getElementById(id); if (e) e.remove(); }); });
  await page.waitForTimeout(300);

  // open the Agent console
  await page.evaluate(() => window.openApp('distnet'));
  await page.waitForSelector('#dnStart', { timeout: 5000 });

  // click Start Agent -> real kernel runs distnet agent start
  await page.click('#dnStart');
  await page.waitForTimeout(5000);

  const state = await page.$eval('#dnState', e => e.textContent);
  const log = await page.$eval('#dnLog', e => e.textContent);
  const started = /\[AGENT\] started|running:\s*yes|started agent/.test(log);
  const running = /running:\s*yes/.test(log) || /● running/.test(state);
  console.log('STATE     :', JSON.stringify(state));
  console.log('LOG has [AGENT] started :', started);
  console.log('LOG has running:yes     :', running);

  // click Status -> real kernel returns agent status (nodes may be 0 with no peers)
  await page.click('#dnStatusBtn');
  await page.waitForTimeout(1200);
  const state2 = await page.$eval('#dnState', e => e.textContent);
  const count = await page.$eval('#dnNodeCount', e => e.textContent);
  console.log('AFTER STATUS -> STATE:', JSON.stringify(state2), 'COUNT:', JSON.stringify(count));

  // Scan (discover) -> real kernel broadcasts QUERY
  await page.click('#dnScan');
  await page.waitForTimeout(800);
  const scanLog = await page.$eval('#dnLog', e => e.textContent);
  console.log('SCAN broadcast sent     :', /discovery QUERY|discover/.test(scanLog));

  console.log('ERRORS    :', errors.length ? errors : 'none');
  console.log('RESULT    :', (started && running && loggedIn) ? 'AGENT LIVE OK' : 'CHECK ABOVE');
  await browser.close();
})().catch(e => { console.error('LIVE E2E FAILED:', e); process.exit(1); });
