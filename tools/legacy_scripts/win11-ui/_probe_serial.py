import socket, time
s=socket.create_connection(('127.0.0.1',4321), timeout=5)
time.sleep(0.5)
s.sendall(b'login nexos nexos\n')
# wait up to 5s for any response
s.settimeout(5.0)
try:
    data = s.recv(4096)
    print('RECV:', repr(data))
except Exception as e:
    print('ERR/NO DATA:', e)
s.close()
