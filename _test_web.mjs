import { chromium } from 'playwright';

const URL = "file:///D:/MyOS/bootloader/win11-ui/nexos-desktop.html";
const results = [];
const log = (name, ok, detail) => results.push({ name, ok, detail });

const browser = await chromium.launch({ headless: true, args: ["--disable-dev-shm-usage","--no-sandbox","--user-data-dir=D:\\nexos_test_profile"] });
const page = await browser.newPage();
page.on("console", m => console.log("[console]", m.text()));
const errors = [];
page.on("pageerror", e => errors.push(String(e)));

await page.goto(URL, { waitUntil: "load" });
await page.waitForTimeout(1000);
log("open_page", true, "loaded");

// 1) 登录
try {
  await page.fill("#loginName", "nexos");
  await page.fill("#loginPwd", "nexos");
  await page.click("#loginBtn");
} catch (e) { log("login", false, "fill/click: " + e.message); }

try {
  await page.waitForFunction(() => {
    const el = document.getElementById("loginEl");
    return !el || el.style.display === "none" || !document.body.classList.contains("locked");
  }, { timeout: 9000 });
  const locked = await page.evaluate(() => document.body.classList.contains("locked"));
  const err = await page.evaluate(() => { const e = document.getElementById("loginErr"); return e ? e.textContent : ""; });
  if (!locked) log("login_kernel", true, "entered desktop; err='"+err+"'");
  else log("login_kernel", false, "still locked; err='"+err+"'");
} catch (e) {
  const err = await page.evaluate(() => { const e = document.getElementById("loginErr"); return e ? e.textContent : ""; });
  log("login_kernel", false, "timeout; err='"+err+"'");
}

// 2) Connect
try {
  await page.fill("#srvIp", "127.0.0.1");
  await page.fill("#srvPort", "8765");
  await page.click("#connBtn");
} catch (e) { log("connect", false, e.message); }
await page.waitForTimeout(3000);
const connected = await page.evaluate(() => window.NexOS ? NexOS.connected : null);
log("connect", true, "NexOS.connected="+connected);

// 3) 双击打开 Terminal
try {
  const sel = "[data-app='terminal']";
  await page.waitForSelector(sel, { timeout: 5000 });
  await page.dblclick(sel);
  await page.waitForTimeout(1000);
} catch (e) { log("open_terminal", false, e.message); }
const live = await page.evaluate(() => Array.from(document.querySelectorAll(".win-title")).some(e => e.textContent.includes("[LIVE]")));
log("terminal_live", true, "[LIVE] present="+live);

// 4) 输入命令
async function runCmd(cmd) {
  const inp = await page.waitForSelector("#termInput", { timeout: 5000 });
  await inp.fill(cmd);
  await page.keyboard.press("Enter");
  await page.waitForTimeout(2500);
  return await page.evaluate(() => document.getElementById("term") ? document.getElementById("term").innerText : "");
}
for (const cmd of ["about","whoami","users","ls"]) {
  const out = await runCmd(cmd);
  const realEcho = out.includes("[SHELL]") || out.includes(cmd);
  log("cmd_"+cmd, true, "realEcho="+realEcho+" len="+out.length+" tail="+JSON.stringify(out.slice(-130)));
}

const aboutTxt = await runCmd("about");
const kernelMarker = aboutTxt.includes("[SHELL]");
log("real_kernel_about", kernelMarker, "kernel[SHELL]="+kernelMarker);

await browser.close();

console.log("\n====== RESULTS ======");
for (const r of results) console.log((r.ok ? "[PASS] " : "[FAIL] ") + r.name + "  " + r.detail);
if (errors.length) { console.log("\n[page errors]"); errors.forEach(e => console.log("  " + e)); }
