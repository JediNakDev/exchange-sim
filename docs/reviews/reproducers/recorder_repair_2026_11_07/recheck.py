#!/usr/bin/env python3
"""Independent repair assertions; original defect assertions remain unchanged.

Only the pinned, already-built binary is accepted. All processes bind private
loopback port 0, and all synthetic data stays under a new --output directory.
Reuses the original harness definitions and executes its positive tail verbatim.
No builds, services, existing data directories or external endpoints are used.
"""
import argparse
import ast
import hashlib
import json
import pathlib
import struct
import time
from decimal import Decimal

parser = argparse.ArgumentParser()
parser.add_argument('--binary', type=pathlib.Path, required=True)
parser.add_argument('--output', type=pathlib.Path, required=True)
args = parser.parse_args()
expected = 'b6f020104ebf1cdf38015c4004187024bce92a17d58e937945623c5e2b2f9b8a'
assert hashlib.sha256(args.binary.read_bytes()).hexdigest() == expected, 'unreviewed executable'
args.output.mkdir(parents=True, exist_ok=False)
print('binary_sha256', expected, flush=True)
original = pathlib.Path(__file__).resolve().parents[1] / 'integrated_api_2026_11_07' / 'probe.py'
source = original.read_text()
module = ast.parse(source, filename=str(original))
# Load imports and harness definitions, not its parser or defect assertions.
helpers = [node for node in module.body if isinstance(node, (ast.Import, ast.ImportFrom, ast.FunctionDef, ast.ClassDef))]
exec(compile(ast.Module(body=helpers, type_ignores=[]), str(original), 'exec'))


def config_for(instance, duration='600000000000'):
    status, catalogue = instance.request('GET', '/v1/baselines')
    assert status == 200
    config = catalogue['items'][0]['config']
    config['bounds']['endSimulationTimeNs'] = duration
    config['bounds']['maxCanonicalEventCount'] = '5000000'
    return config


def wait_state(instance, path, state):
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        status, run = instance.request('GET', path)
        assert status == 200, run
        if run['state'] == state:
            return run
        time.sleep(.01)
    raise AssertionError(('state timeout', state, run))


def all_items(instance, path):
    from urllib.parse import quote
    items, cursor = [], None
    while True:
        status, page = instance.request('GET', path + '?limit=1000' + ('&cursor=' + quote(cursor) if cursor else ''))
        assert status == 200, page
        items.extend(page['items'])
        cursor = page['page']['nextCursor']
        if cursor is None:
            return items


def assert_accounting(instance, path, run):
    status, summary = instance.request('GET', path + '/summary')
    assert status == 200, summary
    fills = all_items(instance, path + '/fills')
    orders = all_items(instance, path + '/orders')
    accounting = summary['accounting']
    signed_qty = sum((1 if f['side'] == 'buy' else -1) * int(f['quantity']) for f in fills)
    fees = sum(Decimal(f['feePerShare']) * int(f['quantity']) for f in fills)
    cash = Decimal(accounting['initialCash']) - sum((1 if f['side'] == 'buy' else -1) * Decimal(f['price']) * int(f['quantity']) for f in fills) - fees
    assert int(accounting['positionShares']) == signed_qty
    assert Decimal(accounting['cash']) == cash
    assert Decimal(accounting['netFees']) == fees
    assert int(run['terminalDiagnostics']['residualPositionShares']) == signed_qty
    assert int(run['terminalDiagnostics']['residualOpenOrderCount']) == sum(o['finalStatus'] == 'open_at_run_end' and o['orderId'] != '0' for o in orders)
    assert summary['availability'] == 'final'
    assert summary['asOfEventPosition'] == run['progress']['processedCanonicalEventCount']
    assert summary['asOfSimulationTimeNs'] == run['progress']['simulationTimeNs']
    assert run['stateHistory'][-1]['atEventPosition'] == summary['asOfEventPosition']
    assert run['stateHistory'][-1]['atSimulationTimeNs'] == summary['asOfSimulationTimeNs']
    print('reconciled', path, 'events', summary['asOfEventPosition'], 'time_ns', summary['asOfSimulationTimeNs'], 'fills', len(fills), 'position', signed_qty, 'cash', cash, 'fees', fees, 'open', run['terminalDiagnostics']['residualOpenOrderCount'], flush=True)
    return summary


