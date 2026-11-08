#!/usr/bin/env python3
"""Generate facilitator-only outcomes on an EMPTY disposable API, never participant data."""
import argparse
import copy
import json
from pathlib import Path
import time
from urllib.request import Request, urlopen
from uuid import uuid4

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--api', required=True, choices=['http://127.0.0.1:45993'])
parser.add_argument('--output', required=True, type=Path)
args = parser.parse_args()
args.output.mkdir(exist_ok=False)

def save(name, value):
    (args.output / (name + '.json')).write_text(json.dumps(value, indent=2) + '\n')

def call(path, body=None):
    req = Request(args.api + '/v1' + path, data=None if body is None else json.dumps(body).encode(),
                  headers={'Content-Type': 'application/json', 'Idempotency-Key': 'HUMAN-REFERENCE-' + str(uuid4())})
    with urlopen(req, timeout=15) as response:
        return json.load(response)

assert call('/experiments')['items'] == [], 'reference API must be empty'
base = {b['baselineId']: b for b in call('/baselines')['items']}
receipt = {'human_result': 'NOT RUN', 'api': args.api, 'studies': {}, 'comparisons': {}}

def study(name, config, baseline='twap-execution', parent=None):
    body = {'name': 'HUMAN-REFERENCE ' + name, 'derivedFromBaselineId': baseline, 'config': config}
    if parent:
        body['parentExperimentId'] = parent
    experiment = call('/experiments', body)
    entry = {'experiment': experiment, 'runs': {}}
    receipt['studies'][name] = entry
    for seed in config['seedSet']['masterSeeds']:
        run = call('/runs', {'experimentId': experiment['experimentId'], 'masterSeed': seed})
        end = time.monotonic() + 30
        while run['state'] not in ['completed', 'failed', 'cancelled']:
            assert time.monotonic() < end
            time.sleep(.05)
            run = call('/runs/' + run['runId'])
        assert run['state'] == 'completed', run
        entry['runs'][seed] = run
        save(name + '-' + seed + '-summary', call('/runs/' + run['runId'] + '/summary'))
        manifest = call('/runs/' + run['runId'] + '/manifest')
        assert manifest['provenance']['sourceRevision'] == '360ad69529aa57b5e8856391812cc3d1d93c6896'
        assert manifest['provenance']['executableSha256'] == '10501a33317f4aa53fa6a658ee715674edee56b5fe217d2a106511934d3b56c5'
        save(name + '-' + seed + '-manifest', manifest)
    save('receipt', receipt)
    return entry

execution = copy.deepcopy(base['twap-execution']['config'])
execution['seedSet']['masterSeeds'] = ['101', '102', '103']
twap = study('twap', execution)
sweep_config = copy.deepcopy(execution)
sweep_config['strategy'] = {k: v for k, v in execution['strategy'].items() if k not in ['sliceIntervalNs', 'sliceOrderType']}
sweep_config['strategy']['strategyKind'] = 'immediate_sweep_execution'
sweep = study('sweep', sweep_config, parent=twap['experiment']['experimentId'])
cap = copy.deepcopy(execution)
cap['seedSet']['masterSeeds'] = ['101']
cap['risk']['maxOrderQuantity'] = '10'
study('capped', cap, parent=twap['experiment']['experimentId'])
censor = copy.deepcopy(execution)
censor['seedSet']['masterSeeds'] = ['101']
censor['bounds']['endSimulationTimeNs'] = '5000000000'
study('censored', censor, parent=twap['experiment']['experimentId'])
mm_config = copy.deepcopy(base['fixed-spread-2-ticks']['config'])
mm_config['seedSet']['masterSeeds'] = ['101', '102', '103']
mm = study('mm', mm_config, 'fixed-spread-2-ticks')
width = copy.deepcopy(mm_config)
width['strategy']['halfSpreadTicks'] = '2'
study('width', width, 'fixed-spread-2-ticks', mm['experiment']['experimentId'])
skew = copy.deepcopy(mm_config)
skew['strategy'].update(strategyKind='inventory_aware_market_maker', inventorySkewTicksPerUnit='1', inventorySkewUnitShares='100', scaleSizeWithInventory=False)
study('inventory', skew, 'fixed-spread-2-ticks', mm['experiment']['experimentId'])
latency = copy.deepcopy(mm_config)
latency['latency']['cancelLatencyNs'] = '100000000'
study('latency', latency, 'fixed-spread-2-ticks', mm['experiment']['experimentId'])
for baseline, candidate in [('twap', 'sweep'), ('twap', 'capped'), ('twap', 'censored'), ('mm', 'width'), ('mm', 'inventory'), ('mm', 'latency')]:
    a = receipt['studies'][baseline]['experiment']['experimentId']
    b = receipt['studies'][candidate]['experiment']['experimentId']
    comparison = call('/comparisons?baselineExperimentId=' + a + '&candidateExperimentId=' + b)
    receipt['comparisons'][candidate] = {'baseline': a, 'candidate': b, 'runSelection': comparison['runSelection']}
    save(candidate + '-comparison', comparison)
run_id = twap['runs']['102']['runId']
save('twap-102-trace', call('/runs/' + run_id + '/execution-trace?limit=1000'))
fills = call('/runs/' + run_id + '/fills?limit=1')
save('twap-102-fill', fills)
if fills['items']:
    save('twap-102-lifecycle', call('/runs/' + run_id + '/orders/' + fills['items'][0]['orderId'] + '/lifecycle'))
paused = call('/runs', {'experimentId': twap['experiment']['experimentId'], 'masterSeed': '101', 'startPaused': True})
receipt['controlRun'] = paused
save('receipt', receipt)
print('Reference studies complete; actual human result NOT RUN.')
