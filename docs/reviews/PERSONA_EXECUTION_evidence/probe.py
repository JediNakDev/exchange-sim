"""Review-only probes. Writes only EXEC-REVIEW experiments on disposable loopback API."""
import copy, json, pathlib, time, urllib.request, uuid
from decimal import Decimal as D

BASE = 'http://127.0.0.1:45773/v1'
OUT = pathlib.Path(__file__).parent
evidence = {}
def call(path, body=None):
    req = urllib.request.Request(BASE + path, data=None if body is None else json.dumps(body).encode(), headers={'Content-Type': 'application/json', 'Idempotency-Key': 'EXEC-REVIEW-' + str(uuid.uuid4())})
    with urllib.request.urlopen(req) as r:
        result = json.load(r)
    evidence[str(len(evidence)) + ' ' + path] = result
    return result

baseline = call('/experiments/exp_b8d6039b9e647a2e')
assert baseline['name'].startswith('EXEC-REVIEW')
rows = []
for name, side, method, cap, end in [
    ('sell TWAP', 'sell', 'twap_execution', '200', '60000000000'),
    ('sell sweep', 'sell', 'immediate_sweep_execution', '200', '60000000000'),
    ('buy cap10', 'buy', 'twap_execution', '10', '60000000000'),
    ('buy censored', 'buy', 'twap_execution', '200', '5000000000'),
]:
    config = copy.deepcopy(baseline['config'])
    config['strategy']['parentSide'] = side
    config['strategy']['strategyKind'] = method
    if method == 'immediate_sweep_execution':
        config['strategy'].pop('sliceIntervalNs', None)
        config['strategy'].pop('sliceOrderType', None)
    config['risk']['maxOrderQuantity'] = cap
    config['bounds']['endSimulationTimeNs'] = end
    config['seedSet']['masterSeeds'] = ['101', '102', '103'] if side == 'sell' else ['101']
    exp = call('/experiments', {'name': 'EXEC-REVIEW ' + name, 'derivedFromBaselineId': baseline['derivedFromBaselineId'], 'parentExperimentId': baseline['experimentId'], 'config': config})
    for seed in config['seedSet']['masterSeeds']:
        run = call('/runs', {'experimentId': exp['experimentId'], 'masterSeed': seed})
        for _ in range(100):
            run = call('/runs/' + run['runId'])
            if run['state'] in ['completed', 'failed', 'cancelled']: break
            time.sleep(.1)
        summary = call('/runs/' + run['runId'] + '/summary')
        ex = summary['execution']
        metrics = {m['metricKey']: m['value'] for m in summary['metrics']}
        q = D(ex['filledQuantity'])
        signed = (1 if side == 'buy' else -1) * (D(ex['fillNotional']) - D(ex['benchmarkMid']) * q)
        assert signed == D(metrics['decision_slippage_total'])
        assert signed + D(ex['netFees']) == D(metrics['implementation_shortfall_total'])
        assert D(ex['netFees']) == q * D('0.003')
        rows.append({'name':name,'seed':seed,'experiment':exp['experimentId'],'run':run['runId'],'execution':ex,'metrics':metrics})
for run_id in ['run_a46fec5c8df84bdb','run_2f94393d568b1cf1','run_a7a31e1cb306c5fa','run_f89c3d33981683ea','run_b08b095190352448','run_8de5b83072e9a944']:
    call('/runs/' + run_id + '/summary')
call('/comparisons?baselineExperimentId=exp_b8d6039b9e647a2e&candidateExperimentId=exp_8c3586e643695c8d')
fills = call('/runs/run_2f94393d568b1cf1/fills?limit=1')
call('/runs/run_2f94393d568b1cf1/orders/' + fills['items'][0]['orderId'] + '/lifecycle')
OUT.joinpath('native-api.json').write_text(json.dumps(evidence, indent=2))
OUT.joinpath('probe-summary.json').write_text(json.dumps(rows, indent=2))
print(json.dumps([{k:v for k,v in row.items() if k not in ['execution','metrics']} | {'filled':row['execution']['filledQuantity'], 'status':row['execution']['observationStatus'], 'slippage':row['metrics']['decision_slippage_total']} for row in rows], indent=2))
