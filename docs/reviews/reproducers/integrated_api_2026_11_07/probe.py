#!/usr/bin/env python3
"""Private loopback probes. Needs an already-built exsim_api; never builds it.

Keeps all test data and raw responses below --output. Each server binds port 0.
A finding being repaired should make its original defect assertion fail.
"""
import argparse
import contextlib
import hashlib
import http.client
import json
import pathlib
import re
import resource
import selectors
import signal
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument('--binary', type=pathlib.Path, required=True)
parser.add_argument('--output', type=pathlib.Path, required=True)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=False)
print('binary_sha256', hashlib.sha256(args.binary.read_bytes()).hexdigest(), flush=True)

class Server:
    def __init__(self, directory, file_limit=None):
        self.directory = directory
        directory.mkdir(exist_ok=True)
        self.log = open(directory / 'server.stderr', 'ab', buffering=0)
        def limit():
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
            resource.setrlimit(resource.RLIMIT_FSIZE, (file_limit, file_limit))
        self.process = subprocess.Popen(
            [str(args.binary.resolve()), '--port', '0', '--bind', '127.0.0.1', '--data-dir', str(directory / 'data')],
            stdout=subprocess.PIPE, stderr=self.log, preexec_fn=limit if file_limit else None)
        with selectors.DefaultSelector() as selector:
            selector.register(self.process.stdout, selectors.EVENT_READ)
            if not selector.select(5):
                self.process.kill()
                raise RuntimeError('private API startup timed out')
        self.startup_line = self.process.stdout.readline().decode()
        match = re.search(r'127\.0\.0\.1:(\d+)', self.startup_line)
        if not match:
            raise RuntimeError('private API did not start: ' + self.startup_line + (directory / 'server.stderr').read_text())
        self.port = int(match.group(1))
        self.sequence = 0
    def request(self, method, path, body=None, key=None):
        self.sequence += 1
        connection = http.client.HTTPConnection('127.0.0.1', self.port, timeout=8)
        raw = json.dumps(body) if body is not None else ''
        key = key or f'review-key-{self.sequence:08}'
        connection.request(method, path, raw, {'Idempotency-Key': key, 'Connection': 'close'})
        response = connection.getresponse()
        payload = response.read()
        connection.close()
        result = json.loads(payload)
        with open(self.directory / 'responses.jsonl', 'a') as out:
            out.write(json.dumps({'path': path, 'key': key, 'status': response.status, 'body': result, 'raw': payload.decode(), 'replayed': response.getheader('Idempotent-Replayed')}) + '\n')
        return response.status, result
    def stop(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=3)
        self.log.close()
        self.process.stdout.close()

@contextlib.contextmanager
def server(directory, file_limit=None):
    instance = Server(directory, file_limit)
    try:
        yield instance
    finally:
        instance.stop()

def create(server, config):
    status, experiment = server.request('POST', '/v1/experiments', {
        'name': 'Independent API review', 'derivedFromBaselineId': 'fixed-spread-2-ticks', 'config': config})
    assert status == 201, (status, experiment)
    status, run = server.request('POST', '/v1/runs', {
        'experimentId': experiment['experimentId'], 'masterSeed': '1', 'startPaused': True})
    assert status == 201, (status, run)
    path = '/v1/runs/' + run['runId']
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        status, run = server.request('GET', path)
        if run['state'] == 'paused':
            return path
        time.sleep(.01)
    raise AssertionError(('did not pause', run))

# Force an intermediate native journal drain to fail. The 64 KiB limit is
# larger than all metadata/response records but smaller than native streams.
with server(args.output / 'drain-failure', 65536) as instance:
    status, catalogue = instance.request('GET', '/v1/baselines')
    config = catalogue['items'][0]['config']
    config['bounds']['endSimulationTimeNs'] = '600000000000'
    config['bounds']['maxCanonicalEventCount'] = '5000000'
    path = create(instance, config)
    failed = None
    for step in range(20):
        status, reply = instance.request('POST', path + '/step', {'canonicalEventCount': 100000})
        if status != 200:
            failed = (status, reply)
            break
    assert failed is not None, 'fixture did not reach the forced intermediate write failure'
    status, run = instance.request('GET', path)
    print('drain_failure', failed, 'run_state', run['state'], 'diagnostics', run['terminalDiagnostics'], flush=True)
    # Original defect assertion: the run stays actionable after native I/O loss.
    assert run['state'] == 'paused' and run['terminalDiagnostics'] is None, run
    status, reply = instance.request('POST', path + '/step', {'canonicalEventCount': 100000})
    print('drain_failure_second_step', status, reply, flush=True)

