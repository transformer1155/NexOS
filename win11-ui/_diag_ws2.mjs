import { chromium } from 'playwright';
const EXEC = 'D:\\pw-browsers\\chromium-1234\\chrome-win64\\chrome.exe';
(async () => {
  const browser = await chromium.launch({ executablePath: EXEC });
  const page = await browser.newPage();
  page.on('console', m => console.log('[PAGE]', m.text()));
  await page.goto('file:///D:/MyOS/bootloader/win11-ui/nexos-desktop.html');
  await page.waitForTimeout(500);
  // Build a raw WebSocket to the bridge and send a command directly.
  const res = await page.evaluate(() => new Promise(resolve => {
    const ws = new WebSocket('ws://127.0.0.1:8765');
    let got = [];
    ws.onopen = () => { ws.send('login nexos nexos\n'); setTimeout(()=>resolve({state:'open-sent', got}), 4000); };
    ws.onmessage = (e) => { got.push(String(e.data).slice(0,120)); };
    ws.onerror = (e) => resolve({state:'error', got});
    ws.onclose = () => resolve({state:'closed', got});
    setTimeout(() => resolve({state:'timeout', got}), 6000);
  }));
  console.log('RAW WS RESULT:', JSON.stringify(res, null, 2));
  await browser.close();
})().catch(e => { console.error('FAILED', e); process.exit(1); });
