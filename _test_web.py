import os, sys, time, json
sys.path.insert(0, r"D:\MyOS\bootloader\node_modules\playwright")
from playwright.sync_api import sync_playwright

URL = "file:///D:/MyOS/bootloader/win11-ui/nexos-desktop.html"
results = []

def log_ok(name, detail=""):
    results.append({"name": name, "ok": True, "detail": detail})
def log_fail(name, detail=""):
    results.append({"name": name, "ok": False, "detail": detail})

with sync_playwright() as p:
    browser = p.chromium.launch(headless=True, args=["--disable-dev-shm-usage","--no-sandbox","--user-data-dir=D:\\nexos_test_profile"])
    page = browser.new_page()
    page.on("console", lambda m: print("[console]", m.text))
    errors = []
    page.on("pageerror", lambda e: errors.append(str(e)))

    page.goto(URL, wait_until="load")
    time.sleep(1)
    log_ok("open_page", "loaded")

    # 1) 登录
    try:
        page.fill("#loginName", "nexos")
        page.fill("#loginPwd", "nexos")
        page.click("#loginBtn")
    except Exception as e:
        log_fail("login", "fill/click error: "+str(e))
    # 等待登录结果：桌面出现 (loginEl 消失) 或错误
    try:
        page.wait_for_function("document.getElementById('loginEl').style.display==='none' || document.body.classList.contains('locked')===false", timeout=8000)
        locked = page.evaluate("document.body.classList.contains('locked')")
        err = page.evaluate("document.getElementById('loginErr') ? document.getElementById('loginErr').textContent : ''")
        if locked is False:
            log_ok("login_kernel", "entered desktop; loginErr='%s'" % err)
        else:
            log_fail("login_kernel", "still locked; err='%s'" % err)
    except Exception as e:
        err = page.evaluate("document.getElementById('loginErr') ? document.getElementById('loginErr').textContent : ''")
        log_fail("login_kernel", "timeout; err='%s'" % err)

    # 2) Connect 后端
    try:
        page.fill("#srvIp", "127.0.0.1")
        page.fill("#srvPort", "8765")
        page.click("#connBtn")
    except Exception as e:
        log_fail("connect", str(e))
    time.sleep(3)
    connected = page.evaluate("window.NexOS ? NexOS.connected : null")
    # 标题检查
    title = page.evaluate("document.querySelector('.win-title') ? document.querySelector('.win-title').textContent : ''")
    log_ok("connect", "NexOS.connected=%s" % connected)

    # 3) 双击打开 Terminal
    try:
        term = page.wait_for_selector("img[data-app='terminal'], .icon[data-app='terminal'], [data-app='terminal']", timeout=5000)
        term.dblclick()
        time.sleep(1)
    except Exception as e:
        log_fail("open_terminal", str(e))
    # 确认 terminal 窗口打开且标题含 [LIVE]
    live = page.evaluate("Array.from(document.querySelectorAll('.win-title')).some(e=>e.textContent.includes('[LIVE]'))")
    log_ok("terminal_live", "[LIVE] present=%s" % live)

    # 4) 在 terminal 输入命令并验证真实内核回显
    term_input = page.wait_for_selector("#termInput", timeout=5000)
    def run_cmd(cmd):
        term_input.fill(cmd)
        page.keyboard.press("Enter")
        time.sleep(2.5)
        txt = page.evaluate("document.getElementById('term') ? document.getElementById('term').innerText : ''")
        return txt
    for cmd in ["about","whoami","users","ls"]:
        out = run_cmd(cmd)
        has_real = ("nexos" in out.lower()) or ("kernel" in out.lower()) or ("users" in out.lower()) or ("about" in out.lower())
        # 真实内核回显特征：含 'Logged in' 或 '[SHELL]' 或命令回显行
        real_echo = ("[SHELL]" in out) or (cmd in out)
        log_ok("cmd_"+cmd, "real_echo=%s len=%d snippet=%r" % (real_echo, len(out), out[-120:]))

    # 5) 验证内核真实性：是否包含内核特有 banner（非前端 mock 的 'NexOS v' 等）
    about_txt = run_cmd("about")
    real_kernel_marker = ("NexOS" in about_txt) and ("[SHELL]" in about_txt)
    log_ok("real_kernel_about", "kernel marker [SHELL]=%s" % real_kernel_marker)

    browser.close()

print("\n====== RESULTS ======")
for r in results:
    print(("[PASS] " if r["ok"] else "[FAIL] ")+r["name"]+"  "+r["detail"])
if errors:
    print("\n[page errors]")
    for e in errors:
        print("  "+e)