# Native drain failure before recorder.clear(), plus missing-cache restart.
with server(args.output / 'drain-failure', 65536) as instance:
    path = create(instance, config_for(instance))
    for index in range(20):
        status, reply = instance.request('POST', path + '/step', {'canonicalEventCount': 100000})
        if status != 200:
            assert status == 503, reply
            break
    else:
        raise AssertionError('native drain fixture did not fail')
    _, failed = instance.request('GET', path)
    assert failed['state'] == 'failed' and failed['allowedActions'] == []
    assert 'command journal write failed' in failed['terminalDiagnostics']['message']
    assert int(failed['progress']['processedCanonicalEventCount']) > 100000
    assert int(failed['progress']['simulationTimeNs']) < 600000000000
    saved_summary = assert_accounting(instance, path, failed)
    for action, body in (('step', {'canonicalEventCount': 1}), ('resume', {}), ('cancel', {})):
        status, reply = instance.request('POST', path + '/' + action, body)
        assert status == 409 and reply['error']['code'] == 'invalid_state_transition', reply
    _, after = instance.request('GET', path)
    assert after == failed
    drain_path = path
    drain_run = failed

with server(args.output / 'drain-failure') as instance:
    _, restarted = instance.request('GET', drain_path)
    assert restarted == drain_run
    status, summary = instance.request('GET', drain_path + '/summary')
    assert status == 200 and summary == saved_summary
    status, reply = instance.request('GET', drain_path + '/events')
    assert status == 500 and reply['error']['code'] == 'internal_error'
    print('missing_cache_restart_preserves_progress_cause_summary_and_rejects_events', flush=True)

# One older failure must not keep a younger healthy short run at event zero.
with server(args.output / 'fairness', 65536) as instance:
    broken = create(instance, config_for(instance))
    healthy = create(instance, config_for(instance, '200000000'))
    status, _ = instance.request('POST', broken + '/resume', {})
    assert status == 200
    failed = wait_state(instance, broken, 'failed')
    status, _ = instance.request('POST', healthy + '/resume', {})
    assert status == 200
    done = wait_state(instance, healthy, 'completed')
    assert int(done['progress']['processedCanonicalEventCount']) > 0
    assert_accounting(instance, broken, failed)
    print('healthy_run_completed_after_older_native_failure', done['progress'], flush=True)

# Native JSON failure after recorder.clear(), rather than a journal append error.
with server(args.output / 'post-clear', 16 * 1024 * 1024) as instance:
    path = create(instance, config_for(instance))
    assert instance.request('POST', path + '/step', {'canonicalEventCount': 100000})[0] == 200
    assert instance.request('POST', path + '/step', {'canonicalEventCount': 100000})[0] == 503
    _, run = instance.request('GET', path)
    assert run['state'] == 'failed' and run['allowedActions'] == []
    assert 'write failed in ' in run['terminalDiagnostics']['message']
    assert_accounting(instance, path, run)
    for action, body in (('step', {'canonicalEventCount': 1}), ('resume', {})):
        assert instance.request('POST', path + '/' + action, body)[0] == 409
    print('post_clear_failure_contained_without_recorder_reentry', flush=True)

