#!/usr/bin/env python3
"""Facilitator only: add paused/failed task fixtures AFTER initial unaided research tasks."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import time
from urllib.parse import urlsplit
from urllib.request import Request, urlopen

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--session', type=Path, required=True)
args = parser.parse_args()
receipt = json.loads((args.session / 'session.json').read_text())
assert receipt['state'] == 'ready', 'session must be running'
origin = receipt['api_url']
url = urlsplit(origin)
assert url.scheme == 'http' and url.hostname == '127.0.0.1' and not url.path
result_path = args.session / 'control-task.json'
assert not result_path.exists(), 'fixtures already prepared; retain their IDs instead of duplicating them'
identity = hashlib.sha256(str(args.session.resolve()).encode()).hexdigest()[:24]

def call(path, body=None, operation='read'):
    request = Request(origin + '/v1' + path, data=None if body is None else json.dumps(body).encode(),
                      headers={'Content-Type': 'application/json', 'Idempotency-Key': 'human-control-' + identity + '-' + operation})
    with urlopen(request, timeout=10) as response:
        return json.load(response)

baseline = next(item for item in call('/baselines')['items'] if item['baselineId'] == 'twap-execution')
config = copy.deepcopy(baseline['config'])
config['seedSet']['masterSeeds'] = ['601', '602']
paused = call('/experiments', {'name': 'Control and recovery exercise', 'derivedFromBaselineId': baseline['baselineId'], 'config': config}, 'paused-experiment')
run = call('/runs', {'experimentId': paused['experimentId'], 'masterSeed': '601', 'startPaused': True}, 'paused-run')
config['bounds']['maxCanonicalEventCount'] = '100'
failed = call('/experiments', {'name': 'Bound failure and missing seed exercise', 'derivedFromBaselineId': baseline['baselineId'], 'config': config}, 'failed-experiment')
failure = call('/runs', {'experimentId': failed['experimentId'], 'masterSeed': '601'}, 'failed-run')
deadline = time.monotonic() + 15
while failure['state'] not in ['completed', 'failed', 'cancelled']:
    assert time.monotonic() < deadline, 'terminal fixture state timed out; rerun uses identical operation keys'
    time.sleep(.05)
    failure = call('/runs/' + failure['runId'])
assert failure['state'] == 'failed', 'expected deliberate event-bound failure'
result = {'human_result': 'NOT RUN', 'pausedExperimentId': paused['experimentId'], 'pausedRunId': run['runId'],
          'failedExperimentId': failed['experimentId'], 'failedRunId': failure['runId'], 'unlaunchedSeed': '602',
          'fixtureCreationIsNotParticipantSuccess': True}
result_path.write_text(json.dumps(result, indent=2) + '\n')
print('Prepared facilitator fixtures:', result_path)
