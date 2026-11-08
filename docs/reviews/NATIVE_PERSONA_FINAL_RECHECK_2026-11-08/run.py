"""Owned-process wrapper. Run from repo root, no shared service or production writes."""
import pathlib,subprocess,tempfile,sys,time,json,hashlib,shutil
H=pathlib.Path(__file__).resolve().parent; repo=H.parents[2]
root=pathlib.Path(tempfile.mkdtemp(prefix='native-final-360ad69-')); print(root,flush=True)
api=pathlib.Path('/tmp/exsim-persona-review-360ad69/api/exsim_api'); cli=pathlib.Path('/Volumes/PortableSSD/Documents/programming/lob/build/persona-race/exsim_research')
parent=pathlib.Path('/Volumes/PortableSSD/Documents/programming/lob')
rev='360ad69529aa57b5e8856391812cc3d1d93c6896'
def git(*a):return subprocess.check_output(['git',*a],cwd=parent,text=True)
assert git('diff',rev,'--','src','include','tools','Makefile')==''
assert subprocess.check_output(['git','diff',rev,'--','src','include','tools','Makefile'],cwd=repo)==b''
identity={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in [api,cli,parent/'build/persona-race/exsim_api']}
assert identity[str(api)]==identity[str(parent/'build/persona-race/exsim_api')]
delivery=json.loads(pathlib.Path('/tmp/exsim-persona-review-360ad69/delivery.json').read_text()); assert identity[str(api)]==delivery['entries']['api/exsim_api']['sha256']; assert delivery['api_revision']==rev
(root/'binary-identity.json').write_text(json.dumps({'source':rev,'hashes':identity,'native_source_diff':'empty'},indent=2))
for phase in ['baseline_probe.py','corrected_probe.py']:
 with (root/(phase+'.server.log')).open('w') as log:
  p=subprocess.Popen([str(api),'--port','0','--data-dir',str(root/'data'),'--source-revision',rev],stdout=log,stderr=subprocess.STDOUT)
  try:
   for _ in range(1000):
    text=(root/(phase+'.server.log')).read_text()
    if 'http://' in text:break
    assert p.poll() is None,text
    time.sleep(.01)
   base='http://'+text.split('http://')[1].split()[0]
   with (root/(phase+'.log')).open('w') as out:
    r=subprocess.run([sys.executable,str(H/phase),base,str(root),str(repo),str(api),str(cli)],stdout=out,stderr=subprocess.STDOUT)
   print(phase,r.returncode,flush=True);assert r.returncode==0,(root/(phase+'.log')).read_text()[-3000:]
  finally:
   p.terminate();p.wait(timeout=20)
(root/'cleanup.json').write_text(json.dumps({'owned_servers_stopped':True}))
shutil.copy2(root/'binary-identity.json',H/'binary-identity.json')
print('ALL PASS',flush=True)
