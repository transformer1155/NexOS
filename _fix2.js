const fs = require('fs');
let c = fs.readFileSync('E:/NexOS/NexOS/nexos_ai.cpp', 'utf8');
// Last 2 remaining - do simple function name swap, then re-read and fix size params
c = c.split('my_sprintf(').join('my_snprintf(');
// But we need to add size param. Let's find specific lines by their content
// Line with '"from"' pattern
c = c.replace(/my_snprintf(g_out, "{\"ok\":%d,\"fs\":%d,\"from\":\""/g, 
  'my_snprintf(g_out, (int)sizeof(g_out), "{\"ok\":%d,\"fs\":%d,\"from\":\""');
// Line with 'to' pattern  
c = c.replace(/my_snprintf(g_out + j, "\",\"to\":\""/g,
  'my_snprintf(g_out + j, (int)sizeof(g_out) - j, "\",\"to\":\""');
fs.writeFileSync('E:/NexOS/NexOS/nexos_ai.cpp', c);
console.log('DONE');