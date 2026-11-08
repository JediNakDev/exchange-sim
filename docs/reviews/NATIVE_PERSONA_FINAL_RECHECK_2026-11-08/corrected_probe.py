"""New independent fixed-build assertions. Only mutates its wrapper-owned store."""
import sys,pathlib,json,subprocess,copy
src=pathlib.Path(__file__).with_name('baseline_probe.py').read_text();exec(src[:src.index('c=json.loads')])
i=json.loads((S/'identity.json').read_text()); a,b,pin=i['a'],i['b'],i['pin'];r=i['run']; paused=i['paused']
assert compare(a,b,pin)['runSelection']==pin
assert call('/runs/'+r['runId']+'/manifest')['provenance']==i['provenance']
exps=call('/experiments?limit=1000')['items']; byname={e['name']:e['experimentId'] for e in exps}
def selected(name):return next(x for x in call('/runs?experimentId='+byname['NATIVE-RECHECK '+name])['items'] if x['state']=='completed' and x['masterSeed']=='101')
slow=call('/runs?experimentId='+byname['NATIVE-RECHECK slow'])['items'][0]
l=call('/runs/'+slow['runId']+'/orders/7/lifecycle'); assert any(x['submitEventPosition']=='48' and x['arrivalEventPosition']=='104' and x['fillBeforeArrivalEventPosition']=='69' for x in l['replaceTransits']),l
nofill=[]
for order in call('/runs/'+slow['runId']+'/orders?limit=1000')['items']:
 oid=order['orderId']
 nl=call('/runs/'+slow['runId']+'/orders/'+oid+'/lifecycle')
 if any(x['fillBeforeArrivalEventPosition'] is None for x in nl['replaceTransits']):nofill.append(nl);break
assert nofill,'no no-fill transit found'
assert any('unknown_order' in x['detail'] for x in nofill[0]['stages'] if x['stageKind']=='replace_rejected')
es=call('/runs/'+slow['runId']+'/events?fromEventPosition=48&toEventPosition=105&limit=1000')['items']
ev={x['eventPosition']:x for x in es};assert ev['48']['orderId']=='7' and ev['48']['simulationTimeNs']=='63099715';assert ev['69']['quantity']=='39' and ev['69']['simulationTimeNs']=='105073965';assert ev['104']['orderId'] is None and ev['104']['simulationTimeNs']=='163120271'
(S/'race.json').write_text(json.dumps({'lifecycle':l,'events':es,'noFillCounterexample':nofill[0]},indent=2))
# CLI observation exclusion on both sides and genuine full-window residual eligibility.
full=selected('cap10');cens=selected('censored')
def native(x):return str(S/'data/runs'/x['runId']/'runner')
cli_results=[]
for x,y,code in [(cens,full,6),(full,cens,6),(full,full,0)]:
 q=subprocess.run([CLI,'compare',native(x),native(y),'--json'],capture_output=True,text=True);v=json.loads(q.stdout);assert q.returncode==code,(q.returncode,q.stdout,q.stderr)
 if code==6:
  assert v['paired_seeds']==[] and any(e['reason']=='execution_window_censored' for e in v['excluded_runs']),v
 else:assert v['paired_seeds']==['101'],v
 cli_results.append({'exit':str(q.returncode),'result':v})
(S/'cli.json').write_text(json.dumps(cli_results,indent=2))
cp=compare(byname['NATIVE-RECHECK cap10'],byname['NATIVE-RECHECK censored']);assert cp['seedPairing']['pairedSeeds']==[]
assert 'execution_window_censored' in json.dumps(cp)
# Partial trace must reject malformed evidence beyond limit1, regardless of page.
f=S/'data/runs'/paused['runId']/'runner/execution-decisions.jsonl';save=f.read_bytes();row=json.loads(save.splitlines()[-1]); cases=[('syntax',b'{invalid-json}\n')]
for key,value in [('time_ns',123),('has_touch','false'),('submitted_qty','4294967296'),('report_qty','-1'),('reason','unknown-new-label'),('reject_reason','unknown'),('cancel_reason','unknown'),('kind','unknown')]:
 bad=dict(row);bad[key]=value;cases.append((key,(json.dumps(bad)+'\n').encode()))
