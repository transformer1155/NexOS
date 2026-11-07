import { chromium } from 'playwright';
import { pathToFileURL, fileURLToPath } from 'url';
import { dirname, resolve } from 'path';
const __dirname = dirname(fileURLToPath(import.meta.url));
const url = pathToFileURL(resolve(__dirname, 'nexos-desktop.html')).href;
(async () => {
  const browser = await chromium.launch({ executablePath: 'D:\\pw-browsers\\chromium-1234\\chrome-win64\\chrome.exe' });
  const page = await browser.newPage();
  page.on('pageerror', e => console.log('PAGEERROR:', e.message));
  page.on('console', m => console.log('CONSOLE['+m.type()+']:', m.text()));
  await page.goto(url);
  await page.waitForTimeout(600);
  const probe = await page.evaluate(() => {
    try { return { am: (typeof APP_META), openApp: (typeof openApp), nexos: (typeof NexOS), win: (typeof window.NexOS) }; }
    catch (e) { return { err: e.message }; }
  });
  console.log('PROBE:', JSON.stringify(probe));
  await browser.close();
})().catch(e => { console.error('FAIL', e); process.exit(1); });
