import { chromium } from 'playwright';
import { pathToFileURL, fileURLToPath } from 'url';
import { dirname, resolve } from 'path';

const __dirname = dirname(fileURLToPath(import.meta.url));
const url = pathToFileURL(resolve(__dirname, 'nexos-desktop.html')).href;

const KERNEL_NODES = [
  { ip: '10.0.2.20', role: 'ai',      weight: 2, busy: 0 },
  { ip: '10.0.2.21', role: 'compute', weight: 1, busy: 1 },
];
function kernelStatus(){
  return `[AGENT] identity: mesh-1
[AGENT] running: yes
[AGENT] nodes (${KERNEL_NODES.length}):
${KERNEL_NODES.map(n => '  ' + n.ip + ' role=' + n.role + ' weight=' + n.weight + ' busy=' + n.busy).join('\n')}
[AGENT] queue (1):
  #1 type=fib args=20 ai=0
[AGENT] pending handoff -> (none)`;
}

(async () => {
  const browser = await chromium.launch({
    executablePath: 'D:\\pw-browsers\\chromium-1234\\chrome-win64\\chrome.exe',
  });
  const page = await browser.newPage();
  const errors = [];
  page.on('pageerror', e => { errors.push('PAGEERROR: ' + e.message); console.log('PAGEERROR:', e.message); });
  page.on('console', m => { if (m.type() === 'error') { errors.push('CONSOLE: ' + m.text()); console.log('CONSOLE-ERR:', m.text()); } });

  await page.routeWebSocket('ws://127.0.0.1:8765', ws => {
    ws.onMessage(m => {
      const cmd = (typeof m === 'string') ? m : (m.text ? m.text() : String(m));
      const send = t => ws.send(t);
      if (/^distnet agent start/.test(cmd)) send('[AGENT] started agent\n[AGENT] running: yes\n[AGENT] tick=0');
      else if (/^distnet agent stop/.test(cmd)) send('[AGENT] stopped\n[AGENT] running: no');
      else if (/^distnet discover/.test(cmd)) send('[AGENT] discovery QUERY sent (broadcast)');
      else if (/^distnet agent status/.test(cmd)) send(kernelStatus());
      else if (/^distnet agent del (\S+)/.test(cmd)) {
        const ip = cmd.match(/^distnet agent del (\S+)/)[1];
        KERNEL_NODES.splice(KERNEL_NODES.findIndex(n => n.ip === ip), 1);
        send('[AGENT] node removed ' + ip);
      }
      else if (/^distnet agent add (\S+)/.test(cmd)) {
        const m = cmd.match(/^distnet agent add (\S+)(?: (\S+))?(?: (\d+))?/);
        KERNEL_NODES.push({ ip: m[1], role: m[2] || 'compute', weight: m[3] ? +m[3] : 1, busy: 0 });
        send('[AGENT] node added ' + m[1]);
      }
      else if (/^login/.test(cmd)) send('Logged in as nexos');
      else if (/^whoami/.test(cmd)) send('nexos');
      else send('(mock) ok');
    });
  });

  await page.goto(url);
  await page.waitForTimeout(500);   // let the synchronous script fully initialize (APP_META, etc.)
  await page.evaluate(() => window.NexOS.connect('127.0.0.1', '8765'));
  await page.waitForFunction(() => window.NexOS && window.NexOS.connected === true, { timeout: 5000 });

  // dismiss the login overlay so the desktop (and distnet window) is interactive
  await page.evaluate(() => { const l = document.getElementById('login'); if (l) l.style.display = 'none'; });
  await page.evaluate(() => window.openApp('distnet'));
  await page.waitForSelector('#dnStart', { timeout: 5000 });

  await page.click('#dnStart');
  await page.waitForTimeout(300);
  await page.click('#dnStatusBtn');
  await page.waitForTimeout(1500);

  const state = await page.$eval('#dnState', e => e.textContent);
  const nodeRows = await page.$$eval('#dnNodes .drow', els => els.map(e => e.textContent.replace(/\s+/g, ' ').trim()));
  const log = await page.$eval('#dnLog', e => e.textContent);
  const nodeCount = await page.$eval('#dnNodeCount', e => e.textContent);

  console.log('STATE   :', JSON.stringify(state));
  console.log('COUNT   :', JSON.stringify(nodeCount));
  console.log('NODES   :', JSON.stringify(nodeRows, null, 0));
  console.log('LOG[started] :', /started agent/.test(log));
  console.log('LOG[AGENT]   :', /\[AGENT\]/.test(log));

  // exercise Delete button on first node (after a refresh has rendered nodes)
  await page.click('#dnStatusBtn');
  await page.waitForTimeout(1200);
  const before = await page.$$eval('#dnNodes .drow', els => els.length);
  await page.click('#dnNodes .drow [data-act=del]');
  await page.waitForTimeout(800);
  const after = await page.$$eval('#dnNodes .drow', els => els.length);
  console.log('DEL clicked: rows', before, '->', after, after < before ? 'OK' : 'NOCHANGE');

  // exercise AI task
  await page.fill('#dnAi', 'what is 2+2');
  await page.click('#dnAiBtn');
  await page.waitForTimeout(300);
  const cmdEcho = await page.$eval('#dnLog', e => e.textContent);
  console.log('AI echo :', /distnet agent ai "what is 2\+2"/.test(cmdEcho));

  console.log('ERRORS  :', errors.length ? errors : 'none');
  await browser.close();
})().catch(e => { console.error('E2E FAILED:', e); process.exit(1); });
