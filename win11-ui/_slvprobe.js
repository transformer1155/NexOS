const ws = new WebSocket('ws://127.0.0.1:8765');
ws.addEventListener('open', () => {
  console.log('[bridge] connected');
  setTimeout(()=>ws.send('\n'), 3000);
  setTimeout(()=>ws.send('login nexos nexos\n'), 4000);
  setTimeout(()=>ws.send('net info\n'), 6000);
  setTimeout(()=>ws.send('distnet scheduler 10.0.2.2 fib 30\n'), 8000);
});
ws.addEventListener('message', (e) => { process.stdout.write(e.data.toString()); });
ws.addEventListener('error', (e)=>{ console.error('[bridge] ERR', e.message||e); });
setTimeout(()=>{ console.log('\n[slvprobe] done'); process.exit(0); }, 16000);
