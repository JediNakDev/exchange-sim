"""Independent native recheck. Requires owned disposable API; no private service.
Usage: python3 probe.py BASE SCRATCH REPO API_BINARY CLI_BINARY
"""
import sys,json,pathlib,urllib.request,urllib.error,uuid,time,copy,subprocess,hashlib
from decimal import Decimal as D
B,S,REPO,API,CLI=sys.argv[1:6]; S=pathlib.Path(S); S.mkdir(exist_ok=True)
records=[]
def call(p,b=None,expect=200):
 req=urllib.request.Request(B+'/v1'+p,data=None if b is None else json.dumps(b).encode(),headers={'Content-Type':'application/json','Idempotency-Key':'RECHECK-'+uuid.uuid4().hex})
 try:r=urllib.request.urlopen(req,timeout=120)
 except urllib.error.HTTPError as e:r=e
 v=json.load(r); records.append(dict(path=p,status=r.status,result=v)); (S/(pathlib.Path(__file__).stem+'-responses.json')).write_text(json.dumps(records))
 assert r.status==expect,(p,r.status,v)
 return v
def exp(c,n):return call('/experiments',dict(name='NATIVE-RECHECK '+n,derivedFromBaselineId='fixed-spread-2-ticks',config=c),201)['experimentId']
def run(e,seed='101',paused=False):
 r=call('/runs',dict(experimentId=e,masterSeed=seed,startPaused=paused),201)
 for i in range(1000):
  r=call('/runs/'+r['runId'])
  if r['state'] in ('completed','failed','cancelled','paused'):return r
  time.sleep(.01)
 raise Exception('timeout')
def summary(r):return call('/runs/'+r['runId']+'/summary')
def compare(a,b,pin=None):return call('/comparisons?baselineExperimentId='+a+'&candidateExperimentId='+b+('' if pin is None else '&runSelection='+pin))
def trace(r,limit=13):
 rows=[];cursor=''
 while True:
  p=call('/runs/'+r['runId']+'/execution-trace?limit='+str(limit)+cursor)
  rows+=p['items']; cursor=p['page']['nextCursor']
  if cursor is None:break
  cursor='&cursor='+cursor
 assert len(rows)==int(p['totalCount'])
 assert [int(x['index']) for x in rows]==list(range(len(rows)))
 return p,rows
def export(r,name):
 dest=S/name
 x=subprocess.run([sys.executable,REPO+'/scripts/research_audit.py','download',B,r['runId'],str(dest)],capture_output=True,text=True)
 (S/(name+'-download.log')).write_text(x.stdout+x.stderr);assert x.returncode==0
 return dest
c=json.loads(pathlib.Path(REPO+'/docs/reviews/PERSONA_MM_EVIDENCE_2026-11-08/baseline.json').read_text())['config']
exec_c=copy.deepcopy(c);exec_c['strategy']=dict(strategyKind='twap_execution',parentSide='buy',parentQuantity='500',sliceIntervalNs='1000000000',sliceOrderType='limit',startTimeNs='1000000000',executionDurationNs='30000000000',limitOffsetTicks='5')
a=exp(exec_c,'twap');r=run(a,'102');s=summary(r);assert s['execution']['filledQuantity']=='483',s['execution']
p,rows=trace(r);assert p['finalTargetTimeNs']=='30000000000' and p['windowEndTimeNs']=='31000000000'
known=0
for x in rows:
 if x['kind']=='child_report' and x['report']['kind']=='fill':known+=int(x['report']['quantity'])
 assert int(x['knownFilledQuantity'])==known
