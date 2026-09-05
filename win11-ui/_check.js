const fs=require('fs');
const html=fs.readFileSync(__dirname+'/nexos-desktop.html','utf8');
const m=html.match(/<script>([\s\S]*?)<\/script>/);
if(!m){ console.log('no script block found'); process.exit(1); }
const code=m[1];
try{ new Function(code); console.log('JS syntax OK ('+code.split('\n').length+' lines)'); }
catch(e){ console.log('JS SYNTAX ERROR:', e.message); process.exit(2); }
