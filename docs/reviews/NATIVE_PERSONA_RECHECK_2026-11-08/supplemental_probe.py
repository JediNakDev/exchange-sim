"""Additional independent bounded API probes against original owned API."""
import pathlib,json,sys,copy
src=pathlib.Path(__file__).with_name('probe.py').read_text();exec(src[:src.index('c=json.loads')])
e=call('/experiments?limit=1000')['items'];mm=next(x for x in e if x['name']=='NATIVE-RECHECK mm');mr=call('/runs?experimentId='+mm['experimentId'])['items'][0]
l=call('/runs/'+mr['runId']+'/orders/7/lifecycle');fifo=l['fifoAheadEvidence'];print('actual FIFO rows',len(fifo),'truncated',l['fifoEvidenceTruncated'],'reasons',sorted(set(x['reason'] for x in fifo)))
print('nonzero reset rest',[(x['orderId'],x['sharesAhead']) for x in fifo if x['replacedOldOrderId'] and x['queuePositionAvailable'] and int(x['sharesAhead'])>0][:5])
# >1000 trace exact rows and source causality on sweep.
c=copy.deepcopy(mm['config']);c['strategy']=dict(strategyKind='immediate_sweep_execution',parentSide='buy',parentQuantity='100000',startTimeNs='1000000000',executionDurationNs='30000000000',limitOffsetTicks='5');c['latency']['acknowledgmentLatencyNs']='100000000';c['seedSet']['masterSeeds']=['1'];c['bounds']['endSimulationTimeNs']='40000000000'
x=exp(c,'sweep pages');r=run(x,'1');p,rows=trace(r,1000);assert len(rows)>1000
for row in rows:
 if row['kind']=='decision':assert int(row['deliveredSourceTimeNs'])<=int(row['simulationTimeNs'])
assert any(row['reason']=='child_in_flight' for row in rows);print('sweep rows',len(rows))
(S/'supplemental-responses.json').write_text(json.dumps(records,indent=2))
