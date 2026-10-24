import json
d = json.load(open('apps.json'))
print('total apps:', len(d['apps']))
for a in d['apps']:
    print(' ', a['app_id'], '- tools:', len(a.get('mcp_server', [])))
print()
w = [a for a in d['apps'] if a['app_id'] == 'winhost'][0]
for t in w['mcp_server']:
    print('   ', t['tool_name'])