# Finalization: original writer error must precede the secondary cache failure.
with server(args.output / 'cancel-failure', 65536) as instance:
    path = create(instance, config_for(instance))
    assert instance.request('POST', path + '/step', {'canonicalEventCount': 100000})[0] == 200
    assert instance.request('POST', path + '/cancel', {})[0] == 503
    _, run = instance.request('GET', path)
    assert run['state'] == 'failed' and run['allowedActions'] == []
    message = run['terminalDiagnostics']['message']
    assert 0 <= message.find('command journal write failed') < message.find('could not be persisted'), message
    assert_accounting(instance, path, run)
    print('finalization_retains_native_cause_before_cache_error', message, flush=True)

# A lost saved summary must remain unavailable, including after a failed read.
summary_file = args.output / 'drain-failure' / 'data' / 'runs' / drain_run['runId'] / 'summary.json'
summary_file.unlink()
with server(args.output / 'drain-failure') as instance:
    assert instance.request('GET', drain_path)[1] == drain_run
    for endpoint in ('summary', 'events', 'summary'):
        status, reply = instance.request('GET', drain_path + '/' + endpoint)
        assert status == (500 if endpoint == 'events' else 409), reply
        assert reply['error']['code'] == ('internal_error' if endpoint == 'events' else 'artifact_not_ready')
    print('missing_summary_never_fabricates_zero_accounting', flush=True)

# Control persistence effects cannot become a successful response or a live run.
for action, body in (('step', {'canonicalEventCount': 10}), ('resume', {})):
    with server(args.output / ('control-' + action)) as instance:
        path = create(instance, config_for(instance))
        assert instance.request('POST', path + '/step', {'canonicalEventCount': 1000})[0] == 200
        run_id = path.rsplit('/', 1)[1]
        obstruction = instance.directory / 'data' / 'runs' / run_id / 'run.json.tmp'
        obstruction.mkdir()
        key = 'repair-control-' + action
        status, reply = instance.request('POST', path + '/' + action, body, key)
        assert status == 503, reply
        _, run = instance.request('GET', path)
        assert run['state'] == 'failed' and run['allowedActions'] == []
        assert 'run.json.tmp' in run['terminalDiagnostics']['message']
        assert_accounting(instance, path, run)
        assert instance.request('POST', path + '/' + action, body, key)[0] == 503
        obstruction.rmdir()
        print('control_persistence_failure_contained', action, flush=True)

# Occupy admission slots to make queued startup/cancel faults deterministic.
with server(args.output / 'startup') as instance:
    config = config_for(instance)
    paused = [create(instance, config) for _ in range(8)]
    _, experiment = instance.request('POST', '/v1/experiments', {'name': 'startup fault', 'derivedFromBaselineId': 'fixed-spread-2-ticks', 'config': config})
    queued = []
    for _ in range(2):
        status, run = instance.request('POST', '/v1/runs', {'experimentId': experiment['experimentId'], 'masterSeed': '1', 'startPaused': True})
        assert status == 201 and run['state'] == 'queued', run
        path = '/v1/runs/' + run['runId']
        (instance.directory / 'data' / 'runs' / run['runId'] / 'runner').write_text('blocked')
        queued.append(path)
    assert instance.request('POST', queued[0] + '/cancel', {}, 'repair-queued-cancel')[0] == 503
    status, waiting = instance.request('POST', '/v1/runs', {'experimentId': experiment['experimentId'], 'masterSeed': '1', 'startPaused': True})
    assert status == 201 and waiting['state'] == 'queued'
    # A control failure must release one of the eight slots. The next startup
    # failure must not consume that slot, so the third queued run can activate.
    (instance.directory / 'data' / 'runs' / paused[0].rsplit('/', 1)[1] / 'run.json.tmp').mkdir()
    assert instance.request('POST', paused[0] + '/step', {'canonicalEventCount': 1})[0] == 503
    for path in queued:
        run = wait_state(instance, path, 'failed')
        assert run['allowedActions'] == [] and 'runner' in run['terminalDiagnostics']['message']
    ready = wait_state(instance, '/v1/runs/' + waiting['runId'], 'paused')
    assert ready['progress']['processedCanonicalEventCount'] == '0'
    print('queued_cancel_startup_faults_contained_and_failure_releases_admission', flush=True)

