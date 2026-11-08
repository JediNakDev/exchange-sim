"""Review-only native API study. Mutates only fresh MM-REVIEW experiments on loopback."""
import json, urllib.request, uuid, copy, pathlib
BASE='http://127.0.0.1:45773/v1'
OUT=pathlib.Path(__file__).parent
def call(path, body=None):
    req=urllib.request.Request(BASE+path, data=None if body is None else json.dumps(body).encode(), headers={'Content-Type':'application/json','Idempotency-Key':'MM-REVIEW-'+str(uuid.uuid4())})
    with urllib.request.urlopen(req) as r: return json.load(r)
def save(name, value):
    (OUT/(name+'.json')).write_text(json.dumps(value,indent=2)+'\n')
baseline=call('/experiments/exp_cb36794a01e24837'); save('baseline',baseline)
variants={}
for name, field, value in [('width','halfSpreadTicks','2'),('skew','strategyKind','inventory_aware_market_maker'),('feed','feedLatencyNs','100000000'),('cancel','cancelLatencyNs','100000000')]:
    cfg=copy.deepcopy(baseline['config'])
    if name in ('feed','cancel'): cfg['latency'][field]=value
    else: cfg['strategy'][field]=value
    if name=='skew': cfg['strategy'].update(inventorySkewTicksPerUnit='1',inventorySkewUnitShares='100',scaleSizeWithInventory=False)
    body=dict(name='MM-REVIEW '+name,description='One hypothesis versus baseline: '+name,derivedFromBaselineId=baseline['derivedFromBaselineId'],parentExperimentId=baseline['experimentId'],config=cfg)
    exp=call('/experiments',body); variants[name]=exp; save(name+'-experiment',exp)
    for seed in cfg['seedSet']['masterSeeds']:
        run=call('/runs',dict(experimentId=exp['experimentId'],masterSeed=seed)); save(name+'-'+seed+'-run',run)
save('variants',variants)
print(json.dumps({k:v['experimentId'] for k,v in variants.items()}))
