"""Review-only native API probes. Writes only VALIDATION-REVIEW disposable data.

Run while the authorized disposable service is available:
python3 docs/reviews/PERSONA_VALIDATION_probe.py > /tmp/PERSONA_VALIDATION_probe.json
No build, server startup, private service or filesystem artifact mutation.
"""
import copy
import json
import time
import urllib.error
import urllib.request
import uuid

BASE = 'http://127.0.0.1:45773'
records = []


def call(path, body=None, key=None):
    headers = {'Content-Type': 'application/json'}
    if body is not None:
        headers['Idempotency-Key'] = key or 'VALIDATION-REVIEW-' + uuid.uuid4().hex
    request = urllib.request.Request(BASE + path, data=None if body is None else json.dumps(body).encode(), headers=headers)
    try:
        response = urllib.request.urlopen(request)
    except urllib.error.HTTPError as error:
        response = error
    result = json.load(response)
    records.append({'path': path, 'body': body, 'status': response.status, 'replayed': response.headers.get('Idempotent-Replayed'), 'result': result})
    return result


def terminal(run):
    for _ in range(200):
        value = call('/v1/runs/' + run['runId'])
        if value['state'] in ('completed', 'failed', 'cancelled', 'paused'):
            return value
        time.sleep(0.05)
    raise RuntimeError('run did not settle')


baseline = call('/v1/experiments/exp_9cf39bf838c23b72')
assert baseline['name'].startswith('VALIDATION-REVIEW')
comparison_path = '/v1/comparisons?baselineExperimentId=exp_9cf39bf838c23b72&candidateExperimentId=exp_b475fe9282596e9b'
call(comparison_path)
original = call('/v1/runs/run_e284f2e19edbfc9e')
key = 'VALIDATION-REVIEW-' + uuid.uuid4().hex
body = {'experimentId': baseline['experimentId'], 'masterSeed': '101'}
repeat = terminal(call('/v1/runs', body, key))
call('/v1/runs', body, key)
records.append({'repeatDigestEquality': {k: repeat['digests'][k] == original['digests'][k] for k in repeat['digests']}})
call(comparison_path)
paused = call('/v1/runs', {**body, 'masterSeed': '102', 'startPaused': True})
terminal(paused)
call('/v1/runs/' + paused['runId'] + '/step', {'canonicalEventCount': 500})
cancelled = call('/v1/runs/' + paused['runId'] + '/cancel', {'reason': 'VALIDATION-REVIEW partial-run eligibility probe'})
terminal(cancelled)
call('/v1/runs/' + paused['runId'] + '/summary')
call(comparison_path)
for label, seeds in [('duplicate', ['101', '101']), ('large', ['18446744073709551615', '9007199254740993', '0']), ('overflow', ['18446744073709551616'])]:
    config = copy.deepcopy(baseline['config'])
    config['seedSet']['masterSeeds'] = seeds
    request = {'name': 'VALIDATION-REVIEW ' + label, 'derivedFromBaselineId': 'twap-execution', 'config': config}
    preview = call('/v1/experiment-previews', request)
    if label == 'large':
        experiment = call('/v1/experiments', request)
        for seed in seeds:
            terminal(call('/v1/runs', {'experimentId': experiment['experimentId'], 'masterSeed': seed}))
        call('/v1/comparisons?baselineExperimentId=' + baseline['experimentId'] + '&candidateExperimentId=' + experiment['experimentId'])
config = copy.deepcopy(baseline['config'])
config['bounds']['maxCanonicalEventCount'] = '100'
request = {'name': 'VALIDATION-REVIEW event bound', 'derivedFromBaselineId': 'twap-execution', 'config': config}
failed_experiment = call('/v1/experiments', request)
failed = terminal(call('/v1/runs', {'experimentId': failed_experiment['experimentId'], 'masterSeed': '101'}))
call('/v1/runs/' + failed['runId'] + '/summary')
call('/v1/comparisons?baselineExperimentId=' + baseline['experimentId'] + '&candidateExperimentId=' + failed_experiment['experimentId'])
for suffix in ['manifest', 'summary', 'events?limit=1000', 'fills?limit=1000', 'orders/95/lifecycle']:
    call('/v1/runs/run_e284f2e19edbfc9e/' + suffix)
call('/v1/connections')
print(json.dumps(records, indent=2))
