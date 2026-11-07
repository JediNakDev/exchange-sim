"""Run against the same owned scratch store after restart, then distinct actual build.
Usage: restart_probe.py BASE SCRATCH REPO API CLI [crossbuild]
"""
import sys,json,pathlib,hashlib,subprocess
src=pathlib.Path(__file__).with_name('probe.py').read_text();exec(src[:src.index("c=json.loads")])
i=json.loads((S/'identity.json').read_text());a,b,pin=i['a'],i['b'],i['pin'];r=i['run'];paused=i['paused']
assert compare(a,b,pin)['runSelection']==pin
assert compare(a,b)['runSelection']!=pin
assert call('/runs/'+r['runId']+'/manifest')['provenance']==i['provenance']
if 'crossbuild' in sys.argv:
 new=run(a);p=call('/runs/'+new['runId']+'/manifest')['provenance'];assert p['executableSha256']==hashlib.sha256(pathlib.Path(API).read_bytes()).hexdigest();assert p['executableSha256']!=i['provenance']['executableSha256'];assert p['sourceRevision']=='explicit-review-source' and p['releaseId']=='explicit-review-release'
 print('Distinct actual native API builds reproduce selected old runs; old provenance stable, new exact bytes persisted');sys.exit()
# Completed old artifacts without additive trace sidecar are independently reproduced.
d=S/'data'/'runs'/r['runId']/'runner';f=d/'execution-decisions.jsonl';save=f.read_bytes();f.unlink();assert trace(r)[0]['verification']=='native_reproduction_and_replay_verified';f.write_bytes(save)
# A pinned corrupt native artifact fails explicitly without selecting a valid retry.
rd=json.loads((S/'responses.json').read_text()) if False else None
# pin has selected a/b runs; decode is opaque, use ledger selected run identity.
cp=compare(a,b,pin);chosen=next(x['selectedRunId'] for x in cp['attemptLedger'] if x['experimentId']==b and x['selectedRunId']);nf=S/'data'/'runs'/chosen/'runner'/'events.jsonl';data=nf.read_bytes();nf.write_bytes(data+b'{}\n')
call('/comparisons?baselineExperimentId='+a+'&candidateExperimentId='+b+'&runSelection='+pin,expect=409);nf.write_bytes(data)
# Audit refuses symlinks and >2GiB files, never truncated exports.
f=d/'execution-decisions.jsonl';save=f.read_bytes();f.unlink();f.symlink_to(S/'twap-package'/'runner'/'execution-decisions.jsonl');call('/runs/'+r['runId']+'/audit-export',expect=409);f.unlink();f.write_bytes(save)
orig=d/'provenance.json';saved=orig.read_bytes();orig.unlink()
with orig.open('wb') as out:out.truncate((2<<30)+1)
call('/runs/'+r['runId']+'/audit-export',expect=413);orig.unlink();orig.write_bytes(saved)
# Concrete corrupt-partial counterexample: invalid off-page evidence accepted.
pf=S/'data'/'runs'/paused['runId']/'runner'/'execution-decisions.jsonl';ps=pf.read_bytes();pf.write_bytes(ps+b'{invalid-json}\n')
first=call('/runs/'+paused['runId']+'/execution-trace?limit=1');assert first['verification']=='recorded_terminal_partial';assert int(first['totalCount'])==4
call('/runs/'+paused['runId']+'/execution-trace?limit=1000',expect=409);pf.write_bytes(ps)
(S/'restart-responses.json').write_text(json.dumps(records,indent=2));print('Restart, old-sidecar, corrupt-pin, symlink, oversized pass; partial corruption counterexample reproduced')
