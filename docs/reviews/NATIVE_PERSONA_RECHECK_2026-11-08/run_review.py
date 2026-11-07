"""Reproduce on ORIGINAL binaries. Fresh disposable loopback store; always stop children.
python3 run_review.py ORIGINAL_API ORIGINAL_CLI [DISTINCT_ORIGINAL_IMPLEMENTATION_API]
No compilation, installation, private service, or production writes.
"""
import pathlib,subprocess,tempfile,sys,time,shutil,json,hashlib
here=pathlib.Path(__file__).resolve().parent;repo=here.parents[2]
root=pathlib.Path(tempfile.mkdtemp(prefix='native-persona-recheck-'));print(root,flush=True)
api=root/'original-exsim_api';cli=root/'original-exsim_research'
shutil.copy2(sys.argv[1],api);shutil.copy2(sys.argv[2],cli)
receipt={'root':str(root),'binaries':{str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in (api,cli)},'phases':[]}
for script,cross in [('probe.py',False),('restart_probe.py',False),('supplemental_probe.py',False),('mm_probe.py',False)]+([('restart_probe.py',True)] if len(sys.argv)>3 else []):
 executable=pathlib.Path(sys.argv[3]) if cross else api
 logname=script+('-cross' if cross else '')
 with (root/(logname+'.server.log')).open('w') as log:
  p=subprocess.Popen([str(executable),'--port','0','--data-dir',str(root/'data')]+(['--source-revision','explicit-review-source','--release-id','explicit-review-release'] if cross else []),stdout=log,stderr=subprocess.STDOUT)
  start=time.monotonic()
  try:
   for i in range(1000):
    text=(root/(logname+'.server.log')).read_text()
    if 'http://' in text:break
    if p.poll() is not None:raise RuntimeError(text)
    time.sleep(.01)
   base='http://'+text.split('http://')[1].split()[0]
   args=[base,str(root)] if script=='mm_probe.py' else [base,str(root),str(repo),str(executable),str(cli)]+(['crossbuild'] if cross else [])
   with (root/(logname+'.log')).open('w') as output:
    result=subprocess.run([sys.executable,str(here/script)]+args,stdout=output,stderr=subprocess.STDOUT)
   receipt['phases'].append(dict(script=logname,seconds=time.monotonic()-start,exit=result.returncode))
   if result.returncode:raise RuntimeError('Probe failed: '+str(root/(logname+'.log')))
  finally:
   p.terminate();p.wait(timeout=20)
   (root/'review-receipt.json').write_text(json.dumps(receipt,indent=2))
# Reproduce CLI full-window admission objection using the same API-owned native dirs.
exps={json.loads(p.read_text())['experimentId']:json.loads(p.read_text()) for p in (root/'data/experiments').glob('*.json')}
runs={}
for p in (root/'data/runs').glob('*/run.json'):
 x=json.loads(p.read_text());name=exps[x['experimentId']]['name']
 if x['state']=='completed' and x['masterSeed']=='101':runs[name]=p.parent/'runner'
with (root/'cli-censored-comparison.json').open('w') as output:
 subprocess.run([str(cli),'compare',str(runs['NATIVE-RECHECK censored']),str(runs['NATIVE-RECHECK cap10']),'--json'],stdout=output,check=True)
