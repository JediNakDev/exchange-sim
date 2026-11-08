import json,urllib.request,pathlib
O=pathlib.Path(__file__).parent
B='http://127.0.0.1:45773/v1'
def get(p):
 with urllib.request.urlopen(B+p) as r:return json.load(r)
def save(n,v):(O/(n+'.json')).write_text(json.dumps(v,indent=2)+'\n')
base=json.loads((O/'baseline.json').read_text())
for name,e in {'baseline':base,**json.loads((O/'variants.json').read_text())}.items():
 runs=get('/runs?experimentId='+e['experimentId']);save(name+'-runs',runs)
 for r in runs['items']:
  rid=r['runId'];save(name+'-'+r['masterSeed']+'-summary',get('/runs/'+rid+'/summary'))
 if name!='baseline':save(name+'-comparison',get('/comparisons?baselineExperimentId='+base['experimentId']+'&candidateExperimentId='+e['experimentId']))
rid='run_7edbf747de8f7c91'
for suffix in ['fills?limit=200','orders?limit=200','orders/7/lifecycle','events?fromEventPosition=20&limit=100','manifest']:
 save('baseline-103-'+suffix.replace('/','-').split('?')[0],get('/runs/'+rid+'/'+suffix))
print('collected')
