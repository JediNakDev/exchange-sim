"""Read-only full-fill accounting and eligible-share check on the owned baseline."""
import json, pathlib, urllib.request, urllib.parse
from decimal import Decimal as D
P=pathlib.Path(__file__).parent
B='http://127.0.0.1:45773/v1/runs/run_7edbf747de8f7c91/fills?limit=1000'
fills=[];url=B
while url:
 with urllib.request.urlopen(url) as r: page=json.load(r)
 fills+=page['items']; cursor=page['page']['nextCursor'];url=B+'&cursor='+urllib.parse.quote(cursor) if cursor else None
(P/'baseline-103-all-fills.json').write_text(json.dumps(fills,indent=2)+'\n')
# The focused reconciliation log is emitted below.
s=json.loads((P/'baseline-103-summary.json').read_text());a=s['accounting']
qty=sum(int(f['quantity'])*(1 if f['side']=='buy' else -1) for f in fills)
fees=sum(D(f['ledgerEffect']['fee']) for f in fills)
cash=D(a['initialCash'])-sum(D(f['price'])*int(f['quantity'])*(1 if f['side']=='buy' else -1)+D(f['ledgerEffect']['fee']) for f in fills)
assert qty==int(a['positionShares']) and fees==D(a['netFees']) and cash==D(a['cash'])
assert cash+D(qty)*D(a['markPrice'])-D(a['initialCash'])==D(a['netPnl'])
log=[f'PASS {len(fills)} fills; position={qty}; fees={fees}; cash={cash}; net={a["netPnl"]}']
for h in s['markouts']:
 eligible=[(f,m) for f in fills for m in f['markouts'] if m['horizonNs']==h['horizonNs'] and m['status']=='available']
 q=sum(int(f['quantity']) for f,m in eligible); total=sum(D(m['markoutTotal']) for f,m in eligible)
 assert len(eligible)==int(h['availableFillCount']) and total==D(h['totalMarkout'])
 assert (total/D(q)).quantize(D('0.000001'))==D(h['meanMarkoutPerShare'])
 log.append(f'PASS horizon={h["horizonNs"]} eligible fills={len(eligible)} eligible shares={q} total={total} mean={h["meanMarkoutPerShare"]}')
(P/'reconciliation.txt').write_text('\n'.join(log)+'\n');print('\n'.join(log))