assert known==483
cap=copy.deepcopy(exec_c);cap['risk']['maxOrderQuantity']='10';b=exp(cap,'cap10');cr=run(b);cs=summary(cr);assert cs['execution']['filledQuantity']=='240'
_,caprows=trace(cr);sent=[x for x in caprows if x['kind']=='decision' and x['reason']=='submitted'];assert len(sent)==24 and sum(int(x['submittedQuantity']) for x in sent)==240
assert any(x['reason']=='no_delivered_touch' for x in caprows)
run(a,'101');comp=compare(a,b);pin=comp['runSelection']
neutral=[x for x in comp['metricComparisons'] if x['betterDirection']=='neither'];assert all(x['dispersion']['candidateBetterCount']==0 and x['dispersion']['candidateWorseCount']==0 for x in neutral)
assert any(any(q['difference'] is not None and D(q['difference'])!=0 for q in x['perSeed']) for x in neutral)
for x in neutral:assert x['dispersion']['tiedCount']==sum(q['difference'] is not None and D(q['difference'])==0 for q in x['perSeed'])
cens=copy.deepcopy(exec_c);cens['bounds']['endSimulationTimeNs']='5000000000';ce=exp(cens,'censored');run(ce);cc=compare(b,ce);assert cc['seedPairing']['pairedSeeds']==[]
retry=run(b);assert compare(a,b)['runSelection']!=pin;assert compare(a,b,pin)['runSelection']==pin
bad=copy.deepcopy(exec_c);bad['strategy']['sliceIntervalNs']='0';v=call('/experiment-previews',dict(name='bad',derivedFromBaselineId='twap-execution',config=bad),200);assert '/config/strategy/sliceIntervalNs' in json.dumps(v)
short=copy.deepcopy(exec_c);short['bounds']['endSimulationTimeNs']='3000000000';short['strategy'].update(executionDurationNs='1000000000',sliceIntervalNs='100000000');sa=exp(short,'no5s');sb=exp(short,'no5s copy');sr=run(sa,'101');run(sb,'101');sc=compare(sa,sb)
five=[x for x in sc['metricComparisons'] if x['metricKey']=='markout_total' and x.get('horizonNs')=='5000000000'][0];assert five['dispersion']['sampleCount']==0 and five['dispersion']['tiedCount']==0
# Full audit is larger than the ordinary event endpoint page and actually reproduces.
pkg=export(r,'twap-package');events=sum(1 for _ in (pkg/'runner/events.jsonl').open());assert events>1000
v=subprocess.run([CLI,'verify',str(pkg/'runner')],capture_output=True,text=True);(S/'native-verify.log').write_text(v.stdout+v.stderr);assert v.returncode==0
prov=call('/runs/'+r['runId']+'/manifest')['provenance'];assert prov['sourceRevision']=='360ad69529aa57b5e8856391812cc3d1d93c6896' and prov['releaseId'] is None and prov['executableSha256']==hashlib.sha256(pathlib.Path(API).read_bytes()).hexdigest()
# Canceled and failed attempts, including unlaunched seeds.
paused=run(a,'101',True);call('/runs/'+paused['runId']+'/step',{'canonicalEventCount':600});call('/runs/'+paused['runId']+'/cancel',{});part,partrows=trace(paused);assert part['verification']=='recorded_terminal_partial'
partial_pkg=export(paused,'partial-package');assert not json.loads((partial_pkg/'audit-inventory.json').read_text())['reproductionSupported']
failed_c=copy.deepcopy(exec_c);failed_c['bounds']['maxCanonicalEventCount']='100';fe=exp(failed_c,'failed');fr=run(fe);assert fr['state']=='failed';export(fr,'failed-package');compare(a,fe)
# Native MM evidence, not fixture summary reuse.
mm=exp(c,'mm');mr=run(mm,'103');ms=summary(mr);assert [x['availableShares'] for x in ms['markouts']]==['135665','133167','124664']
slow=copy.deepcopy(c);slow['latency']['cancelLatencyNs']='100000000';me=exp(slow,'slow');mrun=run(me,'103');mss=summary(mrun);diag=mss['orders']['requestDiagnostics'];assert [x['exchangeRejectedCount'] for x in diag]==['3','0','505'];assert {x['reason']:x['count'] for x in diag[2]['rejectReasons']}=={'unknown_order':'504','post_only_would_cross':'1'}
mc=compare(mm,me)
for x in ms['markouts']:
 assert abs(D(x['meanMarkoutPerShare'])-D(x['totalMarkout'])/D(x['availableShares']))<=D('.000001')
# API rejects unsafe exports and invalid offset; use only own disposable evidence.
run_dir=S/'data'/'runs'/r['runId']; (S/'identity.json').write_text(json.dumps(dict(a=a,b=b,pin=pin,run=r,paused=paused,provenance=prov,run_dir=str(run_dir)),indent=2))
inv=call('/runs/'+r['runId']+'/audit-export');item=next(x for x in inv['items'] if x['present']);call('/runs/'+r['runId']+'/audit-export?artifactIndex='+item['artifactIndex']+'&offset='+str(int(item['sizeBytes'])+1),expect=400)
call('/runs/'+r['runId']+'/execution-trace?limit=1001',expect=422)
print(json.dumps(dict(events=events,traceRows=len(rows),capRows=len(caprows),partialRows=len(partrows),provenance=prov,identityFile=str(S/'identity.json')),indent=2))
