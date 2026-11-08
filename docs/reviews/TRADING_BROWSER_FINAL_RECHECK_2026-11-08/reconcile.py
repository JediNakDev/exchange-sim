"""Read-only native and captured-evidence reconciliation, exact strings/Decimal."""
import json,pathlib,urllib.request
from decimal import Decimal as D
P=pathlib.Path(__file__).parent
load=lambda n:json.loads((P/n).read_text())
receipt=load('receipt.json'); checks=[]
def ok(n,t):
 assert t,n
 checks.append(n)
def get(route):
 with urllib.request.urlopen('http://127.0.0.1:45873/v1'+route) as r:return json.load(r)
for side in ['twap','sweep']:
 for seed in ['101','102','103']:
  s=load(side+'-'+seed+'-summary.json');e=s['execution'];m={x['metricKey']:x['value'] for x in s['metrics']}
  ok(side+seed+' signed filled-only cost',D(m['decision_slippage_total'])==D(e['fillNotional'])-D(e['benchmarkMid'])*D(e['filledQuantity']))
  ok(side+seed+' fee inclusion',D(m['implementation_shortfall_total'])==D(m['decision_slippage_total'])+D(e['netFees']))
for n in ['execution','width','skew','slow']:
 c=load(n+'-comparison.json')
 for m in c['metricComparisons']:
  if m['betterDirection']=='neither':ok(n+' neutral '+m['metricKey'],m['dispersion']['candidateBetterCount']==0 and m['dispersion']['candidateWorseCount']==0 and m['dispersion']['tiedCount']==sum(x['difference'] is not None and D(x['difference'])==0 for x in m['perSeed']))
ok('three execution pairs',load('execution-comparison.json')['seedPairing']['pairedSeeds']==['101','102','103'])
ok('censored excluded',load('censor-comparison.json')['seedPairing']['pairedSeeds']==[])
ok('observed partial paired',load('cap-comparison.json')['seedPairing']['pairedSeeds']==['101'])
t=load('twap-trace.json');known=D('0')
for row in t['items']:
 if row['kind']=='decision':ok('trace known-fill '+row['index'],D(row['knownFilledQuantity'])==known)
 elif row['report']['kind']=='fill':known+=D(row['report']['quantity'])
ok('final target not deadline',t['finalTargetTimeNs']=='30000000000' and t['windowEndTimeNs']=='31000000000')
run=receipt['studies']['mm']['runs']['103']['runId'];fills=[];cursor=None
while True:
 page=get('/runs/'+run+'/fills?limit=1000'+('' if cursor is None else '&cursor='+cursor));fills+=page['items'];cursor=page['page']['nextCursor']
 if cursor is None:break
s=load('mm-103-summary.json');markouts=[]
for expected,m in zip(['135665','133167','124664'],s['markouts']):
 h=m['horizonNs'];avail=[f for f in fills if next(x for x in f['markouts'] if x['horizonNs']==h)['status']=='available'];qty=sum((D(f['quantity']) for f in avail),D('0'));total=sum((D(next(x for x in f['markouts'] if x['horizonNs']==h)['markoutTotal']) for f in avail),D('0'))
 ok('eligible shares '+h,str(qty)==expected==m['availableShares']);ok('eligible fill count '+h,str(len(avail))==m['availableFillCount']);ok('markout total '+h,total==D(m['totalMarkout']));ok('conditional markout '+h,abs(total/qty-D(m['meanMarkoutPerShare']))<=D('0.0000005'));markouts.append(m)
life=load('slow-lifecycle.json');ok('corrected native related fill69',life['replaceTransits'][0]['fillBeforeArrivalEventPosition']=='69');events=load('race-events.json')['items'];a=next(x for x in events if x['eventPosition']=='104');ok('actual arrival zero or unavailable identity',a.get('orderId') in [None,'0'])
(P/'reconciliation.json').write_text(json.dumps({'checks':checks,'markouts':markouts,'nativeFillCount':str(len(fills)),'arrival104':a},indent=2)+'\n');print(str(len(checks))+' independent reconciliations passed')