results=[]
try:
 for name,bad in cases:
  f.write_bytes(save+bad)
  for limit in [1,2,1000]:call('/runs/'+paused['runId']+'/execution-trace?limit='+str(limit),expect=409)
  results.append(name)
finally:f.write_bytes(save)
assert trace(paused,1)[0]['verification']=='recorded_terminal_partial';trace(paused,1000)
# Old completed sidecar omission still reproduces, page limit guard unchanged.
f=S/'data/runs'/r['runId']/'runner/execution-decisions.jsonl';save=f.read_bytes()
try:f.unlink();assert trace(r,1000)[0]['verification']=='native_reproduction_and_replay_verified'
finally:f.write_bytes(save)
call('/runs/'+r['runId']+'/execution-trace?limit=1001',expect=422)
# Bounded actual native scenario search for genuinely measured zero, not a helper fixture.
c=json.loads(pathlib.Path(REPO+'/docs/reviews/PERSONA_MM_EVIDENCE_2026-11-08/baseline.json').read_text())['config']
c['scenario']['backgroundAgents']=c['scenario']['backgroundAgents'][:1]
for p in c['scenario']['backgroundAgents'][0]['parameters']:
 if p['name']=='fairValueStepIntervalNs':p['value']='100000000'
c['strategy']=dict(strategyKind='immediate_sweep_execution',parentSide='buy',parentQuantity='1',startTimeNs='1000000000',executionDurationNs='100000000',limitOffsetTicks='5')
c['bounds']['endSimulationTimeNs']='2000000000';c['markoutHorizonsNs']=['100000000','5000000000']
z=exp(c,'zero search');found=None
for seed in map(str,range(101,113)):
 zr=run(z,seed);zs=summary(zr)
 if any(m['horizonNs']=='100000000' and int(m['availableShares'])>0 and D(m['totalMarkout'])==0 for m in zs['markouts']):found=(zr,zs);break
assert found,'bounded zero search did not find zero'
zr,zs=found;zcopy=exp(c,'zero independent duplicate');run(zcopy,zr['masterSeed']);zc=compare(z,zcopy)
zero=next(m for m in zc['metricComparisons'] if m['metricKey']=='markout_total' and m.get('horizonNs')=='100000000')
# Search may include earlier runs; exact seed must have zero values and difference.
sample=next(x for x in zero['perSeed'] if x['masterSeed']==zr['masterSeed'])
assert D(sample['baselineValue'])==0 and D(sample['candidateValue'])==0 and D(sample['difference'])==0,sample
assert zero['dispersion']['tiedCount']>=1
five=next(m for m in zc['metricComparisons'] if m['metricKey']=='markout_total' and m.get('horizonNs')=='5000000000');assert five['dispersion']['sampleCount']==0 and five['dispersion']['tiedCount']==0 and five['dispersion']['meanDifference'] is None
native_marks=[json.loads(x) for x in (S/'data/runs'/zr['runId']/'runner/markouts.jsonl').read_text().splitlines()]
m=next(x for x in native_marks if x['status']=='measured');assert D(m['qty'])>0 and (D(m['mid'])-D(m['price']))*D(m['qty'])==D(m['markout'])==0
ze=call('/runs/'+zr['runId']+'/events?limit=1000')['items'];through=int(m['fill_time_ns'])+int(m['horizon_ns'])
marks=[x for x in ze if int(x['simulationTimeNs'])<=through and x['eventKind']=='market_data_delivered' and x['bestBidPrice'] is not None and x['bestAskPrice'] is not None]
assert not any(x['eventKind'] in ['order_rested','order_replaced','order_canceled','order_filled'] and int(x['eventPosition'])>int(marks[-1]['sourceEventPosition']) and int(x['simulationTimeNs'])<=through for x in ze)
assert (D(marks[-1]['bestBidPrice'])+D(marks[-1]['bestAskPrice']))/2==D(m['mid'])
(S/'zero-native.json').write_text(json.dumps({'markouts':native_marks,'lastExchangeBook':marks[-1]},indent=2))
(S/'zero.json').write_text(json.dumps({'run':zr,'summary':zs,'comparison':zc},indent=2))
print(json.dumps({'partialCorruptionsRejected':results,'zeroSeed':zr['masterSeed'],'race':'48/69/104','checks':'pass'}))
