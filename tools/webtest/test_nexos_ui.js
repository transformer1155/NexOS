// NexOS desktop UI E2E test (Playwright, reuses local Chrome)
const { chromium } = require('playwright');
const path = require('path');

const FILE = 'file://' + path.resolve(__dirname, '..', '..', 'win11-ui', 'nexos-desktop.html');
const USER = 'nexos', PASS = 'nexos';

const wait = ms => new Promise(r => setTimeout(r, ms));

(async () => {
  const browser = await chromium.launch({ channel: 'chrome', headless: true, args: ['--no-sandbox'] });
  const page = await browser.newPage();
  const logs = [];
  page.on('console', m => logs.push('[console] ' + m.text()));
  page.on('pageerror', e => logs.push('[pageerror] ' + e.message));

  const results = [];
  const check = (name, cond, detail) => {
    results.push({ name, ok: !!cond, detail });
    console.log(`${cond ? 'PASS' : 'FAIL'} - ${name}${detail ? ' :: ' + detail : ''}`);
  };

  try {
    await page.goto(FILE, { waitUntil: 'load' });
    await wait(500);

    // 1) Login screen visible
    check('login screen visible', await page.isVisible('#loginBtn'));

    // 2) Fill credentials
    await page.fill('#loginName', USER);
    await page.fill('#loginPwd', PASS);
    await page.click('#loginBtn');
    await wait(400);

    // 3) Connection panel visible
    check('connection panel visible after login', await page.isVisible('#connBtn'));
    const host = await page.inputValue('#connHost');
    const port = await page.inputValue('#connPort');
    check('bridge host/port prefilled', host === '127.0.0.1' && port === '8765', `host=${host} port=${port}`);

    // 4) Connect
    await page.click('#connBtn');
    await wait(1500);

    // 5) Verify WebSocket connected (no error shown, status updated)
    const connErr = await page.textContent('#connErr');
    check('no connection error', !connErr || connErr.trim() === '', `err="${connErr}"`);

    // 6) Open Terminal via desktop icon (double-click to launch)
    await page.dblclick('div.dicon[data-app="terminal"]');
    await wait(800);
    check('terminal opened (#term present)', await page.isVisible('#term'));
    check('terminal input (#ti) present', await page.isVisible('#ti'));

    // 7) Run commands that forward to REAL kernel (not frontend mock)
    // Kernel output = #term lines that are NOT the input-echo (prompt + command) lines.
    const runCmd = async (cmd) => {
      const before = (await page.textContent('#term')) || '';
      const beforeLen = before.length;
      await page.fill('#ti', cmd);
      await page.press('#ti', 'Enter');
      await wait(900);
      const after = (await page.textContent('#term')) || '';
      const delta = after.slice(beforeLen);
      // strip input-echo lines ("nexos@nexos:~$ <cmd>") and bare prompt; keep kernel output only
      const lines = delta.split('\n').filter(l => !/^nexos@nexos:~\$\s/.test(l.trim()));
      return lines.join('\n').trim() || '';
    };

    const aboutOut = await runCmd('about');
    check('about -> real kernel echo', /freestanding|kernel/i.test(aboutOut), aboutOut.split(String.fromCharCode(10)).slice(0,2).join(' | '));

    const lsOut = await runCmd('ls');
    // Frontend mock never produces "MKFS not formatted" -> only kernel does
    check('ls -> real kernel echo (not mock)', /MKFS not formatted|fat|volume|dir/i.test(lsOut), lsOut.split(String.fromCharCode(10)).slice(-3).join(' | '));

    const usersOut = await runCmd('users');
    // kernel lists real accounts (root/guest/nexos)
    check('users -> real kernel echo (root/guest/nexos)', /root[\s\S]*guest/.test(usersOut), usersOut.split(String.fromCharCode(10)).slice(-4).join(' | '));

    const psOut = await runCmd('ps');
    check('ps -> real kernel echo (process list)', psOut.trim().length > 0 && !/no such/i.test(psOut), psOut.split(String.fromCharCode(10)).slice(-4).join(' | '));

    // 8) whoami is frontend mock (by design) - just ensure it prints
    const whoamiOut = await runCmd('whoami');
    check('whoami -> frontend session echo', /nexos/i.test(whoamiOut), whoamiOut.trim().split('\n').slice(-1).join(''));

    // 9) Command bus log shows backend connection
    const opLog = await page.textContent('#opLog').catch(() => '');
    check('command bus logged backend connection', /connected to NexOS/i.test(opLog || ''), (opLog || '').split('\n').filter(Boolean).slice(-3).join(' | '));

  } catch (e) {
    console.log('EXCEPTION: ' + e.message);
    results.push({ name: 'script executed without exception', ok: false, detail: e.message });
  }

  if (logs.length) {
    console.log('\n--- browser logs ---');
    logs.forEach(l => console.log(l));
  }

  const failed = results.filter(r => !r.ok);
  console.log(`\n=== SUMMARY: ${results.length - failed.length}/${results.length} passed ===`);
  await browser.close();
  process.exit(failed.length ? 1 : 0);
})();