# Confirm the documented fail-closed restart limit for partial terminal caches.
with server(args.output / 'partial-terminal') as instance:
    path = create(instance, config_for(instance))
    assert instance.request('POST', path + '/step', {'canonicalEventCount': 1000})[0] == 200
    directory = instance.directory / 'data' / 'runs' / path.rsplit('/', 1)[1]
    (directory / 'runner' / 'summary.json.tmp').mkdir()
    (directory / 'fills.bin.tmp').mkdir()
    assert instance.request('POST', path + '/cancel', {})[0] == 503
    _, run = instance.request('GET', path)
    assert run['state'] == 'failed' and run['allowedActions'] == []
    message = run['terminalDiagnostics']['message']
    assert 0 <= message.find('runner/summary.json.tmp') < message.find('fills.bin.tmp'), message
    assert_accounting(instance, path, run)
    assert (directory / 'events.bin').is_file() and not (directory / 'fills.bin').exists()
    record = (directory / 'run.json').read_bytes()
result = subprocess.run([str(args.binary.resolve()), '--port', '0', '--bind', '127.0.0.1', '--data-dir', str(instance.directory / 'data')], capture_output=True, timeout=5)
(instance.directory / 'restart.stdout').write_bytes(result.stdout)
(instance.directory / 'restart.stderr').write_bytes(result.stderr)
assert result.returncode != 0 and b'terminal run is missing' in result.stderr, result
assert (directory / 'run.json').read_bytes() == record
print('partial_terminal_cache_requires_operator_recovery', result.stderr.decode().strip(), flush=True)

# Malformed counts are header-only; the only oversized body is sparse, ~256 MiB.
with server(args.output / 'cache-bounds') as instance:
    path = create(instance, config_for(instance, '200000000'))
    assert instance.request('POST', path + '/step', {'canonicalEventCount': 100000})[0] == 200
run_id = path.rsplit('/', 1)[1]
cache = args.output / 'cache-bounds' / 'data' / 'runs' / run_id / 'events.bin'
original_bytes = cache.read_bytes()
magic, record_size, count = struct.unpack('=QQQ', original_bytes[:24])
for name, contents, size, expected_message in (
    ('near_tib_count', struct.pack('=QQQ', magic, record_size, (1 << 40) // record_size), None, 'length does not match'),
    ('uint64_count', struct.pack('=QQQ', magic, record_size, (1 << 64) - 1), None, 'length does not match'),
    ('truncated', original_bytes[:-1], None, 'length does not match'),
    ('trailing', original_bytes + b'x', None, 'length does not match'),
    ('wrong_record_size', struct.pack('=QQQ', magic, record_size + 1, count), None, 'unreadable artifact'),
    ('over_256m', struct.pack('=QQQ', magic, record_size, (256 * 1024 * 1024) // record_size + 1), 24 + record_size * ((256 * 1024 * 1024) // record_size + 1), '256 MiB payload limit'),
):
    cache.write_bytes(contents)
    if size:
        with cache.open('r+b') as stream:
            stream.truncate(size)
    with server(args.output / 'cache-bounds') as instance:
        status, reply = instance.request('GET', path + '/events')
        assert status == 500 and expected_message in reply['error']['message'], (name, status, reply)
        assert instance.request('GET', '/v1/service')[0] == 200
        print('cache_rejected', name, 'actual_bytes', cache.stat().st_size, reply['error']['message'], flush=True)
cache.write_bytes(original_bytes)

# Execute the former reviewer's positive accounting/identity/idempotency cases
# unchanged. The uncertain-key block uses the new drain-failure fixture above.
tail = source[source.index('from decimal import Decimal, ROUND_HALF_EVEN'):]
exec(compile(tail, str(original), 'exec'))
print('ALL_REPAIR_RECHECKS_PASSED', flush=True)
