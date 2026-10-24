const fs = require('fs');
let c = fs.readFileSync('E:/NexOS/NexOS/nexos_ai.cpp', 'utf8');
const reps = [
  ['my_sprintf(url, ', 'my_snprintf(url, (int)sizeof(url), '],
  ['my_sprintf(out, ', 'my_snprintf(out, outsize, '],
  ['my_sprintf(out + j, ', 'my_snprintf(out + j, outsize - j, '],
  ['my_sprintf(g_env, ', 'my_snprintf(g_env, (int)sizeof(g_env), '],
  ['my_sprintf(g_env + j, ', 'my_snprintf(g_env + j, (int)sizeof(g_env) - j, '],
];
for (const [f, t] of reps) { c = c.split(f).join(t); }
fs.writeFileSync('E:/NexOS/NexOS/nexos_ai.cpp', c);
console.log('DONE ' + reps.length + ' patterns applied');