# Background executor variant: a broken older run prevents a later healthy
# run from receiving any scheduler slice, even though it has been resumed.
with server(args.output / 'drain-starvation', 65536) as instance:
    _, catalogue = instance.request('GET', '/v1/baselines')
    config = catalogue['items'][0]['config']
    config['bounds']['endSimulationTimeNs'] = '600000000000'
    broken = create(instance, config)
    healthy = create(instance, config)
    status, _ = instance.request('POST', broken + '/resume', {})
    assert status == 200
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if 'executor pass failed: command journal write failed' in (instance.directory / 'server.stderr').read_text():
            break
        time.sleep(.02)
    else:
        raise AssertionError('background fixture never reached drain failure')
    status, _ = instance.request('POST', healthy + '/resume', {})
    assert status == 200
    time.sleep(.65)  # Three 200 ms executor retry intervals.
    _, stuck = instance.request('GET', broken)
    _, starved = instance.request('GET', healthy)
    print('background_failure', stuck['state'], 'later_run', starved['state'], starved['progress'], flush=True)
    assert stuck['state'] == 'running' and stuck['terminalDiagnostics'] is None, stuck
    assert starved['state'] == 'running' and starved['progress']['processedCanonicalEventCount'] == '0', starved

# Independent exact arithmetic from individual HTTP fills, rather than from
# native summary fields that share the implementation under review.
from decimal import Decimal, ROUND_HALF_EVEN
import shutil
from urllib.parse import urlencode

with server(args.output / 'execution') as instance:
    _, catalogue = instance.request('GET', '/v1/baselines')
    run_paths = []
    for index in (2, 3):
        for side in ('buy', 'sell'):
            config = json.loads(json.dumps(catalogue['items'][index]['config']))
            config['strategy']['parentSide'] = side
            config['strategy']['parentQuantity'] = '501'
            config['strategy']['executionDurationNs'] = '3000000000'
            config['bounds']['endSimulationTimeNs'] = '3500000000'
            config['fees']['takerFeePerShare'] = '-0.0030'
            path = create(instance, config)
            run_paths.append(path)
            status, before = instance.request('GET', path + '/summary')
            assert status == 200 and before['execution']['parentDeclared'] is False
            assert before['execution']['filledQuantity'] is None
            status, run = instance.request('POST', path + '/step', {'canonicalEventCount': 100000})
            assert status == 200 and run['state'] == 'completed', (status, run)
            _, summary = instance.request('GET', path + '/summary')
            _, page = instance.request('GET', path + '/fills?limit=1000')
            assert page['page']['nextCursor'] is None
            fills = page['items']
            assert fills, 'fixture must execute shares'
            qty = sum(int(fill['quantity']) for fill in fills)
            notional = sum(Decimal(fill['price']) * int(fill['quantity']) for fill in fills)
            fees = sum(Decimal(fill['feePerShare']) * int(fill['quantity']) for fill in fills)
            evidence = summary['execution']
            metrics = {metric['metricKey']: metric for metric in summary['metrics'] if metric['horizonNs'] is None}
            assert int(evidence['filledQuantity']) == qty
            assert Decimal(evidence['fillNotional']) == notional
            assert Decimal(evidence['netFees']) == fees
            assert fees < 0
            cost = (1 if side == 'buy' else -1) * (notional - Decimal(evidence['benchmarkMid']) * qty)
            assert Decimal(metrics['decision_slippage_total']['value']) == cost
            assert Decimal(metrics['implementation_shortfall_total']['value']) == cost + fees
            assert Decimal(metrics['implementation_shortfall_per_share']['value']) == ((cost + fees) / qty).quantize(Decimal('.000001'), rounding=ROUND_HALF_EVEN)
            assert int(metrics['residual_qty']['value']) == 501 - qty
            assert Decimal(metrics['participation_rate']['value']) == (Decimal(qty) / int(evidence['participationMarketQuantity'])).quantize(Decimal('.000001'), rounding=ROUND_HALF_EVEN)
            assert evidence['observationStatus'] == 'censored_run_end'
            assert evidence['unexecutedOpportunityCost'] is None
            _, manifest = instance.request('GET', path + '/manifest')
            expected_build = 'twap_executor/1' if index == 2 else 'immediate_sweep_executor/1'
            assert manifest['strategyBuildId'] == expected_build
            print('execution_exact', config['strategy']['strategyKind'], side, 'qty', qty, 'cost', cost, 'fees', fees, flush=True)
    _, first_run = instance.request('GET', run_paths[0])
    _, other_run = instance.request('GET', run_paths[2])
    comparison_path = '/v1/comparisons?' + urlencode({'baselineExperimentId': first_run['experimentId'], 'candidateExperimentId': other_run['experimentId']})
    status, comparison = instance.request('GET', comparison_path)
    assert status == 200 and comparison['comparable'] is True
    # Bind the substitution to the wrong strategy while retaining the exact seed.
    baseline_runner = instance.directory / 'data' / 'runs' / first_run['runId'] / 'runner'
    candidate_runner = instance.directory / 'data' / 'runs' / other_run['runId'] / 'runner'
    shutil.rmtree(candidate_runner)
    shutil.copytree(baseline_runner, candidate_runner)
    status, comparison = instance.request('GET', comparison_path)
    assert status == 200 and comparison['comparable'] is False
    print('wrong_strategy_same_seed_excluded', comparison['incomparabilityReasons'], flush=True)

