"""Read-only independent assertions over the recorded browser/native evidence."""
import gzip
import hashlib
import json
from pathlib import Path
p = Path(__file__).parent
e = json.loads((p / 'evidence.json').read_text())
for name in ['audit', 'partial-audit', 'cancelled-audit']:
    package = json.loads(gzip.decompress((p / (name + '.json.gz')).read_bytes()))
    artifacts = {a['artifactIndex']: bytes.fromhex(a['contentHex']) for a in package['artifacts']}
    present = [a for a in package['inventory']['items'] if a['present']]
    assert len(artifacts) == len(present)
    for item in present:
        data = artifacts.pop(item['artifactIndex'])
        assert str(len(data)) == item['sizeBytes']
        assert hashlib.sha256(data).hexdigest() == item['sha256']
        if item['name'] == 'runner/events.jsonl' and name == 'audit':
            assert len(data.splitlines()) == 12851
    assert package['inventory']['reproductionSupported'] == (name == 'audit')
    print(name, 'all inventory SHA/size checks pass')
assert e['comparison']['seedPairing']['pairedSeeds'] == ['101', '102', '103']
assert e['receipt']['query']['runSelection'] == e['pin'].split('runSelection=')[1]
old = e['receipt']['comparison']['attemptLedger'][0]
new = e['newest']['attemptLedger'][0]
assert old['selectedRunId'] == e['run']
assert new['selectedRunId'] == e['repeatId']
a = old['attempts'][0]['digests']
b = next(x for x in new['attempts'] if x['runId'] == e['repeatId'])['digests']
assert a['canonicalEventDigest'] == b['canonicalEventDigest']
assert a['ledgerDigest'] == b['ledgerDigest']
assert a['manifestDigest'] != b['manifestDigest']
assert e['paused']['runId'] == e['replay']['runId']
for horizon, n, mean, ties in [('5000000000', 0, None, 0), ('100000000', 1, '0.000000', 1)]:
    m = next(m for m in e['shortComparison']['metricComparisons'] if m['metricKey'] == 'markout_total' and m['horizonNs'] == horizon)
    assert (m['dispersion']['sampleCount'], m['dispersion']['meanDifference'], m['dispersion']['tiedCount']) == (n, mean, ties)
zero = e['zeroSummary3']['markouts'][0]
assert (zero['availableShares'], zero['totalMarkout']) == ('1', '0.00000')
m = next(m for m in e['zeroComparison']['metricComparisons'] if m['metricKey'] == 'markout_total')
assert (m['dispersion']['sampleCount'], m['dispersion']['tiedCount']) == (1, 1)
assert e['recoverySettled']['launchEnabled']
assert e['stepFocus']
assert e['widths']['run']['scroll'] > e['widths']['run']['viewport']
print('pin, retry identity, idempotency, unavailable vs genuine zero, recovery and overflow observations confirmed')
