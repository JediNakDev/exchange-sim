"""One translation unit, serial compiler slot. Read-only corrected native objects."""
import pathlib,subprocess,tempfile,json,hashlib
h=pathlib.Path(__file__).resolve().parent;out=pathlib.Path(tempfile.mkdtemp(prefix='native-final-engine-'));parent=pathlib.Path('/Volumes/PortableSSD/Documents/programming/lob');objs=sorted((parent/'build/persona-race/obj/src').rglob('*.o'));assert objs
rev='360ad69529aa57b5e8856391812cc3d1d93c6896'
assert subprocess.check_output(['git','diff',rev,'--','src','include','tools','Makefile'],cwd=parent)==b''
assert subprocess.check_output(['git','diff',rev,'--','src','include','tools','Makefile'])==b''
args=['c++','-std=c++20','-O2','-g','-Wall','-Wextra','-Wpedantic','-Wshadow','-Werror','-Iinclude','-I.',str(h/'engine_queue.cpp')]+list(map(str,objs))+['-o',str(out/'probe')]
(out/'objects.json').write_text(json.dumps({str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in objs},indent=2))
r=subprocess.run(args,capture_output=True,text=True);(out/'build.log').write_text(r.stdout+r.stderr);print(str(out),flush=True);assert r.returncode==0,r.stderr
q=subprocess.run([str(out/'probe')],capture_output=True,text=True);(out/'evidence.jsonl').write_text(q.stdout);(out/'run.log').write_text(q.stderr);assert q.returncode==0,(q.returncode,q.stderr)
print('PASS real engine queue scenario',flush=True)