# Old HTTP summaries may omit the additive execution block and still load.
# The previous native substitution remains excluded after restart as well.
execution_directory = args.output / 'execution'
summary_file = execution_directory / 'data' / 'runs' / first_run['runId'] / 'summary.json'
saved_summary = json.loads(summary_file.read_text())
del saved_summary['execution']
summary_file.write_text(json.dumps(saved_summary))
with server(execution_directory) as instance:
    status, old_summary = instance.request('GET', run_paths[0] + '/summary')
    assert status == 200 and 'execution' not in old_summary
    status, comparison = instance.request('GET', comparison_path)
    assert status == 200 and comparison['comparable'] is False
    print('legacy_summary_readable_and_substitution_excluded_after_restart', flush=True)

# The uncertain step key stays blocked after restart; another body conflicts.
# This check is independent of the lack of a terminal I/O failure transition.
drain_directory = args.output / 'drain-failure'
records = [json.loads(line) for line in (drain_directory / 'responses.jsonl').read_text().splitlines()]
first_failure = next(row for row in records if row['status'] == 503)
with server(drain_directory) as instance:
    status, _ = instance.request('POST', first_failure['path'], {'canonicalEventCount': 100000}, first_failure['key'])
    assert status == 503
    status, _ = instance.request('POST', first_failure['path'], {'canonicalEventCount': 1}, first_failure['key'])
    assert status == 409
    print('uncertain_control_key_reserved_after_restart', flush=True)

from concurrent.futures import ThreadPoolExecutor
with server(args.output / 'concurrent-idempotency') as instance:
    _, catalogue = instance.request('GET', '/v1/baselines')
    body = {'name': 'Concurrent creation', 'derivedFromBaselineId': 'fixed-spread-2-ticks', 'config': catalogue['items'][0]['config']}
    with ThreadPoolExecutor(max_workers=8) as pool:
        replies = list(pool.map(lambda _: instance.request('POST', '/v1/experiments', body, 'concurrent-review-key'), range(8)))
    assert all(status == 201 for status, reply in replies)
    assert all(reply == replies[0][1] for status, reply in replies)
    _, listed = instance.request('GET', '/v1/experiments')
    assert len(listed['items']) == 1
    status, _ = instance.request('POST', '/v1/experiments', {}, 'rejected-review-key')
    assert status == 422
    status, created = instance.request('POST', '/v1/experiments', body, 'rejected-review-key')
    assert status == 201
    print('concurrent_same_key_one_resource_and_validation_reservation_released', flush=True)
with server(args.output / 'concurrent-idempotency') as instance:
    status, replay = instance.request('POST', '/v1/experiments', body, 'rejected-review-key')
    assert status == 201 and replay == created
    records = [json.loads(line) for line in (instance.directory / 'responses.jsonl').read_text().splitlines()]
    assert records[-1]['replayed'] == 'true'
    print('successful_creation_response_replayed_after_restart', flush=True)

# Legacy successful keys remain readable without rewriting their JSONL source.
legacy_directory = args.output / 'concurrent-idempotency'
new_record = legacy_directory / 'data' / 'idempotency' / (hashlib.sha256(b'concurrent-review-key').hexdigest() + '.json')
legacy_record = json.loads(new_record.read_text())
del legacy_record['schema']
legacy_bytes = (json.dumps(legacy_record) + '\n').encode()
legacy_file = legacy_directory / 'data' / 'idempotency.jsonl'
legacy_file.write_bytes(legacy_bytes)
new_record.unlink()
with server(legacy_directory) as instance:
    status, reply = instance.request('POST', '/v1/experiments', body, 'concurrent-review-key')
    assert status == 201 and reply == replies[0][1]
    assert legacy_file.read_bytes() == legacy_bytes
    print('legacy_jsonl_key_replayed_without_rewrite', flush=True)

with server(args.output / 'no-liquidity') as instance:
    _, catalogue = instance.request('GET', '/v1/baselines')
    config = catalogue['items'][2]['config']
    config['scenario']['backgroundAgents'] = []
    config['strategy']['executionDurationNs'] = '3000000000'
    config['bounds']['endSimulationTimeNs'] = '3500000000'
    path = create(instance, config)
    status, run = instance.request('POST', path + '/step', {'canonicalEventCount': 100000})
    assert status == 200 and run['state'] == 'completed'
    _, summary = instance.request('GET', path + '/summary')
    metrics = {metric['metricKey']: metric for metric in summary['metrics']}
    assert summary['execution']['parentDeclared'] is True
    assert summary['execution']['benchmarkMid'] is None
    assert summary['execution']['filledQuantity'] == '0'
    assert metrics['completion_ratio']['value'] == '0.000000'
    assert metrics['residual_qty']['value'] == config['strategy']['parentQuantity']
    for name in ('implementation_shortfall_total', 'decision_slippage_total', 'participation_rate'):
        assert metrics[name]['value'] is None and metrics[name]['unavailableReason'] == 'no_parent_fills'
    print('no_liquidity_null_costs_and_observed_zero_completion', flush=True)
