#!/bin/sh
# A/B test: which part of the failing body triggers the 400?
# Sends variants with curl from inside the container.
set -e
BB=/src/busybox
cd /tmp
python3 - <<'PYEOF' > /dev/null 2>&1 || true
import re, json
raw = open('/tmp/t2.err','rb').read().decode('utf8','replace')
ms = list(re.finditer(r'request body \((\d+) bytes\): ', raw))
chunks = [raw[m.end():m.end()+int(m.group(1))] for m in ms]
json.loads(chunks[1])            # sanity: parent ok body parses fully
open('/tmp/body_ok.json','w').write(chunks[1])
f = chunks[2]
open('/tmp/body_fail.json','w').write(f)
j = json.loads(f)                # child body parses too (earlier 'extra data' was over-slice)
msgs = j['messages']
print("child msgs:", len(msgs), file=__import__('sys').stderr)

def post(tag, body):
    code = subprocess.run(['curl','-s','-o','/tmp/resp.txt','-w','%{http_code}','-X','POST',
        'http://host.docker.internal:8317/v1/chat/completions',
        '-H','Content-Type: application/json',
        '-H','Authorization: Bearer sk-lloyd-1','--data-binary',body],
        capture_output=True).stdout.decode()
    print(tag, code, open('/tmp/resp.txt').read()[:180])

post('A_exact_child :', f)
b = json.loads(f); b['messages'] = msgs_ok = b['messages']
import copy
# B1: strip the two copied history lines (keep system + tool-call pair + tool)
short = copy.deepcopy(b)
short['messages'] = [m for m in b['messages']
                     if not (m.get('role')=='user' and isinstance(m.get('content'),str)
                             and 'zebra999' in m.get('content',''))]
post('B1_no_hist    :', json.dumps(short))
# B2: assistant content '' -> null
nul = copy.deepcopy(b)
for m in nul['messages']:
    if m.get('role')=='assistant' and m.get('content')=='' :
        m['content']=None
post('B2_null_content:', json.dumps(nul))
# C: whole exact parent-ok body (control)
post('C_parent_ok   :', chunks[1])
PYEOF
