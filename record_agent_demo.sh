#!/usr/bin/env bash
# =====================================================================
#  record_agent_demo.sh
#  Boot NexOS in QEMU, drive its built-in agent with the cloud DeepSeek
#  API (via the host TLS proxy), and record the desktop to a video.
#  Reuses the proven "background QEMU + active poll" survival pattern.
# =====================================================================
set -uo pipefail
cd "$(dirname "$0")"

IMG="build/os_v2.img"
SERIAL_PORT=9999
PROXY_PORT=18999
VIDEO="build/agent_demo.avi"

# DeepSeek API key (provided by user).  The guest sends this as the
# Authorization header; the host proxy forwards it to api.deepseek.com.
DEEPSEEK_KEY="DEEPSEEK_API_KEY_PLACEHOLDER"
AGENT_GOAL="Format the data disk with mkfs. Next: fwrite demo.txt hello nexos. Last: cat demo.txt."

pkill -f "qemu-system" 2>/dev/null
pkill -f "deepseek_proxy" 2>/dev/null
pkill -f "Xvfb" 2>/dev/null
sleep 1

# ---- 1. Virtual X display for SDL + ffmpeg x11grab ----
Xvfb :1 -screen 0 1024x768x24 >/tmp/xvfb.log 2>&1 &
XVFB_PID=$!
sleep 1

# ---- 2. Host-side TLS proxy: guest plain-HTTP -> DeepSeek HTTPS ----
DEEPSEEK_API_KEY="$DEEPSEEK_KEY" python3 -u tools/deepseek_proxy.py \
  --listen "$PROXY_PORT" >/tmp/proxy.log 2>&1 &
PROXY_PID=$!
sleep 1

# ---- 3. QEMU: desktop on Xvfb, serial accepts commands, networking up ----
DISPLAY=:1 qemu-system-x86_64 -drive format=raw,file="$IMG" -m 64M \
  -display sdl -vga std -no-reboot \
  -monitor none \
  -serial tcp:127.0.0.1:$SERIAL_PORT,server,nowait \
  -net nic,model=ne2k_isa -net user,hostfwd=tcp::8080-:8080 \
  >/tmp/qemu.log 2>&1 &
QEMU_PID=$!
echo "QEMU=$QEMU_PID PROXY=$PROXY_PID XVFB=$XVFB_PID"

cleanup(){
  kill -9 "$FFMPEG_PID" 2>/dev/null || true
  kill -9 "$QEMU_PID" 2>/dev/null || true
  kill -9 "$PROXY_PID" 2>/dev/null || true
  kill -9 "$XVFB_PID" 2>/dev/null || true
}
trap cleanup EXIT

# ---- 4. Wait for the OS HTTP server (boot finished) ----
echo "Waiting for OS boot..."
for i in $(seq 1 45); do
  if curl -s -m 2 http://localhost:8080/ >/dev/null 2>&1; then
    echo "OS HTTP reachable after ${i}s"; break
  fi
  sleep 1
done

# ---- 5. Start recording the Xvfb desktop ----
ffmpeg -y -f x11grab -r 12 -s 1024x768 -i :1.0 \
  -c:v mpeg4 -q:v 3 "$VIDEO" >/tmp/ffmpeg.log 2>&1 &
FFMPEG_PID=$!
echo "Recording PID=$FFMPEG_PID -> $VIDEO"
sleep 3   # let a few seconds of boot/desktop show

# ---- 6. Drive the built-in agent over the serial console ----
# One persistent full-duplex connection: send commands AND read back the
# terminal output (mirrored to COM1) so the run can be validated.
python3 - "$SERIAL_PORT" "$DEEPSEEK_KEY" "$AGENT_GOAL" > /tmp/serial_out.log 2>&1 <<'PY'
import socket, sys, threading, time
PORT = int(sys.argv[1]); KEY = sys.argv[2]; GOAL = sys.argv[3]
sock = socket.create_connection(("127.0.0.1", PORT), timeout=5)
sock.settimeout(0.2)
buf = []
stop = False
def reader():
    while not stop:
        try:
            d = sock.recv(4096)
            if d: buf.append(d)
        except socket.timeout:
            pass
        except Exception:
            break
t = threading.Thread(target=reader, daemon=True); t.start()
def send(line):
    sock.sendall((line + "\n").encode()); time.sleep(1.2)
time.sleep(1)
send("agent config url http://10.0.2.2:18999/v1/chat/completions")
send("agent config key " + KEY)
send("agent config model deepseek-chat")
time.sleep(1)
send("agent run " + GOAL)
time.sleep(120)         # ReAct loop: up to 3 DeepSeek round-trips + in-OS execution (slow under TCG)
stop = True; t.join(timeout=1); sock.close()
out = b"".join(buf).decode("utf-8", "replace")
print(out)
PY
echo "--- serial output captured: $(wc -c < /tmp/serial_out.log) bytes ---"

# ---- 7. Hold a few more seconds so the report is visible, then stop ----
sleep 6
# Gracefully finalize the AVI (SIGTERM lets ffmpeg write the index footer).
kill -INT "$FFMPEG_PID" 2>/dev/null || kill -TERM "$FFMPEG_PID" 2>/dev/null || true
sleep 3
echo "Demo finished; capture stopped."
